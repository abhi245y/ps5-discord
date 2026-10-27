// spot5 - Audio sink selection; experimental ABI recovered from PPSA05688.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/output.hpp"
#include "platform/ps5/system.hpp"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int, int, int, unsigned int, unsigned int, unsigned int);
    int sceAudioOutOutput(int, const void *);
    int sceAudioOutClose(int);
    int sceSysmoduleIsLoaded(std::uint16_t);
    int sceSysmoduleLoadModule(std::uint16_t);
    int sceSysmoduleUnloadModule(std::uint16_t);
    int sceSysmoduleGetModuleHandleInternal(std::uint32_t, int *);
    int sceKernelGetModuleList2(int *, std::size_t, std::size_t *);
    int sceKernelDlsym(int, const char *, void **);
}

const char *AudioOutput::name(AudioBackend backend)
{
    return backend == AudioBackend::custom_music ? "custom_music" : "main";
}

AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(AudioBackend backend)
{
    if (is_open())
        return false;
    backend_ = backend;
    last_error_ = 0;
    if (backend == AudioBackend::custom_music)
        return open_custom();
    const int init = sceAudioOutInit();
    if (init != 0 && init != static_cast<int>(0x8026000e))
    {
        last_error_ = init;
        hui::sys::log("[AUDIO] main init rc=0x%08x", static_cast<unsigned>(init));
        return false;
    }
    handle_ = sceAudioOutOpen(0xff, 0, 0, 256, 48000, 1);
    hui::sys::log("[AUDIO] main open rc=0x%08x", static_cast<unsigned>(handle_));
    if (handle_ < 0)
        last_error_ = handle_;
    return handle_ >= 0;
}

bool AudioOutput::open_custom()
{
    // Do not copy NPXS40201 or pretend this foreground process is system-hosted.
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        if (sceSysmoduleIsLoaded(kModules[i]) == 0)
            continue;
        const int rc = sceSysmoduleLoadModule(kModules[i]);
        hui::sys::log("[AUDIO] custom load id=0x%x rc=0x%08x", kModules[i], static_cast<unsigned>(rc));
        if (rc != 0)
        {
            last_error_ = rc;
            release_modules();
            return false;
        }
        owned_modules_[i] = true;
    }
    // Resolve all three exports from one module, using the public self-process
    // module list2 API. The original list returned only app/companion handles
    // on the test console. No kernel memory access or other-process scans.
    std::array<int, 256> handles{};
    std::size_t count = 0;
    const int listed = sceKernelGetModuleList2(handles.data(), handles.size(), &count);
    hui::sys::log("[AUDIO] resolver-v5 module list2 rc=0x%08x count=%zu capacity=%zu",
                  static_cast<unsigned>(listed), count, handles.size());
    if (listed != 0 || count > handles.size())
    {
        last_error_ = listed != 0 ? listed : -1;
        hui::sys::log("[AUDIO] custom module list rc=0x%08x count=%zu capacity=%zu",
                      static_cast<unsigned>(listed), count, handles.size());
        release_modules();
        return false;
    }
    // Console's sysmodule table explicitly maps public ID 0x122 to its live
    // module handle. This API writes a 32-bit handle without loading a module.
    int music_handle = -1;
    const int handle_rc = sceSysmoduleGetModuleHandleInternal(kModules.back(), &music_handle);
    hui::sys::log("[AUDIO] music module handle id=0x%x rc=0x%08x handle=0x%x",
                  kModules.back(), static_cast<unsigned>(handle_rc), static_cast<unsigned>(music_handle));
    if (handle_rc == 0 && music_handle >= 0)
    {
        bool present = false;
        for (std::size_t i = 0; i < count; ++i)
            present |= handles[i] == music_handle;
        if (!present)
        {
            if (count == handles.size())
            {
                last_error_ = -1;
                hui::sys::log("[AUDIO] module list full; cannot append music handle");
                release_modules();
                return false;
            }
            handles[count++] = music_handle;
        }
    }
    bool public_lookup_found = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        void *initialize = nullptr, *output = nullptr, *finalize = nullptr;
        const int init_rc = sceKernelDlsym(handles[i], "sceCustomMusicAudioOutInitialize", &initialize);
        const int output_rc = sceKernelDlsym(handles[i], "sceCustomMusicAudioOutOutput", &output);
        const int finalize_rc = sceKernelDlsym(handles[i], "sceCustomMusicAudioOutFinalize", &finalize);
        // A known public export distinguishes an unusable module list/lookup
        // path from custom exports that are unavailable in this context.
        void *public_export = nullptr;
        const int public_rc = sceKernelDlsym(handles[i], "sceKernelUsleep", &public_export);
        public_lookup_found |= public_rc == 0 && public_export != nullptr;
        hui::sys::log("[AUDIO] lookup index=%zu handle=0x%x init=0x%08x/%d output=0x%08x/%d finalize=0x%08x/%d public=0x%08x/%d",
                      i, static_cast<unsigned>(handles[i]),
                      static_cast<unsigned>(init_rc), initialize != nullptr,
                      static_cast<unsigned>(output_rc), output != nullptr,
                      static_cast<unsigned>(finalize_rc), finalize != nullptr,
                      static_cast<unsigned>(public_rc), public_export != nullptr);
        if (init_rc != 0 || !initialize || output_rc != 0 || !output || finalize_rc != 0 || !finalize)
            continue;
        custom_initialize_ = reinterpret_cast<int (*)(int, int, int)>(initialize);
        custom_output_ = reinterpret_cast<int (*)(const void *)>(output);
        custom_finalize_ = reinterpret_cast<int (*)()>(finalize);
        break;
    }
    if (!custom_initialize_)
    {
        last_error_ = -1;
        hui::sys::log("[AUDIO] custom export set missing; public_lookup_found=%d; no fallback to main output",
                      public_lookup_found);
        release_modules();
        return false;
    }
    // Exact three arguments at official core VA 0x1c7..0x1d6. Their complete
    // SDK enum meanings are unknown; this does not establish core authority.
    last_error_ = custom_initialize_(1024, 1, 1);
    hui::sys::log("[AUDIO] custom initialize(1024,1,1) rc=0x%08x",
                  static_cast<unsigned>(last_error_));
    if (last_error_ < 0)
    {
        release_modules();
        return false;
    }
    custom_initialized_ = true;
    hui::sys::log("[AUDIO] custom output ready; background hosting remains unverified");
    return true;
}

