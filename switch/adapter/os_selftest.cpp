// The OS slots' self-test for the module's one emulator run (v1 step 8, piece b; reports/v1-exefs-os-a.md). Driven
// from the frame hook: at frame kStartFrame it starts one thread through coop_plat_thread_start; that thread ticks 10
// times (holding a mutex the game thread takes every frame: locked on even frames, so the two contend, and try-locked
// on odd ones, so try_lock is seen both getting it and finding it held), then writes a 4 KB file, reads it back and
// hashes both, waits 50 ms on a condition variable nobody notifies (must time out: 0), waits again until the game
// thread notifies it (must wake: 1, well before its 2 s), and fills 32 random bytes twice (not all zero, different).
// The game thread joins it and logs one verdict line:
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
constexpr uint64_t kCondTimeoutUs = 50000;
constexpr uint64_t kCondWakeCapUs = 2000000;
constexpr size_t kRandomBytes = 32;

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
    int cond_timeout_ret;     // cond_wait_us(50 ms) with no notify
    uint64_t cond_timeout_us;
    int cond_wake_ret;        // the wait the game thread notifies
    uint64_t cond_wake_us;
    bool random_nonzero;
    bool random_differ;
    uint64_t random_first8;
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
uint64_t g_try_free = 0;      // odd frames: try_lock got the mutex
uint64_t g_try_held = 0;      // odd frames: try_lock found the thread holding it (and did not wait)
void* g_cond = nullptr;
void* g_cond_mutex = nullptr;
int g_want_notify = 0;        // under g_cond_mutex: the thread waits for the game thread's notify
int g_notified = 0;           // under g_cond_mutex
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

// Condition variable: one wait nobody notifies, one the game thread notifies (coop_os_selftest_frame).
void CondTests(Result& r) {
    coop_plat_mutex_lock(g_cond_mutex);
    uint64_t t0 = coop_plat_now_us();
    r.cond_timeout_ret = coop_plat_cond_wait_us(g_cond, g_cond_mutex, kCondTimeoutUs);
    r.cond_timeout_us = coop_plat_now_us() - t0;
    g_want_notify = 1;
    t0 = coop_plat_now_us();
    do {  // a wake with no notify (spurious) waits again; the game thread can notify only while we wait
        r.cond_wake_ret = coop_plat_cond_wait_us(g_cond, g_cond_mutex, kCondWakeCapUs);
    } while (!g_notified && r.cond_wake_ret != 0);
    r.cond_wake_us = coop_plat_now_us() - t0;
    coop_plat_mutex_unlock(g_cond_mutex);
}

void RandomTests(Result& r) {
    uint8_t a[kRandomBytes];  // (not zeroed first: a zeroing loop may become a memset call; read only when filled)
    uint8_t b[kRandomBytes];
    const bool filled = coop_plat_random_fill(a, kRandomBytes) == 0 && coop_plat_random_fill(b, kRandomBytes) == 0;
    bool nonzero = false, differ = false;
    for (size_t i = 0; filled && i < kRandomBytes; ++i) {
        nonzero = nonzero || a[i] != 0;
        differ = differ || a[i] != b[i];
    }
    r.random_nonzero = filled && nonzero;
    r.random_differ = filled && differ;
    for (int i = 0; filled && i < 8; ++i) r.random_first8 = (r.random_first8 << 8) | a[i];
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
    coop_plat_cond_free(g_cond);
    coop_plat_mutex_free(g_cond_mutex);
    g_cond = g_cond_mutex = nullptr;
    const uint64_t used_after = ArenaUsed();
    Line().s("thread ticks ").dec(static_cast<uint64_t>(r.ticks)).s(", now_us delta min ").dec(r.min_delta_us)
        .s(" max ").dec(r.max_delta_us);
    Line().s("mutex: game took it ").dec(g_game_adds).s(" times, waited (>1 ms) ").dec(g_contended)
        .s(" times, longest ").dec(g_max_wait_us).s(" us; counter ").dec(shared).s(" (expected ")
        .dec(g_game_adds + kTicks).s(")");
    Line().s("try_lock: got it ").dec(g_try_free).s(" times, saw it held ").dec(g_try_held).s(" times (never waited)");
    Line().s("cond: wait 50 ms with no notify returned ").dec(static_cast<uint64_t>(r.cond_timeout_ret)).s(" after ")
        .dec(r.cond_timeout_us).s(" us; notified wait returned ").dec(static_cast<uint64_t>(r.cond_wake_ret))
        .s(" after ").dec(r.cond_wake_us).s(" us");
    Line().s("random: 32 bytes, first 8 ").hex(r.random_first8).s(r.random_nonzero ? ", not all zero" : ", ALL ZERO")
        .s(r.random_differ ? ", two calls differ" : ", two calls SAME");
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
    else if (g_try_free == 0) fail = "try_lock never got the free mutex";
    else if (g_try_held == 0) fail = "try_lock never saw the mutex held";
    else if (r.cond_timeout_ret != 0 || r.cond_timeout_us < kCondTimeoutUs - kCondTimeoutUs / 10 ||
             r.cond_timeout_us > 20 * kCondTimeoutUs)
        fail = "cond timeout (want 0 after about 50 ms)";
    else if (r.cond_wake_ret != 1 || r.cond_wake_us >= kCondWakeCapUs / 2) fail = "cond notify (want 1, soon)";
    else if (!r.random_nonzero || !r.random_differ) fail = "random";
    else if (used_after != g_used_before) fail = "arena not back to its start";
    if (fail) Line().s("FAIL ").s(fail);
    else Line().s("PASS (thread, mutex, try_lock, cond, random, now_us, file, arena)");
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
    CondTests(r);
    RandomTests(r);
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
        g_cond_mutex = coop_plat_mutex_new();
        g_cond = coop_plat_cond_new();
        const bool made = g_mutex && g_cond_mutex && g_cond;
        g_thread = made ? coop_plat_thread_start(&coop_os_selftest_thread, nullptr, -1, 0) : nullptr;
        if (!g_thread) {
            coop_plat_mutex_free(g_mutex);  // (each takes nullptr)
            coop_plat_mutex_free(g_cond_mutex);
            coop_plat_cond_free(g_cond);
            g_mutex = g_cond_mutex = g_cond = nullptr;
            g_state = 2;
            Line().s(made ? "FAIL thread start" : "FAIL mutex or cond new");
        }
        return;
    }
    if (g_state != 1) return;
    if (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        g_state = 2;
        Finish();
        return;
    }
    if (frame & 1) {  // odd frames: try_lock, which must never wait
        if (coop_plat_mutex_try_lock(g_mutex)) {
            ++g_try_free;
            ++g_shared;
            ++g_game_adds;
            coop_plat_mutex_unlock(g_mutex);
        } else {
            ++g_try_held;
        }
    } else {  // even frames: lock, and how long it waited
        const uint64_t before = coop_plat_now_us();
        coop_plat_mutex_lock(g_mutex);
        const uint64_t waited = coop_plat_now_us() - before;
        ++g_shared;
        ++g_game_adds;
        coop_plat_mutex_unlock(g_mutex);
        if (waited > kContendedUs) ++g_contended;
        if (waited > g_max_wait_us) g_max_wait_us = waited;
    }
    // The thread's second condition wait: notify it (only possible while it waits: it holds the mutex otherwise).
    coop_plat_mutex_lock(g_cond_mutex);
    if (g_want_notify && !g_notified) {
        g_notified = 1;
        coop_plat_cond_notify_one(g_cond);
    }
    coop_plat_mutex_unlock(g_cond_mutex);
}

}  // extern "C"
