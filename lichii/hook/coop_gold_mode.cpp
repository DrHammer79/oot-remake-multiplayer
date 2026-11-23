// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_gold_mode.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <set>

#include "coop/host/save_transfer.hpp"

namespace CoopGoldMode {

namespace proto = coop::protocol;

namespace {
std::atomic<Mode> g_choice{Mode::shared};
std::atomic<Mode> g_in_force{Mode::shared};
std::atomic<bool> g_told{false};  // this connection has a mode (a host's settings, or our own Host)
}  // namespace

std::string_view Value(Mode mode) {
    return mode == Mode::own ? "own" : "shared";
}

std::optional<Mode> Parse(std::string_view value) {
    if (value == "shared") return Mode::shared;
    if (value == "own") return Mode::own;
    return std::nullopt;
}

std::optional<Mode> FromSettings(const std::vector<proto::ModSetting>& settings) {
    for (const proto::ModSetting& s : settings) {
        if (s.mod_id == kModId && s.key == kKey) return Parse(s.value);
    }
    return std::nullopt;
}

proto::ModSetting AsSetting(Mode mode) {
    return proto::ModSetting{std::string(kModId), std::string(kKey), std::string(Value(mode))};
}

void SetChoice(Mode mode) {
    g_choice.store(mode);
}

Mode Choice() {
    return g_choice.load();
}

void OnConnection() {
    g_told.store(false);
    g_in_force.store(Mode::shared);
}

void OnHosting() {
    // A host after a migration keeps the mode its old host told it.
    if (!g_told.exchange(true)) g_in_force.store(g_choice.load());
}

void OnWelcomed(bool hosting) {
    OnConnection();
    if (hosting) OnHosting();
}

void OnHostSettings(const std::vector<proto::ModSetting>& settings) {
    if (const auto mode = FromSettings(settings)) {
        g_in_force.store(*mode);
        g_told.store(true);
    }
}

bool Told() {
    return g_told.load();
}

Mode InForce() {
    return g_in_force.load(std::memory_order_relaxed);
}

namespace {

// The save's layout (tools/autotest/save_flags.py; all little-endian): magic, the pool's offset at 8, from 0x20 a
// table of 8-byte records (a 0 first word opens type 0, 1, ... in order; the end record 0x20 is followed by a pair
// whose second word is the 64-bit key set's offset, 0 for none). The set: ascending u64 keys, a u64 0, the file's end.
struct SaveView {
    std::size_t end_pair = 0;  // the end record's pair
    std::size_t set_at = 0;    // the key set (0: none)
    std::vector<std::uint64_t> keys;
    std::optional<std::size_t> count_at;  // the Int's value word
    std::optional<std::size_t> get_at;    // the Bool's value word
};

std::uint32_t Word(const std::vector<std::uint8_t>& d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}
void PutWord(std::vector<std::uint8_t>& d, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t Quad(const std::vector<std::uint8_t>& d, std::size_t at) {
    return static_cast<std::uint64_t>(Word(d, at)) | static_cast<std::uint64_t>(Word(d, at + 4)) << 32;
}

std::optional<SaveView> ReadSave(const std::vector<std::uint8_t>& d, std::uint32_t count_hash,
                                 std::uint32_t get_hash, std::string& why) {
    constexpr std::uint32_t kMagic = 0x01020304;
    constexpr std::size_t kTable = 0x20;
    constexpr std::uint32_t kBool = 0;
    constexpr std::uint32_t kInt = 2;
    constexpr std::uint32_t kEnd = 0x20;
    if (d.size() < kTable || Word(d, 0) != kMagic) {
        why = "not a saved game";
        return std::nullopt;
    }
    const std::size_t pool = Word(d, 8);
    if (pool < kTable || pool > d.size()) {
        why = "bad pool offset";
        return std::nullopt;
    }
    SaveView v;
    std::optional<std::uint32_t> kind;
    for (std::size_t at = kTable; at + 8 <= pool; at += 8) {
        const std::uint32_t first = Word(d, at);
        const std::uint32_t second = Word(d, at + 4);
        if (first == 0) {
            if (second != (kind ? *kind + 1 : 0)) {
                why = "types out of order";
                return std::nullopt;
            }
            if (second == kEnd) {
                if (at + 16 > pool) {
                    why = "the end record's pair runs into the pool";
                    return std::nullopt;
                }
                v.end_pair = at + 8;
                break;
            }
            kind = second;
        } else if (!kind) {
            why = "an entry before the first type";
            return std::nullopt;
        } else if (*kind == kInt && first == count_hash) {
            v.count_at = at + 4;
        } else if (*kind == kBool && first == get_hash) {
            v.get_at = at + 4;
        }
    }
    if (v.end_pair == 0) {
        why = "no end record";
        return std::nullopt;
    }
    v.set_at = Word(d, v.end_pair + 4);
    if (v.set_at == 0) return v;
    if (v.set_at < pool || v.set_at + 8 > d.size()) {
        why = "the key set is outside the file";
        return std::nullopt;
    }
    for (std::size_t at = v.set_at;; at += 8) {
        if (at + 8 > d.size()) {
            why = "the key set has no closing zero";
            return std::nullopt;
        }
        const std::uint64_t k = Quad(d, at);
        if (k == 0) {
            if (at + 8 != d.size()) {
                why = "bytes after the key set";
                return std::nullopt;
            }
            break;
        }
        if (!v.keys.empty() && k <= v.keys.back()) {
            why = "the key set is not ascending";
            return std::nullopt;
        }
        v.keys.push_back(k);
    }
    return v;
}

}  // namespace

GoldKept KeepOwnGold(std::vector<std::uint8_t>& host, const std::vector<std::uint8_t>* own,
                     std::span<const std::uint64_t> skulltula_keys, std::uint32_t count_hash,
                     std::uint32_t get_hash) {
    GoldKept out;
    std::set<std::uint64_t> gold;
    for (const std::uint64_t k : skulltula_keys) {
        gold.insert(k);
        gold.insert(k + 2);
    }
    const auto h = ReadSave(host, count_hash, get_hash, out.why);
    if (!h) {
        out.why = "the host's save: " + out.why;
        return out;
    }
    std::optional<SaveView> o;
    if (own != nullptr) {
        std::string why;
        o = ReadSave(*own, count_hash, get_hash, why);  // ours unreadable: a race from nothing
    }
    std::vector<std::uint64_t> keys;
    for (const std::uint64_t k : h->keys) {
        if (gold.count(k) != 0) {
            ++out.host_keys;
        } else {
            keys.push_back(k);
        }
    }
    if (o) {
        for (const std::uint64_t k : o->keys) {
            if (gold.count(k) != 0) keys.push_back(k);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.own_keys = static_cast<int>(
        std::count_if(keys.begin(), keys.end(), [&](std::uint64_t k) { return gold.count(k) != 0; }));
    if (h->count_at) out.host_count = static_cast<std::int32_t>(Word(host, *h->count_at));
    if (o && o->count_at) out.own_count = static_cast<std::int32_t>(Word(*own, *o->count_at));
    const std::uint32_t own_get = (o && o->get_at) ? Word(*own, *o->get_at) : 0;

    if (h->count_at) PutWord(host, *h->count_at, static_cast<std::uint32_t>(out.own_count));
    if (h->get_at) PutWord(host, *h->get_at, own_get);
    if (h->set_at == 0) {
        // No key set to rewrite: the host's save has no gold keys either. Ours are not added (a new set's place is
        // not known), so the Skulltulas we took are alive again in this save.
        out.keys_left = out.own_keys > 0;
        out.own_keys = 0;
    } else {
        host.resize(h->set_at);
        for (const std::uint64_t k : keys) {
            for (int i = 0; i < 8; ++i) host.push_back(static_cast<std::uint8_t>(k >> (8 * i)));
        }
        host.insert(host.end(), 8, 0);
    }
    out.done = true;
    return out;
}

void ResetForTest() {
    g_choice.store(Mode::shared);
    g_in_force.store(Mode::shared);
    g_told.store(false);
}

namespace {
std::optional<std::vector<std::uint8_t>> NewestProgress(const std::filesystem::path& coop_dir) {
    namespace save = coop::host::save;
    const auto newest = save::newest_slot(coop_dir);
    if (!newest) return std::nullopt;
    auto slot = save::read_slot(*newest);
    if (!slot) return std::nullopt;
    for (auto& f : slot->files) {
        if (f.name == "progress.sav") return std::move(f.bytes);
    }
    return std::nullopt;
}
}  // namespace

namespace {
std::filesystem::path Beside(const std::filesystem::path& coop_dir, std::string_view tail) {
    std::filesystem::path p = coop_dir;
    if (!p.has_filename()) p = p.parent_path();  // a trailing separator
    return p.parent_path() / (p.filename().string() + std::string(tail));
}
}  // namespace

std::filesystem::path RaceFile(const std::filesystem::path& coop_dir) { return Beside(coop_dir, "-gold-race.sav"); }

std::filesystem::path RaceMark(const std::filesystem::path& coop_dir) { return Beside(coop_dir, "-gold-race.on"); }

std::optional<std::vector<std::uint8_t>> RaceSoFar(const std::filesystem::path& coop_dir) {
    // The folder's slot only when it is a race (the mark): a Shared session's slot, also one that switched to Not shared
    // midway (a Resync), holds the shared count, never a race's start.
    std::error_code ec;
    if (std::filesystem::exists(RaceMark(coop_dir), ec)) {
        if (auto bytes = NewestProgress(coop_dir)) return bytes;
    }
    std::ifstream in(RaceFile(coop_dir), std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return std::nullopt;
    return bytes;
}

void MarkRace(const std::filesystem::path& coop_dir, bool race) {
    std::error_code ec;
    if (race) {
        std::ofstream(RaceMark(coop_dir), std::ios::binary | std::ios::trunc).put('1');
    } else {
        std::filesystem::remove(RaceMark(coop_dir), ec);
    }
}

bool KeepRace(const std::filesystem::path& coop_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(RaceMark(coop_dir), ec)) return false;
    const auto bytes = NewestProgress(coop_dir);
    if (!bytes || bytes->empty()) return false;
    const std::filesystem::path to = RaceFile(coop_dir);
    const std::filesystem::path tmp = to.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (!out) return false;
    }
    std::filesystem::rename(tmp, to, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace CoopGoldMode
