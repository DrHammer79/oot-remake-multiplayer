// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+ (coop_ngplus.h has the rules).

#include "coop_ngplus.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop/host/game_flags.hpp"
#include "coop/protocol/difficulty.hpp"
#include "coop_every.h"
#include "coop_flags.h"
#include "coop_live_events.h"

namespace CoopNgPlus {
namespace {

namespace fs = std::filesystem;
namespace proto = coop::protocol;

constexpr std::uint32_t kMaxLifeHash = 0xfbe01da1u;  // PlayerStatus.MaxLife (docs/items.md), quarter hearts
constexpr std::uint64_t kBeatenAgainUs = 600'000'000;  // one Ganon death a 10 minutes (the event is asked more than once)

struct Glue {
    std::mutex mutex;
    Saved saved;
    std::once_flag read;                   // ReadOnce, once (any thread)
    std::atomic<std::uint32_t> packed{0};  // level | unlocked << 8 | hearts << 16, for any thread
    std::uint64_t cursor = 0;              // the game's event requests read so far (the hook's thread)
    std::uint64_t beaten_at = 0;
    std::int32_t hearts_said = -1;
    bool test_done = false;
    bool new_file = false;    // this file began with the game's opening (IsNewFileOpening) since the title
    bool was_in_game = false;
    std::vector<std::string> lines;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

fs::path File() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-ngplus.ini";
}

void Publish(Glue& g) {
    g.packed.store(static_cast<std::uint32_t>(g.saved.level) | static_cast<std::uint32_t>(g.saved.unlocked) << 8 |
                   static_cast<std::uint32_t>(std::clamp(g.saved.hearts, 0, kMostQuarters)) << 16);
}

// Once (Ready's call_once), under the mutex.
void ReadOnce(Glue& g) {
    std::ifstream f(File(), std::ios::binary);
    if (f) {
        std::ostringstream text;
        text << f.rdbuf();
        g.saved = Parse(text.str());
    }
    Publish(g);
    LOG_INFO(Core, "coop: New Game+: level {}, unlocked {}, hearts {} (quarters)", g.saved.level, g.saved.unlocked,
             g.saved.hearts);
}

// Under the mutex. Written whole: a new file, then moved over the old one.
void Keep(Glue& g) {
    Publish(g);
    const fs::path file = File();
    fs::path tmp = file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << Format(g.saved)) || !f.flush()) {
            LOG_ERROR(Core, "coop: New Game+: could not write {}", tmp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        LOG_ERROR(Core, "coop: New Game+: could not write {}", file.string());
    }
}

Glue& Ready() {
    // Any thread (the hook's, the host menu's RunHost, the pause menu): call_once, so no thread reads a half-read
    // file or races the first read (review of PR 236).
    Glue& g = TheGlue();
    std::call_once(g.read, [&g] {
        std::lock_guard lock(g.mutex);
        ReadOnce(g);
    });
    return g;
}

std::string Hearts(std::int32_t quarters) {
    const std::int32_t whole = quarters / 4, part = quarters % 4;
    return std::to_string(whole) + (part ? " " + std::to_string(part) + "/4" : std::string()) + " hearts";
}

}  // namespace

std::uint8_t Level() {
    return static_cast<std::uint8_t>(Ready().packed.load() & 0xff);
}

std::uint8_t Unlocked() {
    return static_cast<std::uint8_t>((Ready().packed.load() >> 8) & 0xff);
}

std::int32_t Hearts() {
    return static_cast<std::int32_t>(Ready().packed.load() >> 16);
}

bool SetLevel(int level) {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    const Saved now = Choose(g.saved, level);
    if (now == g.saved) {
        return false;
    }
    g.saved = now;
    Keep(g);
    const proto::DifficultyValues v = proto::ngplus_difficulty({}, now.level);
    LOG_INFO(Core, "coop: New Game+: level {} chosen", now.level);
    if (g.lines.size() < 16) {
        g.lines.push_back(now.level == 0 ? std::string("New Game+ off")
                                         : "New Game+ " + std::to_string(now.level) + ": enemies " +
                                               std::to_string(v.hp_pct) + "% life, " + std::to_string(v.damage_pct) +
                                               "% damage; you start with " + Hearts(now.hearts));
    }
    return true;
}

