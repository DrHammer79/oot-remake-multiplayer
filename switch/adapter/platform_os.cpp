// The ExeFS adapter's OS slots (v1 step 8, piece b): CoopPlatform's threads, locks, time, memory, files and log on
// the game's own nn::os and nn::fs (nn_imports.h), defined as the coop_plat_* functions a payload build of coop::os
// calls directly (coop_platform_static.h). Sockets are platform_net.cpp's (piece c).
//
// - Threads: nn::os threads with stacks from our arena, priority nn::os's default, the process's default core unless
//   one is asked for. Priority 24, below the game's threads (nn_imports.h kCoopPriority says why).
// - Locks: nn::os mutexes (not recursive) and condition variables in our arena.
// - Random: nn::crypto's cryptographic source (csrng), never nn::os::GenerateRandomBytes (TinyMT).
// - Time: the system tick over the system's own tick frequency (read, never assumed).
// - Memory: our own arena (arena.h) in this module's bss, COOP_ARENA_BYTES big, never the game's heap.
// - Files: the SD card, mounted as "coopsd" on first use, under config/oot-coop/ (on an emulator its sdmc folder).
//   Writes go to "<file>.tmp" first (flushed), then replace the file by a rename, so a file is never half
//   written; a read that finds no file takes a left "<file>.tmp" (a write cut off between delete and rename).
// - Log: the kernel's debug string (svc 0x27), which the emulator's log shows; one "coop-proto: " line per call
//   (the module's one log tag, which tools/switch_mod/run_check.py reads).
#include <coop_platform_static.h>

#include "../proto/hooks.h"  // COOP_EXPORT
#include "../proto/svc.h"
#include "arena.h"
#include "nn_imports.h"
#include "platform_os.h"
#include "os_util.h"

#ifndef COOP_ARENA_BYTES
#define COOP_ARENA_BYTES (8u << 20)
#endif

