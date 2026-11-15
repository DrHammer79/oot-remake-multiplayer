// SPDX-License-Identifier: GPL-3.0-or-later
//
// jitter-b (0.11 playtest, 2026-11-14: "players are always constantly jittering a little on other
// players' screens", townspeople "extremely jittery and choppy"). The place of another player's
// puppet and of a townsperson copy is worked out on the present thread (presence / the object
// samples, interpolated at that moment) and written on the next game frame. The game runs its
// frames on its own thread, so the time between the two changes from frame to frame (0 to one
// present): each frame's step was a whole, half or one-and-a-half frame of their movement. The
// owner's samples are also stamped at their present, not at their game frame.
//
// A Follower per puppet or copy, stepped once a game frame, takes that place forward to the
// frame's own time (by the velocity it came with, at most kAheadMaxMs) and follows it with the
// velocity fed forward, closing what is left of the gap with a short exponential (kTauMs). So:
// - steady movement is drawn in even steps, with no lag (the velocity carries it);
// - it never passes the place it follows (no overshoot), and on a stop it settles there in a
//   few frames (kRestM: exactly there);
// - a gap past kJumpM (a late sample, a teleport) is taken at once: the caller's own easing
//   (coop_snap.h for puppets) or the game's warp handles it, as before.
// The result is written through the same game call as before (the setter the puppet and copy
// already use); nothing new is written. No emulator code: tests/lichii/follow_test.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace CoopFollow {

using Vec3 = std::array<float, 3>;

// The place is taken forward from when it was worked out by at most this.
inline constexpr std::uint64_t kAheadMaxMs = 150;  // a 7 fps frame (review-jitter)
// What is left of the gap closes by 1 - e^(-dt/kTauMs) a frame (a 30 fps frame: 56 %).
inline constexpr float kTauMs = 40.0f;
// A gap past this (metres) is taken at once.
inline constexpr float kJumpM = 0.75f;
// Still and this close: exactly there.
inline constexpr float kRestM = 0.005f;
// A frame counts as at most this (a hitch, a load).
inline constexpr std::uint64_t kMaxStepMs = 100;
// jitter-3: the velocity handed in each frame is presence's curve's slope at that present,
// which wobbles from sample to sample (~0.3 m/s on a walk: a few mm a frame on a 3.6 cm step).
// The Follower carries it by that velocity smoothed over kVelTauMs.
inline constexpr float kVelTauMs = 60.0f;
// jitter-3: while moving, a gap under kSoftM is mostly that wobble of the place handed in
// (~1 cm), not lag: it closes slower (up to kSoftTauMs at no gap), so the wobble is not drawn
// as uneven steps. A gap past kSoftM (a turn, a start) closes at kTauMs as before; still, the
// gap always closes at kTauMs (a stop settles as fast as before).
inline constexpr float kSoftM = 0.05f;
inline constexpr float kSoftTauMs = 160.0f;
// jitter-3: while moving, the velocity may carry it up to this past the place it follows (the
// wobble, ~1 cm); further, or still, it is cut at the place (no overshoot at a stop).
inline constexpr float kPassM = 0.02f;

