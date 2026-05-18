#include "app.h"
#include "log.h"

#include <cstring>

void SteamP2PApp::EnsureListenSocketLocked()
{
    if (m_listenSocket != k_HSteamListenSocket_Invalid)
        return;

    m_listenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(kVirtualPort, 0, nullptr);

    Log() << (m_listenSocket == k_HSteamListenSocket_Invalid
                  ? "[Net] CreateListenSocketP2P failed\n"
                  : "[Net] Listen socket ready\n");
}

void SteamP2PApp::EnsurePollGroupLocked()
{
    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        return;

    m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();

    Log() << (m_pollGroup == k_HSteamNetPollGroup_Invalid
                  ? "[Net] CreatePollGroup failed\n"
                  : "[Net] Poll group ready\n");
}

// connection helpers

void SteamP2PApp::CloseAllConnectionsLocked(const char *reason)
{
    std::vector<HSteamNetConnection> conns;
    conns.reserve(m_pendingByConn.size() + m_connectedBySteamId.size());

    for (const auto &[conn, peer] : m_pendingByConn)
        conns.push_back(conn);

    for (const auto &[steamId, conn] : m_connectedBySteamId)
        conns.push_back(conn);

    m_pendingByConn.clear();
    m_connectedBySteamId.clear();
    m_connToSteamId.clear();

    for (HSteamNetConnection conn : conns)
    {
        if (conn != k_HSteamNetConnection_Invalid)
            SteamNetworkingSockets()->CloseConnection(conn, 0, reason, false);
    }
}
bool SteamP2PApp::HasAnyConnectionToSteamIdLocked(uint64_t steamId64) const
{
    if (steamId64 == 0)
        return false;

    if (m_connectedBySteamId.count(steamId64))
        return true;

    for (const auto &[conn, peer] : m_pendingByConn)
    {
        if (peer.targetSteamId64 == steamId64)
            return true;
    }

    return false;
}

bool SteamP2PApp::IsConnectionConnectedLocked(HSteamNetConnection conn) const
{
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
        return false;
    return info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

bool SteamP2PApp::SendMessageToConnectionLocked(HSteamNetConnection conn, const std::string &text)
{
    if (conn == k_HSteamNetConnection_Invalid || !IsConnectionConnectedLocked(conn))
        return false;

    EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        conn,
        text.data(),
        static_cast<uint32>(text.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr);

    if (r != k_EResultOK)
    {
        Log() << "[Error] Send failed: " << static_cast<int>(r) << "\n";
        return false;
    }

    return true;
}

void SteamP2PApp::TryConnectToLobbyOwnerLocked()
{
    if (!m_lobby.IsValid() || m_isHost)
        return;

    if (m_lobbyOwnerSteamId64 == 0)
        m_lobbyOwnerSteamId64 = SteamMatchmaking()->GetLobbyOwner(m_lobby).ConvertToUint64();

    if (m_lobbyOwnerSteamId64 == 0)
    {
        Log() << "[Net] No lobby owner yet\n";
        return;
    }

    if (HasAnyConnectionToSteamIdLocked(m_lobbyOwnerSteamId64))
        return;

    SteamNetworkingIdentity identity{};
    identity.SetSteamID(CSteamID(m_lobbyOwnerSteamId64));

    HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(identity, kVirtualPort, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid)
    {
        Log() << "[Net] ConnectP2P failed\n";
        return;
    }

    m_pendingByConn[conn] = {m_lobbyOwnerSteamId64,
                             k_ESteamNetworkingConnectionState_Connecting,
                             std::chrono::steady_clock::now()};

    Log() << "[Net] Connecting to host " << m_lobbyOwnerSteamId64 << "\n";
}

void SteamP2PApp::PollIncomingMessages()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_pollGroup == k_HSteamNetPollGroup_Invalid)
        return;

    SteamNetworkingMessage_t *msgs[kMaxReceiveMessages]{};

    while (true)
    {
        int count = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(
            m_pollGroup, msgs, kMaxReceiveMessages);

        if (count <= 0)
            break;

        for (int i = 0; i < count; ++i)
        {
            SteamNetworkingMessage_t *msg = msgs[i];

            // Route stress packets by magic header
            if (msg->m_cbSize >= 4)
            {
                uint32_t magic = 0;
                std::memcpy(&magic, msg->m_pData, 4);
                if (magic == kStressMagic)
                {
                    HandleStressPacket(msg->m_conn, msg->m_pData, static_cast<uint32_t>(msg->m_cbSize));
                    msg->Release();
                    continue;
                }
            }

            uint64_t peerSteamId = 0;
            auto it = m_connToSteamId.find(msg->m_conn);
            if (it != m_connToSteamId.end())
                peerSteamId = it->second;

            std::string text(static_cast<char *>(msg->m_pData), msg->m_cbSize);
            std::string sender = peerSteamId ? "[" + GetSteamName(peerSteamId) + "]" : "";

            Log() << "\n[MsgPayload]" << sender << " " << text << "\n";
            std::cout << "> " << std::flush;

            msg->Release();
        }

        if (count < kMaxReceiveMessages)
            break;
    }
}

// pending timeout
void SteamP2PApp::DumpConnectionDetailsLocked(HSteamNetConnection conn, const char *prefix)
{
    char buf[4096]{};
    int r = SteamNetworkingSockets()->GetDetailedConnectionStatus(conn, buf, sizeof(buf));
    Log() << prefix << " detailed_status_rc=" << r << "\n";
    if (r >= 0)
        Log() << buf << "\n";
}

