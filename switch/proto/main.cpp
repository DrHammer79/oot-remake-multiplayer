// ExeFS prototype (v1-exefs-proto): a code module (subsdk1) inside the game's own process.
//
// It proves four things on a Switch 2 emulator (Fritchii), nothing more:
//   1. rtld loads and starts it: "coop-proto: hello" in the log.
//   2. It hooks the game: the per-frame `bl frame_step` at main+0x6e70f8 goes
//      through coop_frame_entry (start.s); a log line every 600 frames.
//   3. It calls one of the game's own functions: the world clock setter
//      main+0x34522d0 (world manager, seconds), as lichii/hook/coop_time.cpp does.
//   4. It opens a socket with the game's own nn::socket (from sdk) and sends a
//      UDP "hello" to 127.0.0.1:27520 (tools/switch_mod/listen.py prints it).
// No libc, no SDK headers: the game's modules are linked by rtld by name.
//
// Built so a recomp can lift the same binary as subsdk1 (docs/exefs.md, "Lifting"):
// unwind tables kept, entry points exported, and no indirect call except through
// the slots in the hook table (hooks.h), which a lifter turns into direct calls.

#include <stddef.h>
#include <stdint.h>

#include "hooks.h"
#include "../adapter/input.h"
#include "null_guard.h"
#include "null_guards.inc"  // (generated: tools/null_guards/gen.py)
#include "svc.h"

// ---- Imports, resolved by rtld from main and sdk ----
extern "C" {
void nnMain();             // defined only in main: finds main's base
void* malloc(size_t size);  // defined only in main: checks the build
}

struct sockaddr {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};

namespace nn::socket {
uint32_t Initialize(void* pool, unsigned long pool_size, unsigned long alloc_pool_size,
                    int concurrency);  // _ZN2nn6socket10InitializeEPvmmi
bool IsInitialized();                  // _ZN2nn6socket13IsInitializedEv
int Socket(int domain, int type, int protocol);                      // _ZN2nn6socket6SocketEiii
int Connect(int fd, const ::sockaddr* addr, unsigned int len);       // _ZN2nn6socket7ConnectEiPK8sockaddrj
long Send(int fd, const void* data, unsigned long size, int flags);  // _ZN2nn6socket4SendEiPKvmi
int Close(int fd);                                                   // _ZN2nn6socket5CloseEi
}  // namespace nn::socket

// ---- What this build of the game looks like (build e0f3ccfaf50065ae, 1.1.1) ----
namespace game {
constexpr uint64_t kNnMain = 0x1202cd0;     // main's dynsym
constexpr uint64_t kMalloc = 0x14c6c90;     // main's dynsym; coop_game_build.h .malloc
constexpr uint64_t kFrameSite = 0x6e70f8;   // coop_svc.h kHooks[0]
constexpr uint32_t kFrameSiteInsn = 0x94d19c82;  // bl 0x3b4e300 (frame_step)
constexpr uint64_t kWorldMgrPtr = 0x4e7eab0;  // coop_game_build.h .world_mgr_ptr
constexpr uint64_t kTimeObject = 0x510;     // coop_time.cpp
constexpr uint64_t kSetTime = 0x34522d0;    // coop_time.cpp kSetTime
constexpr uint64_t kMainCodeEnd = 0x3c461d0;  // main's text segment size (its PLT is in it)
}  // namespace game

// ---- Slots: the only way this module calls into the game (see hooks.h) ----
extern "C" {
COOP_EXPORT uint64_t coop_slot_frame_step = 0;  // the game's frame step; start.s continues there
COOP_EXPORT uint64_t coop_slot_set_time = 0;    // void set_time(world, seconds)
COOP_EXPORT void coop_frame_entry();            // start.s
COOP_EXPORT void coop_call_set_time(uint64_t world, uint64_t seconds);  // start.s, via coop_slot_set_time
extern uint32_t coop_null_islands[];  // start.s: one page, filled by ApplyNullGuards
}
static_assert(kNullGuardCount * null_guard::kIslandBytes <= 0x1000, "the island page is full");

