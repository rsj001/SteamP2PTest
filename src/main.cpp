#include <windows.h>

#include <steam/steam_api.h>
#include <steam/isteamfriends.h>
#include <steam/isteammatchmaking.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using std::cout;

class SteamP2PApp
{
public:
    SteamP2PApp()
        : m_cbLobbyEnter(this, &SteamP2PApp::OnLobbyEnter), m_cbLobbyJoinRequested(this, &SteamP2PApp::OnLobbyJoinRequested), m_cbLobbyDataUpdate(this, &SteamP2PApp::OnLobbyDataUpdate), m_cbLobbyChatUpdate(this, &SteamP2PApp::OnLobbyChatUpdate)
    {
        s_instance = this;
    }

    ~SteamP2PApp()
    {
        Shutdown();
        s_instance = nullptr;
    }

    bool Init()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (!SteamAPI_Init())
        {
            std::cerr << "[Fatal] SteamAPI_Init failed\n";
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

            utils->SetGlobalConfigValueInt32(
                k_ESteamNetworkingConfig_LogLevel_PacketGaps,
                k_ESteamNetworkingSocketsDebugOutputType_Verbose);

            utils->SetGlobalConfigValueInt32(
                k_ESteamNetworkingConfig_LogLevel_Message,
                k_ESteamNetworkingSocketsDebugOutputType_Verbose);

            utils->SetGlobalConfigValueInt32(
                k_ESteamNetworkingConfig_LogLevel_P2PRendezvous,
                k_ESteamNetworkingSocketsDebugOutputType_Verbose);

            utils->SetGlobalConfigValueInt32(
                k_ESteamNetworkingConfig_LogLevel_PacketGaps,
                k_ESteamNetworkingSocketsDebugOutputType_Verbose);

            utils->SetDebugOutputFunction(
                k_ESteamNetworkingSocketsDebugOutputType_Verbose,
                [](ESteamNetworkingSocketsDebugOutputType,
                   const char *msg)
                {
                    printf("[NetDebug] %s\n", msg);
                });

            printf("[Net] DIRECT ONLY mode enabled\n");
        }

        if (!SteamUser()->BLoggedOn())
        {
            std::cerr << "[Fatal] Steam user not logged in\n";
            return false;
        }

        cout << "[Init] Logged in as: "
                  << SteamFriends()->GetPersonaName()
                  << "\n";

        cout << "[Init] SteamID: "
                  << SteamUser()->GetSteamID().ConvertToUint64()
                  << "\n";

        cout << "[Init] Overlay: "
                  << (SteamUtils()->IsOverlayEnabled() ? "enabled" : "disabled")
                  << "\n";

        SteamNetworkingUtils()->InitRelayNetworkAccess();

