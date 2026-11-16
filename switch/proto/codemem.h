// Writing the game's code without changing its pages' permission (v1 step 8a).
//
// svc 0x73 SetProcessMemoryPermission(RW) turns a code page into CodeData, and the kernel then
// refuses the switch back to RX (0xd401, InvalidCurrentMemory): on a console that page would no
// longer run. Instead the module maps a second, writable view (an "alias") of the code once:
//
//   1. a real handle to its own process: the kernel refuses the pseudo-handle 0xFFFF8001 for
//      svc 0x74, but translates it when it is copied in an IPC message. So the module opens a
//      session to itself (0x40), queues a request that copies 0xFFFF8001 (0x23, async: no second
//      thread), receives it (0x43) and keeps the handle the kernel put in the message;
//   2. a free gap in the region where SharedCode may go (GetInfo 12/13, outside the heap and
//      alias regions), found with a QueryMemory walk;
//   3. svc 0x74 MapProcessMemory: the same physical pages at the gap, RW. Main's own mapping
//      stays RX; every patch is written through the alias, then the RX address's cache lines
//      are cleaned and invalidated.
// If any step is refused, writes fall back to 0x73 (RW, write, RX asked once; see codemem.cpp).
//
// This header holds the pure parts (the IPC message, the gap fit, the address translation) so
// tests/switch/codemem_test.cpp runs them on the host; codemem.cpp holds the kernel calls.
#pragma once

#include <stdint.h>

namespace codemem {

constexpr uint64_t kPage = 0x1000;

// ---- 1. The IPC message that copies a handle (HIPC layout, as the kernel reads it) ----
// word 0: type (bits 0-15), no buffers; word 1: 0 data words, bit 31 = a special header follows;
// word 2 (special header): bit 0 = process id, bits 1-4 copy handles, bits 5-8 move handles;
// then the process id (2 words) if any, then the handles.
constexpr uint32_t kMessageWords = 4;
constexpr uint32_t kRequestType = 4;  // "Request"; the kernel does not look at it

constexpr void HandleCopyRequest(uint32_t* msg, uint32_t handle) {
    msg[0] = kRequestType;
    msg[1] = 0x80000000u;
    msg[2] = 1u << 1;  // one copy handle, no process id
    msg[3] = handle;
}

// The handle the kernel wrote into the received message (its first copy handle), or false.
constexpr bool CopiedHandle(const uint32_t* msg, uint32_t& out) {
    if ((msg[1] & 0x80000000u) == 0) return false;
    const uint32_t special = msg[2];
    if (((special >> 1) & 0xf) == 0) return false;
    out = msg[(special & 1) ? 5 : 3];
    return out != 0 && out != 0xFFFF8001u && out != 0xFFFF8000u;  // real handles only
}

// ---- 2. Where the alias may go ----
struct Range {
    uint64_t lo = 0, hi = 0;  // [lo, hi); lo == hi is empty
};

constexpr uint64_t AlignUp(uint64_t v) { return (v + (kPage - 1)) & ~(kPage - 1); }

constexpr bool Overlaps(Range r, uint64_t addr, uint64_t size) {
    return r.lo < r.hi && addr < r.hi && r.lo < addr + size;
}

// The lowest page-aligned address in the free block [free, free + free_size) where `size` bytes
// fit inside `region` and touch neither `heap` nor `alias` (what the kernel's CanContain asks of
// SharedCode); 0 if there is none.
constexpr uint64_t FitIn(uint64_t free, uint64_t free_size, uint64_t size, Range region, Range heap,
                         Range alias) {
    if (size == 0 || free + free_size < free) return 0;
    const uint64_t hi = free + free_size < region.hi ? free + free_size : region.hi;
    uint64_t at = AlignUp(free > region.lo ? free : region.lo);
    for (int i = 0; i < 3; ++i) {  // each pass steps past one region it hit
        if (at < free || at > hi || hi - at < size) return 0;
        if (Overlaps(heap, at, size)) {
            at = AlignUp(heap.hi);
        } else if (Overlaps(alias, at, size)) {
            at = AlignUp(alias.hi);
        } else {
            return at;
        }
    }
    return 0;
}

// ---- 3. The alias itself ----
struct Alias {
    uint64_t rx = 0;    // the code as it runs
    uint64_t rw = 0;    // the same pages, writable
    uint64_t size = 0;  // 0: not mapped
};

// The writable address of [addr, addr + bytes), or 0 when the alias does not hold all of it.
constexpr uint64_t Translate(const Alias& a, uint64_t addr, uint64_t bytes) {
    if (a.size == 0 || bytes == 0 || addr < a.rx || bytes > a.size || addr - a.rx > a.size - bytes) return 0;
    return a.rw + (addr - a.rx);
}

// ---- The module's side (codemem.cpp) ----
enum Step : uint32_t {
    kOk = 0,
    kCreateSession,   // svc 0x40
    kSendRequest,     // svc 0x23
    kReceive,         // svc 0x43
    kNoHandle,        // the received message held no handle
    kRegionInfo,      // svc 0x29
    kNoRoom,          // no free gap (svc 0x06 walk)
    kMap,             // svc 0x74
    kTooMany,         // more ranges than alias slots
};

constexpr uint32_t kMaxAliases = 2;  // main's text, the island page

struct Status {
    Step step = kOk;
    uint32_t result = 0;  // the refused call's result
    uint32_t mapped = 0;  // aliases mapped (the first `mapped` ranges)
};

// Maps one writable alias of each range (page-aligned, at most kMaxAliases), in order, until one
// is refused. Every handle it makes (session pair, event, the process handle) is closed before it
// returns; the aliases stay mapped for the life of the process.
Status Open(const Range* ranges, uint32_t count);

// The alias of slot `i` (size 0 when not mapped), for the log.
Alias Get(uint32_t i);

// Writes `count` instructions at the code address `at`: through an alias when one holds them,
// else through svc 0x73. Then makes the CPU see them (dc cvau, ic ivau on `at`).
bool Write(uint64_t at, const uint32_t* words, uint32_t count);

// What the 0x73 fallback did (for one log line).
struct Fallback {
    uint32_t writes = 0;        // writes that went through 0x73
    uint32_t refused = 0;       // its first refusal (0: none)
    uint32_t rx_refused = 0;    // the RX switch back's result; after it, RX is not asked again
};
const Fallback& FallbackStats();

}  // namespace codemem
