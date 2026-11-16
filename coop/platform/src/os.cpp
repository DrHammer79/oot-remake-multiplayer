// SPDX-License-Identifier: GPL-3.0-or-later
// coop::os over the installed CoopPlatform table (coop/os.hpp).
#include "coop/os.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#ifdef COOP_PLATFORM_STATIC
#include <coop_platform_static.h>

#include <type_traits>
#endif

namespace coop::os {
namespace {

std::atomic<const CoopPlatform *> g_platform{nullptr};

// Version 1's last slot: a table must reach at least this far.
constexpr std::size_t kMinSize = offsetof(CoopPlatform, log) + sizeof(CoopPlatform::log);

template <class Slot>
bool Missing(int required, Slot slot) {
    return required != 0 && slot == nullptr;
}

#ifdef COOP_PLATFORM_STATIC
// ---- The payload build (coop_platform_static.h): every required slot is a direct call of the adapter's
// coop_plat_<slot>, so the payload makes no indirect call for them (a recompiler lifts it). There is one backend from
// the first call on: an object made before Install (a static Mutex or CondVar) uses the same functions as one made
// after it, so a wait and its notify can never land on different backends. An installed table still gives the
// optional game slots, kind and main_base; its required slots are the same functions by contract.
#define COOP_OS_STATIC_TYPE(name, required) COOP_OS_STATIC_TYPE_##required(name)
#define COOP_OS_STATIC_TYPE_0(name)
#define COOP_OS_STATIC_TYPE_1(name)                                                            \
    static_assert(std::is_same_v<decltype(&coop_plat_##name), decltype(CoopPlatform::name)>, \
                  "coop_plat_" #name " must have the type of CoopPlatform::" #name);
#define COOP_OS_STATIC_SLOT(name, required) COOP_OS_STATIC_SLOT_##required(name)
#define COOP_OS_STATIC_SLOT_0(name)
#define COOP_OS_STATIC_SLOT_1(name) p.name = &coop_plat_##name;
COOP_PLATFORM_SLOTS(COOP_OS_STATIC_TYPE)

// The table before any Install: the adapter's functions, no game slots.
constexpr CoopPlatform MakeStatic() {
    CoopPlatform p{};
    p.version = COOP_PLATFORM_VERSION;
    p.size = sizeof(CoopPlatform);
    p.kind = COOP_PLATFORM_KIND_EXEFS_EMU;
    COOP_PLATFORM_SLOTS(COOP_OS_STATIC_SLOT)
    return p;
}
constinit const CoopPlatform kStatic = MakeStatic();
#undef COOP_OS_STATIC_SLOT_1
#undef COOP_OS_STATIC_SLOT_0
#undef COOP_OS_STATIC_SLOT
#undef COOP_OS_STATIC_TYPE_1
#undef COOP_OS_STATIC_TYPE_0
#undef COOP_OS_STATIC_TYPE

const CoopPlatform &P() {
    const CoopPlatform *p = g_platform.load(std::memory_order_acquire);
    return p ? *p : kStatic;
}

// `owner` (the table an object was made with) is not read: there is one set of functions.
#define COOP_OS(owner, slot, ...) ((void)(owner), coop_plat_##slot(__VA_ARGS__))
// A required slot that a table appended later (COOP_PLATFORM_HAS): always there in a payload.
#define COOP_OS_HAS(owner, slot) ((void)(owner), true)
#else
const CoopPlatform &P() {
    const CoopPlatform *p = g_platform.load(std::memory_order_acquire);
    return p ? *p : *StdPlatform();
}

#define COOP_OS(owner, slot, ...) ((owner)->slot(__VA_ARGS__))
#define COOP_OS_HAS(owner, slot) (COOP_PLATFORM_HAS(owner, slot) && (owner)->slot)
#endif

}  // namespace

InstallError Install(const CoopPlatform *platform) {
    if (!platform) return InstallError::kNull;
    if (platform->version != COOP_PLATFORM_VERSION) return InstallError::kVersion;
    if (platform->size < kMinSize) return InstallError::kSize;
#define COOP_OS_CHECK_SLOT(name, required) \
    if (COOP_PLATFORM_HAS(platform, name) && Missing(required, platform->name)) return InstallError::kMissingSlot;
    COOP_PLATFORM_SLOTS(COOP_OS_CHECK_SLOT)
#undef COOP_OS_CHECK_SLOT
    g_platform.store(platform, std::memory_order_release);
    return InstallError::kNone;
}

const CoopPlatform &Platform() { return P(); }

std::uint64_t NowUs() { return COOP_OS(&P(), now_us); }
void SleepUs(std::uint64_t us) { COOP_OS(&P(), sleep_us, us); }
void Log(const char *line) { COOP_OS(&P(), log, line ? line : ""); }

void *Alloc(std::size_t n, std::size_t align) { return COOP_OS(&P(), mem_alloc, n, align); }
void Free(void *p) { COOP_OS(&P(), mem_free, p); }

bool IsModPath(const char *rel) {
    if (!rel || !*rel || rel[0] == '/') return false;
    const char *part = rel;
    for (const char *c = rel;; ++c) {
        if (*c == '\\' || *c == ':') return false;
        if (*c == '/' || *c == '\0') {
            const std::size_t len = static_cast<std::size_t>(c - part);
            if (len == 0) return false;                                     // "a//b" or a trailing '/'
            if (len == 2 && part[0] == '.' && part[1] == '.') return false;
            if (*c == '\0') return true;
            part = c + 1;
        }
    }
}

bool ReadFile(const char *rel, std::vector<std::uint8_t> &out) {
    out.clear();
    if (!IsModPath(rel)) return false;
    const CoopPlatform *p = &P();
    void *buf = nullptr;
    std::size_t n = 0;
    if (COOP_OS(p, file_read, rel, &buf, &n) != 0) return false;
    if (n) out.assign(static_cast<const std::uint8_t *>(buf), static_cast<const std::uint8_t *>(buf) + n);
    COOP_OS(p, mem_free, buf);
    return true;
}

bool WriteFile(const char *rel, const void *data, std::size_t n) {
    if (!IsModPath(rel) || (n && !data)) return false;
    return COOP_OS(&P(), file_write, rel, data, n) == 0;
}

Mutex::Mutex() : owner_(&P()) {
    handle_ = COOP_OS(owner_, mutex_new);
    if (!handle_) COOP_OS(owner_, log, "coop::os: mutex_new failed: this lock gives no exclusion");
}
Mutex::~Mutex() {
    // Cleared first: a lock on a static mutex from a thread still running at exit is then a no-op, not a freed one.
    void *h = handle_;
    handle_ = nullptr;
    if (h) COOP_OS(owner_, mutex_free, h);
}
void Mutex::lock() {
    if (handle_) COOP_OS(owner_, mutex_lock, handle_);
}
void Mutex::unlock() {
    if (handle_) COOP_OS(owner_, mutex_unlock, handle_);
}

bool Mutex::try_lock() {
    if (!handle_) return true;   // as lock(): no handle, no exclusion
    if (COOP_OS_HAS(owner_, mutex_try_lock)) return COOP_OS(owner_, mutex_try_lock, handle_) != 0;
    COOP_OS(owner_, mutex_lock, handle_);
    return true;
}

namespace {
bool HasCond([[maybe_unused]] const CoopPlatform *p) {
    return COOP_OS_HAS(p, cond_new) && COOP_OS_HAS(p, cond_wait_us) && COOP_OS_HAS(p, cond_notify_one) &&
           COOP_OS_HAS(p, cond_notify_all) && COOP_OS_HAS(p, cond_free);
}
}  // namespace

CondVar::CondVar() : owner_(&P()) {
    if (!HasCond(owner_)) return;   // an older table: WaitUs sleeps instead
    handle_ = COOP_OS(owner_, cond_new);
    if (!handle_) COOP_OS(owner_, log, "coop::os: cond_new failed: waits on it sleep instead");
}

CondVar::~CondVar() {
    if (handle_) COOP_OS(owner_, cond_free, handle_);
}

void CondVar::notify_one() {
    if (handle_) COOP_OS(owner_, cond_notify_one, handle_);
}

void CondVar::notify_all() {
    if (handle_) COOP_OS(owner_, cond_notify_all, handle_);
}

bool CondVar::WaitUs(std::unique_lock<Mutex> &lock, std::uint64_t us) {
    Mutex *m = lock.mutex();
    if (!m || !lock.owns_lock()) return false;
#ifdef COOP_PLATFORM_STATIC
    const bool same_backend = true;   // one backend in a payload
#else
    const bool same_backend = m->owner_ == owner_;
#endif
    if (handle_ && m->handle_ && same_backend) return COOP_OS(owner_, cond_wait_us, handle_, m->handle_, us) != 0;
    // No condition variable for this lock: release it, sleep a little, take it again (a spurious wake).
    const std::uint64_t start = COOP_OS(owner_, now_us);
    lock.unlock();
    COOP_OS(owner_, sleep_us, us < 1000 ? us : 1000);
    lock.lock();
    return COOP_OS(owner_, now_us) - start < us;
}

std::optional<std::string> GetEnv(const char *name) {
#ifdef COOP_PLATFORM_STATIC
    (void)name;   // env_get is an optional slot (a table call); a payload runs where there is no environment
    return std::nullopt;
#else
    const CoopPlatform &p = P();
    if (!name || !*name || !COOP_PLATFORM_HAS(&p, env_get) || !p.env_get) return std::nullopt;
    char small[256];
    const int len = p.env_get(name, small, sizeof small);
    if (len < 0) return std::nullopt;
    if (static_cast<std::size_t>(len) < sizeof small) return std::string(small, static_cast<std::size_t>(len));
    std::string value(static_cast<std::size_t>(len) + 1, '\0');   // longer than the first buffer: ask again
    const int again = p.env_get(name, value.data(), value.size());
    if (again < 0) return std::nullopt;
    value.resize(std::min(static_cast<std::size_t>(again), value.size() - 1));
    return value;
#endif
}

bool RandomFill(void *out, std::size_t n) {
    if (n == 0) return true;
    if (!out) return false;
    const CoopPlatform *p = &P();
    if (COOP_OS_HAS(p, random_fill) && COOP_OS(p, random_fill, out, n) == 0) return true;
    // No random source: splitmix64 over the clock, the thread, a call count and this call's stack address.
    static std::atomic<std::uint64_t> calls{0};
    std::uint64_t x = COOP_OS(p, now_us) ^ (COOP_OS(p, thread_id) << 1) ^ (calls.fetch_add(1) * 0xd1b54a32d192ed03ull);
    x ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&x));
    auto *bytes = static_cast<std::uint8_t *>(out);
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 8 == 0) {
            x += 0x9e3779b97f4a7c15ull;
            word = x;
            word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ull;
            word = (word ^ (word >> 27)) * 0x94d049bb133111ebull;
            word ^= word >> 31;
        }
        bytes[i] = static_cast<std::uint8_t>(word >> (8 * (i % 8)));
    }
    return false;
}

