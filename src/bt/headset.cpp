// SPDX-License-Identifier: AGPL-3.0-or-later
// The Bluetooth headset (headset.h): one thread owns the USB adapter and BlueV's stack
// (third_party/bluev). A session: connect (or adopt a headset that dialled in), A2DP with
// the game's sound plus the call, and on AirPods their own mic stream during calls.
#include "headset.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "aacp_mic.hpp"
#include "core/audio/a2dp_session.hpp"
#include "core/audio/a2dp_streamer.hpp"
#include "core/audio/avcap_capture.hpp"
#include "core/audio/avrcp.hpp"
#include "core/bluetooth/address.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/usb_hci_transport.hpp"
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
    int pps = 0, queueMs = 0;
    long micFrames = 0, micErrors = 0;
    double decodeUs = 0;
    std::vector<BtDevice> devices;
    std::vector<BondEntry> bonds;
};

std::mutex g_mu;
std::deque<Request> g_queue;
Snapshot g_snap;
bool g_started = false;

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
                char name[96];
                std::snprintf(name, sizeof name, "%04x:%04x %s", d.vid, d.pid, d.product.c_str());
                m_adapterName = name;
                break;
            }
        }
        if (node.empty())
        {
            publish("off", "no USB Bluetooth adapter");
            return;
        }
        m_bt.useAdapter(node);
        if (!m_bt.start())
        {
            publish("off", m_bt.startError().empty() ? "the adapter did not start" : m_bt.startError().c_str());
            return;
        }
        m_a2dp.reset();
        m_a2dp.listen();
        m_avrcp.reset();
        m_avrcp.listen();
        m_on = true;
        publish("idle", "ready");
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
    bool connect(const BdAddr& addr)
    {
        for (int attempt = 0; attempt < 3; attempt++)
        {
            if (m_bt.connectTo(addr))
            {
                return true;
            }
            m_bt.wait([] { return false; }, 2000, false);
        }
        return false;
    }

    void session(const BdAddr* addr)
    {
        publish("connecting", addr ? "connecting" : "a paired headset is connecting");
        bool ok = addr ? connect(*addr) : m_bt.adoptIncoming();
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
                g_snap.route = m_airpods ? "airpods" : "a2dp";
                g_snap.mic = "off";
            }
            m_micFailed = false;
            publish("connected", "connected");
            m_bt.setTick([this] {
                m_avrcp.service();
                tick();
            });
            m_streamer.run(capturing);
            m_bt.setTick(nullptr);
        }
        m_mic.close();
        releaseExt();
        m_avrcp.stop();
        m_a2dp.stop();
        m_capture.close();
        m_bt.disconnect();
        m_a2dp.reset();
        m_a2dp.listen();
        m_avrcp.reset();
        m_avrcp.listen();
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
        publish("idle", ok ? "disconnected" : "could not connect");
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
        if (m_airpods && inCall && !m_mic.running() && !m_micFailed)
        {
            m_mic.start();
        }
        else if (m_mic.running() && !inCall)
        {
            m_mic.stop();
        }
        if (m_mic.silentSeconds() >= kMicSilentLimit)
        {
            LOG("bt: no AirPods mic audio for %d s: the mic counts as failed", kMicSilentLimit);
            m_micFailed = true;
            m_mic.stop();
        }
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap.mic = m_micFailed ? "failed" : m_mic.running() ? "on" : "off";
        g_snap.micFrames = m_mic.frames();
        g_snap.micErrors = m_mic.errors();
        g_snap.decodeUs = m_mic.decodeUs();
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
    bool m_on = false, m_avcapLoaded = false, m_airpods = false, m_micFailed = false;
    bool m_ownExt = false, m_offAfter = false;
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
    // The radio lives as long as the core; its thread is never joined.
    auto* radio = new Radio(bonds, firmware_dir);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_started = true;
    }
    std::thread([radio] { radio->run(); }).detach();
}

extern "C" int hs_power(int on)
{
    return queue(on ? Cmd::PowerOn : Cmd::PowerOff);
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