namespace {

constexpr const char* kMountName = "coopsd";
constexpr const char* kRoot = "coopsd:/config/oot-coop/";
constexpr size_t kPathMax = 256;
constexpr size_t kDefaultStack = 128 * 1024;
constexpr size_t kMinStack = 16 * 1024;

alignas(0x1000) uint8_t g_arena_mem[COOP_ARENA_BYTES];
constinit coop_arena::Arena g_arena;
alignas(16) uint8_t g_arena_lock[nn::os::kMutexTypeBytes];
alignas(16) uint8_t g_fs_lock[nn::os::kMutexTypeBytes];
bool g_ready = false;
long g_tick_freq = 0;
int g_sd = 0;  // 0 not tried, 1 mounted, -1 refused (logged once)

nn::os::MutexType* ArenaLock() { return reinterpret_cast<nn::os::MutexType*>(g_arena_lock); }
nn::os::MutexType* FsLock() { return reinterpret_cast<nn::os::MutexType*>(g_fs_lock); }

size_t Length(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

// One log line with a number in it, without a formatter.
void LogResult(const char* what, uint32_t result) {
    char line[96];
    size_t n = 0;
    for (const char* c = what; *c && n < 70; ++c) line[n++] = *c;
    const char* hex = "0123456789abcdef";
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) line[n++] = hex[(result >> shift) & 0xf];
    line[n] = '\0';
    coop_plat_log(line);
}

struct ThreadRec {
    alignas(16) uint8_t type[nn::os::kThreadTypeBytes];
    void* stack;
};

nn::os::ThreadType* TypeOf(ThreadRec* rec) { return reinterpret_cast<nn::os::ThreadType*>(rec->type); }

// The SD card, mounted once. Called with the fs lock held.
bool SdReady() {
    if (g_sd == 0) {
        const uint32_t r = nn::fs::MountSdCard(kMountName);
        g_sd = r == 0 ? 1 : -1;
        if (r != 0) LogResult("files: the SD card could not be mounted (no file slots), result", r);
    }
    return g_sd == 1;
}

// Every folder of `path` below the mount's root, made if missing (an existing one is not an error).
void MakeParents(const char* path) {
    char part[kPathMax];
    const size_t root = Length(kMountName) + 2;  // "coopsd:/"
    for (size_t i = 0; path[i] && i < kPathMax - 1; ++i) {
        part[i] = path[i];
        if (path[i] == '/' && i > root) {
            part[i] = '\0';
            nn::fs::CreateDirectory(part);
            part[i] = '/';
        }
    }
}

int ReadLocked(const char* path, void** buf, size_t* n) {
    nn::fs::FileHandle file{};
    if (nn::fs::OpenFile(&file, path, nn::fs::kOpenRead) != 0) return -1;
    long size = 0;
    int result = -1;
    if (nn::fs::GetFileSize(&size, file) == 0 && size >= 0) {
        void* data = coop_plat_mem_alloc(size ? static_cast<size_t>(size) : 1, 16);
        unsigned long got = 0;
        if (data && (size == 0 || (nn::fs::ReadFile(&got, file, 0, data, static_cast<unsigned long>(size)) == 0 &&
                                   got == static_cast<unsigned long>(size)))) {
            *buf = data;
            *n = static_cast<size_t>(size);
            result = 0;
        } else {
            coop_plat_mem_free(data);
        }
    }
    nn::fs::CloseFile(file);
    return result;
}

int WriteLocked(const char* path, const char* tmp, const void* buf, size_t n) {
    MakeParents(path);
    nn::fs::DeleteFile(tmp);  // a leftover from a write that was cut off
    if (nn::fs::CreateFile(tmp, static_cast<long>(n)) != 0) return -1;
    nn::fs::FileHandle file{};
    if (nn::fs::OpenFile(&file, tmp, nn::fs::kOpenWrite) != 0) return -1;
    const nn::fs::WriteOption flush{nn::fs::kWriteFlush};
    const bool written = n == 0 || nn::fs::WriteFile(file, 0, buf, n, flush) == 0;
    const bool flushed = nn::fs::FlushFile(file) == 0;
    nn::fs::CloseFile(file);
    if (!written || !flushed) {
        nn::fs::DeleteFile(tmp);
        return -1;
    }
    nn::fs::DeleteFile(path);  // RenameFile does not replace
    return nn::fs::RenameFile(tmp, path) == 0 ? 0 : -1;
}

}  // namespace

