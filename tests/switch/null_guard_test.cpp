// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS module's null guards (switch/proto/null_guard.h and the generated null_guards.inc,
// v1 blocker B1), on the host: the island words, and a small interpreter for exactly those words
// that runs an island with a null and with a real base, as the game would after `b island`.
#include <doctest/doctest.h>

#include "../../switch/proto/hooks.h"
#include "../../switch/proto/null_guard.h"
#include "../../switch/proto/null_guards.inc"

#include <cstring>
#include <vector>

namespace {

constexpr uint64_t kMain = 0x80020000ull;
constexpr uint64_t kSite = kMain + 0x2dceef8ull;    // ldr w8, [x0, #0x7d8]
constexpr uint64_t kIsland = kMain + 0x6000000ull;  // the module, past main and subsdk0

struct Cpu {
    uint64_t x[32] = {};
    uint64_t pc = 0;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000);
};

int64_t Sext(uint32_t v, int bits) {
    const uint64_t m = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((static_cast<uint64_t>(v) ^ m) - m);
}

// Runs the island from its first word until a `b` leaves it; returns the branch target
// (~0 for a fault: the read the guard is for).
uint64_t Run(Cpu& c, const uint32_t (&w)[null_guard::kIslandWords], uint64_t island) {
    c.pc = island;
    for (int steps = 0; steps < 8; ++steps) {
        if (c.pc < island || c.pc >= island + null_guard::kIslandBytes) return ~0ull;
        const uint32_t i = w[(c.pc - island) / 4];
        if ((i & 0xff000000u) == 0xb4000000u) {  // cbz x
            c.pc = c.x[i & 31] == 0 ? c.pc + Sext((i >> 5) & 0x7ffff, 19) * 4 : c.pc + 4;
        } else if ((i & 0xfc000000u) == 0x14000000u) {  // b
            return c.pc + Sext(i & 0x3ffffff, 26) * 4;
        } else if ((i & 0xffe00000u) == 0xd2800000u) {  // movz x, #imm16
            c.x[i & 31] = (i >> 5) & 0xffff;
            c.pc += 4;
        } else if (i == null_guard::kNop) {
            c.pc += 4;
        } else if (null_guard::Guardable(i)) {  // the game's load (unsigned immediate offset)
            const uint32_t size = 1u << (i >> 30);
            const uint64_t a = c.x[null_guard::Base(i)] + ((i >> 10) & 0xfff) * size;
            if (a < 0x1000 || a + size > c.mem.size()) return ~0ull;  // a null page read: the fault
            uint64_t v = 0;
            std::memcpy(&v, c.mem.data() + a, size);
            if (null_guard::Dest(i) != 31) c.x[null_guard::Dest(i)] = v;
            c.pc += 4;
        } else {
            return ~0ull;
        }
    }
    return ~0ull;
}

void Put(Cpu& c, uint64_t a, uint64_t v) { std::memcpy(c.mem.data() + a, &v, sizeof v); }

}  // namespace

TEST_CASE("null guard island: the words") {
    uint32_t w[null_guard::kIslandWords] = {};
    REQUIRE(null_guard::Island(kSite, kIsland, 0xb947d808u, w));
    CHECK(w[0] == 0xb4000060u);  // cbz x0, +12
    CHECK(w[1] == 0xb947d808u);  // the game's load, unchanged
    CHECK(w[3] == 0xd2800008u);  // movz x8, #0
    CHECK((w[2] & 0xfc000000u) == 0x14000000u);
    CHECK(kIsland + 8 + Sext(w[2] & 0x3ffffff, 26) * 4 == kSite + 4);
    CHECK(kIsland + 16 + Sext(w[4] & 0x3ffffff, 26) * 4 == kSite + 4);
    uint32_t b = 0;
    REQUIRE(null_guard::Branch(kSite, kIsland, b));
    CHECK(kSite + Sext(b & 0x3ffffff, 26) * 4 == kIsland);
}

