// The draw-re research build (draw_re.h): reads and logs, never writes the game's memory, never calls the game.
#include "draw_re.h"

#include <stddef.h>
#include <stdint.h>

#include "hooks.h"
#include "svc.h"

extern "C" {
COOP_EXPORT uint64_t coop_slot_draw_re_apply = 0;
COOP_EXPORT uint64_t coop_slot_draw_re_apply_default = 0;
COOP_EXPORT uint64_t coop_slot_draw_re_register = 0;
}

namespace {

uint64_t g_main = 0;

struct Line {
    char buf[200];
    size_t n = 0;
    Line() { s("coop-proto: draw-re: "); }
    Line& s(const char* t) {
        while (*t && n < sizeof(buf) - 1) buf[n++] = *t++;
        return *this;
    }
    Line& hex(uint64_t v) {
        s("0x");
        char tmp[16];
        int k = 0;
        do {
            tmp[k++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        } while (v && k < 16);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    Line& dec(uint64_t v) {
        char tmp[20];
        int k = 0;
        do {
            tmp[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && k < 20);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    // An address as main+off when it is in main's image (text, ro, data, bss: < 0x5000000), else raw.
    Line& at(uint64_t v) {
        if (g_main != 0 && v >= g_main && v - g_main < 0x5000000) return s("main+").hex(v - g_main);
        return hex(v);
    }
    ~Line() { svc::OutputDebugString(buf, n); }
};

bool Mapped(uint64_t addr, uint64_t len) {
    svc::MemoryInfo mi{};
    if (addr == 0 || svc::QueryMemory(&mi, addr) != 0 || mi.perm == 0) return false;
    return addr >= mi.addr && addr + len <= mi.addr + mi.size;
}

uint64_t U64(uint64_t addr) {
    return (addr & 7) == 0 && Mapped(addr, 8) ? *reinterpret_cast<volatile uint64_t*>(addr) : 0;
}
uint32_t U32(uint64_t addr) {
    return (addr & 3) == 0 && Mapped(addr, 4) ? *reinterpret_cast<volatile uint32_t*>(addr) : 0;
}
uint16_t U16(uint64_t addr) {
    return (addr & 1) == 0 && Mapped(addr, 2) ? *reinterpret_cast<volatile uint16_t*>(addr) : 0;
}

// `bytes` from addr as u64 words, four per line, "tag +off: w w w w".
void Dump(const char* tag, uint64_t addr, uint32_t bytes) {
    if (!Mapped(addr, bytes)) {
        Line().s(tag).s(" ").at(addr).s(": not mapped");
        return;
    }
    for (uint32_t off = 0; off < bytes; off += 32) {
        Line l;
        l.s(tag).s(" +").hex(off).s(":");
        for (uint32_t k = 0; k < 32 && off + k < bytes; k += 8) l.s(" ").hex(*reinterpret_cast<volatile uint64_t*>(addr + off + k));
    }
}

uint32_t Bump(uint32_t* counter) { return __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED); }

// ---- taps ----
uint32_t g_apply_logged = 0;
uint32_t g_register_logged = 0;
constexpr uint32_t kMaxApply = 24;
constexpr uint32_t kMaxRegister = 6;
uint64_t g_registered[kMaxRegister];  // objects to look at again a frame later (their id); index = the tap's count

void Apply(uint32_t which, const uint64_t* r) {
    const uint32_t k = Bump(&g_apply_logged);
    if (k >= kMaxApply) return;
    const uint64_t mat = r[0], slot_ptr = r[1], idx = r[2], holder = r[11];
    const uint64_t entries = U64(mat + 48);
    const uint64_t entry = entries + idx * 0x50 + 72;
    Line().s(which == 0 ? "apply loaded" : "apply default").s(" ").dec(k).s(": material ").at(mat).s(" id ")
        .hex(U64(slot_ptr)).s(" (w ").dec(U16(slot_ptr - 8)).s(" h ").dec(U16(slot_ptr - 6)).s(") texmap ").dec(idx);
    Line().s("apply ").dec(k).s(": entry ").at(entry).s(" holds ").hex(U64(entry)).s(", holder ").at(holder)
        .s(" pane ").at(U64(holder + 712)).s(", material +24 ").hex(U32(mat + 24)).s(" +88 ").hex(U32(mat + 88));
    if (k == 0) Dump("material", mat, 128);
}

void Register(const uint64_t* r) {
    const uint32_t k = Bump(&g_register_logged);
    if (k >= kMaxRegister) return;
    const uint64_t tex = r[0];
    Line().s("register: texture object ").at(tex).s(" view ").at(r[1]).s(" flags ").hex(r[2]).s(" from main+")
        .hex(r[9] - g_main - 4).s(", id before ").hex(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(U32(tex + 104)))));
    Dump("texobj", tex, 256);
    __atomic_store_n(&g_registered[k], tex, __ATOMIC_RELEASE);
}

// ---- frame reads ----
uint64_t g_screens_sig = 0;
uint32_t g_screens_logged = 0;
bool g_gfx_logged = false;
uint32_t g_registered_done = 0;

void Screens(uint64_t frames) {
    const uint64_t mgr = U64(U64(g_main + draw_re::kUiManagerPtr));
    if (mgr == 0) return;
    uint32_t count = U32(mgr + 8);
    const uint64_t arr = U64(mgr + 0x10);
    if (count > 96) count = 96;
    uint64_t sig = 0xcbf29ce484222325ull ^ count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t scr = U64(arr + 8 * i);
        sig = (sig ^ U64(scr)) * 0x100000001b3ull;
        sig = (sig ^ U32(scr + 0x30)) * 0x100000001b3ull;
    }
    if (sig == g_screens_sig || g_screens_logged >= 12) return;
    g_screens_sig = sig;
    ++g_screens_logged;
    Line().s("ui screens ").dec(count).s(" at frame ").dec(frames).s(" (manager ").at(mgr).s(", array ").at(arr).s(")");
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t scr = U64(arr + 8 * i);
        Line l;
        l.s("screen ").dec(i).s(" ").at(scr).s(" vt ").at(U64(scr)).s(" layout ").at(U64(scr + 0x28)).s(" w30..4c");
        for (uint32_t off = 0x30; off < 0x50; off += 4) l.s(" ").hex(U32(scr + off));
        l.s(" w1a8 ").hex(U32(scr + 0x1a8));
    }
}

