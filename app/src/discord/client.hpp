// ps5-discord - The app's side of the core: a background thread that talks to
// the core payload over loopback HTTP and builds the snapshot screens draw.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The core (core payload, ROADMAP.md) owns the Discord session; the app never
// sees the token or TLS. This client polls /api/state (sign-in, connection),
// /api/chat (account, presence, voice, read state), follows /api/events
// (messages), loads history and sends on request.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <pthread.h>

#include "discord/model.hpp"
#include "discord/request.hpp"

namespace discord
{

// The core API versions this app understands (ROADMAP.md, "Core API version").
inline constexpr int kApiVersionMin = 1;
inline constexpr int kApiVersionMax = 1;

class Client
{
  public:
    explicit Client(int port = 9041) : port_(port) {}
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void start();
    // Queues a screen's request for the client thread.
    void submit(const Request &request);
    // Copies the latest snapshot into out when it changed since the last call.
    bool take(Snapshot &out);

  private:
    void run();
    void poll_state();
    void poll_chat();
    void poll_events();
    void poll_mentions();
    void handle(const Request &request);
    void publish(); // marks the snapshot changed (lock held by caller)

    const int port_;
    pthread_t thread_{};
    bool started_ = false;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    Snapshot snapshot_;    // guarded by mutex_
    std::uint64_t version_ = 0;
    std::uint64_t taken_ = 0;
    std::deque<Request> queue_;
    long events_next_ = -1; // -1: skip the backlog on first contact
    std::int64_t last_chat_ms_ = 0;
    std::int64_t last_mentions_ms_ = 0;
    bool ready_ = false;
};

} // namespace discord
