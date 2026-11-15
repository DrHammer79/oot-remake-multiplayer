// The Customize page's live picture (docs/puppet.md "The Link customizer"):
// your own Link, the real one in the game, seen through a clear window in
// the menu (hos_coop_menu_picture.h `live`, patch 0019) while the game's
// camera turns slowly around him (hos_coop_view.h, patch 0019). Longshot's
// menu does not stop the game, so the world keeps drawing under it; the
// camera is the game's own view, only its eye and target are ours.
//
// The menu's thread asks for the picture each drawing (Ask); the game's
// thread frames the camera while it is asked for (Camera). Asked for no more
// (the page left, the menu closed): the game's camera again within kHoldMs.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace CoopLookView {

using Vec3 = std::array<double, 3>;

// The picture's square in the window: fractions of it (y down), and the
// window's width over its height.
struct Window {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float aspect = 16.0f / 9.0f;
    bool Valid() const;
};

struct Shot {
    Vec3 eye{};
    Vec3 target{};
};

// The game's vertical field of view, in radians, when its camera is not
// handed over (FovFromClip has none): measured on a screenshot.
inline constexpr double kFovY = 0.785398;   // 45 degrees
// The field of view FovFromClip believes: outside it, kFovY.
inline constexpr double kFovMin = 0.17;   // 10 degrees
inline constexpr double kFovMax = 2.1;    // 120 degrees
// One turn around Link, in seconds; the camera a little above his middle.
inline constexpr double kTurnSeconds = 16.0;
inline constexpr double kElevation = 0.14;   // radians (8 degrees)
// Link's height fills this much of the window's height.
inline constexpr double kFill = 0.85;
// Asked for no later than this ago: still the picture (the menu draws at the
// display's rate, the game frames its camera at its own).
inline constexpr std::uint64_t kHoldMs = 250;

// The game's vertical field of view from its world-to-clip matrix (row-major,
// clip = m * p: the camera block Hookshot hands the name tags, CoopCamera).
// Its y row is the view's up scaled by 1/tan(fov/2), its w row the view's
// depth axis at scale 1, so the fov is 2 atan(|w| / |y|) (the xyz parts).
// None for a matrix that is not such a perspective.
std::optional<double> FovFromClip(const std::array<float, 16>& clip_from_world);

// The shot that shows Link (his feet, his height) in the middle of the
// window, from `yaw` (the direction from him to the eye, as atan2(x, z): the
// game's views' convention), filling kFill of the window's height.
Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y = kFovY);

// The yaw that looks at Link's front, from the game's own camera (behind
// him): its direction turned round.
double FrontYaw(const Vec3& game_eye, const Vec3& game_target);

// Where a point lands in the window (fractions, y down) for a shot, as the
// game's view (rebuilt from eye and target, hos_main.c oot_coop_view) and a
// projection of `fov_y` show it. For tests and for checking Aim.
std::array<double, 2> Project(const Shot& shot, const Vec3& point, double aspect, double fov_y = kFovY);

// Link as the camera needs him: his feet and his height.
struct Subject {
    std::array<float, 3> feet{};
    float height = 0;
};

// One turn of the camera: started when first asked for, its yaw from the
// game's camera then; ended when not asked for kHoldMs.
class Orbit {
public:
    void Ask(const Window& window, std::uint64_t now_ms);
    bool Asked(std::uint64_t now_ms) const;
    // The game's thread: the shot for now, or none (the game's camera).
    // `fov_y`: the game's (FovFromClip), else kFovY.
    std::optional<Shot> Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                               std::uint64_t now_ms, double fov_y = kFovY);

private:
    Window window_{};
    std::optional<std::uint64_t> asked_ms_;
    std::optional<std::uint64_t> start_ms_;   // the turn's start; none: no turn
    double start_yaw_ = 0;
};

// What the page shows, for the log (0.11.1: adult Link got the flat picture
// where a child got the live one; the log now says which and why).
enum class Showing : std::uint8_t {
    live_child,     // the live picture, a child Link
    live_adult,     // the live picture, an adult Link
    flat_no_link,   // the flat one: the hook has no Link (no game, scene or place) for longer than Keep::kGapMs
    live_event,     // the live picture while our own event (a cutscene) runs: the camera still turns around him
};
const char* Describe(Showing showing);

// Your own Link as the page last saw him, kept through a short gap (#74: the
// picture flipped to the flat one for a moment after each scene entry, when
// the hook had no Link for a frame or two). Not thread-safe: the caller's lock.
struct Seen {
    Subject link;
    Showing showing = Showing::live_child;
};
class Keep {
public:
    static constexpr std::uint64_t kGapMs = 1500;
    // This moment's Link (none: the hook has none): him, or the last one seen
    // no longer than kGapMs ago, or none.
    std::optional<Seen> Take(const std::optional<Seen>& now, std::uint64_t now_ms);

private:
    std::optional<Seen> last_;
    std::uint64_t seen_ms_ = 0;
};

// One line to the log each time the page's picture changes (and again at
// each opening of the page), and once when a live picture's camera is never
// framed (the game's view routine did not ask for it). Not thread-safe: the
// caller's lock.
class Watch {
public:
    // How long a live picture may go without a framed camera before it is said.
    static constexpr std::uint64_t kUnframedMs = 2000;
    // The menu's thread, each drawing of the page: a line to log, or none.
    std::optional<std::string> Drawn(Showing showing, std::uint64_t now_ms);
    // The game's thread: the camera was framed around Link.
    void Framed(std::uint64_t now_ms);

private:
    std::optional<Showing> last_;
    std::uint64_t drawn_ms_ = 0;
    std::uint64_t live_since_ms_ = 0;
    std::optional<std::uint64_t> framed_ms_;
    bool told_unframed_ = false;
};

// The module's one orbit, with your own Link from the co-op hook
// (CoopHook::CurrentMapView). Any thread.
//
// Your own Link can be shown now (the game runs, his place is known, no
// cutscene): the page asks for the live picture, else draws the flat one.
bool LinkShown();
// The menu's thread, each drawing of the live picture.
void Ask(const Window& window);
// hos_coop_view (the game's thread): 1 = eye and target replaced.
bool Camera(double eye[3], double target[3]);

}  // namespace CoopLookView
