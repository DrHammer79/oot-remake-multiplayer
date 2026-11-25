// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy boss fires the owner's shots (Twinrova hand-over; docs/dungeons-boss.md "Shots on the owner's word").
// No emulator code: tested in tests/lichii/boss_native_test.cpp.
//
// Twinrova's bullets are split shots (coop_enemy_shots.h IsSplitShot): every game keeps its own copy boss's bullet,
// and the owner's is not sent. A copy fired only when its own AI ran the owner's move in time. After hand-overs its
// AI is out of phase with the owner's (NEXT49: 4 of 11 copy attacks fired no bullet), so the copy showed the move
// with no shot.
//
// The game fires a shot through its shoot code, main+0xc16cf0 (x0 the shooter record, x1 {actor, flag},
// x2 the shoot values: +0 a string object whose +0 is the bullet's name, +8 Shootable_KeyHash, +0xc a word,
// +0x10 OverrideParamNum). The Shooter component's AI step main+0x2eb07d0 calls it once per record of a map slot.
// Patch 0007's SHOOT_FROM hook gives x1 and x0 at its start.
// - The owner's game keeps each shot its boss fires there, with the record's slot key and index in its own
//   Shooter map, and sends them in the boss's samples (OBJECT_SAMPLE component 25, coop::protocol::BossShots).
// - A copy game calls the same code itself for each of the owner's shots, once, on its own record of that slot,
//   with the same values (built in a block of guest memory). Its own AI's shots of that kind are not made once the
//   owner's first shot has come, so each owner shot is one shot in every game.
// Until the owner's first shot comes (a new copy, an older owner) the copy's own shots are made, as before.
// Hand-over: the new owner's own AI fires from then on; a game made a copy starts again from the new owner's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossShots {

using coop::protocol::BossShot;
using coop::protocol::BossShots;

namespace addr {
// Build e0f3ccfa.
inline constexpr std::uint64_t kShoot = 0xc16cf0;         // main+: the shoot code
inline constexpr std::uint64_t kShootCaller = 0x2eb1150;  // main+: the AI step's return address, at x1 - 0xd0
inline constexpr std::uint64_t kCallerMark = 0xd0;
inline constexpr std::uint64_t kArgValues = 0x68;  // the AI step's x2 is x1 + 0x68 (its own stack)
inline constexpr std::uint64_t kArgFlag = 0x8;     // x1 +8: a byte after the actor
inline constexpr std::uint64_t kValueName = 0x0;   // x2 +0: the string object (its +0 the char*)
inline constexpr std::uint64_t kValueKeyHash = 0x8;
inline constexpr std::uint64_t kValueWord = 0xc;
inline constexpr std::uint64_t kValueParam = 0x10;
// The Shooter component (as coop_shot_reflect.h): actor +0x270, table +0x3d8; its map at +0x20 with +0x2c slots of
// 0x38 bytes keyed by a u32 at +0 (0 empty), +0x8 count, +0x10 records of 0xf8 bytes, +0x20 the slot's key hash.
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentShooter = 0x3d8;
inline constexpr std::uint64_t kShooterMap = 0x20;
inline constexpr std::uint64_t kShooterSlots = 0x2c;
inline constexpr std::uint64_t kSlotSize = 0x38;
inline constexpr std::uint64_t kSlotCount = 0x8;
inline constexpr std::uint64_t kSlotRecords = 0x10;
inline constexpr std::uint64_t kSlotKeyHash = 0x20;
inline constexpr std::uint64_t kRecordSize = 0xf8;
inline constexpr std::uint32_t kMaxSlots = 1024;
inline constexpr std::uint32_t kMaxRecords = 64;
// Our block (made once with the game's malloc): x1 at +0, x2's values at +0x20, the string object at +0x40, the name
// at +0x48.
inline constexpr std::uint64_t kMalloc = 0x14c6c90;  // main+, as coop_boss_native.h
inline constexpr std::uint64_t kBlockValues = 0x20;
inline constexpr std::uint64_t kBlockString = 0x40;
inline constexpr std::uint64_t kBlockName = 0x48;
inline constexpr std::size_t kBlockSize = 0x100;
}  // namespace addr