        SteamRelayNetworkStatus_t relay{};
        SteamNetworkingUtils()->GetRelayNetworkStatus(&relay);
        cout << "[Init] Relay availability: "
                  << relay.m_eAvail
                  << "\n";

        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            &SteamP2PApp::SteamNetConnectionStatusChanged);

        EnsureListenSocketLocked();
        EnsurePollGroupLocked();

        return true;
    }

    void Tick()
    {
        SteamAPI_RunCallbacks();
        // SteamGameServer_RunCallbacks();
        SteamNetworkingSockets()->RunCallbacks();

        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            PumpPendingTimeoutsLocked();
        }

        PollIncomingMessages();
    }

    void CreateLobby()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (m_lobby.IsValid())
        {
            cout << "[Lobby] Already in a lobby, leaving first...\n";
            LeaveLobbyLocked();
        }

        cout << "[Lobby] Creating lobby...\n";

        SteamAPICall_t call =
            SteamMatchmaking()->CreateLobby(
                k_ELobbyTypeFriendsOnly,
                4);

        m_callLobbyCreated.Set(
            call,
            this,
            &SteamP2PApp::OnLobbyCreated);
    }

    void JoinLobby(uint64_t id)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        CSteamID lobby(id);

        if (!lobby.IsValid())
        {
            cout << "[Lobby] Invalid lobby id\n";
            return;
        }

        if (m_lobby.IsValid() && m_lobby != lobby)
        {
            LeaveLobbyLocked();
        }

        cout << "[Lobby] Joining " << id << "\n";
        SteamMatchmaking()->JoinLobby(lobby);
    }

    void OpenInviteOverlay()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (!m_lobby.IsValid())
        {
            cout << "[Error] No active lobby\n";
            return;
        }

        cout << "[UI] Opening invite overlay\n";
        SteamFriends()->ActivateGameOverlayInviteDialog(m_lobby);
        cout << "[UI] Overlay may be disabled. Invite your friends manually in Steam.\n";
    }

    void SendChat(const std::string &text)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (!m_lobby.IsValid())
        {
            cout << "[Error] Not in a lobby\n";
            return;
        }

        if (m_isHost)
        {
            bool sentAny = false;

            for (const auto &[steamId, conn] : m_connectedBySteamId)
            {
                (void)steamId;
                if (SendMessageToConnectionLocked(conn, text))
                {
                    sentAny = true;
                }
            }

            if (!sentAny)
            {
                cout << "[Error] No connected peers to broadcast to\n";
            }
            return;
        }

        if (m_lobbyOwnerSteamId64 == 0)
        {
            cout << "[Error] No lobby owner yet\n";
            return;
        }

        auto it = m_connectedBySteamId.find(m_lobbyOwnerSteamId64);
        if (it == m_connectedBySteamId.end())
        {
            cout << "[Error] Host not connected yet\n";
            return;
        }

        if (!SendMessageToConnectionLocked(it->second, text))
        {
            cout << "[Error] Send failed\n";
        }
    }

    // ============================================================
    // DEBUG OUTPUT
    // ============================================================

    void PrintConnectionInfo(HSteamNetConnection conn)
    {
        SteamNetConnectionInfo_t info{};

        if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
        {
            cout << "[Debug] GetConnectionInfo failed\n";
            return;
        }

        cout << "\n========== CONNECTION INFO ==========\n";

        cout << "Handle: " << conn << "\n";

        cout << "User: "
             << info.m_identityRemote.GetSteamID64()
             << "\n";

        cout << "End reason: "
             << info.m_eEndReason
             << "\n";

        cout << "Description: "
             << info.m_szEndDebug
             << "\n";

        cout << "Listen socket: "
             << info.m_hListenSocket
             << "\n";

        cout << "Connection user data: "
             << info.m_nUserData
             << "\n";

        char remote[256]{};
        info.m_addrRemote.ToString(remote, sizeof(remote), 1);

        std ::cout << "Remote address: "
                   << remote
                   << "\n";

        char status[4096];
        SteamNetworkingSockets()->GetDetailedConnectionStatus(
            conn,
            status,
            sizeof(status));

        cout << "\n----- Detailed Status -----\n";
        cout << status << "\n";

        SteamNetConnectionRealTimeStatus_t realtime{};
        int lanes = 0;

        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(
                conn,
                &realtime,
                0,
                nullptr))
        {
            cout << "\n----- Realtime -----\n";

            cout << "Ping: "
                 << realtime.m_nPing
                 << " ms\n";

            cout << "Connection Quality Local: "
                 << realtime.m_flConnectionQualityLocal
                 << "\n";

            cout << "Connection Quality Remote: "
                 << realtime.m_flConnectionQualityRemote
                 << "\n";

            cout << "Packets/sec out: "
                 << realtime.m_flOutPacketsPerSec
                 << "\n";

            cout << "Packets/sec in: "
                 << realtime.m_flInPacketsPerSec
                 << "\n";

            cout << "Bytes/sec out: "
                 << realtime.m_flOutBytesPerSec
                 << "\n";

            cout << "Bytes/sec in: "
                 << realtime.m_flInBytesPerSec
                 << "\n";

            cout << "Send rate bytes/sec: "
                 << realtime.m_nSendRateBytesPerSecond
                 << "\n";

            cout << "Pending unreliable: "
                 << realtime.m_cbPendingUnreliable
                 << "\n";

            cout << "Pending reliable: "
                 << realtime.m_cbPendingReliable
                 << "\n";

            cout << "Sent unreliable: "
                 << realtime.m_cbSentUnackedReliable
                 << "\n";
        }

        cout << "=====================================\n\n";
    }

    void PrintInfo()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        cout << "\n========== INFO ==========\n";

        cout << "Self: "
                  << SteamFriends()->GetPersonaName()
                  << " ("
                  << SteamUser()->GetSteamID().ConvertToUint64()
                  << ")\n";

        cout << "Lobby valid: "
                  << (m_lobby.IsValid() ? "yes" : "no")
                  << "\n";

        if (m_lobby.IsValid())
        {
            cout << "Lobby ID: "
                      << m_lobby.ConvertToUint64()
                      << "\n";

            CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
            m_lobbyOwnerSteamId64 = owner.ConvertToUint64();

            cout << "Owner: "
                      << m_lobbyOwnerSteamId64
                      << "\n";

            cout << "Role: "
                      << (m_isHost ? "host" : "client")
                      << "\n";

            int members = SteamMatchmaking()->GetNumLobbyMembers(m_lobby);
            cout << "Lobby members: "
                      << members
                      << "\n";

            for (int i = 0; i < members; ++i)
            {
                CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(m_lobby, i);

                cout << "  - "
                          << SteamFriends()->GetFriendPersonaName(member)
                          << " ("
                          << member.ConvertToUint64()
                          << ")";

                if (member == owner)
                {
                    cout << " [HOST]";
                }

                cout << "\n";
            }
        }

        cout << "Listen socket: "
                  << (m_listenSocket == k_HSteamListenSocket_Invalid ? "invalid" : "ready")
                  << "\n";

        cout << "Poll group: "
                  << (m_pollGroup == k_HSteamNetPollGroup_Invalid ? "invalid" : "ready")
                  << "\n";

        cout << "Pending connections: "
                  << m_pendingByConn.size()
                  << "\n";

        for (const auto &[conn, peer] : m_pendingByConn)
        {
            cout << "  - conn="
                      << conn
                      << " target="
                      << peer.targetSteamId64
                      << " state="
                      << static_cast<int>(peer.state)
                      << " age_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - peer.startedAt)
                             .count()
                      << "\n";
        }

        cout << "Connected peers: "
                  << m_connectedBySteamId.size()
                  << "\n";

        for (const auto &[steamId, conn] : m_connectedBySteamId)
        {
            cout << "  - "
                      << steamId
                      << " conn="
                      << conn
                      << "\n";
            PrintConnectionInfo(conn);
        }

        SteamRelayNetworkStatus_t relay{};
        SteamNetworkingUtils()->GetRelayNetworkStatus(&relay);

        cout << "Relay availability: "
                  << relay.m_eAvail
                  << "\n";

        cout << "==========================\n\n";
    }

    void LeaveLobby()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        LeaveLobbyLocked();
    }

