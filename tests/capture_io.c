// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int scripted, step;
static uint8_t written[64];
static size_t written_len;
static ssize_t test_send(int fd, const void *buf, size_t n, int flags)
{
    if (!scripted) return send(fd, buf, n, flags);
    int r = step++;
    if (r == 0 || r == 2) { errno = r == 0 ? EINTR : EAGAIN; return -1; }
    if (r == 1 && n > 3) n = 3;
    if (r == 3 && n > 2) n = 2;
    assert(written_len + n <= sizeof written);
    memcpy(written + written_len, buf, n); written_len += n;
    return (ssize_t)n;
}
#define send test_send
#include "../src/capture_io.h"
#undef send

static _Atomic int going = 1;
static int keep_going(void) { return going; }
static void *cancel(void *arg) { (void)arg; usleep(50000); going = 0; return NULL; }

int main(void)
{
    const uint8_t msg[] = "START---KEYFRAMESTOP----";
    scripted = 1;
    size_t offset = 0;
    while (!cap_write_step(-1, msg, sizeof msg, &offset)) {}
    assert(offset == sizeof msg && written_len == sizeof msg && !memcmp(msg, written, sizeof msg));
    scripted = 0;
    int fds[2], small = 1024;
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    assert(!cap_nonblock(fds[0]));
    assert(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small));
    uint8_t *data = calloc(1, 1024 * 1024);
    assert(data);
    uint64_t begin = cap_clock_ms();
    assert(cap_upload(fds[0], data, 1024 * 1024, begin + 100, keep_going) == -1);
    assert(cap_clock_ms() - begin < 500);
    pthread_t t;
    assert(!pthread_create(&t, NULL, cancel, NULL));
    begin = cap_clock_ms();
    assert(cap_upload(fds[0], data, 1024 * 1024, begin + 3000, keep_going) == -1);
    assert(cap_clock_ms() - begin < 500);
    pthread_join(t, NULL);
    going = 1;
    close(fds[1]);
    assert(cap_upload(fds[0], data, 16, cap_clock_ms() + 1000, keep_going) == -1);
    close(fds[0]);
    // The production connect/upload path against a loader that accepts but never drains.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, 1));
    socklen_t address_len = sizeof address;
    assert(!getsockname(listener, (struct sockaddr *)&address, &address_len));
    int upload_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(!setsockopt(upload_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small));
    assert(!cap_connect(upload_fd, (struct sockaddr *)&address, address_len, cap_clock_ms() + 500, keep_going));
    int loader_fd = accept(listener, NULL, NULL);
    assert(loader_fd >= 0);
    begin = cap_clock_ms();
    assert(cap_upload(upload_fd, data, 1024 * 1024, begin + 100, keep_going) == -1);
    assert(cap_clock_ms() - begin < 500);
    assert(!pthread_create(&t, NULL, cancel, NULL));
    begin = cap_clock_ms();
    assert(cap_upload(upload_fd, data, 1024 * 1024, begin + 3000, keep_going) == -1);
    assert(cap_clock_ms() - begin < 500);
    pthread_join(t, NULL); going = 1;
    close(loader_fd); close(upload_fd); close(listener);
    upload_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(cap_connect(upload_fd, (struct sockaddr *)&address, address_len, cap_clock_ms() + 500, keep_going) == -1);
    close(upload_fd); free(data);
    puts("PASS: short writes, EINTR/EAGAIN offsets, stalled upload deadline, cancellation, EOF");
    return 0;
}
