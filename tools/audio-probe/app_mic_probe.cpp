// DEBUG ONLY: can the app open the microphone? Never ship this build.
// Sweeps users x port types x rates x params, reads ~1 s per successful open,
// logs RMS levels to klog and /data/ps5-discord-probe/app-mic.txt.
#ifdef HUI_MIC_PROBE

#include "platform/ps5/system.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern "C"
{
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetInitialUser(int *user);
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv, std::uint32_t flags,
                                 void *option, int *result);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
    const char *sceKernelGetFsSandboxRandomWord();
}

namespace hui::ps5
{
namespace
{
int (*open_fn)(int, int, int, unsigned int, unsigned int, unsigned int) = nullptr;
int (*input_fn)(int, void *) = nullptr;
int (*close_fn)(int) = nullptr;
std::FILE *out = nullptr;

void note(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    sys::log("[HUI-MIC] %s", line);
    if (out)
    {
        std::fprintf(out, "%s\n", line);
        std::fflush(out);
    }
}

// Same lookup as ime.cpp: the title's own dlsym finds nothing on 13.00, the core can.
std::uintptr_t core_dlsym(const char *lib, const char *sym)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    sockaddr_in to{};
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_port = htons(9041);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char buf[512];
    const int n = std::snprintf(buf, sizeof buf, "GET /api/dlsym?pid=%d&lib=%s&sym=%s HTTP/1.0\r\n"
                                "X-Discord-Native: 1\r\n\r\n", static_cast<int>(getpid()), lib, sym);
    std::string raw;
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0 && send(fd, buf, n, 0) == n)
        for (ssize_t r; (r = recv(fd, buf, sizeof buf, 0)) > 0;)
            raw.append(buf, static_cast<std::size_t>(r));
    close(fd);
    const std::size_t at = raw.find("\"addr\":");
    return at == std::string::npos ? 0 : std::strtoull(raw.c_str() + at + 7, nullptr, 10);
}

template <typename F> void resolve(int handle, const char *sym, F &out)
{
    void *p = nullptr;
    if (handle < 0 || sceKernelDlsym(handle, sym, &p) != 0 || !p)
        p = reinterpret_cast<void *>(core_dlsym("libSceAudioIn.sprx", sym));
    out = reinterpret_cast<F>(p);
}

void capture(int handle, unsigned grain_frames)
{
    static std::int16_t grain[1024];
    for (int window = 0; window < 10; ++window) // ~1 s
    {
        double sum_sq = 0;
        int peak = 0, rc = 0;
        long count = 0;
        for (unsigned got = 0; got < 4800 / 3;) // ~100 ms at 16 kHz, ~33 ms at 48 kHz
        {
            rc = input_fn(handle, grain);
            if (rc < 0)
                break;
            for (unsigned i = 0; i < grain_frames; ++i)
            {
                const int s = grain[i], a = s < 0 ? -s : s;
                sum_sq += static_cast<double>(s) * s;
                peak = a > peak ? a : peak;
                ++count;
            }
            got += grain_frames;
        }
        note("  rms=%7.1f peak=%5d rc=0x%08x", count ? std::sqrt(sum_sq / count) : 0.0, peak,
             static_cast<unsigned>(rc));
        if (rc < 0)
            break;
    }
}
} // namespace

void probe_mic_run()
{
    mkdir("/data/ps5-discord-probe", 0777);
    out = std::fopen("/data/ps5-discord-probe/app-mic.txt", "a");
    note("start: app mic probe");
    const std::string path = std::string("/") + sceKernelGetFsSandboxRandomWord() + "/common/lib/libSceAudioIn.sprx";
    const int lib = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, nullptr);
    resolve(lib, "sceAudioInOpen", open_fn);
    resolve(lib, "sceAudioInInput", input_fn);
    resolve(lib, "sceAudioInClose", close_fn);
    note("module=0x%x open=%p input=%p close=%p", static_cast<unsigned>(lib), reinterpret_cast<void *>(open_fn),
         reinterpret_cast<void *>(input_fn), reinterpret_cast<void *>(close_fn));
    if (!open_fn || !input_fn || !close_fn)
    {
        note("sceAudioIn symbols missing");
        return;
    }
    sceUserServiceInitialize(nullptr);
    int user = -1;
    note("initial user rc=0x%08x user=0x%x", static_cast<unsigned>(sceUserServiceGetInitialUser(&user)),
         static_cast<unsigned>(user));
    const int users[] = {user, 0xff};
    const unsigned rates[] = {16000, 48000};
    int opened = 0;
    for (const int u : users)
        for (int type = 0; type <= 5; ++type)
            for (const unsigned rate : rates)
                for (unsigned param = 0; param <= 1; ++param)
                {
                    const unsigned grain = 256;
                    const int handle = open_fn(u, type, 0, grain, rate, param);
                    note("open user=0x%x type=%d rate=%u param=%u -> 0x%08x", static_cast<unsigned>(u), type, rate,
                         param, static_cast<unsigned>(handle));
                    if (handle < 0)
                        continue;
                    ++opened;
                    capture(handle, grain);
                    note("  close -> 0x%08x", static_cast<unsigned>(close_fn(handle)));
                }
    note("done: %d open(s)", opened);
    if (out)
        std::fclose(out);
    out = nullptr;
}

// Runs once the core (which answers the symbol lookup) has had time to start.
void probe_mic_debug()
{
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        probe_mic_run();
    }).detach();
}

} // namespace hui::ps5

#endif // HUI_MIC_PROBE