void Frame(CoopHook::EmulatorMemory& memory, bool in_game, bool may_give) {
    Glue& g = Ready();
    const std::uint64_t now_us = CoopEvery::NowUs();
    bool beaten = false;
    for (const auto& r : CoopLiveEvents::TheGameRequests().Since(g.cursor)) {
        if (r.taken && IsGanonDeath(r.name)) {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon's death seen ({})", r.name);
        }
        if (r.taken && IsNewFileOpening(r.name) && !g.new_file) {
            g.new_file = true;
            LOG_INFO(Core, "coop: New Game+: a new file ({}): the kept hearts may go into it", r.name);
        }
    }
    // Back on the title: the next file is another one (a loaded save starts after the opening, so it gets nothing).
    if (g.was_in_game && !in_game) {
        g.new_file = false;
    }
    g.was_in_game = in_game;
    // Tests only (OOT_COOP_TEST=1, as the server's test switches): Ganon counted beaten once Link is in a save.
    if (!g.test_done && in_game) {
        g.test_done = true;
        const char* test = std::getenv("OOT_COOP_TEST");
        const char* t = std::getenv("OOT_COOP_TEST_NGPLUS_BEAT");
        if (test != nullptr && std::string_view(test) == "1" && t != nullptr && t[0] == '1') {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon counted beaten (OOT_COOP_TEST_NGPLUS_BEAT, tests only)");
        }
    }
    coop::host::GameFlagStore store(memory);
    const proto::FlagKey max_key{kMaxLifeHash, 0};
    const auto max_life = in_game ? store.read(max_key) : std::nullopt;
    const bool known = max_life && max_life->kind == proto::FlagKind::int32;
    if (beaten && (g.beaten_at == 0 || now_us - g.beaten_at >= kBeatenAgainUs)) {
        g.beaten_at = now_us;
        std::lock_guard lock(g.mutex);
        const Saved was = g.saved;
        g.saved = Beaten(g.saved, known ? static_cast<std::int32_t>(max_life->value) : 0);
        Keep(g);
        LOG_INFO(Core, "coop: New Game+: Ganon beaten at level {}: unlocked {}, hearts {}", was.level, g.saved.unlocked,
                 g.saved.hearts);
        if (g.saved.unlocked != was.unlocked && g.lines.size() < 16) {
            g.lines.push_back("New Game+ " + std::to_string(g.saved.unlocked) + " unlocked, with " +
                              Hearts(g.saved.hearts) + ": Multiplayer, Settings, New Game+, then start a new file");
        }
    }
    if (!in_game) {
        return;
    }
    static CoopEvery::Every every{1'000'000};
    if (!every.Due(now_us)) {
        return;
    }
    const std::uint32_t packed = g.packed.load();
    Give give;
    give.level = static_cast<std::uint8_t>(packed & 0xff);
    give.hearts = static_cast<std::int32_t>(packed >> 16);
    give.max_life = known ? std::optional<std::int32_t>(static_cast<std::int32_t>(max_life->value)) : std::nullopt;
    give.new_file = g.new_file;
    give.may_give = may_give;
    const auto to = HeartsToGive(give);
    if (!to) {
        return;
    }
    // The save's MaxLife through the game's own Int setter (GameFlags::use_calls: CoopFlagNative on the calc thread),
    // the path the boss's one heart container takes (coop_hook.cpp, CoopBoss::TakeMaxLifeWrite). In a session the
    // object walk carries it into Link's own hearts as for any MaxLife a heart container raised (issue 88,
    // docs/items.md); the max rule shares it, so it reaches the guests as the host's save does.
    auto set = *max_life;
    set.value = static_cast<std::uint32_t>(*to);
    const bool ok = store.write(set);
    LOG_INFO(Core, "coop: New Game+: MaxLife {} -> {} (a new file){}", *give.max_life, *to,
             ok ? "" : ": the write failed");
    std::lock_guard lock(g.mutex);
    if (ok && g.hearts_said != *to && g.lines.size() < 16) {
        g.hearts_said = *to;
        g.lines.push_back("New Game+: your " + Hearts(*to) + " carried over");
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

}  // namespace CoopNgPlus