class Follower {
public:
    // Where to draw it this game frame (`now_ms`, steady clock). `target` is where it is as of
    // `target_ms` (the same clock; 0: now), moving at `velocity` metres a second.
    // `now_ms` may be a Cadence time (fractional, and up to kAheadMaxMs before `target_ms`:
    // the place is then taken back by the velocity).
    Vec3 Step(const Vec3& target, const Vec3& velocity, std::uint64_t target_ms, double now_ms) {
        if (!Finite(target) || !Finite(velocity)) {
            Reset();
            return target;
        }
        const bool first = !drawn_ || now_ms < last_ms_;
        const double dt_ms = first ? 0.0 : std::min(now_ms - last_ms_, static_cast<double>(kMaxStepMs));
        if (!first && dt_ms <= 0.0) {
            return *drawn_;  // twice in one frame
        }
        // jitter-3: the velocity smoothed (a first frame: as handed in; still: still at once,
        // so a stop is not carried on).
        if (first || Still(velocity)) {
            vel_ = velocity;
        } else {
            const float k = 1.0f - std::exp(-static_cast<float>(dt_ms) / kVelTauMs);
            for (std::size_t i = 0; i < 3; ++i) {
                vel_[i] += (velocity[i] - vel_[i]) * k;
            }
        }
        const double ahead_ms =
            target_ms == 0 ? 0.0
                           : std::clamp(now_ms - static_cast<double>(target_ms),
                                        -static_cast<double>(kAheadMaxMs), static_cast<double>(kAheadMaxMs));
        Vec3 want = target;
        for (std::size_t i = 0; i < 3; ++i) {
            want[i] += vel_[i] * static_cast<float>(ahead_ms / 1000.0);
        }
        if (first) {
            return Set(want, now_ms);
        }
        Vec3 drawn = *drawn_;
        const float gap = Distance(drawn, want);
        if (gap > kJumpM) {
            return Set(want, now_ms);
        }
        // The velocity carries it; it never passes the place it follows. jitter-3: while moving,
        // a place up to kPassM behind where the velocity carries it is that wobble (about every
        // other frame): it is closed below like any gap, not taken at once (that drew the wobble).
        Vec3 next = drawn;
        float along = 0.0f;  // (want - next) . (want - drawn)
        for (std::size_t i = 0; i < 3; ++i) {
            next[i] += vel_[i] * static_cast<float>(dt_ms) / 1000.0f;
            along += (want[i] - next[i]) * (want[i] - drawn[i]);
        }
        if (along < 0.0f && (Still(velocity) || Distance(next, want) > kPassM)) {
            next = want;
        }
        const float tau = Still(velocity) || gap >= kSoftM
                              ? kTauMs
                              : kSoftTauMs + (kTauMs - kSoftTauMs) * (gap / kSoftM);
        const float close = 1.0f - std::exp(-static_cast<float>(dt_ms) / tau);
        for (std::size_t i = 0; i < 3; ++i) {
            drawn[i] = next[i] + (want[i] - next[i]) * close;
        }
        if (Still(velocity) && Distance(drawn, want) <= kRestM) {
            drawn = want;
        }
        drawn_ = drawn;
        last_ms_ = now_ms;
        return drawn;
    }

    void Reset() { drawn_.reset(); }
    const std::optional<Vec3>& Drawn() const { return drawn_; }

    static float Distance(const Vec3& a, const Vec3& b) {
        float d2 = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            d2 += (a[i] - b[i]) * (a[i] - b[i]);
        }
        return std::sqrt(d2);
    }

private:
    Vec3 Set(const Vec3& at, double now_ms) {
        drawn_ = at;
        last_ms_ = now_ms;
        return at;
    }
    static bool Finite(const Vec3& v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }
    static bool Still(const Vec3& v) {
        return std::fabs(v[0]) < 1e-3f && std::fabs(v[1]) < 1e-3f && std::fabs(v[2]) < 1e-3f;
    }

    std::optional<Vec3> drawn_;
    Vec3 vel_{};  // jitter-3: the velocity smoothed
    double last_ms_ = 0.0;
};

// jitter-2 (owner re-test on 975a0258: "still jitter a little and seem a little choppy"). The
// game moves its world one even step a frame, but its frames come 22 to over 40 ms apart (the
// owner's B log: 'present intervals' at ~28 fps), and the Follower stepped by that raw time:
// the puppet's steps went 0.7x-1.5x frame to frame while the ground, the camera and the
// player's own Link moved evenly. The Cadence is the game frame's time made even: it goes on
// by the mean frame time and closes kPull of its distance to the real time each frame, so
// steps are even and it never drifts. A frame past kResyncMs off (a load, a hitch) starts it
// again from the real time. Once a game frame (several puppets in one frame: the same time).
inline constexpr double kPull = 0.1;
inline constexpr double kPeriodBlend = 0.1;
inline constexpr double kResyncMs = 150.0;
inline constexpr double kResyncFrames = 2.5;   // or this many mean frames, if more
inline constexpr double kSlowestMs = 500.0;    // a first frame time up to this is taken
inline constexpr std::uint64_t kSameFrameMs = 3;  // game frames are further apart than this

