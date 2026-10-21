// ps5-discord - The system keyboard (IME dialog).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDK ships the libSceImeDialog stub but no header; the parameter block
// follows the console's (PS4-era) layout, as homebrew SDKs document it.

#include "platform/ps5/ime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform/ps5/system.hpp"

namespace
{

struct ImeDialogParam
{
    std::int32_t user_id;
    std::int32_t type; // 0: default text
    std::uint64_t supported_languages; // 0: all
    std::int32_t enter_label; // 0: default
    std::int32_t input_method; // 0: default
    void *filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    char16_t *input_text_buffer;
    float pos_x;
    float pos_y;
    std::int32_t horizontal_alignment; // 1: centre
    std::int32_t vertical_alignment;   // 1: centre
    const char16_t *placeholder;
    const char16_t *title;
    std::int8_t reserved[16];
};
static_assert(sizeof(ImeDialogParam) == 96, "IME dialog parameter layout");

struct ImeDialogResult
{
    std::int32_t end_status; // 0: OK, 1: cancelled, 2: aborted
    std::int8_t reserved[12];
};

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t id);
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv, std::uint32_t flags,
                                 void *option, int *result);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
    const char *sceKernelGetFsSandboxRandomWord();
}

// Asks the core where sym sits in this process (0 if it could not say). A
// native title's own dlsym finds nothing on 13.00; the core reads the app's
// library table through the kernel.
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
                                "X-Discord-Native: 1\r\n\r\n",
                                static_cast<int>(getpid()), lib, sym);
    std::string raw;
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0 && send(fd, buf, n, 0) == n)
        for (ssize_t r; (r = recv(fd, buf, sizeof buf, 0)) > 0;)
            raw.append(buf, static_cast<std::size_t>(r));
    close(fd);
    const std::size_t at = raw.find("\"addr\":");
    return at == std::string::npos ? 0 : std::strtoull(raw.c_str() + at + 7, nullptr, 10);
}

// One function: this process's dlsym first, then the core.
template <typename F> void resolve(int handle, const char *lib, const char *sym, F &out)
{
    void *p = nullptr;
    if (handle < 0 || sceKernelDlsym(handle, sym, &p) != 0 || !p)
        p = reinterpret_cast<void *>(core_dlsym(lib, sym));
    out = reinterpret_cast<F>(p);
}

// The dialog's functions, resolved at run time: a native title does not get
// libSceImeDialog loaded for it (calling the bare import jumped to 0).
struct ImeApi
{
    int (*common_init)() = nullptr; // sceCommonDialogInitialize, needed before any dialog
    int (*init)(const ImeDialogParam *, void *) = nullptr;
    int (*status)() = nullptr; // 1: running, 2: finished
    int (*result)(ImeDialogResult *) = nullptr;
    int (*term)() = nullptr;
    bool ok() const
    {
        return common_init && init && status && result && term;
    }
};

// Kept once found; looked for again on the next open until then (the core may
// not be up yet).
const ImeApi &ime_api()
{
    static ImeApi api;
    if (api.ok())
        return api;
    constexpr std::uint16_t kImeDialogModule = 0x0096; // SCE_SYSMODULE_IME_DIALOG
    const int sys_rc = sceSysmoduleLoadModule(kImeDialogModule);
    // System libraries sit under the title's sandbox, not /system.
    const std::string dir = std::string("/") + sceKernelGetFsSandboxRandomWord() + "/common/lib/";
    const int common = sceKernelLoadStartModule((dir + "libSceCommonDialog.sprx").c_str(), 0, nullptr, 0, nullptr,
                                                nullptr);
    const int ime = sceKernelLoadStartModule((dir + "libSceImeDialog.sprx").c_str(), 0, nullptr, 0, nullptr, nullptr);
    ImeApi a;
    resolve(common, "libSceCommonDialog.sprx", "sceCommonDialogInitialize", a.common_init);
    resolve(ime, "libSceImeDialog.sprx", "sceImeDialogInit", a.init);
    resolve(ime, "libSceImeDialog.sprx", "sceImeDialogGetStatus", a.status);
    resolve(ime, "libSceImeDialog.sprx", "sceImeDialogGetResult", a.result);
    resolve(ime, "libSceImeDialog.sprx", "sceImeDialogTerm", a.term);
    hui::sys::log("[IME] sysmodule=0x%08x common=0x%x ime=0x%x found=%d%d%d%d%d", static_cast<unsigned>(sys_rc),
                  static_cast<unsigned>(common), static_cast<unsigned>(ime), !!a.common_init, !!a.init, !!a.status,
                  !!a.result, !!a.term);
    if (a.ok())
    {
        const int rc = a.common_init();
        hui::sys::log("[IME] common dialog init=0x%08x", static_cast<unsigned>(rc));
        api = a;
    }
    return api;
}

// UTF-8 to UTF-16 and back, enough for chat text (surrogate pairs included).
std::size_t to_utf16(const std::string &in, char16_t *out, std::size_t cap)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < in.size() && n + 2 < cap;)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra)
            cp = c & (0x3f >> extra);
        ++i;
        for (; extra > 0 && i < in.size(); --extra, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(in[i]) & 0x3f);
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out[n++] = static_cast<char16_t>(0xd800 + (cp >> 10));
            out[n++] = static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        }
        else
            out[n++] = static_cast<char16_t>(cp);
    }
    out[n] = 0;
    return n;
}

std::string to_utf8(const char16_t *in)
{
    std::string out;
    for (std::size_t i = 0; in[i]; ++i)
    {
        std::uint32_t cp = in[i];
        if (cp >= 0xd800 && cp < 0xdc00 && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (in[++i] - 0xdc00);
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

} // namespace

namespace hui::ps5
{

bool Ime::open(int user_id, const std::string &title, const std::string &initial)
{
    if (state_ == State::open)
        return false;
    to_utf16(initial, buffer_, sizeof buffer_ / sizeof buffer_[0]);
    to_utf16(title, title_, sizeof title_ / sizeof title_[0]);
    ImeDialogParam param{};
    param.user_id = user_id;
    param.max_text_length = 2000; // Discord's limit for a message
    param.input_text_buffer = buffer_;
    param.pos_x = 960.0f;
    param.pos_y = 540.0f;
    param.horizontal_alignment = 1;
    param.vertical_alignment = 1;
    param.title = title_;
    const ImeApi &api = ime_api();
    if (!api.ok())
        return false;
    const int rc = api.init(&param, nullptr);
    sys::log("[IME] open rc=0x%08x", static_cast<unsigned>(rc));
    if (rc < 0)
        return false;
    state_ = State::open;
    return true;
}

Ime::State Ime::poll()
{
    if (state_ != State::open)
    {
        const State out = state_;
        if (out == State::done || out == State::cancelled)
            state_ = State::idle; // reported once
        return out;
    }
    const ImeApi &api = ime_api();
    if (api.status() != 2)
        return State::open;
    ImeDialogResult result{};
    api.result(&result);
    api.term();
    text_ = to_utf8(buffer_);
    state_ = State::idle;
    return result.end_status == 0 ? State::done : State::cancelled;
}

} // namespace hui::ps5