void SteamP2PApp::PumpPendingTimeoutsLocked()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<HSteamNetConnection> expired;

    for (const auto &[conn, peer] : m_pendingByConn)
    {
        if (now - peer.startedAt > kPendingTimeout)
            expired.push_back(conn);
    }

    for (HSteamNetConnection conn : expired)
    {
        auto it = m_pendingByConn.find(conn);
        if (it == m_pendingByConn.end())
            continue;

        Log() << "[Net] Pending timeout conn=" << conn
              << " target=" << it->second.targetSteamId64 << "\n";

        DumpConnectionDetailsLocked(conn, "[NetTimeout]");
        SteamNetworkingSockets()->CloseConnection(conn, 0, "pending-timeout", false);
        m_pendingByConn.erase(it);
    }

    if (!m_isHost && m_lobby.IsValid() && !HasAnyConnectionToSteamIdLocked(m_lobbyOwnerSteamId64))
        TryConnectToLobbyOwnerLocked();
}

// callbacks for SetGlobalCallback_SteamNetConnectionStatusChanged

void SteamP2PApp::SteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
    if (s_instance)
        s_instance->HandleConnectionStatusChanged(pInfo);
    else
        Log() << "[Error] SteamP2PApp instance not initialized\n";
}

void SteamP2PApp::HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto state = pInfo->m_info.m_eState;

    char remote[256]{};
    pInfo->m_info.m_identityRemote.ToString(remote, sizeof(remote));

    Log() << "[ConnectionStatusChanged] state=" << static_cast<int>(state)
          << " remote=" << remote
          << " listen=" << pInfo->m_info.m_hListenSocket
          << " conn=" << pInfo->m_hConn
          << " debug='" << (pInfo->m_info.m_szEndDebug ? pInfo->m_info.m_szEndDebug : "") << "'\n";

    switch (state)
    {
    case k_ESteamNetworkingConnectionState_None:
        break;

    case k_ESteamNetworkingConnectionState_Connecting:
    {
        uint64_t remoteSteamId = pInfo->m_info.m_identityRemote.GetSteamID64();

        m_pendingByConn[pInfo->m_hConn] = {remoteSteamId, state, std::chrono::steady_clock::now()};

        if (pInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
        {
            Log() << "[Net] Incoming connection request\n";

            EResult r = SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);
            if (r != k_EResultOK)
            {
                Log() << "[Net] Accept failed: " << static_cast<int>(r) << "\n";
                DumpConnectionDetailsLocked(pInfo->m_hConn, "[NetAcceptFailed]");
                SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, "accept-failed", false);
                m_pendingByConn.erase(pInfo->m_hConn);
                return;
            }

            Log() << "[Net] AcceptConnection success\n";
        }
        else
        {
            Log() << "[Net] Outbound connection progressing\n";
        }
        break;
    }

    case k_ESteamNetworkingConnectionState_FindingRoute:
    {
        auto it = m_pendingByConn.find(pInfo->m_hConn);
        if (it != m_pendingByConn.end())
            it->second.state = state;

        Log() << "[Net] Finding relay/network route...\n";
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected:
    {
        uint64_t remoteSteamId = pInfo->m_info.m_identityRemote.GetSteamID64();

        auto pit = m_pendingByConn.find(pInfo->m_hConn);
        if (remoteSteamId == 0 && pit != m_pendingByConn.end())
            remoteSteamId = pit->second.targetSteamId64;

        if (remoteSteamId != 0)
        {
            auto existing = m_connectedBySteamId.find(remoteSteamId);
            if (existing != m_connectedBySteamId.end() && existing->second != pInfo->m_hConn)
                SteamNetworkingSockets()->CloseConnection(existing->second, 0, "duplicate-connection", false);

            m_connectedBySteamId[remoteSteamId] = pInfo->m_hConn;
            m_connToSteamId[pInfo->m_hConn] = remoteSteamId;
        }

        m_pendingByConn.erase(pInfo->m_hConn);

        if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        {
            bool ok = SteamNetworkingSockets()->SetConnectionPollGroup(pInfo->m_hConn, m_pollGroup);
            Log() << "[Net] SetConnectionPollGroup: " << (ok ? "OK" : "FAILED") << "\n";
        }

        Log() << "[Net] Connected"
              << (remoteSteamId ? " peer=" + GetSteamName(remoteSteamId) : "") << "\n";

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
                peerSteamId = itPend->second.targetSteamId64;
        }

        if (peerSteamId != 0)
        {
            auto itConnMap = m_connectedBySteamId.find(peerSteamId);
            if (itConnMap != m_connectedBySteamId.end() && itConnMap->second == pInfo->m_hConn)
                m_connectedBySteamId.erase(itConnMap);
        }

        m_connToSteamId.erase(pInfo->m_hConn);
        m_pendingByConn.erase(pInfo->m_hConn);

        Log() << "[Net] Connection closed\n";
        Log() << "[Net] Reason='"
              << (pInfo->m_info.m_szEndDebug ? pInfo->m_info.m_szEndDebug : "") << "'\n";

        DumpConnectionDetailsLocked(pInfo->m_hConn, "[NetClosed]");
        SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);

        if (!m_shutdown && m_lobby.IsValid() && !m_isHost &&
            peerSteamId != 0 && peerSteamId == m_lobbyOwnerSteamId64)
        {
            Log() << "[Net] Lost host connection, will reconnect\n";
            TryConnectToLobbyOwnerLocked();
        }
        break;
    }

    default:
        Log() << "[Net] Unhandled state=" << static_cast<int>(state) << "\n";
        break;
    }
}
