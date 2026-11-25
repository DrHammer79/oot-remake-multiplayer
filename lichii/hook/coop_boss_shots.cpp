// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy boss fires the owner's shots (Twinrova hand-over): coop_boss_shots.h.
#include "coop_boss_shots.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include "coop_enemy_shots.h"

namespace CoopBossShots {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%#llx", static_cast<unsigned long long>(v));
    return text;
}

template <typename T>
bool Get(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return address != 0 && guest.Read(address, &out, sizeof out);
}

// The Shooter's map of `actor`: its slots' base and count (false: none).
bool ShooterMap(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint64_t& map, std::uint32_t& slots) {
    std::uint64_t table = 0, shooter = 0;
    slots = 0;
    return actor != 0 && Get(guest, actor + addr::kActorComponents, table) && table != 0 &&
           Get(guest, table + addr::kComponentShooter, shooter) && shooter != 0 &&
           Get(guest, shooter + addr::kShooterMap, map) && map != 0 &&
           Get(guest, shooter + addr::kShooterSlots, slots) && slots != 0 && slots <= addr::kMaxSlots;
}

}  // namespace

bool Covers(std::string_view boss_name) { return boss_name == "Enemy_DungeonBoss_Twinrova"; }

std::vector<BossShot> Plan(const BossShots& owner, std::uint32_t fired) {
    std::vector<BossShot> out;
    for (const BossShot& s : owner.recent) {
        if (s.seq > fired) {
            out.push_back(s);
        }
    }
    return out;
}

void Table::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
    if (!on) {
        entries_.clear();
    }
}

bool Table::On() const {
    std::lock_guard lock(mutex_);
    return on_;
}

void Table::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        if (!Covers(m.name)) {
            continue;
        }
        auto it = entries_.find(m.actor);
        Entry e = it != entries_.end() ? std::move(it->second) : Entry{};
        if (it != entries_.end() && (e.member.role != m.role || e.member.owner != m.owner)) {
            Note(m.name + ": shots: " +
                 (m.role == Role::owner ? std::string("ours now: our own AI fires")
                                        : "player " + std::to_string(m.owner) + "'s now: its shots come"));
            counts_.dropped += e.queue.size();
            e.queue.clear();
            e.owner_came = false;
            e.fired = 0;
            e.mine.clear();  // (a copy's kept shots are none; ours are no longer sent)
            ++counts_.restarts;
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

bool Table::FindSlot(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint64_t record, std::uint32_t& key,
                     std::uint16_t& index) {
    std::uint64_t map = 0;
    std::uint32_t slots = 0;
    if (record == 0 || !ShooterMap(guest, actor, map, slots)) {
        return false;
    }
    for (std::uint32_t s = 0; s < slots; ++s) {
        const std::uint64_t slot = map + s * addr::kSlotSize;
        std::uint32_t k = 0, count = 0;
        std::uint64_t records = 0;
        if (!Get(guest, slot, k) || k == 0 || !Get(guest, slot + addr::kSlotCount, count) ||
            !Get(guest, slot + addr::kSlotRecords, records) || records == 0 || record < records) {
            continue;
        }
        const std::uint64_t i = (record - records) / addr::kRecordSize;
        if (i < std::min(count, addr::kMaxRecords) && records + i * addr::kRecordSize == record) {
            key = k;
            index = static_cast<std::uint16_t>(i);
            return true;
        }
    }
    return false;
}

std::uint64_t Table::FindRecord(CoopPuppet::Guest& guest, std::uint64_t actor, const BossShot& shot) {
    std::uint64_t map = 0;
    std::uint32_t slots = 0;
    if (!ShooterMap(guest, actor, map, slots)) {
        return 0;
    }
    // By the slot's key; else (another game's key) by the slot's key hash, as the shoot values name it.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint32_t s = 0; s < slots; ++s) {
            const std::uint64_t slot = map + s * addr::kSlotSize;
            std::uint32_t k = 0, hash = 0, count = 0;
            std::uint64_t records = 0;
            if (!Get(guest, slot, k) || k == 0) {
                continue;
            }
            if (pass == 0 ? k != shot.key : (!Get(guest, slot + addr::kSlotKeyHash, hash) || hash != shot.key_hash)) {
                continue;
            }
            if (!Get(guest, slot + addr::kSlotCount, count) || !Get(guest, slot + addr::kSlotRecords, records) ||
                records == 0 || shot.index >= std::min(count, addr::kMaxRecords)) {
                return 0;
            }
            return records + shot.index * addr::kRecordSize;
        }
    }
    return 0;
}

