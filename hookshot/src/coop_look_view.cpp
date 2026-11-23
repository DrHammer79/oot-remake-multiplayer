// The Customize page's live picture: the game's camera around your own Link
// (coop_look_view.h).
#include "coop_look_view.h"

#include <algorithm>
#include <cmath>

namespace CoopLookView {

namespace {

constexpr double kTau = 6.283185307179586;

// The view's rows as the game's view is rebuilt from an eye and a target
// (hos_main.c oot_coop_view, the free camera's): right, up, back.
struct Axes {
    Vec3 right, up, back;
};

Axes AxesOf(double yaw, double pitch) {
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    return {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
}

double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

bool Window::Valid() const {
    return std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1) && std::isfinite(aspect) &&
           x0 >= 0 && y0 >= 0 && x1 <= 1 && y1 <= 1 && x1 > x0 && y1 > y0 && aspect > 0;
}

namespace {

// The shot from `yaw` at `elevation` and distance `d`: Link's middle at the window's middle.
Shot AimAt(const std::array<float, 3>& feet, double h, double yaw, double elevation, double d, const Window& window,
           double fov_y) {
    const Axes a = AxesOf(yaw, elevation);
    const double t = std::tan(fov_y * 0.5);
    // The window's middle in the view's terms (-1 to 1, y up).
    const double nx = static_cast<double>(window.x0) + window.x1 - 1.0;
    const double ny = 1.0 - (static_cast<double>(window.y0) + window.y1);
    const Vec3 middle{feet[0], feet[1] + h * 0.5, feet[2]};
    Shot s;
    for (int i = 0; i < 3; ++i) {
        // Aimed beside him, so that he stands at the window's middle; the eye d behind that aim.
        s.target[i] = middle[i] - a.right[i] * nx * d * t * window.aspect - a.up[i] * ny * d * t;
        s.eye[i] = s.target[i] + a.back[i] * d;
    }
    return s;
}

// How much of the window's height he fills, head to feet, in a shot.
double Filled(const Shot& s, const std::array<float, 3>& feet, double h, const Window& window, double fov_y) {
    const auto top = Project(s, {feet[0], feet[1] + h, feet[2]}, window.aspect, fov_y);
    const auto bottom = Project(s, {feet[0], feet[1], feet[2]}, window.aspect, fov_y);
    return (bottom[1] - top[1]) / (static_cast<double>(window.y1) - window.y0);
}

}  // namespace

double FeetDrop(const Shot& shot, const std::array<float, 3>& feet) {
    return std::atan2(shot.eye[1] - feet[1], std::hypot(shot.eye[0] - feet[0], shot.eye[2] - feet[2]));
}

Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y) {
    const double h = height > 0 ? height : 1.0;
    // Start at the distance that fits his height into kFill of the window seen straight on, and at the elevation
    // whose line to his feet would fall kFeetDrop. Then a few steps: the window's place and the near-far of the
    // view move both a little, and each step corrects the elevation by the drop missed and the distance by the
    // fill missed.
    double elevation = kElevationMin;
    double d = h / (kFill * (window.y1 - window.y0) * 2.0 * std::tan(fov_y * 0.5));
    Shot s = AimAt(feet, h, yaw, elevation, d, window, fov_y);
    for (int step = 0; step < 12; ++step) {
        const double next = std::clamp(elevation + kFeetDrop - FeetDrop(s, feet), kElevationMin, kElevationMax);
        const double filled = Filled(s, feet, h, window, fov_y);
        const double next_d = filled > 0 ? d * filled / kFill : d;
        if (std::fabs(next - elevation) < 1e-7 && std::fabs(next_d - d) < 1e-7 * d) {
            break;
        }
        elevation = next;
        d = next_d;
        s = AimAt(feet, h, yaw, elevation, d, window, fov_y);
    }
    return s;
}

double FrontYaw(const Vec3& game_eye, const Vec3& game_target) {
    return std::atan2(game_eye[0] - game_target[0], game_eye[2] - game_target[2]) + kTau / 2;
}

std::array<double, 2> Project(const Shot& shot, const Vec3& point, double aspect, double fov_y) {
    const Vec3 b{shot.eye[0] - shot.target[0], shot.eye[1] - shot.target[1], shot.eye[2] - shot.target[2]};
    const Axes a = AxesOf(std::atan2(b[0], b[2]), std::atan2(b[1], std::hypot(b[0], b[2])));
    const Vec3 p{point[0] - shot.eye[0], point[1] - shot.eye[1], point[2] - shot.eye[2]};
    const double t = std::tan(fov_y * 0.5), depth = -Dot(a.back, p);
    const double nx = Dot(a.right, p) / (depth * t * aspect), ny = Dot(a.up, p) / (depth * t);
    return {(nx + 1) * 0.5, (1 - ny) * 0.5};
}

void Orbit::Ask(const Window& window, std::uint64_t now_ms) {
    if (!window.Valid()) {
        return;
    }
    window_ = window;
    asked_ms_ = now_ms;
}

bool Orbit::Asked(std::uint64_t now_ms) const {
    return asked_ms_ && now_ms >= *asked_ms_ && now_ms - *asked_ms_ <= kHoldMs;
}

std::optional<double> FovFromClip(const std::array<float, 16>& m) {
    const auto length = [&m](int row) {
        return std::sqrt(static_cast<double>(m[4 * row]) * m[4 * row] + static_cast<double>(m[4 * row + 1]) * m[4 * row + 1] +
                         static_cast<double>(m[4 * row + 2]) * m[4 * row + 2]);
    };
    const double up = length(1), depth = length(3);
    if (!std::isfinite(up) || !std::isfinite(depth) || up <= 1e-9 || depth <= 1e-9) {
        return std::nullopt;
    }
    const double fov = 2.0 * std::atan(depth / up);
    if (!(fov >= kFovMin && fov <= kFovMax)) {
        return std::nullopt;
    }
    return fov;
}

std::optional<Shot> Orbit::Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                                  std::uint64_t now_ms, double fov_y) {
    if (!Asked(now_ms) || !link) {
        start_ms_.reset();   // (the next turn starts in front of him again)
        return std::nullopt;
    }
    if (!start_ms_) {
        start_ms_ = now_ms;
        start_yaw_ = FrontYaw(game_eye, game_target);
    }
    const double turn = static_cast<double>(now_ms - *start_ms_) / 1000.0 / kTurnSeconds;
    return Aim(link->feet, link->height, start_yaw_ + kTau * (turn - std::floor(turn)), window_, fov_y);
}

