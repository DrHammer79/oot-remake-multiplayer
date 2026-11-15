// SPDX-License-Identifier: GPL-3.0-or-later
// Full-body pose copy for puppets (docs/puppet.md, "Pose").
//
// Each game frame, on the game's calc thread (the puppet cave's SVC), the
// local Link's skeleton is read: every bone's local transform as the game's
// animation left it. The network thread sends the bones that move as
// PLAYER_POSE at most kSendHz times a second. A received pose is kept per
// player; each frame the newest two are blended and written onto that
// player's puppet skeleton.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <map>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopPuppet {
class Guest;
}

namespace CoopPose {

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec3 = std::array<float, 3>;
using Rows = std::array<float, 9>;  // 3x3 rotation as the game stores it, rows

// Read from the running game with the emulator's debug server (2026-11-09,
// Kakariko, child): see docs/puppet.md, Pose.
namespace addr {
// Actor -> skeleton object: read a pointer at each offset in turn. The model
// object sits inline at +0x120 of the fourth, its skeleton pointer at +0x20.
inline constexpr std::array<std::uint64_t, 5> kSkeletonPath{0x270, 0x8, 0x18, 0x18, 0x140};
inline constexpr std::uint64_t kModelResource = 0x120;  // of the fourth: "FMDL"
inline constexpr std::uint64_t kModelName = 0x8;  // FMDL: pointer to u16 length + chars
inline constexpr std::uint64_t kBoneCount = 0x48;       // u16 in the skeleton object
inline constexpr std::uint64_t kLocalArray = 0x18;      // pointer to the local transforms
inline constexpr std::uint64_t kLocalStride = 0x60;
// One local transform: u32 flags (bits 23 to 30 change with the pose),
// scale (3 floats at +0x10), rotation (3 rows of 4 floats at +0x20, the 4th
// of each 0) and translation (3 floats at +0x50).
inline constexpr std::uint32_t kFlagShift = 23;
inline constexpr std::uint32_t kFlagMask = 0xffu << kFlagShift;
}  // namespace addr

inline constexpr std::size_t kMaxBones = 512;
// Link's pose goes 15 times a second (30 before the 16-player work); the
// receiver's Track blends the last two, so it still plays smooth.
inline constexpr int kSendHz = 15;
// At most this many of Link's bones go in one pose (about 300 move). First up
// to kRefreshBones of those unsent for kBoneRefreshMs or longer, oldest first
// (a lost or thinned pose, a late joiner: every bone goes again within about
// kBoneRefreshMs + 300 / kRefreshBones sends, ~3 s), then the ones changed most
// since they were last sent. The receiver keeps the rest from its newest pose
// (Track::Fill).
// About 48 x 18 bytes x 15/s = 13 KB/s per player, where every moving bone at
// 30/s was about 160 KB/s.
inline constexpr std::size_t kMaxSendBones = 48;
inline constexpr std::uint64_t kBoneRefreshMs = 500;
inline constexpr std::size_t kRefreshBones = 8;
// A bone is changed once a packed part moved more than this (in the packed
// units: 1/32767 of a quaternion part, 1/kPoseUnit of a translation or scale).
inline constexpr int kBoneStep = 8;
// A part counts as moving once it differs from the first read by more than this.
inline constexpr float kMoved = 1e-4f;

// Held pieces (issue 158). The pieces in a Link's hands have skeletons of
// their own that the game animates apart from Link's: the slingshot's band
// pulled back with a seed in it, then let go when it shoots; the bow's string.
// The puppet's copy of the piece is not animated by anyone, so it stayed
// pulled back after the shot. Each piece out in a hand slot below (the
// EquipmentUser's hand slots: 1 SubWeapon, 2 ThirdWeapon, 3 Bow, 5 Tool,
// 6 SubTool; not the sword or shield) is sent as a PLAYER_POSE of its own
// skeleton, every bone, and written onto the piece of that skeleton in the
// puppet's hands. No new message: the skeleton id tells the poses apart.
inline constexpr std::array<std::uint32_t, 5> kPieceSlots{1, 2, 3, 5, 6};
inline constexpr std::uint32_t kMaxPieceBones = 64;      // bigger: not sent
inline constexpr std::uint64_t kPieceRefreshMs = 250;    // an unchanged piece, again
inline constexpr std::uint64_t kTrackForgetMs = 5000;    // a skeleton not heard from
inline constexpr std::size_t kMaxTracks = 8;             // skeletons kept per player

// One bone of a pose: the parts that move.
struct BonePose {
    std::uint16_t bone = 0;
    std::uint8_t flags = 0;
    std::optional<Quat> rotation;
    std::optional<Vec3> translation;
    std::optional<Vec3> scale;
};

struct Pose {
    std::uint32_t skeleton = 0;
    std::vector<BonePose> bones;  // increasing bone order
};

Quat RowsToQuat(const Rows& m);  // normalised, w >= 0
Rows QuatToRows(const Quat& q);  // q normalised first; identity if it is 0
std::array<std::int16_t, 4> Pack(const Quat& q);
Quat Unpack(const std::array<std::int16_t, 4>& q);
std::array<std::int16_t, 3> PackUnit(const Vec3& v);  // scaled by protocol::kPoseUnit
Vec3 UnpackUnit(const std::array<std::int16_t, 3>& v);
// Shortest-path normalised lerp from a to b; t in [0, 1].
Quat Nlerp(const Quat& a, const Quat& b, float t);

// A skeleton's id, sent with every pose: the bone count in the low 9 bits and
// a hash of the model's name ("Player_Child", "Player_Adult") above it, so a
// puppet applies only a pose of its own model.
std::uint32_t SkeletonId(std::uint32_t bone_count, std::string_view model_name);
std::uint32_t BoneCountOf(std::uint32_t skeleton);
// The name of the actor's model ("Player_Child", "Player_Adult") when it has
// a skeleton and an FMDL model resource; nothing when not (yet).
std::optional<std::string> ModelNameOf(CoopPuppet::Guest& guest, std::uint64_t actor);

// jitter-3: a pose with its sample time is shown at the place's own time:
// presence shows the place Settings::show_delay_us (100 ms) behind the server
// clock, and the pose was blended by its ARRIVAL, about one interval behind:
// the limbs ran ahead of the body's place and played unevenly with the
// network's jitter. Timed poses are kept (kKeepPoses, 0.5 s at 15 Hz) and the
// two around now - kShowDelayMs blended.
inline constexpr std::uint64_t kShowDelayMs = 100;  // coop/presence Settings::show_delay_us
inline constexpr std::size_t kKeepPoses = 8;

// idle-twitch (owner: "other puppets still twitch a little while standing still"). About 300 of
// Link's bones move and a pose carries at most kMaxSendBones (48), so while he stands breathing
// (every bone a little, none much) each bone goes about every 300-500 ms, by age. The receiver
// held it between and blended its whole step over the one pose interval it came in (66 ms):
// every limb moved in small jerks a few times a second. Now a bone sent again after more than
// kRampAfterMs whose step is slow (under kRampRad / kRampUnit a second: idle breathing, not a
// real move) is eased to its new value over the time since it was last sent, from where it was
// shown (it reaches it that much later; a slow bone's lag is a fraction of a degree). A fast step
// (a real move: the bone is sent again at once, by its change) is shown as before.
inline constexpr std::uint64_t kRampAfterMs = 100;  // 1.5 pose intervals at kSendHz
inline constexpr std::uint64_t kRampMaxMs = 600;    // kBoneRefreshMs and a pose
inline constexpr float kRampRad = 0.35f;            // ~20 degrees a second
inline constexpr float kRampUnit = 0.2f;            // a translation or scale part, a second

// The newest poses of one remote player, and the blend shown now.
class Track {
public:
    // `made_ms`: when the sender's game made it, on our steady clock (the
    // sample time through the clock offset); 0: not known, shown by arrival.
    void Push(Pose pose, std::uint64_t now_ms, std::uint64_t made_ms = 0);
    // idle-twitch: a received pose (the bones sent) filled (Fill), its slow bones eased
    // (kRampAfterMs), and pushed. Returns how many bones are being eased.
    std::size_t Add(Pose sent, std::uint64_t now_ms, std::uint64_t made_ms);
    // The pose to show at now_ms. Timed: the one at now_ms - delay_ms (the
    // player's delay, coop_show_delay.h; report 37),
    // the two around it blended. Untimed: one interval behind, the last two
    // blended, so a 30 Hz stream plays smooth at 60 frames a second.
    std::optional<Pose> At(std::uint64_t now_ms, std::uint64_t delay_ms = kShowDelayMs) const;
    std::uint64_t last_ms() const { return newest_ms_; }
    bool Timed() const { return !timed_.empty(); }
    std::size_t Eased() const { return ramps_.size(); }  // idle-twitch: slow bones being eased
    // `pose` (some bones of this track's skeleton: the sender sends only those
    // that changed most) with the bones it lacks taken from the newest pose.
    Pose Fill(Pose pose) const;

private:
    std::optional<Pose> older_;
    std::optional<Pose> newest_;
    std::uint64_t older_ms_ = 0;
    std::uint64_t newest_ms_ = 0;
    std::vector<std::pair<std::uint64_t, Pose>> timed_;  // by made time, oldest first
    // idle-twitch: when each bone was last sent (made time), and the slow ones being eased.
    struct Ramp {
        BonePose from;
        BonePose to;
        std::uint64_t start_ms = 0;
        std::uint64_t span_ms = 0;
    };
    std::map<std::uint16_t, std::uint64_t> sent_ms_;
    std::map<std::uint16_t, Ramp> ramps_;
};

// jitter-3's proof number: how even the shown pose's change is from frame to
// frame (the summed bone turn), as CoopFollow::Steps reads the place: the mean
// |1 - change / last change| over frames that both moved; 0 is even.
class Evenness {
public:
    // A report every kEvenReportMs, else nothing.
    std::optional<float> Step(const Pose& shown, std::uint64_t now_ms, int* frames = nullptr);

private:
    std::optional<Pose> last_;
    float last_change_ = 0.0f;
    float sum_ = 0.0f;
    int moving_ = 0;
    std::uint64_t start_ms_ = 0;
};
inline constexpr std::uint64_t kEvenReportMs = 5000;
inline constexpr float kEvenMovingRad = 0.02f;  // summed over the bones

// The sender's side of kMaxSendBones: which of a full pose's bones to send now.
class BonePicker {
public:
    // `full`: every moving bone, increasing order. Returns at most `budget`
    // of them, increasing order, and remembers them as sent at now_ms.
    std::vector<coop::protocol::PoseBone> Pick(std::uint32_t skeleton,
                                               const std::vector<coop::protocol::PoseBone>& full,
                                               std::uint64_t now_ms,
                                               std::size_t budget = kMaxSendBones);
    // How far `now` is from `then`, in packed units; a part come or gone is far.
    static int Change(const coop::protocol::PoseBone& then, const coop::protocol::PoseBone& now);

private:
    struct Sent {
        coop::protocol::PoseBone bone;
        std::uint64_t ms = 0;
    };
    std::uint32_t skeleton_ = 0;
    std::map<std::uint16_t, Sent> sent_;
};

// The local player's actor, as coop::host::PlayerReader finds it; 0 none.
std::uint64_t FindPlayer(CoopPuppet::Guest& guest, std::uint64_t main_base);

// Calc thread: copy every bone's local transform of `from`'s skeleton onto
// `to`'s, when both skeletons are found and have the same bone count. For
// the fairies: the local Navi's wing beat onto each puppet's Navi, whose own
// actor plays no animation. `note` (may be null) gets both models and bone
// counts, or why nothing was copied.
bool CopySkeleton(CoopPuppet::Guest& guest, std::uint64_t from, std::uint64_t to,
                  std::string* note);

// Calc thread, once a frame: read the local player's skeleton and write the
// shown pose onto each live puppet. puppets: (player number, actor); the
// debug puppet (number 0) copies the local player's own pose.
void OnCalcFrame(CoopPuppet::Guest& guest, std::uint64_t main_base,
                 const std::vector<std::pair<std::uint8_t, std::uint64_t>>& puppets,
                 std::uint64_t now_ms);
// Network thread: send the newest local pose when it is time.
void Send(coop::net::Client& client, std::uint64_t now_ms);
// What Send sends now: Link's pose, then each held piece that changed (or
// every kPieceRefreshMs while it holds still). Empty when it is not time.
std::vector<coop::protocol::PlayerPose> TakeOutgoing(std::uint64_t server_now_us,
                                                     std::uint64_t now_ms);
// Network thread: keep a received PLAYER_POSE.
void OnEvent(const coop::net::Event& event, std::uint64_t now_ms);
void Receive(const coop::protocol::PlayerPose& pose, std::uint64_t now_ms);
// jitter-3: the client's server clock at our steady `now_ms` (Send sets it);
// received poses are then shown at their sample time (Track::Push made_ms).
void SetClock(std::uint64_t server_now_us, std::uint64_t now_ms);
// LICHII_COOP_DEBUG_POSE=1: a line a second with bone counts and rates.
// The pose copy wins over the AS commands: a puppet whose poses arrive plays
// no AS command of its own (see Drives). LICHII_COOP_POSE_AS=1 (as_too) plays
// them as well, as before the pose copy.
void SetLog(std::function<void(const std::string&)> log, bool debug, bool as_too = false);
// Calc thread: whether the pose copy drives this player's puppet now (poses
// arrived in the last kFreshMs), so the director plays no AS command on it.
bool Drives(std::uint8_t number, std::uint64_t now_ms);
inline constexpr std::uint64_t kFreshMs = 1000;
// Calc thread, after OnCalcFrame: whether a body pose of this player was
// written on its puppet in the last kFreshMs (puppet-anim, issue #36/#41).
// Poses that arrive but are not written (another skeleton: age, model) or a
// stream that stopped leave the puppet to its AS commands, never frozen in
// the last pose written. The director goes by Drives && Writes.
bool Writes(std::uint8_t number, std::uint64_t now_ms);

}  // namespace CoopPose