constexpr uint32_t kHookCount = 2 + input::kHookCount + kNullGuardEntries;
extern "C" COOP_EXPORT constinit __attribute__((section(".coop_hooks"), used)) hooks::Table<kHookCount> coop_hooks = {
    {'C', 'O', 'O', 'P', 'H', 'O', 'O', 'K'},
    1,
    sizeof(hooks::Entry),
    kHookCount,
    0,  // applied_by_host: a lifting recomp sets it to 1 in its copy
    0,
    {
        {hooks::kCallRedirect, 0, game::kFrameSiteInsn, game::kFrameSite, &coop_frame_entry,
         &coop_slot_frame_step},
        {hooks::kGameFn, 0, 0, game::kSetTime, nullptr, &coop_slot_set_time},
        COOP_INPUT_HOOK_ENTRIES
        COOP_NULL_GUARD_ENTRIES
    },
};

namespace {

constexpr uint16_t kPort = 27520;  // LANE1's port (27510 + 10 * 1)
constexpr uint32_t kNoon = 12 * 3600;

uint64_t g_main = 0;
uint64_t g_frames = 0;
bool g_host = false;  // the hooks were applied by a lifting recomp: no SVCs
bool g_time_done = false;
bool g_net_done = false;
alignas(0x1000) uint8_t g_socket_pool[0x400000];

// ---- A tiny log: text and numbers into one line, then the kernel's debug string ----
struct Line {
    char buf[200];
    size_t n = 0;
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
    Line& sdec(int64_t v) {
        if (v < 0) s("-");
        return dec(v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v));
    }
    ~Line() { svc::OutputDebugString(buf, n); }
};

Line Log() {
    Line l;
    l.s("coop-proto: ");
    return l;
}

bool Mapped(uint64_t addr) {
    if (g_host) return true;  // no QueryMemory on a recomp; the real core uses null-read guards
    svc::MemoryInfo mi{};
    if (svc::QueryMemory(&mi, addr) != 0) return false;
    return mi.perm != 0 && addr >= mi.addr && addr - mi.addr < mi.size;
}

bool ReadU64(uint64_t addr, uint64_t& out) {
    if (addr == 0 || (addr & 7) || !Mapped(addr)) return false;
    out = *reinterpret_cast<volatile uint64_t*>(addr);
    return true;
}

void FlushCode(uint64_t from, uint64_t size) {
    for (uint64_t a = from & ~uint64_t{63}; a < from + size; a += 64) __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (uint64_t a = from & ~uint64_t{63}; a < from + size; a += 64) __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
}

// Write one instruction of the game's code (same page permissions after).
bool PatchWord(uint64_t site, uint32_t insn) {
    const uint64_t page = site & ~uint64_t{0xfff};
    uint32_t r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRW);
    if (r != 0) {
        Log().s("hook: make main writable failed, result ").hex(r);
        return false;
    }
    *reinterpret_cast<volatile uint32_t*>(site) = insn;
    FlushCode(site, 4);
    r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRX);
    if (r != 0) Log().s("hook: restore RX failed, result ").hex(r);
    return true;
}

// Point the game's `bl` at `target`.
bool PatchBl(uint64_t site, uint64_t target) {
    const int64_t delta = static_cast<int64_t>(target - site);
    if ((delta & 3) || delta >= (int64_t{1} << 27) || delta < -(int64_t{1} << 27)) {
        Log().s("hook: ").hex(target).s(" out of bl range from ").hex(site);
        return false;
    }
    return PatchWord(site, 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu));
}

