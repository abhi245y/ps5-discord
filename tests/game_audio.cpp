// SPDX-License-Identifier: AGPL-3.0-or-later
// The shared game-sound owner (src/bt/game_audio.cpp) against a fake Avcap2 library: the real
// bluev::AvcapCapture sits between them, so what the console would see (Initialize ... Terminate)
// is what is counted.
#define GA_TEST 1
#include "../src/bt/game_audio.cpp"

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>

namespace bluev
{
void logLine(const char*, ...)
{
}
} // namespace bluev

using bluev::IAvcap2;

namespace
{
constexpr int kNothing = static_cast<int>(0x81950002u);

struct Fake final : IAvcap2
{
    std::atomic<int> inits{0}, terms{0}, opens{0}, closes{0}, starts{0}, stops{0};
    std::atomic<int> mode{0};        // 0 records, 1 read error, 2 nothing new
    std::atomic<int> failInits{0};   // Initialize fails this many times first
    std::atomic<int> initDelayMs{0}; // Initialize takes this long (a start in flight)
    std::atomic<unsigned> seq{0};
    std::atomic<long long> lastRead{0};
    int initialize() override
    {
        inits++;
        if (initDelayMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(initDelayMs.load()));
        }
        if (failInits > 0)
        {
            failInits--;
            return static_cast<int>(0x81950001u);
        }
        return 0;
    }
    int terminate() override
    {
        terms++;
        return 0;
    }
    int openAudio(void** handle) override
    {
        opens++;
        *handle = this;
        return 0;
    }
    int close(void*) override
    {
        closes++;
        return 0;
    }
    int start(void*) override
    {
        starts++;
        return 0;
    }
    int stop(void*) override
    {
        stops++;
        return 0;
    }
    int readAudio(void*, float* buffer, std::uint32_t maxBytes) override
    {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if (mode == 1)
        {
            return static_cast<int>(0x81950009u);
        }
        if (mode == 2)
        {
            return kNothing;
        }
        assert(maxBytes >= 480 * 8);
        const unsigned s = seq++;
        for (int i = 0; i < 480; i++)
        {
            buffer[2 * i] = static_cast<float>(s);
            buffer[2 * i + 1] = -static_cast<float>(s);
        }
        return 480 * 8;
    }
};

Options quick()
{
    Options o;
    o.backoffMs[0] = 20, o.backoffMs[1] = 40, o.backoffMs[2] = 80, o.backoffMs[3] = 160;
    o.stallMs = 300;
    o.firstMs = 100000; // off unless a test wants it
    o.verifyMs = 150;
    o.pollMs = 1;
    o.closeWaitMs = 2000;
    return o;
}

