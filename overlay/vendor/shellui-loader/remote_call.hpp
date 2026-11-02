/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <sys/types.h>

namespace common_fps::ps5 {

/* Call the short bootstrap on a fresh 128KiB remote stack. Verify its
 * exact INT3, then restore the original GPR/FP state. No live target stack
 * bytes are borrowed. This local hardening remains console-untested. */
[[nodiscard]] long call_until_breakpoint_on_private_stack(
    pid_t pid,
    std::uintptr_t address,
    std::uint64_t arg0 = 0,
    std::uint64_t arg1 = 0,
    std::uint64_t arg2 = 0,
    std::uint64_t arg3 = 0,
    std::uint64_t arg4 = 0,
    std::uint64_t arg5 = 0,
    std::uintptr_t expected_trap = 0) noexcept;

} // namespace common_fps::ps5