private:
    static constexpr int kVirtualPort = 0;
    static constexpr int kMaxReceiveMessages = 32;
    static constexpr std::chrono::seconds kPendingTimeout{20};

    struct PendingPeer
    {
        uint64_t targetSteamId64 = 0;
        ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_None;
        std::chrono::steady_clock::time_point startedAt{};
    };

    static SteamP2PApp *s_instance;

    std::recursive_mutex m_mutex;
    bool m_shutdown = false;
    bool m_steamInitialized = false;

    CSteamID m_lobby;
    uint64_t m_lobbyOwnerSteamId64 = 0;
    bool m_isHost = false;

    HSteamListenSocket m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_pollGroup = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, PendingPeer> m_pendingByConn;
    std::unordered_map<uint64_t, HSteamNetConnection> m_connectedBySteamId;
    std::unordered_map<HSteamNetConnection, uint64_t> m_connToSteamId;

    CCallResult<SteamP2PApp, LobbyCreated_t> m_callLobbyCreated;
    CCallback<SteamP2PApp, LobbyEnter_t> m_cbLobbyEnter;
    CCallback<SteamP2PApp, GameLobbyJoinRequested_t> m_cbLobbyJoinRequested;
    CCallback<SteamP2PApp, LobbyDataUpdate_t> m_cbLobbyDataUpdate;
    CCallback<SteamP2PApp, LobbyChatUpdate_t> m_cbLobbyChatUpdate;

