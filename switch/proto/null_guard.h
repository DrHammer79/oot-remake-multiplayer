// Null guards in the game's process (v1 blocker B1; hooks.h kind kNullGuard).
//
// A guarded load at main+X becomes `b island`. The island (five words in the module's
// island page, start.s) does what the emulator does for a read through a null pointer:
//
//     cbz   x<base>, 1f        // base is a null pointer?
//     <the game's load>         // no: as shipped (an immediate-offset load is position independent)
//     b     main+X+4
//  1: movz  x<dest>, #0         // yes: the register gets 0 (a nop when the load's register is xzr)
//     b     main+X+4
//
// No scratch register and no flags are touched, so the game's code around the load sees
// exactly what it would after the load. The site and the island must be within a b's
// reach (+-128 MB): the module lands right after subsdk0, next to main (docs/exefs.md).
// Pure functions, no libc: the module uses them at _init, tests/switch/null_guard_test.cpp
// on the host.
#pragma once

#include <stdint.h>

namespace null_guard {

constexpr uint32_t kIslandWords = 5;
constexpr uint32_t kIslandBytes = kIslandWords * 4;
constexpr uint32_t kNop = 0xd503201fu;

// LDR (64/32-bit), LDRH, LDRB with an unsigned immediate offset, base not sp.
constexpr bool Guardable(uint32_t insn) {
    const uint32_t op = insn & 0xffc00000u;
    if (op != 0xf9400000u && op != 0xb9400000u && op != 0x79400000u && op != 0x39400000u) return false;
    return ((insn >> 5) & 31u) != 31u;
}
constexpr uint32_t Dest(uint32_t insn) { return insn & 31u; }
constexpr uint32_t Base(uint32_t insn) { return (insn >> 5) & 31u; }

// `b to` at `from`; false when misaligned or out of reach.
constexpr bool Branch(uint64_t from, uint64_t to, uint32_t& out) {
    const int64_t delta = static_cast<int64_t>(to - from);
    if ((delta & 3) || delta >= (int64_t{1} << 27) || delta < -(int64_t{1} << 27)) return false;
    out = 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    return true;
}

// The island at `island` for the load `insn` at `site` (absolute addresses). False when the
// load is not one the guard knows or a branch is out of reach.
constexpr bool Island(uint64_t site, uint64_t island, uint32_t insn, uint32_t (&out)[kIslandWords]) {
    if (!Guardable(insn)) return false;
    uint32_t back1 = 0, back2 = 0;
    if (!Branch(island + 8, site + 4, back1) || !Branch(island + 16, site + 4, back2)) return false;
    const uint32_t dest = Dest(insn);
    out[0] = 0xb4000000u | (3u << 5) | Base(insn);  // cbz x<base>, +12
    out[1] = insn;
    out[2] = back1;
    out[3] = dest == 31u ? kNop : 0xd2800000u | dest;  // movz x<dest>, #0
    out[4] = back2;
    return true;
}

}  // namespace null_guard
