#include "app.h"
#include "log.h"

#include <cstring>
#include <random>


// Stress packet RAW encode
// this is header = 12 bytes
// rest is zero padding
//   [0..3]   magic   uint32_t  kStressMagic
//   [4..7]   seq     uint32_t  0 .. N-1
//   [8..11]  total   uint32_t  N
//   [12..]   padding (zeroed)

bool SteamP2PApp::SendRawToConnectionLocked(HSteamNetConnection conn, const void *data, uint32_t size, bool reliable)
{
    if (conn == k_HSteamNetConnection_Invalid || !IsConnectionConnectedLocked(conn))
        return false;

    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_Unreliable;

    EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        conn, data, size, flags, nullptr);

    return r == k_EResultOK;
}

void SteamP2PApp::StartStressTest(bool reliable, int count, int bytes, int delayMs)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_stressRunning.load())
    {
        Log() << "[Stress] Already running\n";
        return;
    }

    if (m_connectedBySteamId.empty())
    {
        Log() << "[Stress] No connected peers\n";
        return;
    }

    if (bytes < 13)
        bytes = 13;

    // copy targets so no lock issue
    std::vector<std::pair<uint64_t, HSteamNetConnection>> targets(m_connectedBySteamId.begin(), m_connectedBySteamId.end());

    Log() << "[Stress] Starting: mode=" << (reliable ? "reliable" : "unreliable")
          << " count=" << count
          << " bytes=" << bytes
          << " delayMs=" << delayMs
          << " targets=" << static_cast<int>(targets.size()) << "\n";

    m_stressRunning = true;

    if (m_stressThread.joinable())
        m_stressThread.join();

    m_stressThread = std::thread(
        [this, reliable, count, bytes, delayMs, targets = std::move(targets)]()
        {
            std::vector<uint8_t> buf(static_cast<size_t>(bytes), 0u);
            const uint32_t total = static_cast<uint32_t>(count);
            int sent = 0;

            for (int i = 0; i < count; ++i)
            {
                if (m_shutdown)
                    break;

                const uint32_t magic = kStressMagic;
                const uint32_t seq = static_cast<uint32_t>(i);

                std::memcpy(buf.data() + 0, &magic, 4);
                std::memcpy(buf.data() + 4, &seq, 4);
                std::memcpy(buf.data() + 8, &total, 4);

                // maybe this can prevent compression
                static thread_local std::mt19937 generator(std::random_device{}());
                std::uniform_int_distribution<int> distribution(0, 255);
                std::generate(buf.begin() + 12, buf.end(), [&]() {
                    return static_cast<uint8_t>(distribution(generator));
                });


                {
                    std::lock_guard<std::recursive_mutex> lk(m_mutex);
                    for (const auto &[steamId, conn] : targets)
                    {
                        if (SendRawToConnectionLocked(conn, buf.data(), static_cast<uint32_t>(buf.size()), reliable))
                            ++sent;
                    }
                }

                if (delayMs > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            m_stressRunning = false;

            const int expected = count * static_cast<int>(targets.size());
            Log() << "[Stress] TX Result:"
                  << " sent=" << sent << "/" << expected
                  << " mode=" << (reliable ? "reliable" : "unreliable") << "\n";

            if (sent < expected)
            {
                Log() << "[Stress] Some packets were lost before being queued for transmission.\n";
                Log() << "[Stress] This is usually caused by an extremely aggressive send interval, where the application is generating packets faster than the networking layer or local socket buffer can process them.\n";
                Log() << "[Stress] It may also indicate severe local congestion, bandwidth saturation, or a connection that is already under heavy retransmission pressure.\n";
                Log() << "[Stress] Try increasing the packet interval, reducing packet size, or lowering the total send rate.\n";
            }

            std::ostringstream info_to_send;
            CSteamID self = SteamUser()->GetSteamID();

            info_to_send << "Hello, I am TX " << GetSteamName(self.ConvertToUint64()) << ".\n"
                         << "[Stress] TX Result (from " << GetSteamName(self.ConvertToUint64()) << "):"
                         << " sent=" << sent << "/" << expected
                         << " mode=" << (reliable ? "reliable" : "unreliable") << "\n";

            SendChat(info_to_send.str());
        });
}

void SteamP2PApp::HandleStressPacket(HSteamNetConnection conn, const void *data, uint32_t size)
{
    if (size < 12)
        return;

    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t seq, total;
    std::memcpy(&seq, p + 4, 4);
    std::memcpy(&total, p + 8, 4);

    if (total == 0)
        return;

    uint64_t peerSteamId = 0;
    auto it = m_connToSteamId.find(conn);
    if (it != m_connToSteamId.end())
        peerSteamId = it->second;

    auto &rx = m_stressRx[peerSteamId];
    if (rx.finalized)
        rx = StressRxState{};

    if (rx.total == 0)
    {
        rx.total = total;
        Log() << "[Stress] Receiving " << total
              << " packets from " << GetSteamName(peerSteamId) << "\n";
    }

    rx.seqs.insert(seq);
    rx.lastPacket = std::chrono::steady_clock::now();

    if (static_cast<uint32_t>(rx.seqs.size()) >= rx.total)
        FinalizeStressRx(peerSteamId, rx);
}

void SteamP2PApp::FinalizeStressRx(uint64_t peerSteamId, StressRxState &rx)
{
    if (rx.finalized)
        return;
    rx.finalized = true;

    const int received = static_cast<int>(rx.seqs.size());
    const int total = static_cast<int>(rx.total);
    const int lost = total - received;
    const float pct = total > 0 ? 100.f * static_cast<float>(lost) / total : 0.f;

    Log() << "[Stress] RX Result:"
          << " received=" << received << "/" << total
          << " lost=" << lost
          << " (" << pct << "%)\n";

    std::ostringstream info_to_send;
    CSteamID self = SteamUser()->GetSteamID();
    info_to_send << "Hello, I am RX " << GetSteamName(self.ConvertToUint64()) << ".\n"
                 << "[Stress] RX Result (from " << GetSteamName(self.ConvertToUint64()) << "):"
                 << " received=" << received << "/" << total
                 << " lost=" << lost
                 << " (" << pct << "%)\n";

    SendChat(info_to_send.str());
}

void SteamP2PApp::PollStressRxTimeoutsLocked()
{
    if (m_stressRx.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    for (auto &[steamId, rx] : m_stressRx)
    {
        if (rx.finalized || rx.total == 0)
            continue;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - rx.lastPacket).count();
        if (elapsedMs >= kStressRxTimeoutMs)
        {
            Log() << "[Stress] RX timeout after " << elapsedMs << "ms, finalizing\n";
            FinalizeStressRx(steamId, rx);
        }
    }
}
