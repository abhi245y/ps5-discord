// SPDX-License-Identifier: GPL-3.0-or-later
// Launch-only experiment. Does not read, export, or save Discord credentials.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char *, void *);

static void log_result(const char *step, int result)
{
    FILE *log = fopen("/mnt/usb0/discord-browser-probe.log", "a");
    fprintf(stderr, "[discord-browser-probe] %s: 0x%08x\n", step, (unsigned)result);
    if (log) {
        fprintf(log, "pid=%ld %s: 0x%08x\n", (long)getpid(), step, (unsigned)result);
        fclose(log);
    }
}

int main(void)
{
    // Start before any service or file I/O: a wedged probe must release elfldr.
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
        _exit(0);
    }

    log_result("start (25 second watchdog)", 0);
    const int initialized = sceUserServiceInitialize(NULL);
    log_result("sceUserServiceInitialize", initialized);
    int result = initialized;
    if (initialized == 0) {
        log_result("opening https://discord.com/login", 0);
        result = sceSystemServiceLaunchWebBrowser("https://discord.com/login", NULL);
        log_result("sceSystemServiceLaunchWebBrowser (not login status)", result);
        const int terminated = sceUserServiceTerminate();
        log_result("sceUserServiceTerminate", terminated);
        if (result == 0)
            result = terminated;
    }
    log_result("probe exit; browser rendering and login require manual observation", result);
    kill(watchdog, SIGKILL);
    while (waitpid(watchdog, NULL, 0) < 0 && errno == EINTR) {}
    return result == 0 ? 0 : 1;
}
