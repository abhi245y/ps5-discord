// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared by the loader upload and socket writer; all deadlines use CLOCK_MONOTONIC.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

static inline uint64_t cap_clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static inline int cap_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// One nonblocking write. Offset survives short writes, EINTR and backpressure.
static inline int cap_write_step(int fd, const uint8_t *bytes, size_t len, size_t *offset)
{
    ssize_t n = send(fd, bytes + *offset, len - *offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) *offset += (size_t)n;
    else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
    return *offset == len;
}

static inline int cap_wait_write(int fd, uint64_t deadline, int (*keep_going)(void))
{
    if (!keep_going() || cap_clock_ms() >= deadline) return -1;
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    int r = poll(&p, 1, 25);
    if (r < 0 && errno != EINTR) return -1;
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    return 0;
}

static inline int cap_connect(int fd, const struct sockaddr *address, socklen_t address_len,
                              uint64_t deadline, int (*keep_going)(void))
{
    if (cap_nonblock(fd) || !keep_going() || cap_clock_ms() >= deadline) return -1;
    if (!connect(fd, address, address_len)) return 0;
    if (errno != EINPROGRESS && errno != EINTR) return -1;
    for (;;) {
        if (cap_wait_write(fd, deadline, keep_going)) return -1;
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        if (poll(&pf, 1, 0) > 0) {
            int error = 0;
            socklen_t len = sizeof error;
            return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error ? -1 : 0;
        }
    }
}

static inline int cap_upload(int fd, const uint8_t *bytes, size_t len, uint64_t deadline, int (*keep_going)(void))
{
    size_t offset = 0;
    while (offset < len) {
        if (!keep_going() || cap_clock_ms() >= deadline) return -1;
        int r = cap_write_step(fd, bytes, len, &offset);
        if (r < 0) return -1;
        if (!r && cap_wait_write(fd, deadline, keep_going)) return -1;
    }
    return 0;
}
