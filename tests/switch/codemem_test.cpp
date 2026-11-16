// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS module's code-memory path (switch/proto/codemem.h, v1 step 8a), on the host: the IPC
// message that copies the process handle, where the writable alias may go, and the address
// translation every patch goes through. The kernel calls (codemem.cpp) need a game run.
#include <doctest/doctest.h>

#include "../../switch/proto/codemem.h"

namespace {

using codemem::Range;

// Address-space shape like Fritchii's for a 39-bit game (numbers made up but in that layout).
constexpr Range kRegion{0x0008000000ull, 0x7ff8000000ull};
constexpr Range kHeap{0x1000000000ull, 0x1180000000ull};
constexpr Range kAlias{0x2000000000ull, 0x3000000000ull};
constexpr Range kNone{};

}  // namespace

TEST_CASE("codemem: the handle-copy request is one copy handle and no data") {
    uint32_t msg[codemem::kMessageWords] = {};
    codemem::HandleCopyRequest(msg, 0xFFFF8001u);
    CHECK(msg[0] == 4u);
    CHECK(msg[1] == 0x80000000u);  // special header, 0 data words, no buffers
    CHECK(msg[2] == 2u);           // 1 copy handle, no pid, no move handles
    CHECK(msg[3] == 0xFFFF8001u);
}

TEST_CASE("codemem: the received message gives the real handle") {
    uint32_t msg[8] = {4, 0x80000000u, 2, 0x0003c00au};
    uint32_t h = 0;
    CHECK(codemem::CopiedHandle(msg, h));
    CHECK(h == 0x0003c00au);

    // With a process id before the handles.
    uint32_t with_pid[8] = {4, 0x80000000u, 3, 0x51, 0, 0x0004c00bu};
    CHECK(codemem::CopiedHandle(with_pid, h));
    CHECK(h == 0x0004c00bu);
}

TEST_CASE("codemem: no special header, no copy handle, or a pseudo-handle is not a handle") {
    uint32_t h = 0;
    const uint32_t no_header[4] = {4, 0, 2, 0x0003c00au};
    CHECK_FALSE(codemem::CopiedHandle(no_header, h));
    const uint32_t move_only[4] = {4, 0x80000000u, 1u << 5, 0x0003c00au};
    CHECK_FALSE(codemem::CopiedHandle(move_only, h));
    const uint32_t pseudo[4] = {4, 0x80000000u, 2, 0xFFFF8001u};
    CHECK_FALSE(codemem::CopiedHandle(pseudo, h));
    const uint32_t empty[4] = {4, 0x80000000u, 2, 0};
    CHECK_FALSE(codemem::CopiedHandle(empty, h));
}

TEST_CASE("codemem: a free gap inside the region takes the alias at its start") {
    CHECK(codemem::FitIn(0x5000000000ull, 0x100000000ull, 0x4000000ull, kRegion, kHeap, kAlias) ==
          0x5000000000ull);
    // Unaligned free start (cannot happen in the kernel, but stays page-aligned).
    CHECK(codemem::FitIn(0x5000000800ull, 0x100000000ull, 0x1000, kRegion, kHeap, kAlias) == 0x5000001000ull);
}

TEST_CASE("codemem: a gap too small, or outside the region, is refused") {
    CHECK(codemem::FitIn(0x5000000000ull, 0x3000, 0x4000, kRegion, kHeap, kAlias) == 0);
    CHECK(codemem::FitIn(0x7ff8000000ull, 0x100000000ull, 0x1000, kRegion, kHeap, kAlias) == 0);
    // Free block starting below the region: only the part inside counts.
    CHECK(codemem::FitIn(0x0000000000ull, 0x0008002000ull, 0x2000, kRegion, kNone, kNone) == 0x0008000000ull);
    CHECK(codemem::FitIn(0x0000000000ull, 0x0008001000ull, 0x2000, kRegion, kNone, kNone) == 0);
    CHECK(codemem::FitIn(0x5000000000ull, 0x100000000ull, 0, kRegion, kHeap, kAlias) == 0);
}

TEST_CASE("codemem: the alias steps past the heap and alias regions") {
    // A free block covering the heap: the alias goes right after it.
    CHECK(codemem::FitIn(0x0f00000000ull, 0x1000000000ull, 0x4000000ull, kRegion, kHeap, kAlias) ==
          0x0f00000000ull);
    CHECK(codemem::FitIn(0x0fff000000ull, 0x1000000000ull, 0x4000000ull, kRegion, kHeap, kAlias) ==
          kHeap.hi);
    // Starts inside the alias region and ends before its end: nothing fits.
    CHECK(codemem::FitIn(0x2000000000ull, 0x0800000000ull, 0x1000, kRegion, kHeap, kAlias) == 0);
    // Heap then alias back to back: steps past both.
    constexpr Range heap{0x1000000000ull, 0x2000000000ull};
    CHECK(codemem::FitIn(0x1000000000ull, 0x3000000000ull, 0x1000, kRegion, heap, kAlias) == kAlias.hi);
}

TEST_CASE("codemem: translate holds only whole ranges inside the alias") {
    const codemem::Alias a{0x80020000ull, 0x5000000000ull, 0x3000000ull};
    CHECK(codemem::Translate(a, 0x80020000ull + 0x6e70f8ull, 4) == 0x5000000000ull + 0x6e70f8ull);
    CHECK(codemem::Translate(a, 0x80020000ull, 0x3000000ull) == 0x5000000000ull);
    CHECK(codemem::Translate(a, 0x80020000ull + 0x2fffffcull, 4) == 0x5000000000ull + 0x2fffffcull);
    CHECK(codemem::Translate(a, 0x80020000ull + 0x2fffffcull, 8) == 0);  // runs past the end
    CHECK(codemem::Translate(a, 0x8001fffcull, 4) == 0);                 // before it
    CHECK(codemem::Translate(a, 0x80020000ull, 0) == 0);
    CHECK(codemem::Translate(codemem::Alias{}, 0x80020000ull, 4) == 0);  // not mapped
    CHECK(codemem::Translate(a, ~0ull - 3, 4) == 0);
}