// The null guards (hooks.h kNullGuard, null_guard.h): every island is written in one writable
// window of the island page, then each guarded load becomes `b` to its island. A guard whose
// load or any kCheck before it differs (another mod, another build) is left out.
void ApplyNullGuards() {
    uint64_t sites[kNullGuardCount];  // (no `= {}`: freestanding, no memset; each is written before it is read)
    uint32_t islands[kNullGuardCount][null_guard::kIslandWords];
    uint32_t n = 0, wanted = 0;
    bool checks_ok = true;
    const uint64_t page = reinterpret_cast<uint64_t>(coop_null_islands);
    for (uint32_t i = 0; i < coop_hooks.count && i < kHookCount; ++i) {
        const hooks::Entry& e = coop_hooks.entries[i];
        if (e.kind != hooks::kCheck && e.kind != hooks::kNullGuard) continue;
        const uint64_t at = g_main + e.main_offset;
        const uint32_t insn = *reinterpret_cast<volatile uint32_t*>(at);
        if (e.kind == hooks::kCheck) {
            checks_ok = checks_ok && insn == e.expected_insn;
            continue;
        }
        ++wanted;
        const uint64_t island = page + n * null_guard::kIslandBytes;
        if (checks_ok && insn == e.expected_insn && n < kNullGuardCount &&
            null_guard::Island(at, island, insn, islands[n])) {
            sites[n++] = at;
        } else {
            Log().s("null guard at main+").hex(e.main_offset).s(": not the code it was made for, or out of reach: left out");
        }
        checks_ok = true;
    }
    if (n == 0) return;
    if (svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRW) != 0) {
        Log().s("null guards: the island page could not be made writable: none applied");
        return;
    }
    for (uint32_t k = 0; k < n; ++k)
        for (uint32_t w = 0; w < null_guard::kIslandWords; ++w)
            reinterpret_cast<volatile uint32_t*>(page)[k * null_guard::kIslandWords + w] = islands[k][w];
    FlushCode(page, n * null_guard::kIslandBytes);
    if (svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRX) != 0) {
        Log().s("null guards: the island page could not be made executable again: none applied");
        return;
    }
    uint32_t done = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t b = 0;
        if (null_guard::Branch(sites[k], page + k * null_guard::kIslandBytes, b) && PatchWord(sites[k], b)) ++done;
    }
    Log().s("null guards: ").dec(done).s(" of ").dec(wanted).s(" applied (a load through a null base reads 0)");
}

// hooks.h kImportRedirect: main's GOT entry `at` gets e.replacement; e.slot already holds the import's own target.
void RedirectImport(uint32_t i, const hooks::Entry& e, uint64_t at) {
    const uint64_t cur = *reinterpret_cast<volatile uint64_t*>(at);
    const bool unbound = cur >= g_main && cur - g_main < game::kMainCodeEnd;  // lazy binding: still main's PLT
    if (*e.slot == 0 || (cur != *e.slot && !unbound)) {
        Log().s("hook ").dec(i).s(": GOT main+").hex(e.main_offset).s(" holds ").hex(cur)
            .s(", not the import or main's PLT (another mod?): skipped");
        return;
    }
    svc::MemoryInfo mi{};
    if (svc::QueryMemory(&mi, at) != 0 || (mi.perm & 2) == 0) {
        const uint32_t r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, at & ~uint64_t{0xfff}, 0x1000, svc::kPermRW);
        if (r != 0) {
            Log().s("hook ").dec(i).s(": GOT main+").hex(e.main_offset).s(" not writable, result ").hex(r).s(": skipped");
            return;
        }
        Log().s("hook ").dec(i).s(": GOT page made writable (perm was ").dec(mi.perm).s(")");
    }
    *reinterpret_cast<volatile uint64_t*>(at) = reinterpret_cast<uint64_t>(e.replacement);
    Log().s("hook ").dec(i).s(": import at GOT main+").hex(e.main_offset).s(" now goes through the module (")
        .s(unbound ? "not bound yet" : "was bound").s(")");
}

bool IsBl(uint32_t insn) { return (insn & 0xfc000000u) == 0x94000000u; }

uint64_t BlTarget(uint64_t site, uint32_t insn) {
    const int64_t imm26 = static_cast<int32_t>(insn << 6) >> 6;  // sign-extend
    return site + static_cast<uint64_t>(imm26 * 4);
}