class Cadence {
public:
    double At(std::uint64_t now_ms) {
        const double now = static_cast<double>(now_ms);
        if (have_ && now_ms >= raw_ms_ && now_ms - raw_ms_ < kSameFrameMs) {
            return smooth_ms_;  // the same frame (a caller that reads the clock per call)
        }
        if (!have_ || now_ms < raw_ms_) {
            have_ = true;
            raw_ms_ = now_ms;
            period_ms_ = 0.0;
            return smooth_ms_ = now;
        }
        const double dt = now - static_cast<double>(raw_ms_);
        raw_ms_ = now_ms;
        // review-jitter2: the limit grows with the frame time, so slow but even frames (the
        // owner's 7 fps) stay smooth; a first frame up to kSlowestMs sets the frame time.
        const double base = period_ms_ != 0.0 ? period_ms_ : (dt <= kSlowestMs ? dt : 0.0);
        const double limit = std::max(kResyncMs, kResyncFrames * base);
        if (dt > limit) {
            return Resync(now);  // a hitch: the mean frame time stays
        }
        period_ms_ = period_ms_ == 0.0 ? dt : period_ms_ + (dt - period_ms_) * kPeriodBlend;
        const double guess = smooth_ms_ + period_ms_;
        const double off = now - guess;
        if (off > limit || off < -limit) {
            return Resync(now);
        }
        smooth_ms_ = std::max(guess + off * kPull, smooth_ms_);
        return smooth_ms_;
    }
    double PeriodMs() const { return period_ms_; }

private:
    // From the real time again, never back (review-jitter2: a step back made the Follower start
    // afresh, one frame unsmoothed); a time ahead of the real one waits for it.
    double Resync(double now) { return smooth_ms_ = std::max(now, smooth_ms_); }

    bool have_ = false;
    std::uint64_t raw_ms_ = 0;
    double smooth_ms_ = 0.0;
    double period_ms_ = 0.0;
};

// jitter-2: the proof metric. Each game frame the place drawn and the place the game had
// (read back before the write). Report() every kReportMs: frames, the mean step, how uneven
// the steps are (the mean |1 - step/last step| over frames that both moved, so a steady run
// reads 0 and a 0.5x/1.5x wobble 0.5), and how far the game itself moved the puppet between
// our writes (a second writer, its own root motion).
inline constexpr std::uint64_t kReportMs = 5000;
inline constexpr float kMovingM = 0.01f;

struct StepsReport {
    int frames = 0;
    int moving = 0;
    float mean_step = 0.0f;
    float uneven = 0.0f;      // mean |1 - step / last step|
    float uneven_max = 0.0f;  // the worst frame
    float game_moved = 0.0f;  // mean distance the game moved it between our writes
    float game_moved_max = 0.0f;
};

class Steps {
public:
    // `read` is where the game had it this frame (before the write), `drawn` what is written.
    std::optional<StepsReport> Step(const std::optional<Vec3>& read, const Vec3& drawn, std::uint64_t now_ms) {
        if (start_ms_ == 0 || now_ms < start_ms_) {
            start_ms_ = now_ms;
        }
        if (last_drawn_) {
            const float step = Follower::Distance(drawn, *last_drawn_);
            ++frames_;
            sum_step_ += step;
            if (step > kMovingM && last_step_ > kMovingM) {
                const float u = std::fabs(1.0f - step / last_step_);
                ++moving_;
                sum_uneven_ += u;
                uneven_max_ = std::max(uneven_max_, u);
            }
            last_step_ = step;
            if (read) {
                const float g = Follower::Distance(*read, *last_drawn_);
                ++reads_;
                sum_game_ += g;
                game_max_ = std::max(game_max_, g);
            }
        }
        last_drawn_ = drawn;
        if (now_ms - start_ms_ < kReportMs) {
            return std::nullopt;
        }
        StepsReport r;
        r.frames = frames_;
        r.moving = moving_;
        r.mean_step = frames_ ? sum_step_ / static_cast<float>(frames_) : 0.0f;
        r.uneven = moving_ ? sum_uneven_ / static_cast<float>(moving_) : 0.0f;
        r.uneven_max = uneven_max_;
        r.game_moved = reads_ ? sum_game_ / static_cast<float>(reads_) : 0.0f;
        r.game_moved_max = game_max_;
        const auto keep = last_drawn_;
        *this = Steps{};
        last_drawn_ = keep;
        start_ms_ = now_ms;
        return r;
    }
    // A jump, a hide, a new actor: the next frame starts afresh.
    void Break() {
        last_drawn_.reset();
        last_step_ = 0.0f;
    }

private:
    std::optional<Vec3> last_drawn_;
    float last_step_ = 0.0f;
    std::uint64_t start_ms_ = 0;
    int frames_ = 0, moving_ = 0, reads_ = 0;
    float sum_step_ = 0.0f, sum_uneven_ = 0.0f, uneven_max_ = 0.0f, sum_game_ = 0.0f, game_max_ = 0.0f;
};