private:
    void EnsurePollGroupLocked()
    {
        if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        {
            return;
        }

        m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();
        if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
        {
            cout << "[Net] CreatePollGroup failed\n";
        }
        else
        {
            cout << "[Net] Poll group ready\n";
        }
    }

    void EnsureListenSocketLocked()
    {
        if (m_listenSocket != k_HSteamListenSocket_Invalid)
        {
            return;
        }

        m_listenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(
            kVirtualPort,
            0,
            nullptr);

        if (m_listenSocket == k_HSteamListenSocket_Invalid)
        {
            cout << "[Net] CreateListenSocketP2P failed\n";
        }
        else
        {
            cout << "[Net] Listen socket ready\n";
        }
    }

    void CloseListenSocketLocked()
    {
        if (m_listenSocket == k_HSteamListenSocket_Invalid)
        {
            return;
        }

        SteamNetworkingSockets()->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    void CloseAllConnectionsLocked(const char *reason)
    {
        std::vector<HSteamNetConnection> conns;
        conns.reserve(m_pendingByConn.size() + m_connectedBySteamId.size());

        for (const auto &[conn, peer] : m_pendingByConn)
        {
            (void)peer;
            conns.push_back(conn);
        }

        for (const auto &[steamId, conn] : m_connectedBySteamId)
        {
            (void)steamId;
            conns.push_back(conn);
        }

        m_pendingByConn.clear();
        m_connectedBySteamId.clear();
        m_connToSteamId.clear();

        for (HSteamNetConnection conn : conns)
        {
            if (conn != k_HSteamNetConnection_Invalid)
            {
                SteamNetworkingSockets()->CloseConnection(
                    conn,
                    0,
                    reason,
                    false);
            }
        }
    }

    void DisconnectIfExistsLocked(HSteamNetConnection conn, const char *reason)
    {
        if (conn == k_HSteamNetConnection_Invalid)
        {
            return;
        }
        SteamNetworkingSockets()->CloseConnection(conn, 0, reason, false);
    }

    bool HasAnyConnectionToSteamIdLocked(uint64_t steamId64) const
    {
        if (steamId64 == 0)
        {
            return false;
        }

        if (m_connectedBySteamId.find(steamId64) != m_connectedBySteamId.end())
        {
            return true;
        }

        for (const auto &[conn, peer] : m_pendingByConn)
        {
            (void)conn;
            if (peer.targetSteamId64 == steamId64)
            {
                return true;
            }
        }

        return false;
    }

    bool IsConnectionConnectedLocked(HSteamNetConnection conn) const
    {
        SteamNetConnectionInfo_t info{};
        if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
        {
            return false;
        }
        return info.m_eState == k_ESteamNetworkingConnectionState_Connected;
    }

    bool SendMessageToConnectionLocked(HSteamNetConnection conn, const std::string &text)
    {
        if (conn == k_HSteamNetConnection_Invalid)
        {
            return false;
        }

        if (!IsConnectionConnectedLocked(conn))
        {
            return false;
        }

        EResult r = SteamNetworkingSockets()->SendMessageToConnection(
            conn,
            text.data(),
            static_cast<uint32>(text.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr);

        if (r != k_EResultOK)
        {
            cout << "[Error] Send failed: " << static_cast<int>(r) << "\n";
            return false;
        }

        return true;
    }

    void TryConnectToLobbyOwnerLocked()
    {
        if (!m_lobby.IsValid() || m_isHost)
        {
            return;
        }

        if (m_lobbyOwnerSteamId64 == 0)
        {
            CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
            m_lobbyOwnerSteamId64 = owner.ConvertToUint64();
        }

        if (m_lobbyOwnerSteamId64 == 0)
        {
            cout << "[Net] No lobby owner yet\n";
            return;
        }

        if (HasAnyConnectionToSteamIdLocked(m_lobbyOwnerSteamId64))
        {
            return;
        }

        SteamNetworkingIdentity identity{};
        identity.SetSteamID(CSteamID(m_lobbyOwnerSteamId64));

        HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(
            identity,
            kVirtualPort,
            0,
            nullptr);

        if (conn == k_HSteamNetConnection_Invalid)
        {
            cout << "[Net] ConnectP2P failed\n";
            return;
        }

        PendingPeer peer;
        peer.targetSteamId64 = m_lobbyOwnerSteamId64;
        peer.state = k_ESteamNetworkingConnectionState_Connecting;
        peer.startedAt = std::chrono::steady_clock::now();

        m_pendingByConn[conn] = peer;

        cout << "[Net] Connecting to host "
                  << m_lobbyOwnerSteamId64
                  << "\n";
    }

    void SyncLobbyNetworkingLocked()
    {
        if (!m_lobby.IsValid())
        {
            return;
        }

        CSteamID self = SteamUser()->GetSteamID();
        CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
        uint64_t owner64 = owner.ConvertToUint64();

        bool shouldHost = (owner == self);
        bool roleChanged = (shouldHost != m_isHost);
        bool ownerChanged = (owner64 != m_lobbyOwnerSteamId64);

        m_lobbyOwnerSteamId64 = owner64;
        m_isHost = shouldHost;

        if (roleChanged)
        {
            CloseAllConnectionsLocked("role-changed");
        }
        else if (ownerChanged)
        {
            CloseAllConnectionsLocked("owner-changed");
        }

        EnsureListenSocketLocked();
        EnsurePollGroupLocked();

        if (m_isHost)
        {
            cout << "[Net] Acting as host\n";
        }
        else
        {
            cout << "[Net] Acting as client\n";
            TryConnectToLobbyOwnerLocked();
        }
    }

    void LeaveLobbyLocked()
    {
        CloseAllConnectionsLocked("leave-lobby");

        if (m_lobby.IsValid())
        {
            SteamMatchmaking()->LeaveLobby(m_lobby);
            m_lobby.Clear();
        }

        m_lobbyOwnerSteamId64 = 0;
        m_isHost = false;

        cout << "[Lobby] Left\n";
    }

    void PollIncomingMessages()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
        {
            return;
        }

        SteamNetworkingMessage_t *msgs[kMaxReceiveMessages]{};

        while (true)
        {
            int count = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(
                m_pollGroup,
                msgs,
                kMaxReceiveMessages);

            if (count <= 0)
            {
                break;
            }

            for (int i = 0; i < count; ++i)
            {
                SteamNetworkingMessage_t *msg = msgs[i];

                uint64_t peerSteamId = 0;
                auto it = m_connToSteamId.find(msg->m_conn);
                if (it != m_connToSteamId.end())
                {
                    peerSteamId = it->second;
                }

                std::string text(
                    static_cast<char *>(msg->m_pData),
                    msg->m_cbSize);

                cout << "\n[Message]";
                if (peerSteamId != 0)
                {
                    cout << "[" << peerSteamId << "]";
                }
                cout << " " << text << "\n> " << std::flush;

                msg->Release();
            }

            if (count < kMaxReceiveMessages)
            {
                break;
            }
        }
    }

    void DumpConnectionDetailsLocked(HSteamNetConnection conn, const char *prefix)
    {
        char buf[4096]{};
        int r = SteamNetworkingSockets()->GetDetailedConnectionStatus(conn, buf, sizeof(buf));
        cout << prefix << " detailed_status_rc=" << r << "\n";
        if (r >= 0)
        {
            cout << buf << "\n";
        }
    }

    void PumpPendingTimeoutsLocked()
    {
        auto now = std::chrono::steady_clock::now();

        std::vector<HSteamNetConnection> expired;
        expired.reserve(m_pendingByConn.size());

        for (const auto &[conn, peer] : m_pendingByConn)
        {
            if (now - peer.startedAt > kPendingTimeout)
            {
                expired.push_back(conn);
            }
        }

        for (HSteamNetConnection conn : expired)
        {
            auto it = m_pendingByConn.find(conn);
            if (it == m_pendingByConn.end())
            {
                continue;
            }

            cout << "[Net] Pending timeout for conn="
                      << conn
                      << " target="
                      << it->second.targetSteamId64
                      << "\n";

            DumpConnectionDetailsLocked(conn, "[NetTimeout]");
            SteamNetworkingSockets()->CloseConnection(
                conn,
                0,
                "pending-timeout",
                false);

            m_pendingByConn.erase(it);
        }

        if (!m_isHost && m_lobby.IsValid())
        {
            if (!HasAnyConnectionToSteamIdLocked(m_lobbyOwnerSteamId64))
            {
                TryConnectToLobbyOwnerLocked();
            }
        }
    }

private:
    void OnLobbyCreated(LobbyCreated_t *result, bool bIOFailure)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (bIOFailure || result->m_eResult != k_EResultOK)
        {
            cout << "[Lobby] Create failed\n";
            return;
        }

        cout << "[Lobby] Created successfully\n";
    }

    void OnLobbyEnter(LobbyEnter_t *param)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (param->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
        {
            cout << "[Lobby] Enter failed\n";
            return;
        }

        m_lobby = CSteamID(param->m_ulSteamIDLobby);

        cout << "[Lobby] Entered lobby "
                  << m_lobby.ConvertToUint64()
                  << "\n";

        SyncLobbyNetworkingLocked();
        PrintInfo();
    }

    void OnLobbyJoinRequested(GameLobbyJoinRequested_t *param)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        cout << "[Lobby] Invite accepted\n";

        if (m_lobby.IsValid() && m_lobby != param->m_steamIDLobby)
        {
            LeaveLobbyLocked();
        }

        SteamMatchmaking()->JoinLobby(param->m_steamIDLobby);
    }

    void OnLobbyDataUpdate(LobbyDataUpdate_t *param)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (!m_lobby.IsValid())
        {
            return;
        }

        if (param->m_ulSteamIDLobby != m_lobby.ConvertToUint64())
        {
            return;
        }

        cout << "[Lobby] Data updated\n";
        SyncLobbyNetworkingLocked();
        PrintInfo();
    }

    void OnLobbyChatUpdate(LobbyChatUpdate_t *param)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (!m_lobby.IsValid())
        {
            return;
        }

        if (param->m_ulSteamIDLobby != m_lobby.ConvertToUint64())
        {
            return;
        }

        cout << "[Lobby] Member update\n";
        PrintInfo();
    }