// What a lifting recomp does at build time, done here at run time (emulators, console).
void ApplyHooks() {
    const uint32_t count = coop_hooks.count;
    for (uint32_t i = 0; i < count && i < kHookCount; ++i) {
        const hooks::Entry& e = coop_hooks.entries[i];
        if (e.kind == hooks::kCheck || e.kind == hooks::kNullGuard) continue;  // (ApplyNullGuards)
        const uint64_t at = g_main + e.main_offset;
        const uint32_t insn = *reinterpret_cast<volatile uint32_t*>(at);
        if (e.expected_insn != 0 && insn != e.expected_insn) {
            Log().s("hook ").dec(i).s(": main+").hex(e.main_offset).s(" holds ").hex(insn)
                .s(", not ").hex(e.expected_insn).s(" (another mod?): skipped");
            continue;
        }
        switch (e.kind) {
            case hooks::kCallRedirect:
                if (!IsBl(insn)) {
                    Log().s("hook ").dec(i).s(": main+").hex(e.main_offset).s(" is not a bl: skipped");
                    break;
                }
                *e.slot = BlTarget(at, insn);
                if (PatchBl(at, reinterpret_cast<uint64_t>(e.replacement))) {
                    Log().s("hook ").dec(i).s(": frame hook in at main+").hex(e.main_offset)
                        .s(", continues at main+").hex(*e.slot - g_main);
                }
                break;
            case hooks::kImportRedirect:
                RedirectImport(i, e, at);
                break;
            case hooks::kGameFn:
                *e.slot = at;
                Log().s("hook ").dec(i).s(": game function slot = main+").hex(e.main_offset);
                break;
            default:
                Log().s("hook ").dec(i).s(": kind ").dec(e.kind).s(" not applied by the module");
                break;
        }
    }
    ApplyNullGuards();
}

const char* ButtonName(uint8_t bit) {
    static const char* const kNames[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus",
                                         "Left", "Up", "Right", "Down", "StickL-Left", "StickL-Up", "StickL-Right",
                                         "StickL-Down", "StickR-Left", "StickR-Up", "StickR-Right", "StickR-Down"};
    return bit < sizeof(kNames) / sizeof(kNames[0]) ? kNames[bit] : "other";
}

const char* UiName(uint8_t kind) {
    switch (kind) {
        case COOP_UI_UP: return "UP";
        case COOP_UI_DOWN: return "DOWN";
        case COOP_UI_LEFT: return "LEFT";
        case COOP_UI_RIGHT: return "RIGHT";
        case COOP_UI_CONFIRM: return "CONFIRM";
        case COOP_UI_BACK: return "BACK";
        case COOP_UI_TAB_PREV: return "TAB_PREV";
        case COOP_UI_TAB_NEXT: return "TAB_NEXT";
        case COOP_UI_OPEN: return "OPEN";
        case COOP_UI_CLOSE: return "CLOSE";
        default: return "?";
    }
}

// The pad hook's news since the last frame (switch/adapter/input.h), into the log.
bool g_pad_seen = false;
bool g_menu_was_open = false;
void LogInput() {
    pad_hook::Hook& h = input::hook();
    if (!g_pad_seen && h.pad(0).reads != 0) {
        g_pad_seen = true;
        Log().s("pad: player 1 read through the module (frame ").dec(g_frames).s(")");
    }
    pad_hook::Edge edges[16];
    for (size_t n = h.TakeEdges(edges, 16), k = 0; k < n; ++k) {
        const pad_hook::Edge& e = edges[k];
        const pad_hook::Pad& p = h.pad(e.player);
        Log().s("pad: player ").dec(e.player + 1u).s(e.down ? " down " : " up ").s(ButtonName(e.bit)).s(" (bit ")
            .dec(e.bit).s(", left stick ").sdec(p.stick[0]).s(",").sdec(p.stick[1]).s(")");
    }
    CoopUiEvent events[8];
    for (size_t n = h.TakeEvents(events, 8), k = 0; k < n; ++k) {
        Log().s("ui: ").s(UiName(events[k].kind)).s(events[k].repeat ? " (repeat)" : "");
    }
    if (h.open() != g_menu_was_open) {
        g_menu_was_open = h.open();
        Log().s(g_menu_was_open ? "pad: menu open, the game gets no pad from player 1 (closes by itself in 5 s)"
                                : "pad: menu closed, the game has the pad again");
    }
    if (g_frames % 600 == 0) {
        Line l = Log();
        l.s("pad: reads");
        for (uint32_t s = 0; s < input::kStyles; ++s) l.s(" ").s(input::StyleName(s)).s(" ").dec(input::Calls(s));
        l.s(", lost edges ").dec(h.dropped_edges()).s(", lost events ").dec(h.dropped_events());
    }
}