template <class F> bool until(F f, int ms = 3000)
{
    for (int i = 0; i < ms; i++)
    {
        if (f())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f();
}

// Reads what a consumer has until it has nothing: the first float of each record is its sequence.
std::vector<unsigned> drain(GameAudio& g, int id)
{
    std::vector<unsigned> seqs;
    static thread_local float buf[2048];
    for (int n; (n = g.read(id, buf, sizeof buf)) > 0;)
    {
        assert(n % 8 == 0);
        seqs.push_back(static_cast<unsigned>(buf[0]));
    }
    return seqs;
}

bool increasing(const std::vector<unsigned>& v)
{
    for (std::size_t i = 1; i < v.size(); i++)
    {
        if (v[i] != v[i - 1] + 1)
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    int cases = 0;

    // Headset and Go Live: one capture (one Initialize), both get the records in order; Go Live
    // letting go leaves the headset receiving; the last release terminates exactly once.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        assert(!g.live());
        const int headset = g.acquire(), golive = g.acquire();
        assert(headset >= 0 && golive >= 0 && headset != golive);
        assert(until([&] { return g.live(); }));
        assert(lib.inits == 1 && lib.opens == 1 && lib.starts == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto a = drain(g, headset), b = drain(g, golive);
        assert(a.size() > 10 && b.size() > 10 && increasing(a) && increasing(b));
        g.release(golive);
        assert(lib.terms == 0 && lib.closes == 0 && g.live()); // the headset still holds it
        drain(g, headset);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto c = drain(g, headset);
        assert(c.size() > 5 && increasing(c));
        assert(g.read(golive, nullptr, 0) == -1); // a released id reads nothing
        g.release(headset);
        assert(lib.terms == 1 && lib.closes == 1 && lib.stops == 1 && !g.live());
        g.release(headset); // twice is harmless
        assert(lib.terms == 1);
        cases++;
    }

    // The other order: the headset leaves first, Go Live keeps the capture and is the last out.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        const int golive = g.acquire(), headset = g.acquire();
        assert(until([&] { return g.live(); }));
        g.release(headset);
        assert(lib.terms == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(drain(g, golive).size() > 5);
        g.release(golive);
        assert(lib.terms == 1 && lib.inits == 1);
        // A new holder opens it again, cleanly.
        const int again = g.acquire();
        assert(until([&] { return g.live(); }) && lib.inits == 2);
        g.release(again);
        assert(lib.terms == 2);
        cases++;
    }

    // Reads fail while held: the capture is closed (one Terminate) and opened again by itself after
    // the backoff; reads say -1 meanwhile and records flow again afterwards.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        lib.mode = 1;
        assert(until([&] { return lib.terms >= 1; }));
        float buf[2048];
        assert(until([&] { return g.read(id, buf, sizeof buf) == -1; }));
        lib.mode = 0;
        assert(until([&] { return g.live() && lib.opens >= 2; }));
        assert(lib.inits == lib.terms + 1); // every open but the last was terminated
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(drain(g, id).size() > 3);
        g.release(id);
        assert(lib.inits == lib.terms);
        cases++;
    }

    // While it stays broken the reopen is paced by the backoff (not a spin), and a first open that
    // fails (the audio service is not ready yet) is retried too.
    {
        Fake lib;
        lib.failInits = 3;
        GameAudio g(lib, [] { return true; }, quick());
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        assert(lib.inits == 4 && lib.opens == 1); // 20 + 40 + 80 ms of waiting, three failures
        lib.mode = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(lib.inits <= 4 + 6); // at 20..160 ms apart, not thousands
        g.release(id);
        cases++;
    }

    // Nobody holds it: nothing is opened, and a broken source is left alone.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(lib.inits == 0);
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        g.release(id);
        const int before = lib.inits;
        lib.mode = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(lib.inits == before && lib.terms == 1 && !g.live());
        cases++;
    }

    // Bounded queues: a consumer that never reads loses its oldest records, the other one (and the
    // capture thread) are not held up by it.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        const int idle = g.acquire(), busy = g.acquire();
        assert(until([&] { return g.live(); }));
        std::vector<unsigned> seen;
        auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < stop)
        {
            auto s = drain(g, busy);
            seen.insert(seen.end(), s.begin(), s.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(seen.size() > 600 && increasing(seen)); // 480-frame records, nothing lost for the reader
        assert(g.dropped(idle) > 0 && g.dropped(busy) == 0);
        const auto kept = drain(g, idle);
        assert(kept.size() * 480 <= kQueueFrames + 480 && increasing(kept));
        assert(kept.back() + 100 > seen.back()); // what is kept is the newest
        g.release(idle);
        g.release(busy);
        cases++;
    }

    // Records stop arriving after they had been flowing: reopened. And after a consumer leaves
    // while another stays, a silent source is looked at again (verify), reopened once.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        lib.mode = 2;
        assert(until([&] { return lib.opens >= 2 || lib.terms >= 1; }, 2000)); // after stallMs 300
        lib.mode = 0;
        assert(until([&] { return g.live() && lib.opens >= 2; }));
        g.release(id);
        cases++;
    }
    {
        Fake lib;
        Options o = quick();
        o.stallMs = 100000; // only the verify can notice
        GameAudio g(lib, [] { return true; }, o);
        const int headset = g.acquire(), golive = g.acquire();
        assert(until([&] { return g.live(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        lib.mode = 2; // Go Live's session reset the capture: silence from here
        g.release(golive);
        assert(until([&] { return lib.terms >= 1; }, 2000));
        lib.mode = 0;
        assert(until([&] { return g.live() && lib.opens == 2; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(drain(g, headset).size() > 3);
        g.release(headset);
        cases++;
    }

    // Opened but silent from the start (Initialize, Open and Start succeed, no record ever comes):
    // reopened with the backoff, not left open for ever.
    {
        Fake lib;
        lib.mode = 2;
        Options o = quick();
        o.firstMs = 100;
        GameAudio g(lib, [] { return true; }, o);
        const int id = g.acquire();
        assert(until([&] { return lib.opens >= 3; }, 3000));
        assert(lib.terms >= 2 && lib.inits == lib.terms + (g.live() ? 1 : 0));
        lib.mode = 0; // and when sound comes it stays
        assert(until([&] { return g.live() && lib.opens >= 3; }));
        const int opens = lib.opens;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        assert(lib.opens == opens && drain(g, id).size() > 3);
        g.release(id);
        cases++;
    }

    // Starvation after a reopen: the source fails, comes back silent, and is reopened again.
    {
        Fake lib;
        Options o = quick();
        o.firstMs = 100;
        GameAudio g(lib, [] { return true; }, o);
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        lib.mode = 1;
        assert(until([&] { return lib.terms >= 1; }));
        lib.mode = 2; // reopens fine, delivers nothing
        assert(until([&] { return lib.opens >= 3; }, 3000));
        lib.mode = 0;
        assert(until([&] { return g.live(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(drain(g, id).size() > 2);
        g.release(id);
        cases++;
    }

    // A consumer leaves while another stays, and the capture never delivered anything: the check
    // after the release covers that too.
    {
        Fake lib;
        lib.mode = 2;
        Options o = quick(); // firstMs and stallMs far away: only the post-release check can notice
        o.stallMs = 100000;
        GameAudio g(lib, [] { return true; }, o);
        const int headset = g.acquire(), golive = g.acquire();
        assert(until([&] { return g.live(); }));
        g.release(golive);
        assert(until([&] { return lib.terms >= 1 && lib.opens >= 2; }, 2000));
        g.release(headset);
        cases++;
    }

    // A cut in the sound is reported: a consumer that fell behind (records dropped) is told, one that
    // kept up is not; a stale backlog is discarded when read; a reopened capture is a cut.
    {
        Fake lib;
        Options o = quick();
        o.stallMs = 100000; // isolate stale-queue behavior from the independent reopen deadline
        GameAudio g(lib, [] { return true; }, o);
        const int slow = g.acquire(), fast = g.acquire();
        assert(until([&] { return g.live(); }));
        float buf[2048];
        int disc = -1;
        auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        int fast_cuts = 0;
        while (std::chrono::steady_clock::now() < stop)
        {
            while (g.read(fast, buf, sizeof buf, 0, &disc) > 0)
            {
                fast_cuts += disc;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(fast_cuts == 0 && g.dropped(slow) > 0);
        assert(g.read(slow, buf, sizeof buf, 0, &disc) > 0 && disc == 1); // told once
        assert(g.read(slow, buf, sizeof buf, 0, &disc) > 0 && disc == 0);
        // A stall: the sound stops, the consumer wakes 300 ms later. Everything older than 100 ms is
        // gone when it reads (no stale sound passed off as current) and the next sound is a cut.
        const unsigned queued_after = lib.seq;
        assert(until([&] { return lib.seq >= queued_after + 8; }));
        lib.mode = 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const unsigned before = g.stale(fast);
        assert(g.read(fast, buf, sizeof buf, 100, &disc) == 0);
        assert(g.stale(fast) > before + 3);
        lib.mode = 0;
        int n = 0;
        assert(until([&] { return (n = g.read(fast, buf, sizeof buf, 100, &disc)) > 0; }));
        assert(disc == 1);
        {
            std::vector<unsigned> rest{static_cast<unsigned>(buf[0])};
            auto more = drain(g, fast);
            rest.insert(rest.end(), more.begin(), more.end());
            assert(increasing(rest));
        }
        // The capture fails and opens again: the next sound is a cut.
        drain(g, fast);
        lib.mode = 1;
        assert(until([&] { return lib.terms >= 1; }));
        lib.mode = 0;
        assert(until([&] { return g.live() && lib.opens >= 2; }));
        int cut = 0;
        assert(until([&] {
            int d = 0;
            const int r = g.read(fast, buf, sizeof buf, 0, &d);
            cut |= d;
            return r > 0 && cut;
        }));
        assert(cut == 1);
        g.release(slow);
        g.release(fast);
        cases++;
    }

    // The last release while a start is still in flight waits for it and for the cleanup:
    // Terminate has happened when release returns.
    {
        Fake lib;
        lib.initDelayMs = 300;
        GameAudio g(lib, [] { return true; }, quick());
        const int id = g.acquire();
        assert(until([&] { return lib.inits == 1; })); // Initialize is running
        g.release(id);
        assert(lib.terms == 1 && lib.closes == 1 && !g.live());
        // A new holder right after (teardown and reacquire racing) gets a clean open.
        lib.initDelayMs = 0;
        const int again = g.acquire();
        assert(until([&] { return g.live(); }));
        assert(lib.inits == lib.terms + 1);
        g.release(again);
        assert(lib.inits == lib.terms);
        cases++;
    }

    // Reads of any size: whole 8-byte frames, a record in pieces, nothing lost or repeated.
    {
        Fake lib;
        GameAudio g(lib, [] { return true; }, quick());
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        float buf[2048];
        assert(g.read(id, buf, 7) == 0); // less than a frame: nothing
        int got = g.read(id, buf, 100); // 12 frames
        assert(got == 96);
        const float first = buf[0];
        got = g.read(id, buf, 480 * 8); // the rest of that record: 468 frames
        assert(got == 468 * 8 && buf[0] == first);
        g.release(id);
        cases++;
    }

    // A library that cannot load yet: tried again, not given up on.
    {
        Fake lib;
        std::atomic<int> tries{0};
        GameAudio g(lib, [&] { return ++tries >= 3; }, quick());
        const int id = g.acquire();
        assert(until([&] { return g.live(); }));
        assert(tries == 3);
        g.release(id);
        cases++;
    }
    std::printf("PASS: game audio owner: %d cases (acquire/release order, one Terminate, reopen, bounded queues, verify)\n", cases);
    return 0;
}
