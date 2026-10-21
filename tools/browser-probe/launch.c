// SPDX-License-Identifier: GPL-3.0-or-later
// One-shot browser launch. No child process, watchdog, or credential access.
#include <stdio.h>
#include <unistd.h>

#ifndef PROBE_URL
#error Define PROBE_URL for the browser launch test
#endif

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char *, void *);

static void mark(const char *stage, int result)
{
    FILE *file = fopen("/mnt/usb0/discord-browser-no-watchdog.log", "a");
    if (file) {
        fprintf(file, "pid=%ld %s: 0x%08x\n", (long)getpid(), stage, (unsigned)result);
        fclose(file);
    }
}

int main(void)
{
    mark("start; no watchdog or child process", 0);
    int result = sceUserServiceInitialize(NULL);
    mark("UserService initialize", result);
    if (result == 0) {
        mark("before browser launch", 0);
        result = sceSystemServiceLaunchWebBrowser(PROBE_URL, NULL);
        mark("browser launch returned (not rendering status)", result);
        const int terminated = sceUserServiceTerminate();
        mark("UserService terminate", terminated);
        if (result == 0)
            result = terminated;
    }
    mark("returning from main; SDK shutdown pending", result);
    return result == 0 ? 0 : 1;
}