extern "C" {

// Once, from the module's _init (one thread then): the locks, the arena, the tick frequency.
COOP_EXPORT void coop_plat_os_init() {
    if (g_ready) return;
    nn::os::InitializeMutex(ArenaLock(), false, 0);
    nn::os::InitializeMutex(FsLock(), false, 0);
    g_arena.Init(g_arena_mem, sizeof(g_arena_mem));
    g_tick_freq = nn::os::GetSystemTickFrequency();
    g_ready = true;
}

// The arena's numbers, for the self-test's log.
COOP_EXPORT void coop_plat_arena_stats(uint64_t out[5]) {
    nn::os::LockMutex(ArenaLock());
    const coop_arena::Stats s = g_arena.GetStats();
    nn::os::UnlockMutex(ArenaLock());
    out[0] = s.capacity;
    out[1] = s.used;
    out[2] = s.peak;
    out[3] = s.largest_free;
    out[4] = s.failed + s.bad_frees;
}

// ---- threads
COOP_EXPORT void* coop_plat_thread_start(void (*fn)(void* arg), void* arg, int core, size_t stack) {
    if (!fn) return nullptr;
    size_t size = stack ? (stack + 0xfff) & ~size_t{0xfff} : kDefaultStack;
    if (size < kMinStack) size = kMinStack;
    auto* rec = static_cast<ThreadRec*>(coop_plat_mem_alloc(sizeof(ThreadRec), 16));
    void* mem = coop_plat_mem_alloc(size, 0x1000);
    if (!rec || !mem) {
        coop_plat_mem_free(rec);
        coop_plat_mem_free(mem);
        coop_plat_log("threads: no memory for a thread");
        return nullptr;
    }
    rec->stack = mem;
    const uint32_t r = core < 0
        ? nn::os::CreateThread(TypeOf(rec), fn, arg, mem, size, nn::os::kCoopPriority)
        : nn::os::CreateThread(TypeOf(rec), fn, arg, mem, size, nn::os::kCoopPriority, core);
    if (r != 0) {
        LogResult("threads: CreateThread failed, result", r);
        coop_plat_mem_free(mem);
        coop_plat_mem_free(rec);
        return nullptr;
    }
    nn::os::SetThreadName(TypeOf(rec), "coop");
    nn::os::StartThread(TypeOf(rec));
    return rec;
}

COOP_EXPORT int coop_plat_thread_join(void* thread) {
    auto* rec = static_cast<ThreadRec*>(thread);
    if (!rec) return -1;
    nn::os::WaitThread(TypeOf(rec));
    nn::os::DestroyThread(TypeOf(rec));
    coop_plat_mem_free(rec->stack);
    coop_plat_mem_free(rec);
    return 0;
}

COOP_EXPORT uint64_t coop_plat_thread_id(void) { return nn::os::GetThreadId(nn::os::GetCurrentThread()); }

// ---- locks
COOP_EXPORT void* coop_plat_mutex_new(void) {
    void* m = coop_plat_mem_alloc(nn::os::kMutexTypeBytes, 16);
    if (m) nn::os::InitializeMutex(static_cast<nn::os::MutexType*>(m), false, 0);
    return m;
}
COOP_EXPORT void coop_plat_mutex_lock(void* mutex) { nn::os::LockMutex(static_cast<nn::os::MutexType*>(mutex)); }
COOP_EXPORT void coop_plat_mutex_unlock(void* mutex) { nn::os::UnlockMutex(static_cast<nn::os::MutexType*>(mutex)); }
COOP_EXPORT void coop_plat_mutex_free(void* mutex) {
    if (!mutex) return;
    nn::os::FinalizeMutex(static_cast<nn::os::MutexType*>(mutex));
    coop_plat_mem_free(mutex);
}
COOP_EXPORT int coop_plat_mutex_try_lock(void* mutex) {
    return nn::os::TryLockMutex(static_cast<nn::os::MutexType*>(mutex)) ? 1 : 0;
}

// ---- condition variables (with a mutex from coop_plat_mutex_new)
COOP_EXPORT void* coop_plat_cond_new(void) {
    void* c = coop_plat_mem_alloc(nn::os::kCondTypeBytes, 16);
    if (c) nn::os::InitializeConditionVariable(static_cast<nn::os::ConditionVariableType*>(c));
    return c;
}
COOP_EXPORT int coop_plat_cond_wait_us(void* cond, void* mutex, uint64_t us) {
    constexpr uint64_t kMaxUs = uint64_t{1} << 40;
    const nn::TimeSpan timeout{static_cast<long>((us < kMaxUs ? us : kMaxUs) * 1000u)};
    return nn::os::TimedWaitConditionVariable(static_cast<nn::os::ConditionVariableType*>(cond),
                                              static_cast<nn::os::MutexType*>(mutex), timeout) == nn::os::kCondTimedOut
               ? 0
               : 1;
}
COOP_EXPORT void coop_plat_cond_notify_one(void* cond) {
    nn::os::SignalConditionVariable(static_cast<nn::os::ConditionVariableType*>(cond));
}
COOP_EXPORT void coop_plat_cond_notify_all(void* cond) {
    nn::os::BroadcastConditionVariable(static_cast<nn::os::ConditionVariableType*>(cond));
}
COOP_EXPORT void coop_plat_cond_free(void* cond) {
    if (!cond) return;
    nn::os::FinalizeConditionVariable(static_cast<nn::os::ConditionVariableType*>(cond));
    coop_plat_mem_free(cond);
}

// ---- random: the system's cryptographic source (keys and join codes come from it; never TinyMT)
COOP_EXPORT int coop_plat_random_fill(void* out, size_t n) {
    if (!out) return -1;
    if (n) nn::crypto::GenerateCryptographicallyRandomBytes(out, n);
    return 0;
}

// ---- time
COOP_EXPORT uint64_t coop_plat_now_us(void) {
    if (g_tick_freq <= 0) g_tick_freq = nn::os::GetSystemTickFrequency();
    return coop_os_util::TicksToUs(static_cast<uint64_t>(nn::os::GetSystemTick().value), g_tick_freq);
}
COOP_EXPORT void coop_plat_sleep_us(uint64_t us) {
    constexpr uint64_t kMaxUs = uint64_t{1} << 40;  // ~12 days; keeps the nanoseconds in range
    nn::os::SleepThread(nn::TimeSpan{static_cast<long>((us < kMaxUs ? us : kMaxUs) * 1000u)});
}

// ---- memory
COOP_EXPORT void* coop_plat_mem_alloc(size_t n, size_t align) {
    if (!g_ready) coop_plat_os_init();
    nn::os::LockMutex(ArenaLock());
    void* p = g_arena.Alloc(n, align);
    nn::os::UnlockMutex(ArenaLock());
    return p;
}
COOP_EXPORT void coop_plat_mem_free(void* p) {
    if (!p || !g_ready) return;
    nn::os::LockMutex(ArenaLock());
    g_arena.Free(p);
    nn::os::UnlockMutex(ArenaLock());
}

// ---- files
COOP_EXPORT int coop_plat_file_read(const char* rel, void** buf, size_t* n) {
    if (!buf || !n) return -1;
    *buf = nullptr;
    *n = 0;
    char path[kPathMax];
    char tmp[kPathMax];
    if (!coop_os_util::JoinPath(path, sizeof(path), kRoot, rel, nullptr) ||
        !coop_os_util::JoinPath(tmp, sizeof(tmp), kRoot, rel, ".tmp"))
        return -1;
    if (!g_ready) coop_plat_os_init();
    nn::os::LockMutex(FsLock());
    // A .tmp is only ever renamed over the file after it was written whole and flushed: when the file is missing
    // (a write cut off after the delete), the .tmp is the newest complete copy.
    const int r = SdReady() ? (ReadLocked(path, buf, n) == 0 ? 0 : ReadLocked(tmp, buf, n)) : -1;
    nn::os::UnlockMutex(FsLock());
    return r;
}

COOP_EXPORT int coop_plat_file_write(const char* rel, const void* buf, size_t n) {
    if (n && !buf) return -1;
    char path[kPathMax];
    char tmp[kPathMax];
    if (!coop_os_util::JoinPath(path, sizeof(path), kRoot, rel, nullptr) ||
        !coop_os_util::JoinPath(tmp, sizeof(tmp), kRoot, rel, ".tmp"))
        return -1;
    if (!g_ready) coop_plat_os_init();
    nn::os::LockMutex(FsLock());
    const int r = SdReady() ? WriteLocked(path, tmp, buf, n) : -1;
    nn::os::UnlockMutex(FsLock());
    return r;
}

// ---- log
COOP_EXPORT void coop_plat_log(const char* line) {
    char out[256];  // (filled by hand: an initialised array becomes a memset call, which the module has not got)
    size_t n = 0;
    for (const char* c = "coop-proto: "; *c; ++c) out[n++] = *c;
    for (const char* c = line ? line : ""; *c && n < sizeof(out) - 1; ++c) out[n++] = *c;
    svc::OutputDebugString(out, n);
}

}  // extern "C"
