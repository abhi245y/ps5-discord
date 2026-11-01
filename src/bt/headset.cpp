// SPDX-License-Identifier: AGPL-3.0-or-later
// The Bluetooth headset (headset.h): one thread owns the USB adapter and BlueV's stack
// (third_party/bluev). A session: connect (or adopt a headset that dialled in), A2DP with
// the game's sound plus the call, and on AirPods their own mic stream during calls.
#include "headset.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "aacp_mic.hpp"
#include "hfp_ag.hpp"
#include "core/audio/a2dp_session.hpp"
#include "core/audio/a2dp_streamer.hpp"
#include "core/audio/avcap_capture.hpp"
#include "core/audio/avrcp.hpp"
#include "core/bluetooth/address.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/usb_hci_transport.hpp"
#include "core/hfp/iso.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/log.hpp"
#include "platform/posix/file_bond_store.hpp"
#include "platform/posix/file_firmware_store.hpp"
#include "platform/ps5/avcap_library.hpp"
#include "platform/ps5/ps5_usb_directory.hpp"
#include "platform/ps5/ugen_device.hpp"

extern "C" {
#include "broadcast.h"
#include "log.h"
#include "prefs.h"
#include "voice.h"
#include "voice_audio.h"
}

using namespace bluev;

namespace
{

constexpr int kScanMs = 30000;
constexpr int kMicSilentLimit = 30; // seconds without a mic frame before the mic counts as failed

struct CoreLog final : ILogSink
{
    void line(const char* text) override
    {
        LOG("bt: %s", text);
    }
    void notify(const char* text) override
    {
        LOG("bt: %s", text);
    }
};

// The game's sound with the call mixed in, while the headset is the call's device.
class MixSource final : public IAudioSource
{
  public:
    explicit MixSource(IAudioSource& game) : m_game(game)
    {
    }
    bool call = false;

    int read(float* buffer, std::uint32_t maxBytes) override
    {
        const int n = m_game.read(buffer, maxBytes);
        if (n <= 0 || !call)
        {
            return n;
        }
        const std::size_t frames = static_cast<std::size_t>(n) / 8;
        m_pcm.resize(frames * 2);
        const std::size_t got = vau_ext_playback(m_pcm.data(), frames);
        for (std::size_t i = 0; i < got * 2; i++)
        {
            const float v = buffer[i] + m_pcm[i] / 32768.0f;
            buffer[i] = v > 1 ? 1 : v < -1 ? -1 : v;
        }
        return n;
    }
    bool restart() override
    {
        return m_game.restart();
    }
    long overruns() override
    {
        return m_game.overruns();
    }

  private:
    IAudioSource& m_game;
    std::vector<std::int16_t> m_pcm;
};

enum class Cmd
{
    PowerOn,
    PowerOff,
    Scan,
    Connect,
    Disconnect,
    Forget
};

struct Request
{
    Cmd cmd;
    BdAddr addr{};
};

// What /api/bt/state shows; written by the radio thread under g_mu.
struct Snapshot
{
    bool on = false;
    std::string state = "off", message, adapter;
    std::string name, addr, route, mic = "off";
    int pps = 0, queueMs = 0, delayReport = -1; // tenths of a ms, the headset's own
    long micFrames = 0, micErrors = 0;
    double decodeUs = 0;
    std::vector<BtDevice> devices;
    std::vector<BondEntry> bonds;
};

std::mutex g_mu;
std::deque<Request> g_queue;
Snapshot g_snap;
bool g_started = false;
std::string g_onFile; // exists while Bluetooth is on: on again at the next start

class Radio
{
  public:
    Radio(std::string bondDir, std::string firmwareDir)
        : m_bonds(std::move(bondDir)), m_firmware(std::move(firmwareDir)),
          m_transport(m_device, m_clock, kConsoleBluetoothNode), m_bt(m_transport, m_bonds, m_usb, m_firmware, m_clock),
          m_a2dp(m_bt, m_clock), m_avrcp(m_bt), m_capture(m_avcap), m_mix(m_capture),
          m_streamer(m_bt, m_a2dp, m_mix, m_clock,
                     [](int, int queueMs, int pps, long) {
                         std::lock_guard<std::mutex> lock(g_mu);
                         g_snap.pps = pps;
                         g_snap.queueMs = queueMs;
                     }),
          m_mic(m_bt, [this](const std::int16_t* pcm, int n) {
              if (m_ownExt)
              {
                  vau_ext_mic(pcm, static_cast<std::size_t>(n));
              }
          })
    {
    }