// jitter-3: the rotation followed as the place is. The rotation handed in is presence's at the
// present (its slerp between samples), drawn on a game frame 0 to 16 ms (and the Cadence's
// difference) later, so while turning each frame's turn was uneven, and a new sample's turn
// rate showed as a kink. The Turner takes it forward to the frame's time by the turn rate
// (worked out from the rotations handed in, smoothed over kVelTauMs), turns on by that rate
// and closes what is left at kTauMs. A turn past kTurnJumpRad is taken at once (a warp, a
// respawn). Quaternions x, y, z, w.
using Quat = std::array<float, 4>;
inline constexpr float kTurnJumpRad = 2.0f;  // ~115 degrees
inline constexpr float kTurnRestRad = 0.003f;
inline constexpr float kTurnStillRad = 0.01f;  // rad a second: under this, not turning
inline constexpr double kTurnLagMs = 20.0;     // a present's age (16 ms) and a little

class Turner {
public:
    // Like Follower::Step: `target` is the rotation as of `target_ms` (0: now).
    Quat Step(const Quat& target, std::uint64_t target_ms, double now_ms) {
        const std::optional<Quat> to = Normal(target);
        if (!to) {
            Reset();
            return target;
        }
        const bool first = !drawn_ || now_ms < last_ms_;
        const double dt_ms = first ? 0.0 : std::min(now_ms - last_ms_, static_cast<double>(kMaxStepMs));
        if (!first && dt_ms <= 0.0) {
            return *drawn_;  // twice in one frame
        }
        // The turn rate from the last rotation handed in (rad a second, about an axis).
        if (first) {
            rate_ = {};
        } else if (target_ms != 0 && target_ && target_ms > target_ms_) {
            const Vec3 turn = Log(Mul(*to, Conj(*target_)));
            const float seconds = static_cast<float>(target_ms - target_ms_) / 1000.0f;
            const float k = 1.0f - std::exp(-static_cast<float>(dt_ms) / kVelTauMs);
            const bool jump = Length(turn) > kTurnJumpRad;
            // review-jitter3: no turn since the last rotation handed in (a stop): no rate at once,
            // as the Follower's velocity (else it turned on past the heading for ~60 ms).
            const bool stopped = Length(turn) / seconds < kTurnStillRad;
            for (std::size_t i = 0; i < 3; ++i) {
                rate_[i] = stopped ? 0.0f : rate_[i] + ((jump ? 0.0f : turn[i] / seconds) - rate_[i]) * k;
            }
        }
        if (!target_ || target_ms != target_ms_) {
            target_ = *to;
            target_ms_ = target_ms;
        }
        // review-jitter3: drawn as of kTurnLagMs before the frame, so it is only ever taken back
        // along the turn, never on past it (a turn that stops is not yet known to stop).
        const double ahead_ms =
            target_ms == 0 ? 0.0
                           : std::clamp(now_ms - static_cast<double>(target_ms) - kTurnLagMs,
                                        -static_cast<double>(kAheadMaxMs), 0.0);
        const Quat want = Mul(Exp(Scale(rate_, static_cast<float>(ahead_ms / 1000.0))), *to);
        if (first || Angle(*drawn_, want) > kTurnJumpRad) {
            return Set(want, now_ms);
        }
        Quat next = Mul(Exp(Scale(rate_, static_cast<float>(dt_ms / 1000.0))), *drawn_);
        // review-jitter3: it never turns past the heading it follows (a quick turn that stops
        // overshot by 5-18 degrees): a rate that carries it past is cut at the heading.
        const Vec3 left = Log(Mul(want, Conj(next)));     // what is left after the rate's turn
        const Vec3 gap = Log(Mul(want, Conj(*drawn_)));   // what was left before it
        if (left[0] * gap[0] + left[1] * gap[1] + left[2] * gap[2] < 0.0f) {
            next = want;
        }
        const float close = 1.0f - std::exp(-static_cast<float>(dt_ms) / kTauMs);
        Quat drawn = Slerp(next, want, close);
        if (Length(rate_) < 1e-3f && Angle(drawn, want) <= kTurnRestRad) {
            drawn = want;
        }
        return Set(drawn, now_ms);
    }

