// SPDX-License-Identifier: AGPL-3.0-or-later
// Real filesystem/flock/process lifetimes; USB recovery is stubbed, no hardware I/O.
#include "ds5_usb_owner.h"
#include "ds5_usb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *test_directory;
static int recoveries, recovery_result, recovery_logs;

int ds5_usb_recover(void)
{
    ++recoveries;
    // Recovery must run under the lock, with the recovering owner's durable marker present.
    assert(ds5_usb_owner_busy(test_directory));
    char path[512];
    snprintf(path, sizeof path, "%s/%s", test_directory, DS5_USB_TAKEN);
    assert(access(path, F_OK) == 0);
    FILE *f = fopen(path, "r");
    char name[64];
    long pid;
    long long timestamp;
    assert(f && fscanf(f, "%63s %ld %lld", name, &pid, &timestamp) == 3);
    assert(fclose(f) == 0 && pid == (long)getpid() && timestamp > 0);
    return recovery_result;
}

static void owner_log(void *user, const char *message)
{
    (void)user;
    if (strstr(message, "stale USB taken marker"))
        ++recovery_logs;
}

static void child_status(pid_t child)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void usb_owner_tests(void)
{
    char directory[] = "/tmp/ds5-owner-test-XXXXXX";
    assert(mkdtemp(directory));
    test_directory = directory;
    recoveries = recovery_logs = 0;
    recovery_result = DS5_USB_OK;
    char marker[512], lock[512];
    snprintf(marker, sizeof marker, "%s/%s", directory, DS5_USB_TAKEN);
    snprintf(lock, sizeof lock, "%s/%s", directory, DS5_USB_LOCK);
    ds5_usb_owner owner = DS5_USB_OWNER_INIT, competitor = DS5_USB_OWNER_INIT;

    assert(ds5_usb_owner_try(&owner, directory, "controller", owner_log, NULL) == 0);
    FILE *f = fopen(marker, "r");
    char name[64];
    long pid;
    long long timestamp;
    assert(f && fscanf(f, "%63s %ld %lld", name, &pid, &timestamp) == 3);
    fclose(f);
    assert(!strcmp(name, "controller") && pid == (long)getpid() && timestamp > 0);
    assert(ds5_usb_owner_try(&competitor, directory, "remote_play", owner_log, NULL) == 1);
    assert(!recoveries);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        ds5_usb_owner other = DS5_USB_OWNER_INIT;
        assert(ds5_usb_owner_try(&other, directory, "remote_play", owner_log, NULL) == 1);
        _exit(0);
    }
    child_status(child);
    assert(ds5_usb_owner_release(&owner, 1) == 0);
    assert(access(marker, F_OK) != 0 && !ds5_usb_owner_busy(directory));
    assert(access(lock, F_OK) == 0); // Lock inode never removed by release.

    // Crash: the kernel closes flock, while the durable taken marker survives.
    child = fork();
    assert(child >= 0);
    if (!child) {
        ds5_usb_owner other = DS5_USB_OWNER_INIT;
        assert(ds5_usb_owner_try(&other, directory, "controller", owner_log, NULL) == 0);
        _exit(0); // Deliberately no owner_release.
    }
    child_status(child);
    assert(!ds5_usb_owner_busy(directory) && access(marker, F_OK) == 0);
    assert(ds5_usb_owner_try(&owner, directory, "remote_play", owner_log, NULL) == 0);
    assert(recoveries == 1 && recovery_logs == 1);

    // Failed handback must retain the marker, even with the owner process still alive.
    assert(ds5_usb_owner_release(&owner, 0) == 0);
    assert(access(marker, F_OK) == 0);
    recovery_result = DS5_USB_ERROR;
    assert(ds5_usb_owner_try(&owner, directory, "controller", owner_log, NULL) == -1);
    assert(recoveries == 2 && !ds5_usb_owner_busy(directory));
    assert(access(marker, F_OK) == 0);
    recovery_result = DS5_USB_OK;
    assert(ds5_usb_owner_try(&owner, directory, "controller", owner_log, NULL) == 0);
    assert(recoveries == 3);
    assert(ds5_usb_owner_release(&owner, 1) == 0);
    assert(access(marker, F_OK) != 0);

    // A live PID in a marker without flock is refused conservatively, not recovered.
    f = fopen(marker, "w");
    assert(f && fprintf(f, "controller %ld 1\n", (long)getpid()) > 0);
    assert(fclose(f) == 0);
    assert(ds5_usb_owner_try(&owner, directory, "remote_play", owner_log, NULL) == 1);
    assert(recoveries == 3);
    assert(unlink(marker) == 0);

    // An interrupted/partial marker still requires recovery.
    f = fopen(marker, "w");
    assert(f && fclose(f) == 0);
    assert(ds5_usb_owner_try(&owner, directory, "controller", owner_log, NULL) == 0);
    assert(recoveries == 4);
    assert(ds5_usb_owner_release(&owner, 1) == 0);
    assert(unlink(lock) == 0 && rmdir(directory) == 0);
    test_directory = NULL;
    puts("USB ownership: cross-process exclusion, crash recovery, live marker and verified handback passed");
}

#ifdef USB_OWNER_TEST_MAIN
int main(void)
{
    usb_owner_tests();
    return 0;
}
#endif
