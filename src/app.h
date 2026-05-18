#pragma once

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
#include <unordered_set>
#include <vector>

class SteamP2PApp
{
public:
    SteamP2PApp();
    ~SteamP2PApp();

    bool Init();
    void Tick();
    void Shutdown();

    void CreateLobby();
    void JoinLobby(uint64_t id);
    void LeaveLobby();
    void OpenInviteOverlay();
    void SendChat(const std::string &text);
    void StartStressTest(bool reliable, int count, int bytes, int delayMs);

    void PrintInfo();
    void PrintConnectionInfo(HSteamNetConnection conn);

    std::string GetSteamName(uint64_t steamId64) const;

private:
    static constexpr int kVirtualPort = 0;
    static constexpr int kMaxReceiveMessages = 32;
    static constexpr std::chrono::seconds kPendingTimeout{20};

    // Stress test
    static constexpr uint32_t kStressMagic = 0x53545253u; // 'STRS'
    static constexpr int kStressRxTimeoutMs = 3000;

    struct PendingPeer
    {
        uint64_t targetSteamId64 = 0;
        ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_None;
        std::chrono::steady_clock::time_point startedAt{};
    };

    struct StressRxState
    {
        uint32_t total = 0;
        std::unordered_set<uint32_t> seqs;
        std::chrono::steady_clock::time_point lastPacket{};
        bool finalized = false;
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

    // Stress test state
    std::atomic<bool> m_stressRunning{false};
    std::thread m_stressThread;
    std::unordered_map<uint64_t, StressRxState> m_stressRx;

    CCallResult<SteamP2PApp, LobbyCreated_t> m_callLobbyCreated;
    CCallback<SteamP2PApp, LobbyEnter_t> m_cbLobbyEnter;
    CCallback<SteamP2PApp, GameLobbyJoinRequested_t> m_cbLobbyJoinRequested;
    CCallback<SteamP2PApp, LobbyDataUpdate_t> m_cbLobbyDataUpdate;
    CCallback<SteamP2PApp, LobbyChatUpdate_t> m_cbLobbyChatUpdate;

    // --- Net ---
    void EnsureListenSocketLocked();
    void EnsurePollGroupLocked();
    void CloseAllConnectionsLocked(const char *reason);
    bool HasAnyConnectionToSteamIdLocked(uint64_t steamId64) const;
    bool IsConnectionConnectedLocked(HSteamNetConnection conn) const;
    bool SendMessageToConnectionLocked(HSteamNetConnection conn, const std::string &text);

    void TryConnectToLobbyOwnerLocked();
    void PollIncomingMessages();
    void DumpConnectionDetailsLocked(HSteamNetConnection conn, const char *prefix);
    void PumpPendingTimeoutsLocked();

    // --- Net for Stress Test ---
    bool SendRawToConnectionLocked(HSteamNetConnection conn, const void *data,
                                   uint32_t size, bool reliable);

    // --- Stress test ---
    void HandleStressPacket(HSteamNetConnection conn, const void *data, uint32_t size);
    void FinalizeStressRx(uint64_t peerSteamId, StressRxState &rx);
    void PollStressRxTimeoutsLocked();

    // --- Net Steam callbacks ---
    static void SteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);
    void HandleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);

    // --- Lobby ---
    void LeaveLobbyLocked();
    void SyncLobbyNetworkingLocked();
    // --- Lobby Steam LobbyCreate result callbacks ---
    void OnLobbyCreated(LobbyCreated_t *result, bool bIOFailure);
    
    // --- Lobby Steam callbacks ---
    void OnLobbyEnter(LobbyEnter_t *param);
    void OnLobbyJoinRequested(GameLobbyJoinRequested_t *param);
    void OnLobbyDataUpdate(LobbyDataUpdate_t *param);
    void OnLobbyChatUpdate(LobbyChatUpdate_t *param);
};