    void run()
    {
        m_avcapLoaded = m_avcap.load();
        for (;;)
        {
            Request r;
            if (take(r))
            {
                handle(r);
                continue;
            }
            if (!m_on)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            m_bt.poll(200);
            if (m_bt.linkUp()) // a paired headset dialled in
            {
                session(nullptr);
            }
        }
    }

  private:
    static bool take(Request& r)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_queue.empty())
        {
            return false;
        }
        r = g_queue.front();
        g_queue.pop_front();
        return true;
    }

    void publish(const char* state, const char* message)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap.on = m_on;
        g_snap.state = state;
        g_snap.message = message;
        g_snap.adapter = m_adapterName;
        if (m_on)
        {
            g_snap.devices = m_bt.devices();
            g_snap.bonds = m_bt.bonds();
        }
    }

    void handle(const Request& r)
    {
        switch (r.cmd)
        {
        case Cmd::PowerOn:
            powerOn();
            break;
        case Cmd::PowerOff:
            powerOff();
            break;
        case Cmd::Scan:
            if (!m_on)
            {
                break;
            }
            publish("scanning", "looking for headsets");
            m_bt.scan(kScanMs);
            publish("idle", "scan finished");
            break;
        case Cmd::Connect:
            if (m_on)
            {
                session(&r.addr);
            }
            break;
        case Cmd::Disconnect:
            break; // only means something during a session (tick)
        case Cmd::Forget:
            if (m_on)
            {
                m_bt.forget(r.addr);
                publish("idle", "forgotten");
            }
            break;
        }
    }

    // The first USB Bluetooth adapter (class e0/01/01); the console's own chip is never used.
    void powerOn()
    {
        if (m_on)
        {
            return;
        }
        std::string node;
        for (const UsbDevice& d : m_usb.adapters())
        {
            if (d.deviceClass == 0xe0 && d.subClass == 0x01 && d.protocol == 0x01)
            {
                node = d.node;
                m_adapterName = d.product;
                LOG("bt: adapter %04x:%04x %s", d.vid, d.pid, d.product.c_str());
                break;
            }
        }
        if (node.empty())
        {
            publish("off", "No Bluetooth adapter on USB.");
            return;
        }
        m_bt.useAdapter(node);
        if (!m_bt.start())
        {
            publish("off", m_bt.startError().empty() ? "The adapter did not start." : m_bt.startError().c_str());
            return;
        }
        listen();
        m_on = true;
        publish("idle", "ready");
        // AirPods only call back the host they last used, and not after it dropped them: call
        // the paired headset. ponytail: the first pairing; the last used one when several matter.
        const std::vector<BondEntry> bonds = m_bt.bonds();
        if (!bonds.empty())
        {
            session(&bonds.front().peer, 1);
        }
    }

    void powerOff()
    {
        if (!m_on)
        {
            return;
        }
        m_bt.setTick(nullptr);
        m_bt.stop();
        m_on = false;
        publish("off", "adapter off");
    }

    // A headset just after a session may refuse the first page (0x0d, limited resources).
    // A headset that answers and then refuses (AirPods just after a session: 0x0d, 0x14) is
    // worth up to three tries even when one was asked for; one in its case never answers
    // (16 s of paging). ponytail: "answered" = failed in under 12 s.
    bool connect(const BdAddr& addr, int attempts)
    {
        for (int attempt = 0; attempt < 3; attempt++)
        {
            const auto began = std::chrono::steady_clock::now();
            if (m_bt.connectTo(addr))
            {
                return true;
            }
            const bool answered = std::chrono::steady_clock::now() - began < std::chrono::seconds(12);
            if (attempt + 1 >= attempts && !answered)
            {
                return false;
            }
            m_bt.wait([] { return false; }, 2000, false);
        }
        return false;
    }

    // Ready for a headset that dials in: A2DP, AVRCP and hands-free.
    void listen()
    {
        m_a2dp.reset();
        m_a2dp.listen();
        m_avrcp.reset();
        m_avrcp.listen();
        m_hfp.reset();
        m_hfp.listen();
    }

    void route(const char* route, const char* mic)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap.route = route;
        g_snap.mic = mic;
    }

    // Music mode (A2DP: the game and the call), and hands-free for as long as a call needs
    // it (tick decides), then music again.
    // attempts: 1 when the adapter comes on and calls the paired headset (it may be in its case).
    void session(const BdAddr* addr, int attempts = 3)
    {
        publish("connecting", addr ? "connecting" : "a paired headset is connecting");
        bool ok = addr ? connect(*addr, attempts) : m_bt.adoptIncoming();
        ok = ok && m_a2dp.start(addr != nullptr);
        if (ok)
        {
            m_avrcp.start();
            const bool capturing = m_avcapLoaded && m_capture.start();
            m_airpods = m_mic.open();
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_snap.name = m_bt.peerName();
                g_snap.addr = m_bt.peerAddress();
            }
            route(m_airpods ? "airpods" : "a2dp", "off");
            m_micFailed = m_hfpFailed = m_wantSco = m_stopSession = m_resetA2dp = false;
            m_resetIn = 0;
            publish("connected", "connected");
            m_bt.setTick([this] {
                m_avrcp.service();
                tick();
            });
            for (;;)
            {
                m_streamer.run(capturing);
                if (m_stopSession || !m_bt.linkUp() || (!m_wantSco && !m_resetA2dp))
                {
                    break;
                }
                if (m_wantSco)
                {
                    handsFree();
                    if (m_stopSession || !m_bt.linkUp())
                    {
                        break;
                    }
                    route(m_airpods ? "airpods" : "a2dp", m_micFailed ? "failed" : "off");
                }
                else
                {
                    LOG("bt: the music stream set up again after the AirPods mic started or stopped");
                    m_a2dp.stop();
                }
                m_resetA2dp = false;
                m_a2dp.reset();
                m_a2dp.listen();
                if (!m_a2dp.start(true))
                {
                    break;
                }
            }
            m_bt.setTick(nullptr);
        }
        m_mic.close();
        releaseExt();
        m_avrcp.stop();
        m_a2dp.stop();
        m_capture.close();
        m_bt.disconnect();
        listen();
        if (m_offAfter)
        {
            m_offAfter = false;
            powerOff();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_snap.name.clear();
            g_snap.addr.clear();
            g_snap.route.clear();
            g_snap.mic = "off";
        }
        publish("idle", ok              ? "disconnected"
                        : attempts == 1 ? "the paired headset did not answer"
                                        : "Could not connect. Is the headset on and close by?");
    }

    // A call on the hands-free link: the headset's mic to the call, the game and the call
    // to the headset, until the call ends (or the session does).
    void handsFree()
    {
        m_mic.stop();
        m_a2dp.stop();
        int codec = 0;
        if (m_hfp.slcUp() || m_hfp.connect())
        {
            codec = m_hfp.openAudio(true);
        }
        if (codec == 0 || !g_iso.start || !g_iso.start())
        {
            LOG("bt: hands-free did not start: the call stays on the TV");
            m_hfp.closeAudio();
            m_hfpFailed = true;
            m_wantSco = false;
            return;
        }
        route("hands-free", codec == 2 ? "mSBC" : "CVSD");
        HfpVoice voice(codec == 2, m_bt.scoHandle());
        std::vector<float> rec(4096), mono;
        std::vector<std::int16_t> call;
        std::uint8_t in[4096];
        const auto mic = [this](const std::int16_t* pcm, int n) {
            if (m_ownExt)
            {
                vau_ext_mic(pcm, static_cast<std::size_t>(n));
            }
        };
        while (m_bt.linkUp() && m_wantSco && !m_stopSession)
        {
            m_bt.poll(2);
            const int got = m_capture.read(rec.data(), static_cast<std::uint32_t>(rec.size() * sizeof(float)));
            const std::size_t frames = got > 0 ? static_cast<std::size_t>(got) / 8 : 0;
            if (frames > 0)
            {
                call.assign(frames * 2, 0);
                if (m_ownExt)
                {
                    vau_ext_playback(call.data(), frames);
                }
                mono.resize(frames);
                for (std::size_t i = 0; i < frames; i++)
                {
                    mono[i] = 0.5f * (rec[2 * i] + rec[2 * i + 1] + (call[2 * i] + call[2 * i + 1]) / 32768.0f);
                }
                voice.play(mono.data(), static_cast<int>(frames));
            }
            voice.pump();
            const int n = g_iso.read(in, sizeof in);
            if (n > 0)
            {
                voice.receive(in, n, mic);
            }
        }
        LOG("bt: hands-free ended: %ld frames from the headset", voice.frames());
        g_iso.stop();
        m_hfp.closeAudio();
    }

    void releaseExt()
    {
        m_mix.call = false;
        if (m_ownExt && !bc_holds_controller())
        {
            vau_ext_set(0);
        }
        m_ownExt = false;
    }

    // Once a second while connected: requests, who owns the call's audio, the mic.
    void tick()
    {
        Request r;
        while (take(r))
        {
            if (r.cmd == Cmd::Disconnect || r.cmd == Cmd::PowerOff)
            {
                m_offAfter = r.cmd == Cmd::PowerOff;
                m_stopSession = true;
                m_a2dp.requestStop();
            }
        }
        // Go Live with the USB controller has the call's audio; otherwise the headset.
        const bool want = !bc_holds_controller();
        if (want && !vau_ext_get())
        {
            vau_ext_set(1);
        }
        m_ownExt = want;
        m_mix.call = want;

        const bool inCall = want && voice_wanted();
        // Hands-free while in a call when there is no AirPods mic, it failed, or the user
        // chose hands-free (Settings: the AirPods mic does not work for them).
        const bool sco = inCall && (!m_airpods || m_micFailed || prefs_get("bt_handsfree") == 1) && !m_hfpFailed;
        if (sco && !m_wantSco)
        {
            m_wantSco = true;
            m_a2dp.requestStop(); // the music stream ends; session() opens hands-free
        }
        m_wantSco = sco;
        if (m_wantSco)
        {
            m_resetIn = 0; // hands-free sets up the music stream again anyway
            return;
        }
        // AirPods can leave one bud silent after their mic starts or stops until the music
        // stream is set up again (LibrePods resets A2DP 0.8 s after a start, and after a stop).
        if (m_resetIn > 0 && --m_resetIn == 0)
        {
            m_resetA2dp = true;
            m_a2dp.requestStop();
        }
        if (m_airpods && inCall && !m_mic.running() && !m_micFailed)
        {
            m_mic.start();
            m_resetIn = 1;
        }
        else if (m_mic.running() && !inCall)
        {
            m_mic.stop();
            m_resetIn = 1;
        }
        if (m_mic.silentSeconds() >= kMicSilentLimit)
        {
            LOG("bt: no AirPods mic audio for %d s: the mic counts as failed, hands-free takes over", kMicSilentLimit);
            m_micFailed = true;
            m_mic.stop();
        }
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap.mic = m_micFailed ? "failed" : m_mic.running() ? "on" : "off";
        g_snap.micFrames = m_mic.frames();
        g_snap.micErrors = m_mic.errors();
        g_snap.decodeUs = m_mic.decodeUs();
        g_snap.delayReport = m_a2dp.delayReport();
    }

    UgenDevice m_device;
    SystemClock m_clock;
    Ps5UsbDirectory m_usb;
    FileBondStore m_bonds;
    FileFirmwareStore m_firmware;
    UsbHciTransport m_transport;
    BtStack m_bt;
    A2dpSession m_a2dp;
    Avrcp m_avrcp;
    AvcapLibrary m_avcap;
    AvcapCapture m_capture;
    MixSource m_mix;
    A2dpStreamer m_streamer;
    AacpMic m_mic;
    HfpAg m_hfp{m_bt};
    bool m_on = false, m_avcapLoaded = false, m_airpods = false, m_micFailed = false;
    bool m_hfpFailed = false, m_wantSco = false, m_stopSession = false;
    bool m_ownExt = false, m_offAfter = false, m_resetA2dp = false;
    int m_resetIn = 0; // ticks until the music stream is set up again (m_resetA2dp)
    std::string m_adapterName;
};