int AudioOutput::write(const std::int16_t *stereo)
{
    if (!is_open() || stereo == nullptr)
        return -1;
    return backend_ == AudioBackend::custom_music ? custom_output_(stereo) : sceAudioOutOutput(handle_, stereo);
}

int AudioOutput::drain()
{
    if (!is_open())
        return 0;
    // NULL output is observed in the official custom worker too; its precise
    // platform semantics remain unknown, so no successful-drain claim is made.
    return backend_ == AudioBackend::custom_music ? custom_output_(nullptr) : sceAudioOutOutput(handle_, nullptr);
}

int AudioOutput::close()
{
    if (custom_initialized_)
    {
        const int rc = custom_finalize_();
        hui::sys::log("[AUDIO] custom finalize rc=0x%08x", static_cast<unsigned>(rc));
        if (rc < 0)
        {
            last_error_ = rc;
            return rc; // Retain code references while platform state may be live.
        }
        custom_initialized_ = false;
    }
    if (handle_ >= 0)
    {
        const int rc = sceAudioOutClose(handle_);
        if (rc < 0)
        {
            last_error_ = rc;
            return rc;
        }
        handle_ = -1;
    }
    release_modules();
    return 0;
}

void AudioOutput::release_modules()
{
    custom_initialize_ = nullptr;
    custom_output_ = nullptr;
    custom_finalize_ = nullptr;
    for (std::size_t i = kModules.size(); i-- > 0;)
        if (owned_modules_[i])
        {
            const int rc = sceSysmoduleUnloadModule(kModules[i]);
            hui::sys::log("[AUDIO] custom unload id=0x%x rc=0x%08x", kModules[i], static_cast<unsigned>(rc));
            owned_modules_[i] = rc != 0;
        }
}
