// ps5-discord - Loading system modules and finding their functions in a native title.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

extern "C" int sceKernelDlsym(int handle, const char *symbol, void **address);

namespace hui::ps5
{

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