void Table::OnShoot(CoopPuppet::Guest& guest, std::uint64_t args, std::uint64_t record, std::uint64_t main_base,
                    std::uint64_t now_us) {
    std::uint64_t actor = 0;
    if (args == 0 || main_base == 0 || !Get(guest, args, actor) || actor == 0) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(actor);
        if (!on_ || it == entries_.end() || it->second.member.role != Role::owner) {
            return;
        }
    }
    if (!FromAiStep(guest, args, main_base)) {
        return;
    }
    const std::uint64_t values = args + addr::kArgValues;
    BossShot s;
    std::uint64_t string = 0, text = 0;
    std::array<char, coop::protocol::kMaxBossShotChars + 1> name{};
    if (!Get(guest, values + addr::kValueName, string) || !Get(guest, string, text) || text == 0 ||
        !guest.Read(text, name.data(), name.size() - 1)) {
        return;
    }
    s.name = std::string(name.data(), strnlen(name.data(), name.size() - 1));
    if (!coop::protocol::ValidShotName(s.name) || !CoopEnemyShots::IsSplitShot(s.name)) {
        return;  // (another shot of its: sent as an object of its own, coop_enemy_shots.h)
    }
    if (!Get(guest, values + addr::kValueKeyHash, s.key_hash) || !Get(guest, values + addr::kValueWord, s.w4) ||
        !Get(guest, values + addr::kValueParam, s.param) || !Get(guest, args + addr::kArgFlag, s.flag) ||
        !FindSlot(guest, actor, record, s.key, s.index)) {
        return;
    }
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.member.role != Role::owner) {
        return;
    }
    Entry& e = it->second;
    s.seq = ++e.seq;
    e.mine.push_back({s, now_us});
    while (e.mine.size() > coop::protocol::kMaxBossShots) {
        e.mine.pop_front();
    }
    ++counts_.owner_kept;
}

bool Table::FromAiStep(CoopPuppet::Guest& guest, std::uint64_t args, std::uint64_t main_base) {
    // The AI step's own call: its return address sits below x1 on its stack, x2 at x1 + 0x68.
    std::uint64_t caller = 0;
    return args != 0 && Get(guest, args - addr::kCallerMark, caller) && caller == main_base + addr::kShootCaller;
}

bool Table::StopsOwn(std::uint64_t shooter, std::string_view name, bool from_ai_step) {
    if (!from_ai_step) {
        return false;  // (the owner sends only its AI step's shots: this one comes from no one)
    }
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(shooter);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy || !it->second.owner_came ||
        !CoopEnemyShots::IsSplitShot(name)) {
        return false;
    }
    ++counts_.copy_stopped;
    return true;
}

std::optional<BossShots> Table::OwnerShots(std::uint64_t actor, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.member.role != Role::owner) {
        return std::nullopt;
    }
    Entry& e = it->second;
    while (!e.mine.empty() && now_us - e.mine.front().at_us > kKeepUs) {
        e.mine.pop_front();
    }
    if (e.mine.empty()) {
        return std::nullopt;
    }
    BossShots out;
    out.seq = e.seq;
    for (const Kept& k : e.mine) {
        out.recent.push_back(k.shot);
    }
    return out;
}

std::uint32_t Table::Through(const Entry& e) { return e.queue.empty() ? e.fired : e.queue.back().seq; }

