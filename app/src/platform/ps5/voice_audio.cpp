// ps5-discord - Voice audio on the console (see voice_audio.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/voice_audio.hpp"

#include "platform/ps5/sce_module.hpp"
#include "platform/ps5/system.hpp"

#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv,
                                 std::uint32_t flags, void *option, int *result);
    const char *sceKernelGetFsSandboxRandomWord();
    int sceAudioInOpen(int user, int type, int index, unsigned int grain_frames,
                       unsigned int frequency, unsigned int format);
    int sceAudioInInput(int handle, void *buffer);
    int sceAudioInClose(int handle);
}

namespace hui::ps5
{

namespace
{

// As tools/audio-probe/app_mic_probe.cpp: foreground user, type 0 (voice),
// 48 kHz, format 1 (S16 mono).
constexpr int kPortType = 0;
constexpr unsigned int kFrequency = 48000;
constexpr unsigned int kFormatS16Mono = 1;
constexpr std::size_t kPendingMax = 9600 * 2; // 200 ms of mic PCM waiting for the socket, bytes
// RMS of a full-scale-ish speech grain on the DualSense mic is about 6000-10000.
constexpr float kLevelFullScale = 6000.0f;

using OpenFn = int (*)(int, int, int, unsigned int, unsigned int, unsigned int);
using InputFn = int (*)(int, void *);
using CloseFn = int (*)(int);

struct MicApi
{
    OpenFn open = nullptr;
    InputFn input = nullptr;
    CloseFn close = nullptr;
    bool ok() const
    {
        return open && input && close;
    }
};

// The port's functions, found once; retried on the next open until they are.
const MicApi &mic_api()
{
    static MicApi api;
    if (api.ok())
        return api;
    const std::string path =
        std::string("/") + sceKernelGetFsSandboxRandomWord() + "/common/lib/libSceAudioIn.sprx";
    const int lib = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, nullptr);
    MicApi found;
    resolve(lib, "libSceAudioIn.sprx", "sceAudioInOpen", found.open);
    resolve(lib, "libSceAudioIn.sprx", "sceAudioInInput", found.input);
    resolve(lib, "libSceAudioIn.sprx", "sceAudioInClose", found.close);
    if (found.ok())
        api = found;
    return api;
}

// The core's audio socket on loopback; -1 when it is not listening (yet).
int connect_core(std::uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in to{};
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) != 0)
    {
        static int failures = 0; // every 20th (10 s at the retry pace), so the log stays readable
        if (failures++ % 20 == 0)
            sys::log("[HUI] voice link connect to %u failed errno=%d", port, errno);
        close(fd);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

void sleep_ms(std::uint32_t ms)
{
    sys::sleep_us(ms * 1000);
}

} // namespace

VoiceAudio::VoiceAudio() = default;

VoiceAudio::~VoiceAudio()
{
    stop();
}

void VoiceAudio::start(int user)
{
    if (started_)
        return;
    user_ = user;
    std::signal(SIGPIPE, SIG_IGN); // a core that closes mid-write is an error, not a crash
    stop_.store(false);
    if (pthread_create(&mic_thread_, nullptr, &VoiceAudio::mic_main, this) != 0)
    {
        sys::log("[HUI] voice mic thread creation failed");
        return;
    }
    if (pthread_create(&link_thread_, nullptr, &VoiceAudio::link_main, this) != 0)
    {
        sys::log("[HUI] voice link thread creation failed");
        stop_.store(true);
        pthread_join(mic_thread_, nullptr);
        return;
    }
    started_ = true;
}

void VoiceAudio::stop()
{
    if (!started_)
        return;
    stop_.store(true);
    pthread_join(mic_thread_, nullptr);
    pthread_join(link_thread_, nullptr);
    started_ = false;
}

void *VoiceAudio::mic_main(void *self)
{
    static_cast<VoiceAudio *>(self)->mic_run();
    return nullptr;
}

void *VoiceAudio::link_main(void *self)
{
    static_cast<VoiceAudio *>(self)->link_run();
    return nullptr;
}

void VoiceAudio::mic_run()
{
    int handle = -1;
    bool reported = false; // the first failure to open is logged, then only retried quietly
    std::int64_t retry_at = 0;
    std::int16_t grain[kGrainFrames];
    while (!stop_.load(std::memory_order_relaxed))
    {
        if (!want_mic_.load(std::memory_order_relaxed))
        {
            if (handle >= 0)
            {
                mic_api().close(handle);
                handle = -1;
                sys::log("[HUI] voice mic closed");
            }
            level_.store(0.0f, std::memory_order_relaxed);
            sleep_ms(20);
            continue;
        }
        if (handle < 0)
        {
            if (sys::monotonic_us() < retry_at)
            {
                sleep_ms(50);
                continue;
            }
            const MicApi &api = mic_api();
            if (api.ok())
                handle = api.open(user_, kPortType, 0, kGrainFrames, kFrequency, kFormatS16Mono);
            if (handle < 0)
            {
                if (!reported)
                    sys::log("[HUI] voice mic open failed handle=0x%08x",
                             static_cast<unsigned>(handle));
                reported = true;
                handle = -1;
                retry_at = sys::monotonic_us() + 2000000;
                continue;
            }
            reported = false;
            sys::log("[HUI] voice mic open handle=%d", handle);
        }
        const int rc = mic_api().input(handle, grain);
        if (rc < 0)
        {
            sys::log("[HUI] voice mic read failed rc=0x%08x", static_cast<unsigned>(rc));
            mic_api().close(handle);
            handle = -1;
            retry_at = sys::monotonic_us() + 1000000;
            continue;
        }
        double sum_sq = 0.0;
        for (const std::int16_t s : grain)
            sum_sq += static_cast<double>(s) * s;
        const float rms = static_cast<float>(std::sqrt(sum_sq / kGrainFrames));
        level_.store(std::fmin(1.0f, rms / kLevelFullScale), std::memory_order_relaxed);
        mic_ring_.push(grain, kGrainFrames);
    }
    if (handle >= 0)
        mic_api().close(handle);
}

