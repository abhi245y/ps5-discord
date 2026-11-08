// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>

// Normalize existing progressive SPS VUI timing in an Annex-B buffer. Other NALs stay exact.
// Returns -1 for malformed/unsupported SPS, absent timing, HRD, or insufficient capacity.
int capture_h264_timing(uint8_t *annexb, size_t *len, size_t capacity, unsigned fps);