void Table::Receive(std::uint64_t actor, const BossShots& shots) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy || shots.recent.empty()) {
        return;
    }
    Entry& e = it->second;
    if (!e.owner_came) {
        e.owner_came = true;
        Note(e.member.name + ": shots: the owner's came: our own AI's are not made");
    }
    if (shots.seq < Through(e)) {
        // An owner whose count is behind ours: another game's count (a hand-over we have not published yet).
        counts_.dropped += e.queue.size();
        e.queue.clear();
        e.fired = 0;
        ++counts_.restarts;
    }
    const std::uint32_t through = Through(e);
    if (through != 0 && shots.recent.front().seq > through + 1) {
        counts_.dropped += shots.recent.front().seq - through - 1;  // (missed: older than the owner's list)
    }
    for (const BossShot& s : Plan(shots, through)) {
        if (e.queue.size() >= kMaxQueued) {
            ++counts_.dropped;
            e.fired = s.seq;  // (not fired, never asked again)
            continue;
        }
        e.queue.push_back(s);
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        if (main_base_ != 0) {
            Note("shots: a new game: block, bosses and call in flight dropped");
            DropGame();
        }
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        malloc_failed_ = false;
        if (pending_ == Pending::malloc) {
            allocating_ = false;
        }
        pending_ = Pending::none;  // (last frame's call was not made: a malloc is asked again, a shot stays queued)
    }
    if (after_call) {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                Note("shots: block at " + Hex(result));
            } else {
                malloc_failed_ = true;
            }
            break;
        case Pending::fire:
            if (const auto it = entries_.find(pending_actor_); it != entries_.end() && !it->second.queue.empty()) {
                it->second.fired = it->second.queue.front().seq;
                it->second.queue.pop_front();
                ++counts_.fired;
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (!on_ || main_base == 0) {
        return std::nullopt;
    }
    for (auto& [actor, e] : entries_) {
        if (e.member.role != Role::copy || e.queue.empty()) {
            continue;
        }
        if (block_ == 0) {
            if (allocating_ || malloc_failed_) {
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = main_base + addr::kMalloc;
            c.x[0] = addr::kBlockSize;
            allocating_ = true;
            pending_ = Pending::malloc;
            return c;
        }
        const BossShot& s = e.queue.front();
        const std::uint64_t record = FindRecord(guest, actor, s);
        if (record == 0) {
            Note(e.member.name + ": shots: no record of slot " + Hex(s.key) + " index " + std::to_string(s.index) +
                 " for " + s.name + ": not fired");
            e.fired = s.seq;
            e.queue.pop_front();
            ++counts_.dropped;
            continue;
        }
        std::array<std::uint8_t, addr::kBlockSize> block{};
        const auto put = [&](std::uint64_t at, const void* v, std::size_t n) {
            std::memcpy(block.data() + at, v, n);
        };
        const std::uint64_t string = block_ + addr::kBlockString;
        const std::uint64_t text = block_ + addr::kBlockName;
        put(0, &actor, sizeof actor);
        put(addr::kArgFlag, &s.flag, sizeof s.flag);
        put(addr::kBlockValues + addr::kValueName, &string, sizeof string);
        put(addr::kBlockValues + addr::kValueKeyHash, &s.key_hash, sizeof s.key_hash);
        put(addr::kBlockValues + addr::kValueWord, &s.w4, sizeof s.w4);
        put(addr::kBlockValues + addr::kValueParam, &s.param, sizeof s.param);
        put(addr::kBlockString, &text, sizeof text);
        put(addr::kBlockName, s.name.data(), s.name.size());  // (ends in the block's zeros)
        if (!guest.Write(block_, block.data(), block.size())) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        c.function = main_base + addr::kShoot;
        c.x[0] = record;
        c.x[1] = block_;
        c.x[2] = block_ + addr::kBlockValues;
        c.copy_game = false;  // (our own shot: its create is never stopped)
        pending_ = Pending::fire;
        pending_actor_ = actor;
        return c;
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    block_ = 0;
    allocating_ = false;
    malloc_failed_ = false;
    pending_ = Pending::none;
    pending_actor_ = 0;
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    DropGame();
    main_base_ = 0;
}

Counts Table::GetCounts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

Table& TheTable() {
    static Table table;
    return table;
}

}  // namespace CoopBossShots
