// Tests for the even-step follower of puppets and townspeople (lichii/hook/coop_follow.h, jitter-b).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "coop_follow.h"
#include "coop_frame_clock.h"

using CoopFollow::Follower;
using CoopFollow::Vec3;

namespace {

constexpr float kSpeed = 6.0f;  // a run, metres a second

// Where a runner along x is at `ms`.
Vec3 RunnerAt(std::uint64_t ms) {
    return {kSpeed * static_cast<float>(ms) / 1000.0f, 0.0f, 0.0f};
}

}  // namespace

TEST_CASE("follow: steady running is drawn in even steps although the place is worked out at uneven times") {
    // The place is worked out at the last present before each game frame: 0 to 16 ms before it,
    // changing from frame to frame (the 0.11 jitter). Drawn at 33 ms game frames.
    Follower f;
    const std::uint64_t made_before[] = {0, 16, 3, 12, 16, 0, 8, 15, 1, 10};
    std::vector<float> steps;
    float last = 0.0f;
    for (int i = 0; i < 40; ++i) {
        const std::uint64_t now = 1000 + 33 * static_cast<std::uint64_t>(i);
        const std::uint64_t made = now - made_before[i % 10];
        const Vec3 drawn = f.Step(RunnerAt(made), {kSpeed, 0.0f, 0.0f}, made, now);
        if (i > 0) {
            steps.push_back(drawn[0] - last);
        }
        last = drawn[0];
        // No lag while running: drawn where the runner is at this frame.
        CHECK(drawn[0] == doctest::Approx(RunnerAt(now)[0]).epsilon(0.001));
    }
    const auto [lo, hi] = std::minmax_element(steps.begin(), steps.end());
    CHECK(*hi - *lo < 0.01f);  // even steps (raw: 0.1 to 0.29 m)
}