void VoiceAudio::link_run()
{
    int fd = -1;
    std::int64_t retry_at = 0;
    std::vector<std::uint8_t> out;     // mic bytes waiting for the socket (even length kept)
    std::vector<std::uint8_t> in_left; // received bytes not yet a whole stereo frame
    std::int16_t mic[1024];
    std::uint8_t buf[4096];
    // A stats line every 5 s while linked: where audio stops, at a glance.
    std::size_t st_recv = 0, st_played = 0, st_sent = 0;
    std::int64_t st_at = sys::monotonic_us();
    const auto stats = [&]
    {
        const std::int64_t now = sys::monotonic_us();
        if (now - st_at < 5000000)
            return;
        sys::log("[HUI] voice link 5 s: recv %zu B, to playback %zu frames, mic sent %zu B, want mic=%d play=%d, "
                 "buffered %zu",
                 st_recv, st_played, st_sent, want_mic_.load() ? 1 : 0, want_playback_.load() ? 1 : 0,
                 playback_.available());
        st_recv = st_played = st_sent = 0;
        st_at = now;
    };
    const auto drop = [&]
    {
        if (fd >= 0)
            close(fd);
        fd = -1;
        out.clear();
        in_left.clear();
    };
    while (!stop_.load(std::memory_order_relaxed))
    {
        if (!want_link_.load(std::memory_order_relaxed))
        {
            if (fd >= 0)
            {
                drop();
                sys::log("[HUI] voice link closed");
            }
            sleep_ms(50);
            continue;
        }
        if (fd < 0)
        {
            if (sys::monotonic_us() < retry_at)
            {
                sleep_ms(50);
                continue;
            }
            fd = connect_core(kCorePort);
            if (fd < 0)
            {
                retry_at = sys::monotonic_us() + 500000;
                continue;
            }
            // Mic audio from before the connection is stale.
            while (mic_ring_.pop(mic, 1024) > 0)
            {
            }
            sys::log("[HUI] voice link connected");
        }

        stats();
        pollfd p{fd, static_cast<short>(POLLIN | (out.empty() ? 0 : POLLOUT)), 0};
        poll(&p, 1, 5);
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            sys::log("[HUI] voice link lost, retrying");
            drop();
            retry_at = sys::monotonic_us() + 500000;
            continue;
        }

        // Mic: whatever the ring holds, after what is already waiting.
        for (std::size_t n; (n = mic_ring_.pop(mic, 1024)) > 0;)
        {
            const std::size_t at = out.size();
            out.resize(at + n * 2);
            std::memcpy(out.data() + at, mic, n * 2);
        }
        if (out.size() > kPendingMax) // the socket fell behind: the oldest goes
            out.erase(out.begin(),
                      out.begin() + static_cast<std::ptrdiff_t>((out.size() - kPendingMax + 1) &
                                                                ~std::size_t{1}));
        if (!out.empty())
        {
            // MSG_DONTWAIT on every call: O_NONBLOCK from fcntl did not take on the console,
            // and a blocking call here stalls both the mic and the playback.
            const ssize_t sent = send(fd, out.data(), out.size(), MSG_DONTWAIT);
            if (sent > 0)
            {
                st_sent += static_cast<std::size_t>(sent);
                out.erase(out.begin(), out.begin() + sent);
            }
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                sys::log("[HUI] voice link send failed errno=%d", errno);
                drop();
                retry_at = sys::monotonic_us() + 500000;
                continue;
            }
        }

        // Playback: stereo S16 at 48 kHz, 4 bytes a frame. A partial frame waits for its rest.
        if (p.revents & POLLIN)
        {
            bool closed = false;
            for (int reads = 0; reads < 16; ++reads) // bounded: a steady stream cannot hold the thread
            {
                const ssize_t got = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
                if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    closed = true;
                    break;
                }
                if (got < 0)
                    break;
                in_left.insert(in_left.end(), buf, buf + got);
                st_recv += static_cast<std::size_t>(got);
            }
            const std::size_t frames = in_left.size() / 4;
            if (frames > 0 && want_playback_.load(std::memory_order_relaxed))
            {
                std::vector<float> stereo(frames * 2);
                for (std::size_t i = 0; i < frames * 2; ++i)
                {
                    const std::int16_t s =
                        static_cast<std::int16_t>(in_left[i * 2] | (in_left[i * 2 + 1] << 8));
                    stereo[i] = s / 32768.0f;
                }
                st_played += playback_.write(stereo.data(), frames);
            }
            in_left.erase(in_left.begin(),
                          in_left.begin() + static_cast<std::ptrdiff_t>(frames * 4));
            if (closed)
            {
                sys::log("[HUI] voice link closed by the core, retrying");
                drop();
                retry_at = sys::monotonic_us() + 500000;
                continue;
            }
        }
    }
    drop();
}

} // namespace hui::ps5
