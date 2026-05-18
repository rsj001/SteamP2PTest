#include "app.h"
#include "log.h"

// public lobby action interface
void SteamP2PApp::CreateLobby()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_lobby.IsValid())
    {
        Log() << "[Lobby] Already in a lobby, leaving first...\n";
        LeaveLobbyLocked();
    }

    Log() << "[Lobby] Creating lobby...\n";

    SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, 4);
    m_callLobbyCreated.Set(call, this, &SteamP2PApp::OnLobbyCreated);
}

void SteamP2PApp::JoinLobby(uint64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    CSteamID lobby(id);
    if (!lobby.IsValid())
    {
        Log() << "[Lobby] Invalid lobby id\n";
        return;
    }

    if (m_lobby.IsValid() && m_lobby != lobby)
        LeaveLobbyLocked();

    Log() << "[Lobby] Joining " << id << "\n";
    SteamMatchmaking()->JoinLobby(lobby);
}

void SteamP2PApp::OpenInviteOverlay()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_lobby.IsValid())
    {
        Log() << "[Error] Create a lobby with `create` first.\n";
        return;
    }

    if (SteamUtils()->IsOverlayEnabled()) {
        SteamFriends()->ActivateGameOverlayInviteDialog(m_lobby);
    } else
        Log() << "[UI] Overlay is disabled. Invite your friends manually in Steam.\n";
}

void SteamP2PApp::LeaveLobby()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LeaveLobbyLocked();
}

// (private?) lobby helpers
void SteamP2PApp::LeaveLobbyLocked()
{
    CloseAllConnectionsLocked("leave-lobby");

    if (m_lobby.IsValid())
    {
        SteamMatchmaking()->LeaveLobby(m_lobby);
        m_lobby.Clear();
    }

    m_lobbyOwnerSteamId64 = 0;
    m_isHost = false;

    Log() << "[Lobby] Left\n";
}

void SteamP2PApp::SyncLobbyNetworkingLocked()
{
    if (!m_lobby.IsValid())
        return;

    CSteamID self = SteamUser()->GetSteamID();
    CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
    uint64_t owner64 = owner.ConvertToUint64();

    bool shouldHost = (owner == self);
    bool roleChanged = (shouldHost != m_isHost);
    bool ownerChanged = (owner64 != m_lobbyOwnerSteamId64);

    m_lobbyOwnerSteamId64 = owner64;
    m_isHost = shouldHost;

    if (roleChanged || ownerChanged)
        CloseAllConnectionsLocked(roleChanged ? "role-changed" : "owner-changed");

    EnsureListenSocketLocked();
    EnsurePollGroupLocked();

    if (m_isHost)
        Log() << "[Net] I am host\n";
    else
    {
        Log() << "[Net] I am client\n";
        TryConnectToLobbyOwnerLocked();
    }
}

// lobby create result callback

void SteamP2PApp::OnLobbyCreated(LobbyCreated_t *result, bool bIOFailure)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (bIOFailure || result->m_eResult != k_EResultOK)
        Log() << "[Lobby] Create failed\n";
    else
        Log() << "[Lobby] Created successfully\n";
}

// steam lobby update callbacks
void SteamP2PApp::OnLobbyEnter(LobbyEnter_t *param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (param->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
    {
        Log() << "[Lobby] Enter failed\n";
        return;
    }

    m_lobby = CSteamID(param->m_ulSteamIDLobby);
    Log() << "[Lobby] Entered lobby " << m_lobby.ConvertToUint64() << "\n";
    
    SyncLobbyNetworkingLocked();
}

void SteamP2PApp::OnLobbyJoinRequested(GameLobbyJoinRequested_t *param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    Log() << "[Lobby] Invite accepted\n";

    if (m_lobby.IsValid() && m_lobby != param->m_steamIDLobby)
        LeaveLobbyLocked();

    SteamMatchmaking()->JoinLobby(param->m_steamIDLobby);
}

void SteamP2PApp::OnLobbyDataUpdate(LobbyDataUpdate_t *param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_lobby.IsValid() || param->m_ulSteamIDLobby != m_lobby.ConvertToUint64())
        return;

    Log() << "[Lobby] Metadata updated\n";
    SyncLobbyNetworkingLocked();
    PrintInfo();
}

void SteamP2PApp::OnLobbyChatUpdate(LobbyChatUpdate_t *param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_lobby.IsValid() || param->m_ulSteamIDLobby != m_lobby.ConvertToUint64())
        return;

    Log() << "[Lobby] Member updated\n";
    PrintInfo();
}
