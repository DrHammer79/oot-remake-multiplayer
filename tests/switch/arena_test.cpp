// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS adapter's heap (switch/adapter/arena.h), built for PC.
#include <doctest/doctest.h>

#include "../../switch/adapter/arena.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

alignas(4096) std::uint8_t g_mem[1 << 20];

bool Aligned(const void *p, std::uint64_t a) { return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0; }

}  // namespace

TEST_CASE("arena: init takes the block whole and refuses one too small") {
    coop_arena::Arena a;
    CHECK_FALSE(a.Init(nullptr, 4096));
    CHECK_FALSE(a.Init(g_mem, 16));
    REQUIRE(a.Init(g_mem + 3, 4096));  // base rounded up to 16, size down
    const auto s = a.GetStats();
    CHECK(s.capacity == 4080);
    CHECK(s.free_blocks == 1);
    CHECK(s.largest_free == 4080);
    CHECK(a.Check());
}

TEST_CASE("arena: allocations are aligned, distinct, and come back whole") {
    coop_arena::Arena a;
    REQUIRE(a.Init(g_mem, sizeof g_mem));
    std::vector<void *> got;
    for (std::uint64_t align : {1u, 8u, 16u, 32u, 64u, 256u, 4096u}) {
        void *p = a.Alloc(100, align);
        REQUIRE(p != nullptr);
        CHECK(Aligned(p, align < 16 ? 16 : align));
        CHECK(a.Owns(p));
        std::memset(p, 0xab, 100);
        got.push_back(p);
        REQUIRE(a.Check());
    }
    for (std::size_t i = 0; i < got.size(); ++i)
        for (std::size_t j = i + 1; j < got.size(); ++j) CHECK(got[i] != got[j]);
    for (void *p : got) a.Free(p);
    const auto s = a.GetStats();
    CHECK(s.used == 0);
    CHECK(s.used_blocks == 0);
    CHECK(s.free_blocks == 1);  // merged back into one
    CHECK(s.largest_free == sizeof g_mem);
    CHECK(s.peak > 0);
    CHECK(a.Check());
}

TEST_CASE("arena: refuses what it cannot give and counts it") {
    coop_arena::Arena a;
    REQUIRE(a.Init(g_mem, 4096));
    CHECK(a.Alloc(8192, 16) == nullptr);
    CHECK(a.Alloc(16, 3) == nullptr);      // not a power of two
    CHECK(a.Alloc(16, 8192) == nullptr);   // over 4096
    CHECK(a.Alloc(16, 0) == nullptr);
    void *big = a.Alloc(4096 - 16, 16);    // exactly the whole arena
    CHECK(big != nullptr);
    CHECK(a.Alloc(1, 16) == nullptr);
    CHECK(a.GetStats().failed == 5);
    a.Free(big);
    CHECK(a.Alloc(0, 16) != nullptr);      // size 0 still gives a block
    CHECK(a.Check());
}

TEST_CASE("arena: bad and double frees are counted, never freed") {
    std::memset(g_mem, 0, 8192);  // (a pointer inside a block reads the block's data as a tag: make it plain)
    coop_arena::Arena a;
    REQUIRE(a.Init(g_mem, 8192));
    void *p = a.Alloc(64, 16);
    void *q = a.Alloc(64, 16);
    REQUIRE(p);
    REQUIRE(q);
    a.Free(nullptr);
    a.Free(static_cast<std::uint8_t *>(p) + 16);  // inside a block
    int stack = 0;
    a.Free(&stack);                               // not ours at all
    a.Free(p);
    a.Free(p);                                    // twice
    CHECK(a.GetStats().bad_frees == 3);
    CHECK_FALSE(a.Owns(p));
    CHECK(a.Owns(q));
    CHECK(a.Check());
}

TEST_CASE("arena: merges with both neighbours") {
    coop_arena::Arena a;
    REQUIRE(a.Init(g_mem, 4096));
    void *x = a.Alloc(100, 16);
    void *y = a.Alloc(100, 16);
    void *z = a.Alloc(100, 16);
    void *w = a.Alloc(100, 16);  // keeps the tail apart from z
    a.Free(x);
    a.Free(z);
    CHECK(a.GetStats().free_blocks == 3);  // x, z, the tail
    a.Free(y);                              // x + y + z become one
    CHECK(a.GetStats().free_blocks == 2);
    CHECK(a.Check());
    a.Free(w);
    CHECK(a.GetStats().free_blocks == 1);
    CHECK(a.Check());
}

TEST_CASE("arena: random alloc and free keep the tags right and the data intact") {
    coop_arena::Arena a;
    REQUIRE(a.Init(g_mem, sizeof g_mem));
    std::mt19937 rng(1234);
    struct Live {
        std::uint8_t *p;
        std::uint64_t n;
        std::uint8_t fill;
    };
    std::vector<Live> live;
    for (int step = 0; step < 4000; ++step) {
        if (live.empty() || rng() % 3 != 0) {
            const std::uint64_t n = rng() % 3000;
            const std::uint64_t align = std::uint64_t{1} << (rng() % 13);  // 1 .. 4096
            auto *p = static_cast<std::uint8_t *>(a.Alloc(n, align));
            if (!p) continue;
            REQUIRE(Aligned(p, align < 16 ? 16 : align));
            const auto fill = static_cast<std::uint8_t>(rng());
            std::memset(p, fill, n);
            live.push_back({p, n, fill});
        } else {
            const std::size_t i = rng() % live.size();
            bool intact = true;
            for (std::uint64_t k = 0; k < live[i].n; ++k) intact = intact && live[i].p[k] == live[i].fill;
            REQUIRE(intact);
            a.Free(live[i].p);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (step % 50 == 0) REQUIRE(a.Check());
    }
    for (const Live &l : live) a.Free(l.p);
    CHECK(a.Check());
    CHECK(a.GetStats().used == 0);
    CHECK(a.GetStats().free_blocks == 1);
    CHECK(a.GetStats().bad_frees == 0);
}
