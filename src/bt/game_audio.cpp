// SPDX-License-Identifier: AGPL-3.0-or-later
// The shared game-sound capture (game_audio.h). One reader thread owns bluev::AvcapCapture;
// consumers (the headset's A2DP stream, Go Live) get copies through bounded queues.
#include "game_audio.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/audio/avcap_capture.hpp"

#ifdef GA_TEST
#define GA_LOG(...) (void)0
#else
#include "platform/ps5/avcap_library.hpp"
extern "C" {
#include "log.h"
}
#define GA_LOG(...) LOG(__VA_ARGS__)
#endif

namespace
{

using Clock = std::chrono::steady_clock;
constexpr int kSlots = 4;
constexpr int kRecordBytes = 0x2000;       // the most one capture read returns
constexpr std::size_t kQueueFrames = 24000; // 500 ms per consumer; older records are dropped

struct Options
{
    int backoffMs[4] = {500, 1000, 2000, 5000};
    int stallMs = 5000;  // records had been flowing and stopped
    int verifyMs = 1000; // after a consumer let go while others stay: records must still arrive
    int pollMs = 3;
    int closeWaitMs = 3000;
};

class GameAudio
{
  public:
    // prepare: loads the capture library (true when ready; tried again on every open)
    GameAudio(bluev::IAvcap2& library, std::function<bool()> prepare, Options options = Options())
        : m_capture(library), m_prepare(std::move(prepare)), m_options(options), m_thread([this] { loop(); })
    {
    }
    ~GameAudio()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    int acquire()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < kSlots; i++)
        {
            if (!m_slot[i].used)
            {
                m_slot[i] = Slot();
                m_slot[i].used = true;
                m_holders++;
                m_wake.notify_all();
                return i;
            }
        }
        return -1;
    }

    void release(int id)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (id < 0 || id >= kSlots || !m_slot[id].used)
        {
            return;
        }
        m_slot[id] = Slot();
        m_holders--;
        if (m_holders > 0)
        {
            // someone stays: the capture must still be delivering after what this one did
            m_verifyAt = Clock::now() + std::chrono::milliseconds(m_options.verifyMs);
            m_verifySeen = m_records;
            return;
        }
        m_wake.notify_all();
        m_closed.wait_for(lock, std::chrono::milliseconds(m_options.closeWaitMs),
                          [this] { return !m_open || m_holders > 0 || m_quit; });
    }

    int read(int id, float* pcm, unsigned maxBytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (id < 0 || id >= kSlots || !m_slot[id].used)
        {
            return -1;
        }
        Slot& s = m_slot[id];
        if (s.queue.empty())
        {
            return m_open ? 0 : -1;
        }
        Record& r = s.queue.front();
        const std::size_t have = r.data.size() - r.offset;                 // floats
        const std::size_t take = std::min<std::size_t>(have, (maxBytes / 8) * 2); // whole frames
        if (take == 0)
        {
            return 0;
        }
        std::copy(r.data.begin() + static_cast<long>(r.offset), r.data.begin() + static_cast<long>(r.offset + take), pcm);
        r.offset += take;
        s.frames -= take / 2;
        if (r.offset >= r.data.size())
        {
            s.queue.pop_front();
        }
        return static_cast<int>(take * 4);
    }

    bool live()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_open;
    }

    // For the tests.
    unsigned dropped(int id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_slot[id].dropped;
    }
    unsigned opens()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_opens;
    }

  private:
    struct Record
    {
        std::vector<float> data;
        std::size_t offset = 0;
    };
    struct Slot
    {
        bool used = false;
        std::deque<Record> queue;
        std::size_t frames = 0;
        unsigned dropped = 0;
    };

    // m_mutex held: a copy of one capture record for every consumer; a slow one loses its oldest.
    void fanOut(const float* data, std::size_t floats)
    {
        for (Slot& s : m_slot)
        {
            if (!s.used)
            {
                continue;
            }
            s.queue.push_back({std::vector<float>(data, data + floats), 0});
            s.frames += floats / 2;
            while (s.frames > kQueueFrames && s.queue.size() > 1)
            {
                s.frames -= (s.queue.front().data.size() - s.queue.front().offset) / 2;
                s.queue.pop_front();
                s.dropped++;
            }
        }
    }

    // m_mutex held on entry and exit. The capture is down; try again after the next backoff.
    void failed(std::unique_lock<std::mutex>& lock, [[maybe_unused]] const char* why)
    {
        if (m_open)
        {
            lock.unlock();
            m_capture.close();
            lock.lock();
        }
        m_open = false;
        const int step = std::min(m_failures, 3);
        m_failures++;
        m_nextTry = Clock::now() + std::chrono::milliseconds(m_options.backoffMs[step]);
        GA_LOG("game audio: %s; opening again in %d ms", why, m_options.backoffMs[step]);
        m_closed.notify_all();
    }

    void loop()
    {
        std::vector<float> buffer(kRecordBytes / 4);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_quit)
        {
            if (m_holders == 0)
            {
                if (m_open)
                {
                    lock.unlock();
                    m_capture.close(); // the one Terminate, when the last holder let go
                    lock.lock();
                    m_open = false;
                    GA_LOG("game audio: closed, nobody needs it");
                }
                m_failures = 0;
                m_nextTry = Clock::time_point();
                m_closed.notify_all();
                m_wake.wait(lock, [this] { return m_quit || m_holders > 0; });
                continue;
            }
            if (!m_open)
            {
                if (Clock::now() < m_nextTry)
                {
                    m_wake.wait_until(lock, m_nextTry);
                    continue;
                }
                lock.unlock();
                const bool ready = m_loaded || (m_loaded = m_prepare());
                const bool ok = ready && m_capture.start();
                lock.lock();
                if (!ok)
                {
                    m_open = false;
                    const int step = std::min(m_failures, 3);
                    m_failures++;
                    m_nextTry = Clock::now() + std::chrono::milliseconds(m_options.backoffMs[step]);
                    if (m_failures == 1 || m_failures % 10 == 0)
                    {
                        GA_LOG("game audio: could not open (%d); trying again", m_failures);
                    }
                    continue;
                }
                m_open = true;
                m_opens++;
                m_flowed = false;
                m_lastRecord = Clock::now();
                GA_LOG("game audio: open");
                continue;
            }
            lock.unlock();
            const int n = m_capture.read(buffer.data(), kRecordBytes);
            lock.lock();
            const auto now = Clock::now();
            if (n >= 8)
            {
                fanOut(buffer.data(), static_cast<std::size_t>(n) / 8 * 2);
                m_records++;
                if (!m_flowed)
                {
                    m_failures = 0; // sound arrives: the backoff starts over only now
                }
                m_flowed = true;
                m_lastRecord = now;
                continue; // drain: more may be waiting
            }
            if (n < 0)
            {
                failed(lock, "reads failed");
                continue;
            }
            if (m_flowed && now - m_lastRecord > std::chrono::milliseconds(m_options.stallMs))
            {
                failed(lock, "no sound arrives any more");
                continue;
            }
            if (m_verifyAt != Clock::time_point() && now >= m_verifyAt)
            {
                m_verifyAt = Clock::time_point();
                if (m_flowed && m_records == m_verifySeen)
                {
                    failed(lock, "no sound after a consumer left");
                    continue;
                }
            }
            m_wake.wait_for(lock, std::chrono::milliseconds(m_options.pollMs));
        }
        if (m_open)
        {
            lock.unlock();
            m_capture.close();
            lock.lock();
            m_open = false;
        }
    }

    bluev::AvcapCapture m_capture;
    std::function<bool()> m_prepare;
    Options m_options;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_closed;
    Slot m_slot[kSlots];
    int m_holders = 0, m_failures = 0;
    bool m_open = false, m_quit = false, m_loaded = false, m_flowed = false;
    unsigned m_opens = 0;
    unsigned long m_records = 0, m_verifySeen = 0;
    Clock::time_point m_nextTry{}, m_lastRecord{}, m_verifyAt{};
    std::thread m_thread; // last: it starts running the moment the members above exist
};

} // namespace

#ifndef GA_TEST
namespace
{
GameAudio& owner()
{
    static bluev::AvcapLibrary library;
    static GameAudio game(library, [] { return library.load(); });
    return game;
}
} // namespace

extern "C" {
int ga_acquire(void)
{
    return owner().acquire();
}
void ga_release(int id)
{
    owner().release(id);
}
int ga_read(int id, float* pcm, unsigned max_bytes)
{
    return owner().read(id, pcm, max_bytes);
}
int ga_live(void)
{
    return owner().live() ? 1 : 0;
}
}
#endif
