// ps5-discord - Loading system modules and finding their functions in a native title.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

extern "C" int sceKernelDlsym(int handle, const char *symbol, void **address);

namespace hui::ps5
{

// The core's per-boot key (src/core_key.h), sent with every request and first on the
// audio link; empty before the core has written it. The core writes it to the install
// folder (a title's sandbox has no /data). Inline: the host snapshots use it too.
// ponytail: read on every use, so a core restarted with a new key needs nothing; cache it if it shows in profiles.
inline std::string core_key()
{
    char key[64];
    std::FILE *f = std::fopen("/app0/control.key", "rb");
    const std::size_t n = f ? std::fread(key, 1, sizeof key, f) : 0;
    if (f)
        std::fclose(f);
    return n == sizeof key ? std::string(key, sizeof key) : std::string();
}

// Asks the core where sym sits in this process (0 if it could not say). A
// native title's own dlsym finds nothing on 13.00; the core reads the app's
// library table through the kernel.
std::uintptr_t core_dlsym(const char *lib, const char *sym);

// One function: this process's dlsym first, then the core.
template <typename F> void resolve(int handle, const char *lib, const char *sym, F &out)
{
    void *p = nullptr;
    if (handle < 0 || sceKernelDlsym(handle, sym, &p) != 0 || !p)
        p = reinterpret_cast<void *>(core_dlsym(lib, sym));
    out = reinterpret_cast<F>(p);
}

} // namespace hui::ps5