Thread::~Thread() { Join(); }
bool Thread::Start(void (*fn)(void *), void *arg, int core, std::size_t stack) {
    if (handle_ || !fn) return false;
    owner_ = &P();
    handle_ = COOP_OS(owner_, thread_start, fn, arg, core, stack);
    return handle_ != nullptr;
}
bool Thread::Join() {
    if (!handle_) return false;
    const bool ok = COOP_OS(owner_, thread_join, handle_) == 0;
    handle_ = nullptr;
    return ok;
}
std::uint64_t ThreadId() { return COOP_OS(&P(), thread_id); }

bool Resolve(const char *host, std::uint32_t sock_kind, CoopAddr &out) {
    out = CoopAddr{};
    return host && COOP_OS(&P(), resolve, host, sock_kind, &out) == 0;
}

CoopAddr Ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    CoopAddr addr{};
    addr.family = 4;
    addr.port = port;
    addr.addr[0] = a;
    addr.addr[1] = b;
    addr.addr[2] = c;
    addr.addr[3] = d;
    return addr;
}

bool UdpSocket::Open(std::uint32_t sock_kind) {
    Close();
    owner_ = &P();
    handle_ = COOP_OS(owner_, sock_open, sock_kind);
    return handle_ >= 0;
}
bool UdpSocket::Bind(const CoopAddr &local) { return handle_ >= 0 && COOP_OS(owner_, sock_bind, handle_, &local) == 0; }
bool UdpSocket::Local(CoopAddr &out) const {
    out = CoopAddr{};
    return handle_ >= 0 && COOP_OS(owner_, sock_local, handle_, &out) == 0;
}
int UdpSocket::SendTo(const void *data, std::size_t n, const CoopAddr &to) {
    return handle_ >= 0 ? COOP_OS(owner_, sock_sendto, handle_, data, n, &to) : -1;
}
int UdpSocket::RecvFrom(void *buf, std::size_t cap, CoopAddr &from) {
    from = CoopAddr{};
    return handle_ >= 0 ? COOP_OS(owner_, sock_recvfrom, handle_, buf, cap, &from) : -1;
}
void UdpSocket::Close() {
    if (handle_ >= 0) COOP_OS(owner_, sock_close, handle_);
    handle_ = -1;
}

#undef COOP_OS_HAS
#undef COOP_OS

}  // namespace coop::os
