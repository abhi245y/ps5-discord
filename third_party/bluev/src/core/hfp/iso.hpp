// SPDX-License-Identifier: GPL-3.0-or-later
// HFP probe: the adapter's isochronous endpoints (interface 1), which carry
// the voice link's HCI SCO packets. Filled in by the platform's USB device;
// everything runs on the radio thread (the transfers complete inside poll()).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bluev
{

struct IsoPort
{
    std::function<bool()> start; // arm the reads; false when the endpoints are not open
    std::function<void()> stop;
    // bytes that came in on the in endpoint (the HCI SCO packet stream), at most `max`
    std::function<int(std::uint8_t* out, int max)> read;
    // queue bytes for the out endpoint (whole HCI SCO packets)
    std::function<bool(const std::uint8_t* data, int length)> write;
    std::function<std::string()> describe;
};

extern IsoPort g_iso;

} // namespace bluev
