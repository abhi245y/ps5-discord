// SPDX-License-Identifier: AGPL-3.0-or-later
// Controlled lifecycle test; no credentials, Discord, or PSN operations.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PROBE_LOCAL_PAGE
#define PROBE_LOCAL_PAGE 0
#endif
#ifndef PROBE_PASSIVE_GUARD
#define PROBE_PASSIVE_GUARD 0
#endif
#if PROBE_LOCAL_PAGE
#ifndef PROBE_URL
#error Define PROBE_URL for the local-page build
#endif
#if PROBE_PASSIVE_GUARD
#define LOG_PATH "/mnt/usb0/discord-browser-passive.log"
#else
#define LOG_PATH "/mnt/usb0/discord-browser-local.log"
#endif
#else
#define LOG_PATH "/mnt/usb0/discord-browser-lifecycle.log"
#endif

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char *, void *);

static void mark(const char *stage, int result)
{
    FILE *file = fopen(LOG_PATH, "a");
    if (file) {
        fprintf(file, "pid=%ld %s: 0x%08x\n", (long)getpid(), stage, (unsigned)result);
        fclose(file);
    }
}

int main(void)
{
    const pid_t parent = getpid();
    const pid_t watchdog = fork();
    if (watchdog < 0)
        return 1;
    if (watchdog == 0) {
        unsigned remaining = 25;
        while (remaining)
            remaining = sleep(remaining);
        if (getppid() == parent)
            kill(parent, SIGKILL);
#if PROBE_PASSIVE_GUARD
        mark("watchdog reached deadline; exiting child", 0);
#endif
        _exit(0);
    }
    mark("watchdog child pid", watchdog);
    mark("before UserService init", 0);
    int result = sceUserServiceInitialize(NULL);
    mark("after UserService init", result);
    if (result == 0) {
#if PROBE_LOCAL_PAGE
        mark("before local browser launch", 0);
        result = sceSystemServiceLaunchWebBrowser(PROBE_URL, NULL);
        mark("after local browser launch", result);
#else
        mark("baseline: browser launch omitted", 0);
#endif
        mark("before UserService terminate", 0);
        const int terminated = sceUserServiceTerminate();
        mark("after UserService terminate", terminated);
        if (result == 0)
            result = terminated;
    }
#if PROBE_PASSIVE_GUARD
    // User-requested experiment: do not signal or reap the watchdog from parent.
    // Child remains for 25 seconds, then exits; only kills parent if still its parent.
    mark("returning without watchdog kill/reap; SDK shutdown still pending", result);
#else
    mark("before watchdog kill", 0);
    const int killed = kill(watchdog, SIGKILL);
    mark("after watchdog kill", killed);
    // Bound the reap independently: after kill the watchdog no longer protects us.
    int status = 0;
    pid_t reaped = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        reaped = waitpid(watchdog, &status, WNOHANG);
        if (reaped == watchdog || (reaped < 0 && errno != EINTR))
            break;
        usleep(100000);
    }
    mark("after watchdog reap (pid, 0 means timeout)", reaped);
    mark("watchdog wait status", status);
    if (reaped != watchdog)
        result = -1;
    mark("returning from main; SDK shutdown still pending", result);
#endif
    return result == 0 ? 0 : 1;
}
