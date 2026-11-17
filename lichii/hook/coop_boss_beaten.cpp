// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_beaten.h"

#include <algorithm>
#include <variant>

#include "coop_boss.h"

namespace CoopBossBeaten {

namespace {

#include "coop_boss_beaten.inc"

bool Has(std::span<const std::uint8_t> rooms, std::uint8_t room) {
    return std::find(rooms.begin(), rooms.end(), room) != rooms.end();
}

} // namespace

std::span<const BeatenSet> Sets() {
    return kSets;
}

const BeatenSet* ForRoom(std::uint8_t room) {
    for (const BeatenSet& s : kSets) {
        if (s.room == room) {
            return &s;
        }
    }
    return nullptr;
}

bool Record::Add(std::uint8_t room) {
    if (ForRoom(room) == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (Has(rooms_, room)) {
        return false;
    }
    rooms_.push_back(room);
    return true;
}

void Record::Clear() {
    std::lock_guard lock(mutex_);
    rooms_.clear();
}

std::vector<std::uint8_t> Record::Rooms() const {
    std::lock_guard lock(mutex_);
    return rooms_;
}

std::optional<std::uint8_t> ProvenDeath(const coop::protocol::Message& m) {
    namespace proto = coop::protocol;
    const auto in_its_dungeon = [](std::uint8_t room, const std::string& scene) -> std::optional<std::uint8_t> {
        const CoopBoss::Room* where = CoopBoss::RoomByIndex(room);
        if (ForRoom(room) == nullptr || where == nullptr || where->scene != scene) {
            return std::nullopt;
        }
        return room;
    };
    const auto* s = std::get_if<proto::BossScene>(&m);
    if (s == nullptr ||
        (s->kind != proto::BossSceneKind::outro_start && s->kind != proto::BossSceneKind::outro_end)) {
        return std::nullopt;  // never BOSS_ROOM defeated: a game also sends it on a guess
    }
    return in_its_dungeon(s->boss, s->scene);
}

Record& TheRecord() {
    static Record record;
    return record;
}

std::vector<Write> Apply::Check(const std::optional<std::string>& scene, bool place_known,
                                std::span<const std::uint8_t> beaten,
                                const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read) {
    if (!scene) {
        scene_.reset();  // loading: the scene after it (the same one too) checks again
        loaded_.clear();
        checked_.clear();
        return {};
    }
    if (!scene_ || *scene_ != *scene) {
        scene_ = scene;
        loaded_.assign(beaten.begin(), beaten.end());  // a later record change waits for the next load
        checked_.clear();
    }
    if (!place_known) {
        return {};
    }
    std::vector<Write> out;
    for (const std::uint8_t room : loaded_) {
        if (!Has(beaten, room)) {
            continue;  // the record was cleared since (the host loaded a save): that boss fights again
        }
        const BeatenSet* set = ForRoom(room);
        const CoopBoss::Room* where = CoopBoss::RoomByIndex(room);
        if (set == nullptr || where == nullptr || Has(checked_, room) || where->scene == *scene) {
            continue;  // in the boss's own scene: its room's logic is loaded (maybe a fight); the next scene gives it
        }
        checked_.push_back(room);
        for (const BeatenBool& b : set->bools) {
            const std::optional<std::uint32_t> now = read(b.hash);
            if (now && (*now != 0) != b.value) {
                out.push_back(Write{room, b.hash, false, b.value ? 1u : 0u, b.name});
            }
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (step && std::find(set->below.begin(), set->below.end(), *step) != set->below.end()) {
            out.push_back(Write{room, set->quest, true, set->step, set->quest_name});
        }
    }
    return out;
}

void Apply::Reset() {
    scene_.reset();
    loaded_.clear();
    checked_.clear();
}

} // namespace CoopBossBeaten
