// ps5-discord - The app's side of the core: a background thread that talks to
// the core payload over loopback HTTP and builds the snapshot screens draw.
// SPDX-License-Identifier: AGPL-3.0-or-later
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
inline constexpr int kApiVersionMin = 5;
inline constexpr int kApiVersionMax = 5;

// A Discord image fetched through the core, decoded to RGBA (w 0: it failed).
struct Image
{
    std::string url;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
    int frames = 1, columns = 1; // an animated sticker: a sheet of frames (discord/stickers.hpp)
    float fps = 0.0f;
};

// A key from the console's keyboard, as the core read it (USB HID usage code,
// modifier bits, lock lights).
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
    // Development: raises a notice as if a DM had arrived (the toast's path, from the PC).
    void test_notice(const std::string &text);
    // The app is the one on screen (not minimised to the home screen, not under the PS
    // menu). The core shows the PS5's own alert while it is not.
    void set_front(bool front);
    bool front() const;
    // The first frame was presented: requests say so (the core's proof that a new build runs).
    void set_drawn();
    bool quit_wanted() const { return quit_; }

  private:
    void run();
    void poll_state();
    void poll_chat();
    void poll_voice();
    bool poll_events(); // false: the core did not answer
    void poll_mentions();
    void handle(const Request &request);
    void fetch_images();
    void run_images();
    void run_events();
    void publish(); // marks the snapshot changed (lock held by caller)

    const int port_;
    pthread_t thread_{};
    // Download images apart from polling, so neither waits; several, as each picture is
    // a TLS fetch of its own in the core (a picker full of server emoji).
    pthread_t image_threads_[4]{};
    pthread_t events_thread_{}; // holds a long poll open: messages arrive as they are sent
    bool events_started_ = false;
    bool started_ = false;
    int images_started_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    Snapshot snapshot_;    // guarded by mutex_
    std::uint64_t version_ = 0;
    std::uint64_t taken_ = 0;
    std::deque<Request> queue_;
    Request open_; // the last open_channel: the chat on screen (guarded by mutex_)
    std::atomic<unsigned> account_{0}; // grows when the account's data is reset
    std::deque<std::string> image_queue_; // guarded by mutex_
    std::vector<Image> images_;           // guarded by mutex_
    std::atomic<long> events_next_{-1}; // -1: skip the backlog on first contact
    std::atomic<std::int64_t> last_chat_ms_{0}; // zeroed for a refresh now
    std::atomic<bool> chat_dirty_{false};        // an event changed the tree: refresh within a second
    // The tree's version the snapshot was built from (/api/chat?v=, 304 while it holds); -1
    // whenever the snapshot's account part was dropped, so the next poll gets it whole.
    std::atomic<long long> chat_version_{-1};
    std::int64_t last_mentions_ms_ = 0;
    std::int64_t last_voice_ms_ = 0; // a voice command zeroes it: the state comes back at once
    std::uint64_t notice_seq_ = 0; // Notice::seq, under mutex_
    std::atomic<bool> quit_{false}; // a quit request went through: the app closes
    std::string browser_error_; // client thread only; retained across state polls
    int state_failures_ = 0;   // /api/state polls in a row the core did not answer (client thread)
    bool been_ready_ = false;  // signed in and connected once this launch (same account)
    bool core_seen_ = false;   // it answered once: from then on a missed poll may be only slow
    std::string start_error_;     // why /api/update/start was refused (client thread)
    bool update_checked_ = false; // the launch-time check was queued (once)
    bool replaced_core_ = false; // an older running core was told to quit (once per launch)
    std::string replaced_version_; // the version it reported (client thread)
    std::int64_t replaced_ms_ = 0;
    bool voice_known_ = false;   // /api/voice answered once: "not in a call" is the core's word
    bool prefetched_ = false; // recent conversations' history asked for once after sign-in
    std::atomic<bool> ready_{false}; // read by the image thread too
};

// Starts the bundled core if nothing answers on its port. For main(), before the display and
// fonts open: a half-updated folder whose UI cannot load must still start the core that finishes
// the swap.
void ensure_core();

} // namespace discord