    void Reset() {
        drawn_.reset();
        target_.reset();
        target_ms_ = 0;
        rate_ = {};
    }
    const Vec3& Rate() const { return rate_; }

    // The angle between two rotations, radians (0 to pi).
    static float Angle(const Quat& a, const Quat& b) {
        const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2.0f * std::acos(std::min(d, 1.0f));
    }

private:
    Quat Set(const Quat& q, double now_ms) {
        drawn_ = q;
        last_ms_ = now_ms;
        return q;
    }
    static std::optional<Quat> Normal(const Quat& q) {
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!std::isfinite(n) || n < 1e-6f) {
            return std::nullopt;
        }
        return Quat{q[0] / n, q[1] / n, q[2] / n, q[3] / n};
    }
    static Quat Conj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
    static Quat Mul(const Quat& a, const Quat& b) {
        return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    }
    static float Length(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    static Vec3 Scale(const Vec3& v, float k) { return {v[0] * k, v[1] * k, v[2] * k}; }
    // The rotation vector (axis * angle) of a unit quaternion, the short way round.
    static Vec3 Log(Quat q) {
        if (q[3] < 0.0f) {
            q = {-q[0], -q[1], -q[2], -q[3]};
        }
        const float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (s < 1e-7f) {
            return {2.0f * q[0], 2.0f * q[1], 2.0f * q[2]};
        }
        const float angle = 2.0f * std::atan2(s, q[3]);
        return {q[0] / s * angle, q[1] / s * angle, q[2] / s * angle};
    }
    static Quat Exp(const Vec3& v) {
        const float angle = Length(v);
        if (angle < 1e-7f) {
            return {0.5f * v[0], 0.5f * v[1], 0.5f * v[2], 1.0f};
        }
        const float s = std::sin(angle / 2.0f) / angle;
        return {v[0] * s, v[1] * s, v[2] * s, std::cos(angle / 2.0f)};
    }
    static Quat Slerp(const Quat& a, const Quat& b, float t) {
        const Quat q = Mul(Exp(Scale(Log(Mul(b, Conj(a))), t)), a);
        return Normal(q).value_or(b);
    }

    std::optional<Quat> drawn_;
    std::optional<Quat> target_;  // the last rotation handed in, and its time
    std::uint64_t target_ms_ = 0;
    Vec3 rate_{};                 // rad a second, smoothed
    double last_ms_ = 0.0;
};

// jitter-3: the animation shown at the same time as the place. Presence shows the place
// show_delay (100 ms) behind the newest sample, but the newest sample's animation was played at
// once: the walk started ~100 ms before the puppet moved and it slid on its stand at a stop
// (14 cm on a walk, ~60 cm on a run), feet sliding that reads as "a little choppy". A Delayed
// keeps the last few values by their sample time and gives the newest at or before the shown
// time.
template <typename T>
class Delayed {
public:
    static constexpr std::size_t kKeep = 48;  // 0.8 s at 60 samples a second

    void Push(std::uint64_t time_us, T value) {
        if (!items_.empty() && time_us < items_.back().first) {
            return;  // older than the newest (out of order): presence drops it too
        }
        if (items_.size() >= kKeep) {
            items_.erase(items_.begin());
        }
        items_.emplace_back(time_us, std::move(value));
    }
    // The newest value at or before `show_us`; before them all, the oldest (nothing is hidden
    // at a join). The older ones are dropped.
    const T* At(std::uint64_t show_us) {
        if (items_.empty()) {
            return nullptr;
        }
        std::size_t i = 0;
        while (i + 1 < items_.size() && items_[i + 1].first <= show_us) {
            ++i;
        }
        items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(i));
        return &items_.front().second;
    }
    void Clear() { items_.clear(); }

private:
    std::vector<std::pair<std::uint64_t, T>> items_;
};

}  // namespace CoopFollow