int queue(Cmd cmd, const char* addr = nullptr)
{
    Request r{cmd};
    if (addr != nullptr && !parseAddress(addr, r.addr))
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started)
    {
        return -1;
    }
    g_queue.push_back(r);
    return 0;
}

} // namespace

extern "C" void hs_init(const char* data_dir, const char* firmware_dir)
{
    static CoreLog log;
    setLogSink(&log);
    std::string bonds = std::string(data_dir) + "/bt";
    mkdir(bonds.c_str(), 0700);
    // The radio lives as long as the core; its thread is never joined. The console's
    // default thread stack is too small for the stack and the decoder: 2 MB, as the
    // other threads set theirs.
    auto* radio = new Radio(bonds, firmware_dir);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int r = pthread_create(
        &thread, &attr,
        [](void* p) -> void* {
            static_cast<Radio*>(p)->run();
            return nullptr;
        },
        radio);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        LOG("bt: no radio thread (%d)", r);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_started = true;
    }
    g_onFile = bonds + "/on";
    struct stat st;
    if (stat(g_onFile.c_str(), &st) == 0)
    {
        queue(Cmd::PowerOn);
    }
}

extern "C" int hs_power(int on)
{
    const int r = queue(on ? Cmd::PowerOn : Cmd::PowerOff);
    if (r == 0 && on)
    {
        if (FILE* f = std::fopen(g_onFile.c_str(), "w"))
        {
            std::fclose(f);
        }
    }
    else if (r == 0)
    {
        std::remove(g_onFile.c_str());
    }
    return r;
}