// The bosses whose shots go this way (Twinrova, as coop_boss_native.h HoldsOwnStarts).
bool Covers(std::string_view boss_name);

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;  // the owning player's number (a new one: the copy starts again)
    std::string name;
};

struct Counts {
    std::uint64_t owner_kept = 0;    // shots our boss fired (sent)
    std::uint64_t copy_stopped = 0;  // a copy's own AI's shots not made (the owner's come)
    std::uint64_t fired = 0;         // owner's shots fired by a copy
    std::uint64_t dropped = 0;       // owner's shots not fired (too many queued, no record, or missed)
    std::uint64_t restarts = 0;      // a copy started again (a new owner)
};

// The planner, without game memory: the owner's shots a copy fires next, those newer than `fired` (0: none since a
// start), oldest first.
std::vector<BossShot> Plan(const BossShots& owner, std::uint32_t fired);

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossShots;
    static constexpr std::size_t kMaxQueued = 16;
    static constexpr std::uint64_t kKeepUs = 500'000;  // an owner shot stays in the samples this long

    // SHOOT_FROM: true when the shoot code was called by the Shooter's AI step (its return address below x1).
    static bool FromAiStep(CoopPuppet::Guest& guest, std::uint64_t args, std::uint64_t main_base);

    void SetOn(bool on);
    bool On() const;

    // The walk's bosses this round. One no longer listed is forgotten; a role or owner change starts it again.
    void Publish(std::span<const Member> members);

    // SHOOT_FROM (patch 0007), on the game's thread: `args` is x1, `record` x0. Ours: a shot of a covered boss
    // fired by its AI step is kept for its samples.
    void OnShoot(CoopPuppet::Guest& guest, std::uint64_t args, std::uint64_t record, std::uint64_t main_base,
                 std::uint64_t now_us);
    // CREATE_REQUEST of the shoot code's own create (not ours), on the game's thread: true when `shooter`'s shot
    // named `name` is not made (a copy that fires the owner's shots). Only a shot of the AI step (`from_ai_step`,
    // FromAiStep at its SHOOT_FROM): the owner sends only those, so a shot from the code's other callers is made.
    bool StopsOwn(std::uint64_t shooter, std::string_view name, bool from_ai_step);

    // Ours: the shots this boss fired lately, for its samples (nothing: none, or not ours).
    std::optional<BossShots> OwnerShots(std::uint64_t actor, std::uint64_t now_us);
    // A copy's: the owner's shots from its samples (coop_objects_sync.cpp BossShotsIn).
    void Receive(std::uint64_t actor, const BossShots& shots);

    // The caller (coop_callers.h contract): the owner's shots fired by our copies, through the game's shoot code.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);

    // A new game process: every boss, the block and any call in flight are dropped.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Kept {
        BossShot shot;
        std::uint64_t at_us = 0;
    };
    struct Entry {
        Member member;
        std::uint32_t seq = 0;           // ours: the newest shot's seq (never goes back in this game)
        std::deque<Kept> mine;           // ours: the newest shots
        bool owner_came = false;         // a copy: the owner's shots have come (its own are not made)
        std::uint32_t fired = 0;         // a copy: the newest owner seq fired or dropped (0: start again)
        std::deque<BossShot> queue;      // a copy: the owner's shots to fire, oldest first
    };
    void Note(std::string text);
    void DropGame();  // (mutex_ held)
    // The newest seq queued or fired for a copy.
    static std::uint32_t Through(const Entry& e);
    // Our record of `actor`'s Shooter for the owner's slot key (else its key hash) and index (0: none).
    static std::uint64_t FindRecord(CoopPuppet::Guest& guest, std::uint64_t actor, const BossShot& shot);
    // The slot key and record index of `record` in `actor`'s Shooter map.
    static bool FindSlot(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint64_t record, std::uint32_t& key,
                         std::uint16_t& index);

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool malloc_failed_ = false;
    enum class Pending : std::uint8_t { none, malloc, fire } pending_ = Pending::none;
    std::uint64_t pending_actor_ = 0;
};

Table& TheTable();

}  // namespace CoopBossShots
