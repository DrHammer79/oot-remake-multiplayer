// The module's hook table (section .coop_hooks, exported as `coop_hooks`).
//
// One table, two readers:
//   - on an emulator or console, the module's own _init applies it (rewrites the
//     `bl`, fills the slots with main's addresses);
//   - a recomp that lifts this module as subsdk1 reads it at build time, turns every
//     call through a slot into a direct call to the lifted game function, applies
//     the redirects in its lifted code, and sets `applied_by_host` to 1 in the lifted
//     data, so _init leaves everything alone.
// So the module makes no call the lifter cannot resolve: every game call and every
// hook continue goes through a named, exported slot listed here.
//
// Layout (little-endian, version 1), read from the file's data segment. The linker
// writes module-relative addresses into the pointer fields (--apply-dynamic-relocs),
// so a reader needs no relocation pass:
//   header, 32 bytes: char magic[8] "COOPHOOK"; u32 version; u32 entry_size (32);
//                     u32 count; u32 applied_by_host; u64 reserved
//   entry,  32 bytes: u16 kind; u16 reserved; u32 expected_insn; u64 main_offset;
//                     u64 replacement (module address or 0); u64 slot (module address or 0)
// The entries are in one array of `count`; null guards (kinds 4 and 5) come from
// tools/null_guards/sites.py through the generated null_guards.inc.
#pragma once

#include <stdint.h>

#define COOP_EXPORT __attribute__((visibility("protected")))

namespace hooks {

enum Kind : uint16_t {
    // The `bl` at main_offset (expected_insn) calls `replacement` instead; the slot
    // gets the bl's original target, where `replacement` continues.
    kCallRedirect = 1,
    // Calls to the function at main_offset go to `replacement`; the slot gets an
    // entry to the original. Defined for the format; the prototype has none and
    // _init does not apply it (it needs a trampoline on emulators).
    kEntry = 2,
    // No hook: the slot gets the game function at main_offset. expected_insn is
    // that function's first instruction, or 0 for no check.
    kGameFn = 3,
    // A null guard (v1 blocker B1): the load at main_offset (expected_insn: LDR/LDRB/LDRH
    // with an unsigned immediate offset, base not sp) gives its register 0 when its base
    // register is 0, what the emulator reads, and runs as shipped otherwise. No replacement,
    // no slot. Applied only when every kCheck entry since the previous guard matched.
    // A lifting recomp emits it in its lifted code (Longshot: Lifter.unless, patch 0023).
    kNullGuard = 4,
    // No hook: main_offset must hold expected_insn for the next kNullGuard to be applied.
    kCheck = 5,
    // main's calls of an import go to `replacement`: main_offset is main's GOT entry for it (the
    // relocation there names the import), expected_insn is 0. The slot gets the import's own target,
    // where `replacement` continues. On an emulator the module fills the slot with its own import of
    // the same name, then writes `replacement` into the GOT entry (a data write; the entry must hold
    // that import or, before main's first call, main's PLT). A lifting recomp routes main's calls of
    // that import to `replacement` and fills the slot with its own implementation.
    kImportRedirect = 6,
};

struct Entry {
    uint16_t kind;
    uint16_t reserved;
    uint32_t expected_insn;
    uint64_t main_offset;
    void (*replacement)();  // data only: never called through, so no indirect call
    uint64_t* slot;
};
static_assert(sizeof(Entry) == 32);

template <uint32_t N>
struct Table {
    char magic[8];
    uint32_t version;
    uint32_t entry_size;
    uint32_t count;
    uint32_t applied_by_host;
    uint64_t reserved;
    Entry entries[N];
};
static_assert(sizeof(Table<2>) == 32 + 2 * 32);

}  // namespace hooks
