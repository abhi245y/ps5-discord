// SPDX-License-Identifier: AGPL-3.0-or-later
// The AirPods' own microphone stream (after LibrePods, github.com/librepods-org/librepods
// PR #655): Apple's AACP channel on L2CAP PSM 0x1001, a handshake, then a "start audio"
// (0x58) message. The AirPods then send AAC-ELD frames (64 kHz mono, 480 samples, about
// 80 kbit/s) in 0x58 messages while A2DP keeps playing. Decoded with FFmpeg's AAC decoder
// and resampled to 48 kHz for the call. The 0x58 message is not documented by Apple: a
// firmware update can change it, so the stream is watched (silent()).
#pragma once

#include <cstdint>
#include <functional>

#include "core/bluetooth/bt_stack.hpp"
#include "resample.h"

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

class AacpMic
{
  public:
    // pcm: 48 kHz mono samples for the call, from the radio thread
    using Sink = std::function<void(const std::int16_t* pcm, int frames)>;
    AacpMic(bluev::BtStack& bt, Sink sink) : m_bt(bt), m_sink(std::move(sink))
    {
    }
    ~AacpMic();

    bool open(); // the channel and the handshake; false: not AirPods (or they refuse)
    void close();
    bool isOpen() const
    {
        return m_open;
    }
    void start(); // the mic stream
    void stop();
    bool running() const
    {
        return m_running;
    }
    // Seconds the stream has been running without a single frame (once a second).
    int silentSeconds();
    long frames() const
    {
        return m_frames;
    }
    long errors() const
    {
        return m_errors;
    }
    double decodeUs() const
    {
        return m_frames ? m_decodeUs / m_frames : 0;
    }

  private:
    void onFrame(const std::uint8_t* d, int n);
    void send(const std::uint8_t* d, int n);
    void decode(const std::uint8_t* au, int length);

    bluev::BtStack& m_bt;
    Sink m_sink;
    bluev::L2Channel m_ch{"aacp", 0x4a};
    bool m_open = false, m_running = false;
    int m_silent = 0;
    long m_framesAtTick = 0;
    ::AVCodecContext* m_ctx = nullptr;
    ::AVPacket* m_pkt = nullptr;
    ::AVFrame* m_frame = nullptr;
    rs_state m_rs{};
    long m_frames = 0, m_errors = 0;
    double m_decodeUs = 0;
};