private:
    static void SteamNetConnectionStatusChanged(
        SteamNetConnectionStatusChangedCallback_t *pInfo)
    {
        if (!s_instance)
        {
            return;
        }

        s_instance->HandleConnectionStatusChanged(pInfo);
    }

    void HandleConnectionStatusChanged(
        SteamNetConnectionStatusChangedCallback_t *pInfo)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        auto state = pInfo->m_info.m_eState;

        char remote[256]{};
        pInfo->m_info.m_identityRemote.ToString(remote, sizeof(remote));

        cout << "[NetDebug] state="
                  << static_cast<int>(state)
                  << " remote="
                  << remote
                  << " listen="
                  << pInfo->m_info.m_hListenSocket
                  << " conn="
                  << pInfo->m_hConn
                  << " debug='"
                  << (pInfo->m_info.m_szEndDebug ? pInfo->m_info.m_szEndDebug : "")
                  << "'\n";

        switch (state)
        {
        case k_ESteamNetworkingConnectionState_None:
            break;

        case k_ESteamNetworkingConnectionState_Connecting:
        {
            uint64_t remoteSteamId = pInfo->m_info.m_identityRemote.GetSteamID64();

            PendingPeer &peer = m_pendingByConn[pInfo->m_hConn];
            peer.targetSteamId64 = remoteSteamId;
            peer.state = state;
            peer.startedAt = std::chrono::steady_clock::now();

            if (pInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
            {
                cout << "[Net] Incoming connection request\n";

                EResult r = SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);
                if (r != k_EResultOK)
                {
                    cout << "[Net] Accept failed: " << static_cast<int>(r) << "\n";
                    DumpConnectionDetailsLocked(pInfo->m_hConn, "[NetAcceptFailed]");
                    SteamNetworkingSockets()->CloseConnection(
                        pInfo->m_hConn,
                        0,
                        "accept-failed",
                        false);
                    m_pendingByConn.erase(pInfo->m_hConn);
                    return;
                }

                cout << "[Net] AcceptConnection success\n";
            }
            else
            {
                cout << "[Net] Outbound connection progressing\n";
            }

            break;
        }

        case k_ESteamNetworkingConnectionState_FindingRoute:
        {
            auto it = m_pendingByConn.find(pInfo->m_hConn);
            if (it != m_pendingByConn.end())
            {
                it->second.state = state;
            }

            cout << "[Net] Finding relay/network route...\n";
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected:
        {
            uint64_t remoteSteamId = pInfo->m_info.m_identityRemote.GetSteamID64();

            auto pit = m_pendingByConn.find(pInfo->m_hConn);
            if (remoteSteamId == 0 && pit != m_pendingByConn.end())
            {
                remoteSteamId = pit->second.targetSteamId64;
            }

            if (remoteSteamId != 0)
            {
                auto existing = m_connectedBySteamId.find(remoteSteamId);
                if (existing != m_connectedBySteamId.end() &&
                    existing->second != pInfo->m_hConn)
                {
                    SteamNetworkingSockets()->CloseConnection(
                        existing->second,
                        0,
                        "duplicate-connection",
                        false);
                }

                m_connectedBySteamId[remoteSteamId] = pInfo->m_hConn;
                m_connToSteamId[pInfo->m_hConn] = remoteSteamId;
            }

            m_pendingByConn.erase(pInfo->m_hConn);

            if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
            {
                bool ok = SteamNetworkingSockets()->SetConnectionPollGroup(
                    pInfo->m_hConn,
                    m_pollGroup);

                cout << "[Net] SetConnectionPollGroup: "
                          << (ok ? "OK" : "FAILED")
                          << "\n";
            }

            cout << "[Net] Connected";
            if (remoteSteamId != 0)
            {
                cout << " peer=" << remoteSteamId;
            }
            cout << "\n";

            PrintInfo();
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            uint64_t peerSteamId = 0;

            auto itConn = m_connToSteamId.find(pInfo->m_hConn);
            if (itConn != m_connToSteamId.end())
            {
                peerSteamId = itConn->second;
            }
            else
            {
                auto itPend = m_pendingByConn.find(pInfo->m_hConn);
                if (itPend != m_pendingByConn.end())
                {
                    peerSteamId = itPend->second.targetSteamId64;
                }
            }

            if (peerSteamId != 0)
            {
                auto itConnMap = m_connectedBySteamId.find(peerSteamId);
                if (itConnMap != m_connectedBySteamId.end() &&
                    itConnMap->second == pInfo->m_hConn)
                {
                    m_connectedBySteamId.erase(itConnMap);
                }
            }

            m_connToSteamId.erase(pInfo->m_hConn);
            m_pendingByConn.erase(pInfo->m_hConn);

            cout << "[Net] Connection closed\n";
            cout << "[Net] Reason='"
                      << (pInfo->m_info.m_szEndDebug ? pInfo->m_info.m_szEndDebug : "")
                      << "'\n";

            DumpConnectionDetailsLocked(pInfo->m_hConn, "[NetClosed]");

            SteamNetworkingSockets()->CloseConnection(
                pInfo->m_hConn,
                0,
                nullptr,
                false);

            if (!m_shutdown && m_lobby.IsValid() && !m_isHost)
            {
                if (peerSteamId != 0 && peerSteamId == m_lobbyOwnerSteamId64)
                {
                    cout << "[Net] Lost host connection, will reconnect\n";
                    TryConnectToLobbyOwnerLocked();
                }
            }

            break;
        }

        default:
            cout << "[Net] Unhandled state="
                      << static_cast<int>(state)
                      << "\n";
            break;
        }
    }

    void Shutdown()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        if (m_shutdown)
        {
            return;
        }
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
};

