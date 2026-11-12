// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ds5_usb_owner.h"
#include "ds5_usb.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mark(const char *path, const char *name, long pid)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%s %ld %lld\n", name, pid, (long long)time(NULL)) > 0;
    if (fflush(f) || fsync(fileno(f)))
        ok = 0;
    if (fclose(f))
        ok = 0;
    return ok ? 0 : -1;
}

int ds5_usb_owner_try(ds5_usb_owner *owner, const char *directory, const char *name,
                      void (*log)(void *, const char *), void *user)
{
    char path[512];
    if (owner->fd >= 0)
        return 1;
    if (mkdir(directory, 0755) && errno != EEXIST)
        return -1;
    if (snprintf(path, sizeof path, "%s/%s", directory, DS5_USB_LOCK) >= (int)sizeof path ||
        snprintf(owner->marker, sizeof owner->marker, "%s/%s", directory, DS5_USB_TAKEN) >=
            (int)sizeof owner->marker)
        return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int busy = errno == EWOULDBLOCK || errno == EAGAIN;
        close(fd);
        return busy ? 1 : -1;
    }
    int recover = 0;
    FILE *f = fopen(owner->marker, "r");
    if (f) {
        char previous[64];
        long pid = 0;
        long long timestamp;
        int valid = fscanf(f, "%63s %ld %lld", previous, &pid, &timestamp) == 3;
        fclose(f);
        // Conservative on PID reuse or a live process that lost its lock unexpectedly.
        if (valid && pid > 0 && pid <= INT_MAX && (kill((pid_t)pid, 0) == 0 || errno != ESRCH)) {
            close(fd);
            return 1;
        }
        recover = 1;
    } else if (errno != ENOENT) {
        close(fd);
        return -1;
    }
    // Durable before recovery OR takeover can touch the device.
    if (mark(owner->marker, name, (long)getpid())) {
        (void)mark(owner->marker, "recovery_needed", 0);
        close(fd);
        return -1;
    }
    if (recover) {
        if (log)
            log(user, "stale USB taken marker: recovering controller before takeover");
        if (ds5_usb_recover() != DS5_USB_OK) {
            (void)mark(owner->marker, "recovery_needed", 0);
            if (log)
                log(user, "USB recovery failed; taken marker retained");
            close(fd);
            return -1;
        }
        if (log)
            log(user, "USB recovery handback verified");
    }
    owner->fd = fd;
    return 0;
}

int ds5_usb_owner_release(ds5_usb_owner *owner, int verified)
{
    if (owner->fd < 0)
        return 0;
    int rc = verified ? unlink(owner->marker) : mark(owner->marker, "recovery_needed", 0);
    // Never unlink the lock inode. A crash leaves the marker for the next lock holder.
    close(owner->fd);
    owner->fd = -1;
    return rc;
}

int ds5_usb_owner_busy(const char *directory)
{
    char path[512];
    if (snprintf(path, sizeof path, "%s/%s", directory, DS5_USB_LOCK) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return errno != ENOENT;
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return busy;
}
