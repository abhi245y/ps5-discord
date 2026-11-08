// SPDX-License-Identifier: GPL-3.0-only
#include "../src/h264_timing.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = argc == 4 ? argv[1] : "../tests/data/capture-720p30.h264";
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); rewind(f);
    assert(size > 0 && size < (32 << 20));
    size_t n = (size_t)size, capacity = n + 1024;
    uint8_t *bytes = malloc(capacity), *copy = malloc(capacity);
    assert(bytes && copy && fread(bytes, 1, n, f) == n); fclose(f);
    memcpy(copy, bytes, n);
    unsigned fps = argc == 4 ? (unsigned)atoi(argv[3]) : 30;
    assert(!capture_h264_timing(bytes, &n, capacity, fps));
    if (argc == 4) {
        f = fopen(argv[2], "wb"); assert(f && fwrite(bytes, 1, n, f) == n); fclose(f);
    } else {
        // Only SPS can change; every byte beginning at PPS remains identical.
        size_t sps_end = 4;
        while (sps_end + 4 < (size_t)size && memcmp(copy + sps_end, "\0\0\0\1", 4)) sps_end++;
        size_t new_end = 4;
        while (new_end + 4 < n && memcmp(bytes + new_end, "\0\0\0\1", 4)) new_end++;
        assert(n - new_end == (size_t)size - sps_end);
        assert(!memcmp(bytes + new_end, copy + sps_end, n - new_end));
        size_t once = n;
        assert(!capture_h264_timing(bytes, &n, capacity, fps) && n == once);
        // Invalid rate, no headroom and truncated/malformed SPS never overrun or succeed.
        assert(capture_h264_timing(copy, &once, capacity, 120) == -1);
        for (size_t len = 5; len < sps_end; len++) {
            memcpy(bytes, copy, len);
            size_t got = len;
            assert(capture_h264_timing(bytes, &got, len, fps) == -1);
        }
        uint32_t random = 12345;
        for (unsigned i = 0; i < 2000; i++) {
            size_t len = 5 + i % 250;
            memcpy(bytes, "\0\0\0\1\x67", 5);
            for (size_t j = 5; j < len; j++) { random = random * 1664525u + 1013904223u; bytes[j] = random >> 24; }
            (void)capture_h264_timing(bytes, &len, capacity, 30);
        }
        // Real SPS/PPS from the separately decoded HDR-TV 1080p60 recording.
        f = fopen("../tests/data/capture-1080p60-headers.h264", "rb"); assert(f);
        size_t headers = fread(bytes, 1, capacity, f); fclose(f);
        assert(headers > 20 && !capture_h264_timing(bytes, &headers, capacity, 60));
        puts("PASS: 720p30/1080p60 SPS timing, payload preservation, idempotence, truncated and malformed SPS");
    }
    free(bytes); free(copy); return 0;
}
