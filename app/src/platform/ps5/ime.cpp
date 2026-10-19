// ps5-discord - The system keyboard (IME dialog).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDK ships the libSceImeDialog stub but no header; the parameter block
// follows the console's (PS4-era) layout, as homebrew SDKs document it.

#include "platform/ps5/ime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
    int sceSysmoduleGetModuleHandleInternal(std::uint32_t id, int *handle);
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv, std::uint32_t flags,
                                 void *option, int *result);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
    int sceKernelGetModuleList2(int *handles, std::size_t capacity, std::size_t *count);
}

// The dialog's functions, resolved at run time: a native title does not get
// libSceImeDialog loaded for it (calling the bare import jumped to 0).
struct ImeApi
{
    int (*init)(const ImeDialogParam *, void *) = nullptr;
    int (*status)() = nullptr; // 1: running, 2: finished
    int (*result)(ImeDialogResult *) = nullptr;
    int (*term)() = nullptr;
    bool ok() const
    {
        return init && status && result && term;
    }
};

const ImeApi &ime_api()
{
    static const ImeApi api = [] {
        ImeApi a;
        constexpr std::uint16_t kImeDialogModule = 0x0096; // SCE_SYSMODULE_IME_DIALOG
        int handle = -1;
        const int load_rc = sceSysmoduleLoadModule(kImeDialogModule);
        const int handle_rc = sceSysmoduleGetModuleHandleInternal(kImeDialogModule, &handle);
        hui::sys::log("[IME] module load=0x%08x handle_rc=0x%08x handle=0x%x", static_cast<unsigned>(load_rc),
                      static_cast<unsigned>(handle_rc), static_cast<unsigned>(handle));
        // The export may live in a module other than the one the ID names: try
        // that handle, then the library by path, then everything loaded.
        std::array<int, 256> handles{};
        std::size_t count = 0;
        if (handle >= 0)
            handles[count++] = handle;
        const int by_path = sceKernelLoadStartModule("/system/common/lib/libSceImeDialog.sprx", 0, nullptr, 0,
                                                     nullptr, nullptr);
        hui::sys::log("[IME] by path handle=0x%x", static_cast<unsigned>(by_path));
        if (by_path >= 0)
            handles[count++] = by_path;
        std::size_t listed = 0;
        if (sceKernelGetModuleList2(handles.data() + count, handles.size() - count, &listed) == 0)
            count += std::min(listed, handles.size() - count);
        for (std::size_t i = 0; i < count && !a.ok(); ++i)
        {
            ImeApi t;
            sceKernelDlsym(handles[i], "sceImeDialogInit", reinterpret_cast<void **>(&t.init));
            sceKernelDlsym(handles[i], "sceImeDialogGetStatus", reinterpret_cast<void **>(&t.status));
            sceKernelDlsym(handles[i], "sceImeDialogGetResult", reinterpret_cast<void **>(&t.result));
            sceKernelDlsym(handles[i], "sceImeDialogTerm", reinterpret_cast<void **>(&t.term));
            if (t.ok())
            {
                a = t;
                hui::sys::log("[IME] resolved in handle 0x%x (%zu of %zu)", static_cast<unsigned>(handles[i]), i,
                              count);
            }
        }
        if (!a.ok())
            hui::sys::log("[IME] not found in %zu modules", count);
        return a;
    }();
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
