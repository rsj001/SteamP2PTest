#include "app.h"
#include "log.h"

SteamP2PApp *SteamP2PApp::s_instance = nullptr;
SteamP2PApp::SteamP2PApp()
    : m_cbLobbyEnter(this, &SteamP2PApp::OnLobbyEnter),
      m_cbLobbyJoinRequested(this, &SteamP2PApp::OnLobbyJoinRequested),
      m_cbLobbyDataUpdate(this, &SteamP2PApp::OnLobbyDataUpdate),
      m_cbLobbyChatUpdate(this, &SteamP2PApp::OnLobbyChatUpdate)
{
    s_instance = this;
}

SteamP2PApp::~SteamP2PApp()
{
    Shutdown();
    s_instance = nullptr;
}

// init, tick, shutdown

bool SteamP2PApp::Init()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!SteamAPI_Init())
    {
        Log() << "[Fatal] SteamAPI_Init failed\n";
        return false;
    }
    m_steamInitialized = true;

    {
        ISteamNetworkingUtils *utils = SteamNetworkingUtils();

        //
        // Penalty on SDR relay
        //
        // utils->SetGlobalConfigValueInt32(
        //     k_ESteamNetworkingConfig_P2P_Transport_SDR_Penalty,
        //     99999999);

        // utils->SetGlobalConfigValueInt32(
        //     k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty,
        //     0);

        //
        // Enable ICE
        //
        // utils->SetGlobalConfigValueInt32(
        //     k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
        //     k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);

        // Note: those are the default values, so we don't need to set them

        //
        // Single UDP socket
        //
        // utils->SetGlobalConfigValueInt32(
        //     k_ESteamNetworkingConfig_SDRClient_SingleSocket,
        //     1);

        //
        // LAN Discovery
        //
        // utils->SetGlobalConfigValueInt32(
        //     k_ESteamNetworkingConfig_IP_AllowWithoutAuth,
        //     1);

        //
        // Debug
        //

        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_PacketGaps,
                                         k_ESteamNetworkingSocketsDebugOutputType_Verbose);

        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_Message,
                                         k_ESteamNetworkingSocketsDebugOutputType_Verbose);

        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous,
                                         k_ESteamNetworkingSocketsDebugOutputType_Verbose);

        utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Verbose,
                                      [](ESteamNetworkingSocketsDebugOutputType, const char *msg)
                                      { Log() << "[Dbg] " << msg << "\n"; });
    }

    if (!SteamUser()->BLoggedOn())
    {
        Log() << "[Fatal] Steam user not logged in\n";
        return false;
    }

    Log() << "[Init] Logged in as: " << SteamFriends()->GetPersonaName() << "\n";
    Log() << "[Init] SteamID: " << SteamUser()->GetSteamID().ConvertToUint64() << "\n";
    Log() << "[Init] Overlay: " << (SteamUtils()->IsOverlayEnabled() ? "enabled" : "disabled") << "\n";

    SteamNetworkingUtils()->InitRelayNetworkAccess();

    SteamRelayNetworkStatus_t relay{};
    SteamNetworkingUtils()->GetRelayNetworkStatus(&relay);
    Log() << "[Init] Relay availability: " << relay.m_eAvail << "\n";

    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&SteamP2PApp::SteamNetConnectionStatusChanged);

    EnsureListenSocketLocked();
    EnsurePollGroupLocked();

    return true;
}

void SteamP2PApp::Tick()
{
    SteamAPI_RunCallbacks();
    SteamNetworkingSockets()->RunCallbacks();

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        PumpPendingTimeoutsLocked();
    }

    PollIncomingMessages();
}

void SteamP2PApp::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_shutdown)
        return;
    m_shutdown = true;

    LeaveLobbyLocked();

    if (m_listenSocket != k_HSteamListenSocket_Invalid)
    {
        SteamNetworkingSockets()->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
    {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    if (m_steamInitialized)
    {
        SteamAPI_Shutdown();
        m_steamInitialized = false;
    }
}

// Send Chat 
void SteamP2PApp::SendChat(const std::string &text)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_lobby.IsValid())
    {
        Log() << "[Error] Not in a lobby\n";
        return;
    }

    if (m_isHost)
    {
        bool sentAny = false;
        for (const auto &[steamId, conn] : m_connectedBySteamId)
        {
            if (SendMessageToConnectionLocked(conn, text))
                sentAny = true;
        }
        if (!sentAny)
            Log() << "[Error] No connected peers to broadcast to\n";
        return;
    }

    if (m_lobbyOwnerSteamId64 == 0)
    {
        Log() << "[Error] No lobby owner yet\n";
        return;
    }

    auto it = m_connectedBySteamId.find(m_lobbyOwnerSteamId64);
    if (it == m_connectedBySteamId.end())
    {
        Log() << "[Error] Host not connected yet\n";
        return;
    }

    if (!SendMessageToConnectionLocked(it->second, text))
        Log() << "[Error] Send failed\n";
}

// debug helpers

