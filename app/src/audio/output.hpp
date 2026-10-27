// spot5 - Foreground and experimental Sony custom-music audio sinks.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class AudioBackend { main, custom_music };

// Owned by one Player. Open before starting its output thread; close after join.
// Custom music is an experiment, not a grant of background-process privileges.
class AudioOutput
{
  public:
    static constexpr std::size_t kMaxFrames = 1024;
    AudioOutput() = default;
    ~AudioOutput();
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    bool open(AudioBackend backend);
    bool is_open() const { return handle_ >= 0 || custom_initialized_; }
    std::size_t grain_frames() const { return backend_ == AudioBackend::custom_music ? 1024 : 256; }
    // Exactly grain_frames() interleaved stereo frames; caller alternates
    // aligned buffers because custom wrapper consumption timing is unverified.
    int write(const std::int16_t *stereo);
    int drain();
    int close();
    int last_error() const { return last_error_; }
    static const char *name(AudioBackend backend);

  private:
    bool open_custom();
    void release_modules();
    AudioBackend backend_ = AudioBackend::main;
    int handle_ = -1, last_error_ = 0;
    bool custom_initialized_ = false;
    // Official core loads these in this order; release only our references.
    static constexpr std::array<std::uint16_t, 3> kModules{0x123, 0x121, 0x122};
    std::array<bool, 3> owned_modules_{};
    int (*custom_initialize_)(int, int, int) = nullptr;
    int (*custom_output_)(const void *) = nullptr;
    int (*custom_finalize_)() = nullptr;
};