SteamP2PApp *SteamP2PApp::s_instance = nullptr;

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    SteamP2PApp app;

    if (!app.Init())
    {
        return 1;
    }

    std::atomic<bool> running = true;

    std::thread inputThread([&]()
                            {
        cout
            << "Commands:\n"
            << "  create\n"
            << "  invite\n"
            << "  join <lobbyid>\n"
            << "  info\n"
            << "  leave\n"
            << "  quit\n"
            << "  <text> send chat\n";

        std::string line;

        while (running) {
            cout << "> " << std::flush;

            if (!std::getline(std::cin, line)) {
                running = false;
                break;
            }

            if (line == "create") {
                app.CreateLobby();
            }
            else if (line == "invite") {
                app.OpenInviteOverlay();
            }
            else if (line == "info") {
                app.PrintInfo();
            }
            else if (line == "leave") {
                app.LeaveLobby();
            }
            else if (line == "quit") {
                running = false;
            }
            else if (line.rfind("join ", 0) == 0) {
                try {
                    uint64_t id = std::stoull(line.substr(5));
                    app.JoinLobby(id);
                }
                catch (...) {
                    cout << "[Error] Invalid lobby id\n";
                }
            }
            else if (!line.empty()) {
                app.SendChat(line);
            }
        } });

    while (running)
    {
        app.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (inputThread.joinable())
    {
        inputThread.join();
    }

    return 0;
}