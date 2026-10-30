// SPDX-License-Identifier: AGPL-3.0-or-later
#include "core_key.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char key[CORE_KEY_LEN + 1];
static char path[256];
static _Atomic int ready; // the audio link's thread runs before main makes the key

static int write_key(void)
{
    char tmp[264];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    int ok = write(fd, key, CORE_KEY_LEN) == CORE_KEY_LEN;
    ok = close(fd) == 0 && ok;
    return ok && rename(tmp, path) == 0 ? 0 : -1;
}

int core_key_make(const char *dir, const unsigned char raw[32])
{
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", raw[i]);
    snprintf(path, sizeof path, "%s/control.key", dir);
    ready = 1;
    return write_key();
}

void core_key_keep(void)
{
    struct stat st;
    if (ready && stat(path, &st) != 0)
        write_key();
}

int core_key_ok(const char *s, size_t n)
{
    if (!ready || n != CORE_KEY_LEN)
        return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < CORE_KEY_LEN; i++)
        d |= (unsigned char)(s[i] ^ key[i]);
    return d == 0;
}
