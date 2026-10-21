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
#include <vector>
#include <pthread.h>

#include "discord/model.hpp"
#include "discord/request.hpp"

namespace discord
{

// The core API versions this app understands (ROADMAP.md, "Core API version").
inline constexpr int kApiVersionMin = 1;
inline constexpr int kApiVersionMax = 1;

// A Discord image fetched through the core, decoded to RGBA (w 0: it failed).
struct Image
{
    std::string url;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
};

// A key from the console's keyboard, as the core read it (USB HID usage code,
// modifier bits, lock lights).
struct KeyPress
{
    std::uint16_t usage = 0;
    std::uint32_t mods = 0;
    std::uint32_t led = 0;
};

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
    // Queues a CDN image (avatar, icon) for download; take_images hands back the results.
    void want_image(const std::string &url);
    std::vector<Image> take_images();
    // Keys typed on the console's keyboard since the last call, oldest first.
    std::vector<KeyPress> take_keys();
    // Development: raises a notice as if a DM had arrived (the toast's path, from the PC).
    void test_notice(const std::string &text);

  private:
    void run();
    void poll_state();
    void poll_chat();
    bool poll_events(); // false: the core did not answer
    void poll_mentions();
    void handle(const Request &request);
    void fetch_images();
    void run_images();
    void run_events();
    void publish(); // marks the snapshot changed (lock held by caller)

    const int port_;
    pthread_t thread_{};
    pthread_t image_thread_{}; // downloads images apart from polling, so neither waits
    pthread_t events_thread_{}; // holds a long poll open: messages arrive as they are sent
    bool events_started_ = false;
    bool started_ = false;
    bool image_started_ = false;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    Snapshot snapshot_;    // guarded by mutex_
    std::uint64_t version_ = 0;
    std::uint64_t taken_ = 0;
    std::deque<Request> queue_;
    std::deque<std::string> image_queue_; // guarded by mutex_
    std::vector<Image> images_;           // guarded by mutex_
    std::vector<KeyPress> keys_;          // guarded by mutex_
    long events_next_ = -1; // -1: skip the backlog on first contact
    std::atomic<std::int64_t> last_chat_ms_{0}; // the events thread zeroes it to ask for a refresh
    std::int64_t last_mentions_ms_ = 0;
    std::string browser_error_; // client thread only; retained across state polls
    bool prefetched_ = false; // recent conversations' history asked for once after sign-in
    std::atomic<bool> ready_{false}; // read by the image thread too
};

} // namespace discord