TEST_CASE("null guard island: a null base reads 0, a real one as shipped") {
    uint32_t w[null_guard::kIslandWords] = {};
    REQUIRE(null_guard::Island(kSite, kIsland, 0xb947d808u, w));  // ldr w8, [x0, #0x7d8]
    Cpu c;
    c.x[0] = 0;
    c.x[8] = 0x1111;
    c.x[9] = 0x2222;
    CHECK(Run(c, w, kIsland) == kSite + 4);
    CHECK(c.x[8] == 0);
    CHECK(c.x[0] == 0);
    CHECK(c.x[9] == 0x2222);  // nothing else touched
    Cpu d;
    d.x[0] = 0x1000;
    d.x[8] = 0x1111;
    Put(d, 0x1000 + 0x7d8, 0xaaaaaaaa12345678ull);
    CHECK(Run(d, w, kIsland) == kSite + 4);
    CHECK(d.x[8] == 0x12345678u);  // a 32-bit load zero-extends
}

TEST_CASE("null guard island: the slot-10 chain loads into its own base") {
    uint32_t w[null_guard::kIslandWords] = {};
    REQUIRE(null_guard::Island(kMain + 0x4b85e4, kIsland, 0xf9402908u, w));  // ldr x8, [x8, #0x50]
    Cpu c;
    CHECK(Run(c, w, kIsland) == kMain + 0x4b85e8);
    CHECK(c.x[8] == 0);
    Cpu d;
    d.x[8] = 0x2000;
    Put(d, 0x2000 + 0x50, 0x3000);
    CHECK(Run(d, w, kIsland) == kMain + 0x4b85e8);
    CHECK(d.x[8] == 0x3000);
}

TEST_CASE("null guard island: byte and halfword loads, and xzr") {
    uint32_t w[null_guard::kIslandWords] = {};
    REQUIRE(null_guard::Island(kSite, kIsland, 0x39797c08u, w));  // ldrb w8, [x0, #0xe5f]
    Cpu c;
    c.x[0] = 0x1000;
    Put(c, 0x1000 + 0xe5f, 0xffffffffffffff7full);
    CHECK(Run(c, w, kIsland) == kSite + 4);
    CHECK(c.x[8] == 0x7f);
    REQUIRE(null_guard::Island(kSite, kIsland, 0x7940001fu, w));  // ldrh wzr, [x0]
    CHECK(w[3] == null_guard::kNop);
}

TEST_CASE("null guard island: refuses what it does not know") {
    uint32_t w[null_guard::kIslandWords] = {};
    CHECK_FALSE(null_guard::Island(kSite, kIsland, 0xf94007e8u, w));  // ldr x8, [sp, #8]
    CHECK_FALSE(null_guard::Island(kSite, kIsland, 0xf8408c08u, w));  // ldr x8, [x0, #8]! (writes its base)
    CHECK_FALSE(null_guard::Island(kSite, kIsland, 0xf9000008u, w));  // str x8, [x0]
    CHECK_FALSE(null_guard::Island(kSite, kIsland, 0xaa0003f7u, w));  // mov x23, x0
    CHECK_FALSE(null_guard::Island(kSite, kSite + (uint64_t{1} << 28), 0xb947d808u, w));  // out of reach
    uint32_t b = 0;
    CHECK_FALSE(null_guard::Branch(kSite, kSite + 2, b));  // misaligned
    CHECK(null_guard::Branch(kSite, kSite - (uint64_t{1} << 27), b));
    CHECK_FALSE(null_guard::Branch(kSite, kSite + (uint64_t{1} << 27), b));
}

TEST_CASE("null guard table: the generated entries") {
    const hooks::Entry entries[] = {COOP_NULL_GUARD_ENTRIES};
    REQUIRE(sizeof(entries) / sizeof(entries[0]) == kNullGuardEntries);
    uint32_t guards = 0;
    bool after_check = false;
    for (const hooks::Entry& e : entries) {
        CHECK(e.replacement == nullptr);
        CHECK(e.slot == nullptr);
        CHECK(e.expected_insn != 0);
        if (e.kind == hooks::kNullGuard) {
            ++guards;
            CHECK(null_guard::Guardable(e.expected_insn));
            after_check = false;
        } else {
            REQUIRE(e.kind == hooks::kCheck);
            after_check = true;
        }
    }
    CHECK_FALSE(after_check);  // the last entry is a guard: no check is left without one
    CHECK(guards == kNullGuardCount);
    CHECK(kNullGuardCount * null_guard::kIslandBytes <= 0x1000u);
}
