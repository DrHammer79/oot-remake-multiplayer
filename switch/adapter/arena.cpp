// The ExeFS adapter's own heap (arena.h).
//
// Every block starts with a 16-byte tag: word 0 = the block's size (a multiple of 16, tag included) with bit 0 set
// while it is in use; word 1 = the size of the block before it (0 for the first). The blocks tile the arena with no
// gap, and two free blocks are never neighbours (Free merges them).
#include "arena.h"

namespace coop_arena {
namespace {

constexpr uint64_t kUsedBit = 1;

uint64_t RoundUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

uint64_t Arena::SizeAt(uint64_t off) const { return reinterpret_cast<const uint64_t *>(Block(off))[0] & ~kUsedBit; }
uint64_t Arena::PrevAt(uint64_t off) const { return reinterpret_cast<const uint64_t *>(Block(off))[1]; }
bool Arena::UsedAt(uint64_t off) const { return (reinterpret_cast<const uint64_t *>(Block(off))[0] & kUsedBit) != 0; }

void Arena::Set(uint64_t off, uint64_t size, uint64_t prev, bool used) {
    uint64_t *w = reinterpret_cast<uint64_t *>(Block(off));
    w[0] = size | (used ? kUsedBit : 0);
    w[1] = prev;
}

void Arena::SetPrevOfNext(uint64_t off) {
    const uint64_t next = off + SizeAt(off);
    if (next < size_) reinterpret_cast<uint64_t *>(Block(next))[1] = SizeAt(off);
}

bool Arena::Init(void *base, uint64_t size) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t aligned = (start + 15) & ~uintptr_t{15};
    const uint64_t lost = aligned - start;
    base_ = nullptr;  // (field by field: no struct copy, which a freestanding build may turn into memcpy)
    size_ = used_ = peak_ = 0;
    failed_ = bad_frees_ = 0;
    if (!base || size < lost + kMinBlock) return false;
    base_ = reinterpret_cast<uint8_t *>(aligned);
    size_ = (size - lost) & ~uint64_t{15};
    Set(0, size_, 0, false);
    return true;
}

void *Arena::Alloc(uint64_t n, uint64_t align) {
    if (!base_ || align == 0 || (align & (align - 1)) != 0 || align > kMaxAlign || n > size_) {
        ++failed_;
        return nullptr;
    }
    if (align < 16) align = 16;
    uint64_t need = RoundUp(n ? n : 1, 16) + kHeader;
    if (need < kMinBlock) need = kMinBlock;
    for (uint64_t off = 0; off < size_; off += SizeAt(off)) {
        if (UsedAt(off)) continue;
        const uint64_t size = SizeAt(off);
        const uintptr_t mem = reinterpret_cast<uintptr_t>(Block(off)) + kHeader;
        uint64_t gap = RoundUp(mem, align) - mem;
        if (gap != 0 && gap < kMinBlock) gap = RoundUp(mem + kMinBlock, align) - mem;  // room for a free block before
        if (gap + need > size) continue;
        uint64_t at = off;
        uint64_t left = size;
        if (gap != 0) {  // the front stays free
            Set(off, gap, PrevAt(off), false);
            at = off + gap;
            left = size - gap;
            Set(at, left, gap, false);
            SetPrevOfNext(at);
        }
        if (left - need >= kMinBlock) {  // the tail stays free
            Set(at, need, PrevAt(at), true);
            Set(at + need, left - need, need, false);
            SetPrevOfNext(at + need);
        } else {
            Set(at, left, PrevAt(at), true);
        }
        used_ += SizeAt(at);
        if (used_ > peak_) peak_ = used_;
        return Block(at) + kHeader;
    }
    ++failed_;
    return nullptr;
}

bool Arena::Owns(const void *p) const {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base_);
    if (!base_ || at < lo + kHeader || at >= lo + size_ || ((at - lo) & 15) != 0) return false;
    const uint64_t off = at - lo - kHeader;
    if (!UsedAt(off)) return false;
    const uint64_t size = SizeAt(off);
    if (size < kMinBlock || (size & 15) != 0 || size > size_ - off) return false;
    if (off + size < size_ && PrevAt(off + size) != size) return false;          // the next block points back here
    if (off != 0 && (PrevAt(off) > off || SizeAt(off - PrevAt(off)) != PrevAt(off))) return false;  // and the one before
    if (off == 0 && PrevAt(off) != 0) return false;
    return true;
}

void Arena::Free(void *p) {
    if (!p) return;
    if (!Owns(p)) {
        ++bad_frees_;
        return;
    }
    uint64_t off = static_cast<uint64_t>(static_cast<uint8_t *>(p) - base_) - kHeader;
    uint64_t size = SizeAt(off);
    used_ -= size;
    Set(off, size, PrevAt(off), false);
    const uint64_t next = off + size;
    if (next < size_ && !UsedAt(next)) {
        size += SizeAt(next);
        Set(off, size, PrevAt(off), false);
    }
    if (off != 0) {
        const uint64_t before = off - PrevAt(off);
        if (!UsedAt(before)) {
            Set(before, SizeAt(before) + size, PrevAt(before), false);
            off = before;
        }
    }
    SetPrevOfNext(off);
}

Stats Arena::GetStats() const {
    Stats s;
    s.capacity = size_;
    s.used = used_;
    s.peak = peak_;
    s.failed = failed_;
    s.bad_frees = bad_frees_;
    for (uint64_t off = 0; base_ && off < size_; off += SizeAt(off)) {
        if (UsedAt(off)) {
            ++s.used_blocks;
        } else {
            ++s.free_blocks;
            if (SizeAt(off) > s.largest_free) s.largest_free = SizeAt(off);
        }
    }
    return s;
}

bool Arena::Check() const {
    if (!base_) return false;
    uint64_t off = 0, prev = 0, used = 0;
    bool prev_free = false;
    while (off < size_) {
        const uint64_t size = SizeAt(off);
        if (size < kMinBlock || (size & 15) != 0 || size > size_ - off || PrevAt(off) != prev) return false;
        const bool free = !UsedAt(off);
        if (free && prev_free) return false;
        if (!free) used += size;
        prev_free = free;
        prev = size;
        off += size;
    }
    return off == size_ && used == used_;
}

}  // namespace coop_arena
