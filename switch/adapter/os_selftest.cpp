// The OS slots' self-test for the module's one emulator run (v1 step 8, piece b; reports/v1-exefs-os-a.md). Driven
// from the frame hook: at frame kStartFrame it starts one thread through coop_plat_thread_start; that thread ticks 10
// times (holding a mutex the game thread also takes every frame, so the two contend), then writes a 4 KB file, reads it
// back and hashes both. The game thread joins it and logs one verdict line:
//   "coop-proto: os selftest: PASS ..." or "... FAIL <what>" (the module's one log tag, run_check.py).
// Everything goes through the coop_plat_* functions, exactly as a payload build of coop::os calls them.
#include <coop_platform_static.h>

#include "../proto/hooks.h"  // COOP_EXPORT
#include "os_util.h"
#include "platform_os.h"
#include "os_selftest.h"

namespace {

constexpr uint64_t kStartFrame = 1200;
constexpr int kTicks = 10;
constexpr uint64_t kHoldUs = 20000;    // the thread holds the mutex this long per tick: a frame (16-33 ms) runs into it
constexpr uint64_t kGapUs = 100000;    // and sleeps this long between ticks
constexpr uint64_t kContendedUs = 1000;
constexpr size_t kFileBytes = 4096;
constexpr const char* kFile = "selftest/os.bin";

// A log line built from text and numbers (no formatter in the module).
struct Line {
    char buf[200];
    size_t n = 0;
    Line() { s("os selftest: "); }
    Line& s(const char* t) {
        while (*t && n < sizeof(buf) - 1) buf[n++] = *t++;
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
    ~Line() {
        buf[n] = '\0';
        coop_plat_log(buf);
    }
};

// What the thread found (written by it, read by the game thread after `g_done`).
struct Result {
    int ticks;
    uint64_t min_delta_us;
    uint64_t max_delta_us;
    bool wrote;
    bool read;
    uint64_t read_bytes;
    uint64_t hash_out;
    uint64_t hash_in;
};

int g_state = 0;  // 0 waiting, 1 running, 2 finished
void* g_thread = nullptr;
void* g_mutex = nullptr;
uint64_t g_shared = 0;        // under g_mutex: both sides add to it
uint64_t g_game_adds = 0;
uint64_t g_contended = 0;     // frames the game thread waited > kContendedUs for the mutex
uint64_t g_max_wait_us = 0;
uint64_t g_used_before = 0;
uint32_t g_done = 0;          // set (release) by the thread when `g_result` is complete
Result g_result;

uint64_t ArenaUsed() {
    uint64_t s[5];
    coop_plat_arena_stats(s);
    return s[1];
}

void LogArena(const char* when) {
    uint64_t s[5];
    coop_plat_arena_stats(s);
    Line().s("arena ").s(when).s(": ").dec(s[0]).s(" bytes, used ").dec(s[1]).s(", peak ").dec(s[2])
        .s(", largest free ").dec(s[3]).s(", refused/bad ").dec(s[4]);
}

void FileRoundTrip(Result& r) {
    auto* out = static_cast<uint8_t*>(coop_plat_mem_alloc(kFileBytes, 16));
    if (!out) return;
    for (size_t i = 0; i < kFileBytes; ++i) out[i] = static_cast<uint8_t>(i * 31 + 7);
    r.hash_out = coop_os_util::Fnv1a(out, kFileBytes);
    r.wrote = coop_plat_file_write(kFile, out, kFileBytes) == 0;
    coop_plat_mem_free(out);
    void* in = nullptr;
    size_t n = 0;
    r.read = coop_plat_file_read(kFile, &in, &n) == 0;
    if (r.read) {
        r.read_bytes = n;
        r.hash_in = coop_os_util::Fnv1a(in, n);
        coop_plat_mem_free(in);
    }
}

void Finish() {
    coop_plat_thread_join(g_thread);
    g_thread = nullptr;
    const Result& r = g_result;
    coop_plat_mutex_lock(g_mutex);
    const uint64_t shared = g_shared;
    coop_plat_mutex_unlock(g_mutex);
    coop_plat_mutex_free(g_mutex);
    g_mutex = nullptr;
    const uint64_t used_after = ArenaUsed();
    Line().s("thread ticks ").dec(static_cast<uint64_t>(r.ticks)).s(", now_us delta min ").dec(r.min_delta_us)
        .s(" max ").dec(r.max_delta_us);
    Line().s("mutex: game took it ").dec(g_game_adds).s(" times, waited (>1 ms) ").dec(g_contended)
        .s(" times, longest ").dec(g_max_wait_us).s(" us; counter ").dec(shared).s(" (expected ")
        .dec(g_game_adds + kTicks).s(")");
    Line().s("file: wrote ").s(r.wrote ? "ok" : "FAILED").s(", read ").s(r.read ? "ok " : "FAILED ")
        .dec(r.read_bytes).s(" bytes, hash out ").hex(r.hash_out).s(" in ").hex(r.hash_in);
    LogArena("after");

    const char* fail = nullptr;
    if (r.ticks != kTicks) fail = "thread ticks";
    else if (r.min_delta_us < kGapUs || r.max_delta_us > 20 * kGapUs) fail = "now_us deltas";
    else if (shared != g_game_adds + kTicks) fail = "mutex counter";
    else if (g_contended == 0) fail = "mutex never contended";
    else if (!r.wrote || !r.read) fail = "file round trip";
    else if (r.read_bytes != kFileBytes || r.hash_in != r.hash_out) fail = "file contents";
    else if (used_after != g_used_before) fail = "arena not back to its start";
    if (fail) Line().s("FAIL ").s(fail);
    else Line().s("PASS (thread, mutex, now_us, file, arena)");
}

}  // namespace

extern "C" {

// The self-test's thread (an exported function: a lifter finds every function handed to thread_start by name).
COOP_EXPORT void coop_os_selftest_thread(void*) {
    Result& r = g_result;
    r.min_delta_us = ~uint64_t{0};
    uint64_t last = coop_plat_now_us();
    for (int k = 0; k < kTicks; ++k) {
        coop_plat_sleep_us(kGapUs);
        coop_plat_mutex_lock(g_mutex);
        ++g_shared;
        coop_plat_sleep_us(kHoldUs);
        coop_plat_mutex_unlock(g_mutex);
        const uint64_t now = coop_plat_now_us();
        const uint64_t delta = now - last;
        last = now;
        if (delta < r.min_delta_us) r.min_delta_us = delta;
        if (delta > r.max_delta_us) r.max_delta_us = delta;
        ++r.ticks;
        Line().s("thread tick ").dec(static_cast<uint64_t>(k + 1)).s(", now_us ").dec(now).s(" (+").dec(delta).s(")");
    }
    FileRoundTrip(r);
    __atomic_store_n(&g_done, 1u, __ATOMIC_RELEASE);
}

COOP_EXPORT void coop_os_selftest_frame(uint64_t frame) {
    if (g_state == 0) {
        if (frame < kStartFrame) return;
        g_state = 1;
        g_used_before = ArenaUsed();
        LogArena("before");
        Line().s("start at frame ").dec(frame).s(", now_us ").dec(coop_plat_now_us()).s(", thread id ")
            .dec(coop_plat_thread_id());
        g_mutex = coop_plat_mutex_new();
        g_thread = g_mutex ? coop_plat_thread_start(&coop_os_selftest_thread, nullptr, -1, 0) : nullptr;
        if (!g_thread) {
            if (g_mutex) coop_plat_mutex_free(g_mutex);
            g_mutex = nullptr;
            g_state = 2;
            Line().s("FAIL thread start");
        }
        return;
    }
    if (g_state != 1) return;
    if (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        g_state = 2;
        Finish();
        return;
    }
    const uint64_t before = coop_plat_now_us();
    coop_plat_mutex_lock(g_mutex);
    const uint64_t waited = coop_plat_now_us() - before;
    ++g_shared;
    ++g_game_adds;
    coop_plat_mutex_unlock(g_mutex);
    if (waited > kContendedUs) ++g_contended;
    if (waited > g_max_wait_us) g_max_wait_us = waited;
}

}  // extern "C"