void SteamP2PApp::PrintConnectionInfo(HSteamNetConnection conn)
{
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
    {
        Log() << "[Debug] GetConnectionInfo failed\n";
        return;
    }

    char remote[256]{};
    info.m_addrRemote.ToString(remote, sizeof(remote), 1);

    char status[4096];
    SteamNetworkingSockets()->GetDetailedConnectionStatus(conn, status, sizeof(status));

    Log() << "\n========== CONNECTION INFO ==========\n"
          << "Handle: " << conn << "\n"
          << "User: " << info.m_identityRemote.GetSteamID64() << "\n"
          << "End reason: " << info.m_eEndReason << "\n"
          << "Description: " << info.m_szEndDebug << "\n"
          << "Listen socket: " << info.m_hListenSocket << "\n"
          << "Connection user data: " << info.m_nUserData << "\n"
          << "Remote address: " << remote << "\n"
          << "\n----- Detailed Status -----\n"
          << status << "\n";

    SteamNetConnectionRealTimeStatus_t rt{};
    if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &rt, 0, nullptr))
    {
        Log() << "\n----- Realtime -----\n"
              << "Ping: " << rt.m_nPing << " ms\n"
              << "Connection Quality Local: " << rt.m_flConnectionQualityLocal << "\n"
              << "Connection Quality Remote: " << rt.m_flConnectionQualityRemote << "\n"
              << "Packets/sec out: " << rt.m_flOutPacketsPerSec << "\n"
              << "Packets/sec in: " << rt.m_flInPacketsPerSec << "\n"
              << "Bytes/sec out: " << rt.m_flOutBytesPerSec << "\n"
              << "Bytes/sec in: " << rt.m_flInBytesPerSec << "\n"
              << "Send rate bytes/sec: " << rt.m_nSendRateBytesPerSecond << "\n"
              << "Pending unreliable: " << rt.m_cbPendingUnreliable << "\n"
              << "Pending reliable: " << rt.m_cbPendingReliable << "\n"
              << "Sent unreliable: " << rt.m_cbSentUnackedReliable << "\n";
    }

    Log() << "=====================================\n\n";
}

std::string SteamP2PApp::GetSteamName(uint64_t steamId64) const
{
    if (steamId64 == 0)
        return "unknown";

    CSteamID id(steamId64);
    const char *name = SteamFriends()->GetFriendPersonaName(id);
    if (name && name[0] != '\0')
        return name;

    return std::to_string(steamId64);
}

void SteamP2PApp::PrintInfo()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Log() << "\n=============== INFO ===============\n"
          << "Self: " << SteamFriends()->GetPersonaName()
          << " (" << SteamUser()->GetSteamID().ConvertToUint64() << ")\n"
          << "Lobby valid: " << (m_lobby.IsValid() ? "yes" : "no") << "\n";

    if (m_lobby.IsValid())
    {
        CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
        m_lobbyOwnerSteamId64 = owner.ConvertToUint64();

        Log() << "Lobby ID: " << m_lobby.ConvertToUint64() << "\n"
              << "Owner: " << m_lobbyOwnerSteamId64 << "\n"
              << "Role: " << (m_isHost ? "host" : "client") << "\n";

        int members = SteamMatchmaking()->GetNumLobbyMembers(m_lobby);
        Log() << "Lobby members: " << members << "\n";

        for (int i = 0; i < members; ++i)
        {
            CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(m_lobby, i);
            Log() << "  - " << SteamFriends()->GetFriendPersonaName(member)
                  << " (" << member.ConvertToUint64() << ")"
                  << (member == owner ? " [HOST]" : "") << "\n";
        }
    }

    Log() << "Listen socket: "
          << (m_listenSocket == k_HSteamListenSocket_Invalid ? "invalid" : "ready") << "\n"
          << "Poll group: "
          << (m_pollGroup == k_HSteamNetPollGroup_Invalid ? "invalid" : "ready") << "\n"
          << "Pending connections: " << m_pendingByConn.size() << "\n";

    for (const auto &[conn, peer] : m_pendingByConn)
    {
        Log() << "  - conn=" << conn
              << " target=" << peer.targetSteamId64
              << " state=" << static_cast<int>(peer.state)
              << " age_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - peer.startedAt)
                     .count()
              << "\n";
    }

    Log() << "Connected peers: " << m_connectedBySteamId.size() << "\n";
    for (const auto &[steamId, conn] : m_connectedBySteamId)
    {
        Log() << "  - " << steamId << " conn=" << conn << "\n";
        PrintConnectionInfo(conn);
    }

    SteamRelayNetworkStatus_t relay{};
    SteamNetworkingUtils()->GetRelayNetworkStatus(&relay);
    Log() << "Relay availability: " << relay.m_eAvail << "\n"
          << "=====================================\n\n";
}
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    SteamP2PApp app;
    if (!app.Init())
        return 1;

    std::atomic<bool> running = true;

    std::thread inputThread([&]()
                            {
        std::cout << "Commands:\n"
                 "  create\n"
                 "  invite\n"
                 "  join <lobbyid>\n"
                 "  info\n"
                 "  leave\n"
                 "  quit\n"
                 "  <text>  send chat\n";

        std::string line;
        while (running)
        {
            std::cout << "> " << std::flush;

            if (!std::getline(std::cin, line))
            {
                running = false;
                break;
            }

            if      (line == "create")  app.CreateLobby();
            else if (line == "invite")  app.OpenInviteOverlay();
            else if (line == "info")    app.PrintInfo();
            else if (line == "leave")   app.LeaveLobby();
            else if (line == "quit")    running = false;
            else if (line.rfind("join ", 0) == 0)
            {
                try
                {
                    app.JoinLobby(std::stoull(line.substr(5)));
                }
                catch (...)
                {
                    Log() << "[Error] Invalid lobby id\n";
                }
            }
            else if (!line.empty())
            {
                app.SendChat(line);
            }
        } });

    while (running)
    {
        app.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (inputThread.joinable())
        inputThread.join();

    return 0;
}