void Gfx() {
    const uint64_t mgr = U64(U64(g_main + draw_re::kGfxManagerPtr));
    if (mgr == 0) return;
    g_gfx_logged = true;
    Line().s("gfx manager ").at(mgr).s(": texture pool +384, id records ").dec(U32(mgr + 2544)).s(" at ")
        .at(U64(mgr + 2552)).s(", id base ").dec(U16(mgr + 2658)).s(", pool memory ").at(U64(mgr + 520))
        .s(" count ").dec(U32(mgr + 536));
}

}  // namespace

extern "C" COOP_EXPORT void coop_draw_re_tap(uint32_t which, const uint64_t* regs) {
    if (which <= 1) {
        Apply(which, regs);
    } else {
        Register(regs);
    }
}

namespace draw_re {

void Frame(uint64_t main, uint64_t frames) {
    g_main = main;
    if (frames == 1) Line().s("taps: apply main+").hex(kApplySite).s(", default main+").hex(kApplyDefaultSite)
                         .s(", register main+").hex(kRegisterSite).s(" (log only)");
    if (!g_gfx_logged && frames % 300 == 0) Gfx();
    if (frames % 120 == 0) Screens(frames);
    // The objects registered since: their id now.
    uint32_t seen = __atomic_load_n(&g_register_logged, __ATOMIC_RELAXED);
    if (seen > kMaxRegister) seen = kMaxRegister;
    while (g_registered_done < seen) {
        const uint64_t tex = __atomic_load_n(&g_registered[g_registered_done], __ATOMIC_ACQUIRE);
        if (tex == 0) break;  // its tap is still logging: next frame
        ++g_registered_done;
        Line().s("register: texture object ").at(tex).s(" id after ").hex(U32(tex + 104)).s(", +16 ").hex(U64(tex + 16))
            .s(", w ").dec(U16(tex + 64)).s(" h ").dec(U16(tex + 66)).s(" fmt+184 ").hex(U32(tex + 184) & 0xff);
    }
}

}  // namespace draw_re
