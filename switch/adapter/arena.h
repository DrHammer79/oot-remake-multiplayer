// The ExeFS adapter's own heap (v1 step 8, piece b): one fixed block of memory (the module's bss), never the game's
// heap. First fit over boundary-tagged blocks, neighbours merged on free. Plain C++ with no library (it builds
// freestanding into the module and on PC for the tests); NOT thread-safe: platform_os.cpp holds a lock around it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace coop_arena {

struct Stats {
    uint64_t capacity = 0;     // bytes the arena manages (headers included)
    uint64_t used = 0;         // bytes in used blocks (headers included)
    uint64_t peak = 0;         // the most `used` has been
    uint64_t largest_free = 0; // the biggest block that is free (header included)
    uint32_t used_blocks = 0;
    uint32_t free_blocks = 0;
    uint32_t failed = 0;       // allocations refused (no room, bad size or alignment)
    uint32_t bad_frees = 0;    // frees of a pointer it never gave out, or given back twice
};

class Arena {
public:
    static constexpr uint64_t kHeader = 16;  // bytes before every block's memory
    static constexpr uint64_t kMinBlock = 32;
    static constexpr uint64_t kMaxAlign = 4096;

    // Takes [base, base + size); base is rounded up to 16 and size down to 16. False when too small for one block.
    bool Init(void *base, uint64_t size);
    // n bytes aligned to `align` (a power of two up to 4096; less than 16 gives 16), or nullptr.
    void *Alloc(uint64_t n, uint64_t align);
    // Gives back a pointer from Alloc. nullptr does nothing; anything else not from Alloc is counted, not freed.
    void Free(void *p);
    // True when p is a live block from this arena.
    bool Owns(const void *p) const;
    Stats GetStats() const;
    // Walks every block and checks the tags (sizes add up, prev sizes match, no two free neighbours).
    bool Check() const;

private:
    uint8_t *base_ = nullptr;
    uint64_t size_ = 0;
    uint64_t used_ = 0;
    uint64_t peak_ = 0;
    uint32_t failed_ = 0;
    uint32_t bad_frees_ = 0;

    uint8_t *Block(uint64_t off) const { return base_ + off; }
    uint64_t SizeAt(uint64_t off) const;
    uint64_t PrevAt(uint64_t off) const;
    bool UsedAt(uint64_t off) const;
    void Set(uint64_t off, uint64_t size, uint64_t prev, bool used);
    void SetPrevOfNext(uint64_t off);
};

}  // namespace coop_arena
