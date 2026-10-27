// ps5-discord - Voice audio on the console: the DualSense/headset mic and the
// socket to the core that carries mic PCM out and playback PCM back.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Two threads, neither ever waits on the other or on the game thread:
//   mic thread:  opens sceAudioIn while wanted, reads 256-frame grains, keeps
//                the level meter, pushes mono S16 into a ring. Opens and
//                closes the port itself (nothing else touches the handle).
//   link thread: owns the socket to 127.0.0.1:9042 (non-blocking). Sends mic
//                PCM from the ring, receives playback and writes it, as float,
//                into playback(), which the mixer reads at unity.
// The game thread only sets the wanted flags and reads the meter.

#pragma once

#include "audio/stream_ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace hui::ps5
{

class VoiceAudio
{
  public:
    static constexpr std::uint16_t kCorePort = 9042;
    static constexpr int kGrainFrames = 256; // as the probe: 48 kHz, S16 mono, grain 256

    VoiceAudio();
    VoiceAudio(const VoiceAudio &) = delete;
    VoiceAudio &operator=(const VoiceAudio &) = delete;
    ~VoiceAudio();

    // user: the foreground user the mic is opened for (pad.user()).
    void start(int user);
    void stop();

    // Called once a frame. link: the core says connected. mic: connected and
    // not muted. playback: connected and not deafened.
    void set_wanted(bool link, bool mic, bool playback)
    {
        want_link_.store(link, std::memory_order_relaxed);
        want_mic_.store(mic, std::memory_order_relaxed);
        want_playback_.store(playback, std::memory_order_relaxed);
    }
    // Our mic level, 0..1 (RMS of the last grain, 0 while the mic is closed).
    float mic_level() const
    {
        return level_.load(std::memory_order_relaxed);
    }
    // Playback the link fills; attach it to the mixer (Mixer::attach_voice).
    audio::StreamRing &playback()
    {
        return playback_;
    }

  private:
    // Mono S16 from the mic thread to the link thread. One producer, one consumer.
    class PcmRing
    {
      public:
        static constexpr std::size_t kSize = 1u << 14; // 340 ms at 48 kHz
        // Producer: keeps what fits, drops the rest.
        void push(const std::int16_t *in, std::size_t n)
        {
            const std::size_t head = head_.load(std::memory_order_relaxed);
            const std::size_t tail = tail_.load(std::memory_order_acquire);
            n = n < kSize - (head - tail) ? n : kSize - (head - tail);
            for (std::size_t i = 0; i < n; ++i)
                data_[(head + i) & (kSize - 1)] = in[i];
            head_.store(head + n, std::memory_order_release);
        }
        // Consumer: the samples there, oldest first.
        std::size_t pop(std::int16_t *out, std::size_t n)
        {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            const std::size_t head = head_.load(std::memory_order_acquire);
            n = n < head - tail ? n : head - tail;
            for (std::size_t i = 0; i < n; ++i)
                out[i] = data_[(tail + i) & (kSize - 1)];
            tail_.store(tail + n, std::memory_order_release);
            return n;
        }
        void clear()
        {
            tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        }

      private:
        std::int16_t data_[kSize]{};
        std::atomic<std::size_t> head_{0};
        std::atomic<std::size_t> tail_{0};
    };

    static void *mic_main(void *self);
    static void *link_main(void *self);
    void mic_run();
    void link_run();

    int user_ = -1;
    bool started_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> want_link_{false};
    std::atomic<bool> want_mic_{false};
    std::atomic<bool> want_playback_{false};
    std::atomic<float> level_{0.0f};
    PcmRing mic_ring_;
    audio::StreamRing playback_{1u << 14};
    pthread_t mic_thread_{};
    pthread_t link_thread_{};
};

} // namespace hui::ps5