namespace {
bool Live(std::optional<Showing> s) {
    return s == Showing::live_child || s == Showing::live_adult || s == Showing::live_event;
}
}  // namespace

const char* Describe(Showing showing) {
    switch (showing) {
    case Showing::live_child: return "live, the camera around your child Link";
    case Showing::live_adult: return "live, the camera around your adult Link";
    case Showing::flat_no_link: return "flat: the co-op hook has no Link this frame (no game, scene or place)";
    case Showing::live_event: return "live, our own event (a cutscene) runs: the camera around your Link";
    }
    return "?";
}

std::optional<Seen> Keep::Take(const std::optional<Seen>& now, std::uint64_t now_ms) {
    if (now) {
        last_ = now;
        seen_ms_ = now_ms;
        return now;
    }
    if (last_ && now_ms >= seen_ms_ && now_ms - seen_ms_ <= kGapMs) {
        return last_;
    }
    last_.reset();
    return std::nullopt;
}

std::optional<std::string> Watch::Drawn(Showing showing, std::uint64_t now_ms) {
    // Not drawn for a while: the page was left; the next opening says its picture again.
    if (last_ && (now_ms < drawn_ms_ || now_ms - drawn_ms_ > kHoldMs)) {
        last_.reset();
    }
    drawn_ms_ = now_ms;
    if (showing != last_) {
        if (Live(showing) && !Live(last_)) {
            live_since_ms_ = now_ms;
            told_unframed_ = false;
        }
        last_ = showing;
        return std::string("coop: look: the Customize picture is ") + Describe(showing);
    }
    if (Live(showing) && !told_unframed_ && now_ms - live_since_ms_ >= kUnframedMs &&
        (!framed_ms_ || *framed_ms_ < live_since_ms_)) {
        told_unframed_ = true;
        return std::string("coop: look: the Customize picture is live but its camera was not framed for ") +
               std::to_string(kUnframedMs / 1000) +
               " s: the game's view routine did not ask (hos_coop_view, patch 0019), so the game's own camera shows";
    }
    return std::nullopt;
}

void Watch::Framed(std::uint64_t now_ms) {
    framed_ms_ = now_ms;
}

}  // namespace CoopLookView