TEST_CASE("follow: no overshoot and no lag on a stop") {
    Follower f;
    std::uint64_t now = 1000;
    for (int i = 0; i < 10; ++i, now += 33) {
        f.Step(RunnerAt(now), {kSpeed, 0.0f, 0.0f}, now, now);
    }
    // They stop dead: the place stays, velocity 0.
    const Vec3 stop = RunnerAt(now - 33);
    // Behind it by a late sample: the follower closes the gap without passing it.
    Vec3 drawn{};
    int frames = 0;
    for (; frames < 10; ++frames, now += 33) {
        drawn = f.Step(stop, {0.0f, 0.0f, 0.0f}, now, now);
        CHECK(drawn[0] <= stop[0] + 1e-5f);
        if (drawn == stop) {
            break;
        }
    }
    CHECK(drawn == stop);  // exactly there
    CHECK(frames <= 2);

    // From behind (a gap left by a late sample), still: settles on it in a few frames, never past it.
    Follower g;
    now = 5000;
    g.Step({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, now, now);
    const Vec3 to{0.5f, 0.0f, 0.0f};
    frames = 0;
    for (now += 33; frames < 20; ++frames, now += 33) {
        drawn = g.Step(to, {0.0f, 0.0f, 0.0f}, now, now);
        CHECK(drawn[0] <= to[0]);
        if (drawn == to) {
            break;
        }
    }
    CHECK(drawn == to);
    CHECK(frames <= 8);  // under 0.3 s
}

TEST_CASE("follow: velocity carrying it past the place is cut at the place") {
    Follower f;
    f.Step({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1000, 1000);
    // 2 cm ahead, but a velocity that would carry it 20 cm in a frame.
    const Vec3 drawn = f.Step({0.02f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, 1033, 1033);
    CHECK(drawn[0] <= 0.02f + 1e-6f);
}

TEST_CASE("follow: a big jump or teleport is taken at once") {
    Follower f;
    f.Step({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1000, 1000);
    const Vec3 far{30.0f, 2.0f, -4.0f};
    CHECK(f.Step(far, {0.0f, 0.0f, 0.0f}, 1033, 1033) == far);
    const Vec3 near_jump{31.0f, 2.0f, -4.0f};  // past kJumpM: the caller eases it (coop_snap.h)
    CHECK(f.Step(near_jump, {0.0f, 0.0f, 0.0f}, 1066, 1066) == near_jump);
}

TEST_CASE("follow: ahead is capped both ways, bad input passes through") {
    Follower f;
    // Worked out 500 ms ago (a stall): taken forward by at most kAheadMaxMs.
    const Vec3 first = f.Step({0.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, 1000, 1500);
    CHECK(first[0] == doctest::Approx(6.0f * CoopFollow::kAheadMaxMs / 1000.0f));
    Follower g;
    const Vec3 at{1.0f, 0.0f, 0.0f};
    // Made after now (an even Cadence time runs a little behind): taken back by the velocity.
    const Vec3 back = g.Step(at, {6.0f, 0.0f, 0.0f}, 2000, 1990);
    CHECK(back[0] == doctest::Approx(0.94f));
    CHECK(g.Step(at, {6.0f, 0.0f, 0.0f}, 0, 1990) == back);  // same frame twice: unchanged
    Follower h;
    const Vec3 far = h.Step({0.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, 2000, 1000);
    CHECK(far[0] == doctest::Approx(-6.0f * CoopFollow::kAheadMaxMs / 1000.0f));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Vec3 bad{nan, 0.0f, 0.0f};
    CHECK(std::isnan(g.Step(bad, {0.0f, 0.0f, 0.0f}, 0, 3000)[0]));
    CHECK_FALSE(g.Drawn().has_value());
}

TEST_CASE("frame clock: age of the newest game frame") {
    CHECK(CoopFrameClock::Age(1'000'000, 0) == 0);           // no game frame seen (the emulator)
    CHECK(CoopFrameClock::Age(1'000'000, 990'000) == 10'000);
    CHECK(CoopFrameClock::Age(1'000'000, 1'000'500) == 0);   // noted after now
    CHECK(CoopFrameClock::Age(1'000'000, 1'000'000 - CoopFrameClock::kFreshUs - 1) == 0);  // a pause
}

// review-jitter: even steps at 60 fps (16 ms frames, a place per present) and at 10 fps (100 ms
// frames, each place worked out at one of the six presents before the frame).
TEST_CASE("follow: even steps at 60 fps and at 10 fps") {
    for (const std::uint64_t frame_ms : {16ull, 100ull}) {
        CAPTURE(frame_ms);
        Follower f;
        float last = 0.0f;
        std::vector<float> steps;
        for (int i = 0; i < 30; ++i) {
            const std::uint64_t now = 1000 + frame_ms * static_cast<std::uint64_t>(i);
            const std::uint64_t made = now - (frame_ms == 16 ? (i % 2) * 8 : (i * 37) % 95);
            const Vec3 drawn = f.Step(RunnerAt(made), {kSpeed, 0.0f, 0.0f}, made, now);
            CHECK(drawn[0] == doctest::Approx(RunnerAt(now)[0]).epsilon(0.001));
            if (i > 0) {
                steps.push_back(drawn[0] - last);
            }
            last = drawn[0];
        }
        const auto [lo, hi] = std::minmax_element(steps.begin(), steps.end());
        CHECK(*hi - *lo < 0.01f);
    }
}

namespace {

// The owner's B game (owner-retest-20261114): ~28 fps, frames 22 to 40 ms apart, now and then
// one over 40 ms. A fixed pattern, so the test is the same every run.
std::vector<std::uint64_t> OwnerFrames(int count) {
    static constexpr std::uint64_t kGaps[] = {24, 38, 27, 33, 23, 41, 30, 36, 22, 35, 29, 45, 26, 31, 39, 25};
    std::vector<std::uint64_t> at;
    std::uint64_t t = 10'000;
    for (int i = 0; i < count; ++i) {
        at.push_back(t);
        t += kGaps[i % 16];
    }
    return at;
}

// The Steps metric of a runner followed at those frames, on the raw or the Cadence time. The
// place is worked out at a present up to 16 ms before the frame (as on the game).
CoopFollow::StepsReport Run(bool even) {
    Follower f;
    CoopFollow::Cadence cadence;
    CoopFollow::Steps steps;
    std::optional<CoopFollow::StepsReport> last;
    int i = 0;
    for (const std::uint64_t now : OwnerFrames(400)) {
        const std::uint64_t made = now - static_cast<std::uint64_t>((i++ * 7) % 17);
        const double t = even ? cadence.At(now) : static_cast<double>(now);
        const Vec3 drawn = f.Step(RunnerAt(made), {kSpeed, 0.0f, 0.0f}, made, t);
        if (auto r = steps.Step(std::nullopt, drawn, now)) {
            last = r;
        }
    }
    REQUIRE(last.has_value());
    return *last;
}

}  // namespace

TEST_CASE("cadence: uneven game frames become even, it stays near the real time") {
    CoopFollow::Cadence c;
    const auto frames = OwnerFrames(300);
    double last = 0.0;
    std::vector<double> gaps;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const double t = c.At(frames[i]);
        CHECK(t <= static_cast<double>(frames[i]) + CoopFollow::kResyncMs);
        CHECK(std::fabs(t - static_cast<double>(frames[i])) < 40.0);
        if (i >= 60) {
            gaps.push_back(t - last);
        }
        last = t;
    }
    const auto [lo, hi] = std::minmax_element(gaps.begin(), gaps.end());
    CHECK(*lo > 25.0);  // raw: 22 to 45 ms
    CHECK(*hi < 36.0);
    CHECK(c.PeriodMs() == doctest::Approx(31.5).epsilon(0.08));
}

TEST_CASE("cadence: the same frame twice, a hitch and a clock going back") {
    CoopFollow::Cadence c;
    CHECK(c.At(1000) == 1000.0);
    CHECK(c.At(1033) > 1000.0);
    const double t = c.At(1066);
    CHECK(c.At(1066) == t);
    CHECK(c.At(1067) == t);  // a caller reading the clock per call, same frame
    CHECK(c.At(2000) == 2000.0);  // a load: from the real time again
    CHECK(c.At(500) == 500.0);    // back: afresh
}

TEST_CASE("cadence: slow even frames stay smooth, a resync never goes back") {
    for (const std::uint64_t frame_ms : {143ull, 250ull}) {  // 7 and 4 fps
        CAPTURE(frame_ms);
        CoopFollow::Cadence c;
        double last = c.At(1000);
        for (std::uint64_t i = 1; i < 40; ++i) {
            const std::uint64_t jitter = (i % 3) * 20;  // uneven too
            const double t = c.At(1000 + i * frame_ms + jitter);
            if (i > 10) {
                CHECK(t - last > 0.6 * static_cast<double>(frame_ms));
                CHECK(t - last < 1.4 * static_cast<double>(frame_ms));
            }
            last = t;
        }
    }
    // Slow frames, then a burst of fast ones, a hitch and slow again: the time never goes back.
    CoopFollow::Cadence c;
    std::uint64_t now = 1000;
    double last = c.At(now);
    const std::uint64_t gaps[] = {140, 150, 145, 10, 8, 9, 7, 400, 33, 33, 200, 5, 140, 6, 6, 6, 6, 300};
    for (int round = 0; round < 5; ++round) {
        for (const std::uint64_t g : gaps) {
            now += g;
            const double t = c.At(now);
            CHECK(t >= last);
            CHECK(t <= static_cast<double>(now) + 2.5 * 150.0);
            last = t;
        }
    }
}

TEST_CASE("follow: the owner's uneven frames: even steps on the Cadence time, not on the raw time") {
    const auto raw = Run(false);
    const auto even = Run(true);
    MESSAGE("uneven raw " << raw.uneven << " cadence " << even.uneven);
    CHECK(raw.uneven > 0.2f);   // the owner's "jitter a little, a little choppy"
    CHECK(even.uneven < 0.08f);
    CHECK(even.mean_step == doctest::Approx(raw.mean_step).epsilon(0.05));  // no slower
}

TEST_CASE("steps: the metric reads even steps, a wobble and a second writer") {
    CoopFollow::Steps s;
    std::optional<CoopFollow::StepsReport> r;
    for (std::uint64_t i = 0; i <= 160 && !r; ++i) {
        const float x = 0.2f * static_cast<float>(i) + (i % 2 ? 0.05f : 0.0f);  // 0.25 / 0.15 steps
        const Vec3 drawn{x, 0.0f, 0.0f};
        const Vec3 read{x - 0.15f, 0.0f, 0.0f};  // 0.15 m after our last write: the game moved it
        r = s.Step(i == 0 ? std::nullopt : std::optional<Vec3>(read), drawn, 1000 + i * 33);
    }
    REQUIRE(r.has_value());
    CHECK(r->frames > 100);
    CHECK(r->mean_step == doctest::Approx(0.2f).epsilon(0.02));
    CHECK(r->uneven > 0.4f);
    CHECK(r->uneven_max < 0.7f);
    CHECK(r->game_moved > 0.0f);
    CoopFollow::Steps even;
    std::optional<CoopFollow::StepsReport> e;
    for (std::uint64_t i = 0; i <= 160 && !e; ++i) {
        const Vec3 drawn{0.2f * static_cast<float>(i), 0.0f, 0.0f};
        e = even.Step(drawn, drawn, 1000 + i * 33);
    }
    REQUIRE(e.has_value());
    CHECK(e->uneven < 0.01f);
    CHECK(e->game_moved == doctest::Approx(0.2f).epsilon(0.02));  // read = this frame's draw
}

namespace {

// jitter-3: the place and velocity handed in wobble around the true path as presence's curve
// does (the 'P1 steps' 0.37-0.40 left on a slow walk in jitter-2's run): a fixed pseudo-random
// +-1 cm on the place and +-0.3 m/s on the velocity, each present.
struct Wobble {
    std::uint32_t state = 12345;
    float Next() {  // -1 to 1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    }
};

constexpr float kWalk = 1.4f;  // a slow walk, metres a second

// Steps of a walker followed on the Cadence time at frames `gap_ms` apart in turn (a pattern),
// the place worked out at a present up to 16 ms before each frame, with the wobble.
CoopFollow::StepsReport Walk(const std::vector<std::uint64_t>& gap_ms, float wobble_m, float wobble_v,
                             float* worst_off = nullptr) {
    Follower f;
    CoopFollow::Cadence cadence;
    CoopFollow::Steps steps;
    Wobble w;
    std::optional<CoopFollow::StepsReport> last;
    std::uint64_t now = 10'000;
    float off = 0.0f;
    for (int i = 0; i < 2000 && now < 10'000 + 12'000; ++i) {
        now += gap_ms[static_cast<std::size_t>(i) % gap_ms.size()];
        const std::uint64_t made = now - static_cast<std::uint64_t>((i * 7) % 17);
        const float t = static_cast<float>(made) / 1000.0f;
        const Vec3 at{kWalk * t + wobble_m * w.Next(), 0.0f, wobble_m * w.Next()};
        const Vec3 v{kWalk + wobble_v * w.Next(), 0.0f, wobble_v * w.Next()};
        const double even = cadence.At(now);
        const Vec3 drawn = f.Step(at, v, made, even);
        if (i > 20) {
            off = std::max(off, std::fabs(drawn[0] - kWalk * static_cast<float>(even) / 1000.0f));
        }
        if (auto r = steps.Step(std::nullopt, drawn, now)) {
            last = r;
        }
    }
    if (worst_off != nullptr) {
        *worst_off = off;
    }
    REQUIRE(last.has_value());
    return *last;
}

}  // namespace

TEST_CASE("follow: a slow walk with a wobbling place is drawn in even steps at 7-18, ~28 and 60 fps") {
    const std::vector<std::vector<std::uint64_t>> patterns = {
        {55, 140, 70, 120, 90, 60, 130, 80},             // the owner's 7-18 fps
        {24, 38, 27, 33, 23, 41, 30, 36, 22, 35, 29, 45},  // the owner's ~28 fps
        {16, 17, 16, 17, 17, 16},                        // 60 fps
    };
    for (const auto& gaps : patterns) {
        CAPTURE(gaps[0]);
        float off = 0.0f;
        const auto clean = Walk(gaps, 0.0f, 0.0f);
        const auto r = Walk(gaps, 0.01f, 0.3f, &off);
        MESSAGE("frames from " << gaps[0] << " ms: uneven " << r.uneven << " (clean " << clean.uneven
                               << ", worst " << r.uneven_max << "), mean step " << r.mean_step
                               << ", worst off the path " << off);
        CHECK(r.uneven < 0.1f);  // jitter-3's target
        CHECK(r.mean_step == doctest::Approx(clean.mean_step).epsilon(0.05));  // no slower
        CHECK(off < 0.03f);  // and still on the path, within 3 cm
    }
}

TEST_CASE("follow: the velocity is smoothed, a stop still stops at once") {
    Follower f;
    std::uint64_t now = 1000;
    f.Step({0.0f, 0.0f, 0.0f}, {kWalk, 0.0f, 0.0f}, now, now);
    // One frame's velocity far off (a wobble): the step moves only part of the way toward it.
    now += 33;
    const Vec3 a = f.Step({kWalk * 0.033f, 0.0f, 0.0f}, {kWalk + 1.0f, 0.0f, 0.0f}, now, now);
    CHECK(a[0] < kWalk * 0.033f + 1.0f * 0.033f * 0.5f);
    // Stopped: still at once, settled exactly where they stopped, never past it.
    const Vec3 stop{0.1f, 0.0f, 0.0f};
    Vec3 drawn{};
    for (int i = 0; i < 6; ++i) {
        now += 33;
        drawn = f.Step(stop, {0.0f, 0.0f, 0.0f}, now, now);
        CHECK(drawn[0] <= stop[0] + 1e-6f);
    }
    CHECK(drawn == stop);
}

namespace {

CoopFollow::Quat Yaw(float radians) {
    return {0.0f, std::sin(radians / 2.0f), 0.0f, std::cos(radians / 2.0f)};
}

float YawOf(const CoopFollow::Quat& q) {
    return 2.0f * std::atan2(q[1], q[3]);
}

}  // namespace

TEST_CASE("turn: a steady turn worked out at uneven times is drawn in even turns") {
    CoopFollow::Turner turner;
    constexpr float kRate = 3.0f;  // rad a second (a quick turn)
    const std::uint64_t made_before[] = {0, 16, 3, 12, 16, 0, 8, 15, 1, 10};
    std::vector<float> turns;
    float last = 0.0f;
    for (int i = 0; i < 40; ++i) {
        const std::uint64_t now = 1000 + 33 * static_cast<std::uint64_t>(i);
        const std::uint64_t made = now - made_before[i % 10];
        const float yaw = YawOf(turner.Step(Yaw(kRate * static_cast<float>(made - 1000) / 1000.0f), made, now));
        if (i > 15) {  // the turn rate learnt
            turns.push_back(yaw - last);
            // Drawn kTurnLagMs behind (never past a turn that stops).
            const float at = static_cast<float>(static_cast<double>(now - 1000) - CoopFollow::kTurnLagMs) / 1000.0f;
            CHECK(yaw == doctest::Approx(kRate * at).epsilon(0.01));
        }
        last = yaw;
    }
    const auto [lo, hi] = std::minmax_element(turns.begin(), turns.end());
    MESSAGE("turn per frame " << *lo << " to " << *hi);
    CHECK(*hi - *lo < 0.01f);  // raw: 0.05 to 0.14 rad
}

TEST_CASE("turn: it settles exactly when the turn stops, a big turn is taken at once, bad input passes") {
    CoopFollow::Turner turner;
    std::uint64_t now = 1000;
    for (int i = 0; i < 20; ++i, now += 33) {
        turner.Step(Yaw(2.0f * static_cast<float>(now - 1000) / 1000.0f), now, now);
    }
    const CoopFollow::Quat stop = Yaw(2.0f * static_cast<float>(now - 33 - 1000) / 1000.0f);
    CoopFollow::Quat drawn{};
    for (int i = 0; i < 40; ++i, now += 33) {
        drawn = turner.Step(stop, now, now);
    }
    CHECK(CoopFollow::Turner::Angle(drawn, stop) < 1e-3f);
    // A respawn facing the other way: at once.
    const CoopFollow::Quat back = Yaw(YawOf(stop) + 3.0f);
    CHECK(CoopFollow::Turner::Angle(turner.Step(back, now, now), back) < 1e-4f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const CoopFollow::Quat bad{nan, 0.0f, 0.0f, 1.0f};
    CHECK(std::isnan(turner.Step(bad, now + 33, now + 33)[0]));
    CHECK(turner.Rate()[1] == 0.0f);
}

TEST_CASE("delayed: the animation at the shown time, not the newest") {
    CoopFollow::Delayed<int> line;
    CHECK(line.At(1000) == nullptr);
    line.Push(1000, 1);
    line.Push(1033, 2);
    line.Push(1066, 3);
    line.Push(1050, 9);  // out of order: dropped
    REQUIRE(line.At(900) != nullptr);
    CHECK(*line.At(900) == 1);  // before them all: the oldest
    CHECK(*line.At(1040) == 2);
    CHECK(*line.At(1040) == 2);  // the same time twice
    CHECK(*line.At(5000) == 3);  // past them all: the newest
    line.Clear();
    CHECK(line.At(5000) == nullptr);
    for (int i = 0; i < 100; ++i) {
        line.Push(static_cast<std::uint64_t>(i), i);
    }
    CHECK(*line.At(0) == 100 - static_cast<int>(CoopFollow::Delayed<int>::kKeep));  // only the newest kept
}

TEST_CASE("follow: an easing stop passes the stop by at most kPassM") {
    Follower f;
    CoopFollow::Cadence cadence;
    // Walking, then slowing to a stop over 0.3 s (presence's curve eases it), then still.
    float x = 0.0f;
    std::uint64_t now = 1000;
    float most = 0.0f;
    for (int i = 0; i < 60; ++i, now += 30) {
        const float slow = i < 20 ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(i - 20) / 10.0f);
        const float v = kWalk * slow;
        x += v * 0.030f;
        const Vec3 drawn = f.Step({x, 0.0f, 0.0f}, {v, 0.0f, 0.0f}, now, cadence.At(now));
        if (i >= 20) {
            most = std::max(most, drawn[0] - x);
        }
        if (i == 59) {
            CHECK(drawn[0] == doctest::Approx(x));  // settled where they stopped
        }
    }
    CHECK(most <= CoopFollow::kPassM + 1e-3f);  // 2 cm (and float rounding)
}

TEST_CASE("turn: a quick turn that stops never passes the heading and settles on it") {
    for (const float rate : {3.0f, 6.0f, 10.0f}) {  // rad a second
        for (const std::uint64_t frame_ms : {16ull, 33ull, 100ull}) {
            CAPTURE(rate);
            CAPTURE(frame_ms);
            CoopFollow::Turner turner;
            CoopFollow::Cadence cadence;
            const std::uint64_t made_before[] = {0, 16, 3, 12, 16, 0, 8, 15, 1, 10};
            std::uint64_t now = 1000;
            const std::uint64_t stop_ms = 1600;
            const float stop = rate * static_cast<float>(stop_ms - 1000) / 1000.0f;
            float most = -10.0f;
            CoopFollow::Quat drawn{};
            for (int i = 0; now < 3000; ++i, now += frame_ms) {
                const std::uint64_t made = now - made_before[i % 10];
                const float yaw = rate * static_cast<float>(std::min(made, stop_ms) - 1000) / 1000.0f;
                drawn = turner.Step(Yaw(yaw), made, cadence.At(now));
                most = std::max(most, YawOf(drawn) - stop);
            }
            CHECK(most <= 0.001f);                                 // never past the heading
            CHECK(CoopFollow::Turner::Angle(drawn, Yaw(stop)) < 1e-3f);  // and settled on it
        }
    }
}