void TrySetTime() {
    uint64_t world = 0, time = 0;
    if (coop_slot_set_time == 0) return;
    if (!ReadU64(g_main + game::kWorldMgrPtr, world) || world == 0) return;
    if (!ReadU64(world + game::kTimeObject, time) || time == 0) return;
    Log().s("game call: set_time(world ").hex(world).s(", ").dec(kNoon).s(") at frame ").dec(g_frames);
    coop_call_set_time(world, kNoon);
    Log().s("game call: set_time returned");
    g_time_done = true;
}

void SayHello() {
    g_net_done = true;
    if (!nn::socket::IsInitialized()) {
        const uint32_t r = nn::socket::Initialize(g_socket_pool, sizeof(g_socket_pool), 0x20000, 4);
        Log().s("net: the game had no sockets yet; Initialize result ").hex(r);
        if (r != 0) return;
    } else {
        Log().s("net: using the game's socket library (already initialized)");
    }
    const int fd = nn::socket::Socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    if (fd < 0) {
        Log().s("net: socket failed");
        return;
    }
    sockaddr addr{};
    addr.sa_len = sizeof(addr);
    addr.sa_family = 2;
    addr.sa_data[0] = static_cast<char>(kPort >> 8);  // port, network order
    addr.sa_data[1] = static_cast<char>(kPort & 0xff);
    addr.sa_data[2] = 127;  // 127.0.0.1
    addr.sa_data[5] = 1;
    const int c = nn::socket::Connect(fd, &addr, sizeof(addr));
    static const char kHello[] = "coop-proto hello from subsdk1";
    long sent = -1;
    for (int i = 0; i < 3 && c == 0; ++i) sent = nn::socket::Send(fd, kHello, sizeof(kHello) - 1, 0);
    Log().s("net: connect ").dec(c == 0 ? 0 : 1).s(", sent ").dec(sent < 0 ? 0 : static_cast<uint64_t>(sent))
        .s(" bytes x3 to 127.0.0.1:").dec(kPort);
    nn::socket::Close(fd);
}

}  // namespace

// Every frame, before the game's frame step (start.s keeps all argument registers).
extern "C" COOP_EXPORT void coop_proto_frame() {
    ++g_frames;
    if (g_frames == 1 || g_frames % 600 == 0) Log().s("frame ").dec(g_frames);
    LogInput();
    if (!g_time_done && g_frames >= 300 && g_frames % 60 == 0) TrySetTime();
    if (!g_net_done && g_frames >= 900) SayHello();
}

// rtld calls this once, after nninitStartup and before nnMain (actor-map.md, Module _init).
extern "C" COOP_EXPORT void _init() {
    Log().s("hello from subsdk1 (ExeFS prototype)");
    // main's base without a kernel call: rtld linked nnMain and malloc from main.
    const uint64_t nn_main = reinterpret_cast<uint64_t>(&nnMain);
    g_main = nn_main - game::kNnMain;
    const uint64_t malloc_off = reinterpret_cast<uint64_t>(&malloc) - g_main;
    Log().s("main at ").hex(g_main).s(", malloc +").hex(malloc_off);
    if (malloc_off != game::kMalloc) {
        Log().s("not build e0f3ccfaf50065ae (1.1.1): no hooks");
        return;
    }
    if (*static_cast<volatile uint32_t*>(&coop_hooks.applied_by_host) != 0) {
        g_host = true;
        Log().s("hooks applied by the host (lifted module): nothing to patch");
        return;
    }
    input::FillSlots();
    ApplyHooks();
}