extern "C" void hs_shutdown(void)
{
    if (queue(Cmd::PowerOff) != 0)
    {
        return;
    }
    for (int i = 0; i < 30; i++) // the session ends at its next tick (a second), then the adapter
    {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_snap.on)
            {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG("bt: the adapter did not close in 3 s");
}

extern "C" int hs_scan(void)
{
    return queue(Cmd::Scan);
}

extern "C" int hs_connect(const char* addr)
{
    return addr ? queue(Cmd::Connect, addr) : -1;
}

extern "C" int hs_disconnect(void)
{
    return queue(Cmd::Disconnect);
}

extern "C" int hs_forget(const char* addr)
{
    return addr ? queue(Cmd::Forget, addr) : -1;
}

extern "C" cJSON* hs_state_json(void)
{
    std::lock_guard<std::mutex> lock(g_mu);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "on", g_snap.on);
    struct stat st;
    cJSON_AddBoolToObject(o, "enabled", stat(g_onFile.c_str(), &st) == 0); // Settings' switch
    cJSON_AddStringToObject(o, "state", g_snap.state.c_str());
    cJSON_AddStringToObject(o, "message", g_snap.message.c_str());
    cJSON_AddStringToObject(o, "adapter", g_snap.adapter.c_str());
    if (!g_snap.addr.empty())
    {
        cJSON* h = cJSON_AddObjectToObject(o, "headset");
        cJSON_AddStringToObject(h, "name", g_snap.name.c_str());
        cJSON_AddStringToObject(h, "addr", g_snap.addr.c_str());
        cJSON_AddStringToObject(h, "route", g_snap.route.c_str());
        cJSON_AddStringToObject(h, "mic", g_snap.mic.c_str());
        cJSON_AddNumberToObject(h, "mic_frames", static_cast<double>(g_snap.micFrames));
        cJSON_AddNumberToObject(h, "mic_errors", static_cast<double>(g_snap.micErrors));
        cJSON_AddNumberToObject(h, "decode_us", g_snap.decodeUs);
        cJSON_AddNumberToObject(h, "packets_per_s", g_snap.pps);
        cJSON_AddNumberToObject(h, "queue_ms", g_snap.queueMs);
        // what the headset says it adds after our queue (-1: it does not say)
        cJSON_AddNumberToObject(h, "headset_delay_ms", g_snap.delayReport < 0 ? -1 : g_snap.delayReport / 10.0);
    }
    cJSON* list = cJSON_AddArrayToObject(o, "devices");
    for (const BondEntry& b : g_snap.bonds)
    {
        cJSON* d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "addr", addressString(b.peer).c_str());
        cJSON_AddStringToObject(d, "name", b.name.c_str());
        cJSON_AddBoolToObject(d, "paired", 1);
        cJSON_AddItemToArray(list, d);
    }
    for (const BtDevice& dev : g_snap.devices)
    {
        bool known = false;
        for (const BondEntry& b : g_snap.bonds)
        {
            known = known || b.peer == dev.address;
        }
        if (known)
        {
            continue;
        }
        cJSON* d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "addr", addressString(dev.address).c_str());
        cJSON_AddStringToObject(d, "name", dev.name.c_str());
        cJSON_AddBoolToObject(d, "paired", 0);
        cJSON_AddItemToArray(list, d);
    }
    return o;
}
