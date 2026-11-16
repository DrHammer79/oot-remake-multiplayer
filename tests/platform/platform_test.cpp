// SPDX-License-Identifier: GPL-3.0-or-later
// coop_platform.h and coop::os with the std backend (docs/platform.md).
#include <doctest/doctest.h>

#include <coop/os.hpp>
#include <coop_platform.h>

#include "hos_coop_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>

extern "C" {
size_t coop_platform_c_size(void);
size_t coop_platform_c_offset_log(void);
size_t coop_platform_c_offset_random_fill(void);
size_t coop_platform_c_addr_size(void);
size_t coop_platform_c_call_size(void);
}

namespace {

namespace os = coop::os;

// Puts the std backend back when a test that installs its own table ends.
struct RestoreStd {
    ~RestoreStd() { os::Install(os::StdPlatform()); }
};

std::filesystem::path TempRoot(const char *name) {
    auto dir = std::filesystem::temp_directory_path() / ("coop_platform_test_" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("coop_platform.h: the same layout in C and C++, CoopCall as HosCoopCall") {
    CHECK(coop_platform_c_size() == sizeof(CoopPlatform));
    CHECK(coop_platform_c_offset_log() == offsetof(CoopPlatform, log));
    CHECK(coop_platform_c_offset_random_fill() == offsetof(CoopPlatform, random_fill));
    CHECK(coop_platform_c_addr_size() == sizeof(CoopAddr));
    CHECK(coop_platform_c_call_size() == sizeof(CoopCall));
    CHECK(sizeof(CoopCall) == sizeof(HosCoopCall));
    CHECK(offsetof(CoopCall, x) == offsetof(HosCoopCall, x));
    CHECK(offsetof(CoopCall, stack) == offsetof(HosCoopCall, stack));
    CHECK(offsetof(CoopCall, s0) == offsetof(HosCoopCall, s0));
    // version and size first, as HosCoopApi has them.
    CHECK(offsetof(CoopPlatform, version) == 0);
    CHECK(offsetof(CoopPlatform, size) == 4);
}

TEST_CASE("coop_platform.h: COOP_PLATFORM_HAS follows size") {
    CoopPlatform p{};
    p.size = static_cast<uint32_t>(offsetof(CoopPlatform, now_us));
    CHECK(COOP_PLATFORM_HAS(&p, thread_id));
    CHECK_FALSE(COOP_PLATFORM_HAS(&p, now_us));
    p.size = sizeof(CoopPlatform);
    CHECK(COOP_PLATFORM_HAS(&p, log));
}

TEST_CASE("coop_platform.h: COOP_PLATFORM_SLOTS names every function slot once") {
    std::size_t slots = 0;
    std::size_t slot_bytes = 0;
#define COOP_TEST_COUNT(name, required) \
    ++slots;                            \
    slot_bytes += sizeof(CoopPlatform::name);
    COOP_PLATFORM_SLOTS(COOP_TEST_COUNT)
#undef COOP_TEST_COUNT
    // The slots run from read to the end; nothing but slots after main_build_id.
    CHECK(slots == 37);
    CHECK(offsetof(CoopPlatform, read) + slot_bytes == sizeof(CoopPlatform));
}

TEST_CASE("coop::os: the std table is version 1 with every OS slot and no game") {
    const CoopPlatform *p = os::StdPlatform();
    REQUIRE(p);
    CHECK(p->version == COOP_PLATFORM_VERSION);
    CHECK(p->size == sizeof(CoopPlatform));
    CHECK(p->kind == COOP_PLATFORM_KIND_STD);
    CHECK(p->read == nullptr);
    CHECK(p->call == nullptr);
    CHECK(p->main_build_id == nullptr);
    CHECK(os::Install(p) == os::InstallError::kNone);
    CHECK(&os::Platform() == p);
}

TEST_CASE("coop::os: Install refuses bad tables and keeps the old one") {
    RestoreStd restore;
    const CoopPlatform *std_table = os::StdPlatform();
    REQUIRE(os::Install(std_table) == os::InstallError::kNone);
    CHECK(os::Install(nullptr) == os::InstallError::kNull);

    CoopPlatform bad = *std_table;
    bad.version = 2;
    CHECK(os::Install(&bad) == os::InstallError::kVersion);
    bad = *std_table;
    bad.size = static_cast<uint32_t>(offsetof(CoopPlatform, log));
    CHECK(os::Install(&bad) == os::InstallError::kSize);
    bad = *std_table;
    bad.mutex_unlock = nullptr;
    CHECK(os::Install(&bad) == os::InstallError::kMissingSlot);
    CHECK(&os::Platform() == std_table);

    CoopPlatform fine = *std_table;
    fine.read = nullptr;   // optional
    CHECK(os::Install(&fine) == os::InstallError::kNone);
    CHECK(&os::Platform() == &fine);
}

namespace {
std::atomic<int> g_logged{0};
std::string g_last_line;
void CountingLog(const char *line) {
    ++g_logged;
    g_last_line = line;
}
}  // namespace

TEST_CASE("coop::os: calls go through the installed table") {
    RestoreStd restore;
    CoopPlatform mine = *os::StdPlatform();
    mine.log = CountingLog;
    REQUIRE(os::Install(&mine) == os::InstallError::kNone);
    g_logged = 0;
    os::Log("hello");
    os::Log(std::string("again"));
    CHECK(g_logged == 2);
    CHECK(g_last_line == "again");
}

TEST_CASE("coop::os: time goes forward") {
    const auto a = os::NowUs();
    os::SleepUs(2000);
    const auto b = os::NowUs();
    CHECK(b >= a + 1000);
}

TEST_CASE("coop::os: aligned memory") {
    for (std::size_t align : {std::size_t{1}, std::size_t{16}, std::size_t{64}, std::size_t{4096}}) {
        void *p = os::Alloc(100, align);
        REQUIRE(p);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
        std::memset(p, 0xab, 100);
        os::Free(p);
    }
    CHECK(os::Alloc(8, 3) == nullptr);      // not a power of two
    CHECK(os::Alloc(8, 8192) == nullptr);   // above 4096
    os::Free(nullptr);
}

namespace {
struct Counter {
    os::Mutex mutex;
    long value = 0;
};
void AddMany(void *arg) {
    auto *c = static_cast<Counter *>(arg);
    for (int i = 0; i < 20000; ++i) {
        std::lock_guard<os::Mutex> lock(c->mutex);
        ++c->value;
    }
}
void StoreId(void *arg) { *static_cast<std::uint64_t *>(arg) = os::ThreadId(); }
}  // namespace

TEST_CASE("coop::os: threads and a mutex") {
    Counter c;
    REQUIRE(c.mutex.ok());
    os::Thread a, b;
    REQUIRE(a.Start(AddMany, &c));
    REQUIRE(b.Start(AddMany, &c));
    CHECK_FALSE(a.Start(AddMany, &c));   // already running
    CHECK(a.Join());
    CHECK(b.Join());
    CHECK_FALSE(a.Join());
    CHECK(c.value == 40000);

    std::uint64_t other = 0;
    os::Thread t;
    REQUIRE(t.Start(StoreId, &other));
    t.Join();
    CHECK(other != 0);
    CHECK(other != os::ThreadId());
}

TEST_CASE("coop::os: mod paths") {
    CHECK(os::IsModPath("coop.ini"));
    CHECK(os::IsModPath("saves/host/slot1.bin"));
    CHECK(os::IsModPath("a/..b/c"));
    CHECK_FALSE(os::IsModPath(nullptr));
    CHECK_FALSE(os::IsModPath(""));
    CHECK_FALSE(os::IsModPath("/etc/passwd"));
    CHECK_FALSE(os::IsModPath("C:/x"));
    CHECK_FALSE(os::IsModPath("..\\x"));
    CHECK_FALSE(os::IsModPath("../x"));
    CHECK_FALSE(os::IsModPath("a/../b"));
    CHECK_FALSE(os::IsModPath("a/.."));
    CHECK_FALSE(os::IsModPath("a//b"));
    CHECK_FALSE(os::IsModPath("a/"));
}

TEST_CASE("coop::os: files under the mod folder") {
    RestoreStd restore;
    const auto root = TempRoot("files");
    const std::u8string root8 = root.u8string();
    REQUIRE(os::Install(os::StdPlatform(std::string(root8.begin(), root8.end()))) == os::InstallError::kNone);

    const char text[] = "co-op";
    CHECK(os::WriteFile("sub/dir/a.txt", text, 5));
    std::vector<std::uint8_t> back;
    REQUIRE(os::ReadFile("sub/dir/a.txt", back));
    CHECK(std::string(back.begin(), back.end()) == "co-op");
    CHECK(std::filesystem::exists(root / "sub" / "dir" / "a.txt"));
    CHECK_FALSE(std::filesystem::exists(root / "sub" / "dir" / "a.txt.tmp"));

    CHECK(os::WriteFile("sub/dir/a.txt", "x", 1));   // replaces it whole
    REQUIRE(os::ReadFile("sub/dir/a.txt", back));
    CHECK(back.size() == 1);

    CHECK(os::WriteFile("empty.bin", nullptr, 0));
    REQUIRE(os::ReadFile("empty.bin", back));
    CHECK(back.empty());

    CHECK_FALSE(os::ReadFile("missing.txt", back));
    CHECK_FALSE(os::WriteFile("../outside.txt", text, 5));
    CHECK_FALSE(std::filesystem::exists(root.parent_path() / "outside.txt"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("coop::os: UDP over loopback") {
    os::UdpSocket server, client;
    REQUIRE(server.Open(COOP_SOCK_UDP4));
    REQUIRE(server.Bind(os::Ipv4(127, 0, 0, 1, 0)));
    CoopAddr where{};
    REQUIRE(server.Local(where));
    CHECK(where.family == 4);
    CHECK(where.port != 0);

    char buf[64];
    CoopAddr from{};
    CHECK(server.RecvFrom(buf, sizeof buf, from) == COOP_SOCK_WOULD_BLOCK);   // non-blocking

    REQUIRE(client.Open(COOP_SOCK_UDP4));
    REQUIRE(client.Bind(os::Ipv4(127, 0, 0, 1, 0)));
    CHECK(client.SendTo("ping", 4, where) == 4);
    int got = COOP_SOCK_WOULD_BLOCK;
    for (int i = 0; i < 200 && got == COOP_SOCK_WOULD_BLOCK; ++i) {
        got = server.RecvFrom(buf, sizeof buf, from);
        if (got == COOP_SOCK_WOULD_BLOCK) os::SleepUs(5000);
    }
    REQUIRE(got == 4);
    CHECK(std::memcmp(buf, "ping", 4) == 0);
    CoopAddr client_addr{};
    REQUIRE(client.Local(client_addr));
    CHECK(from.family == 4);
    CHECK(from.port == client_addr.port);
    CHECK(from.addr[0] == 127);

    server.Close();
    CHECK_FALSE(server.open());
    CHECK(server.RecvFrom(buf, sizeof buf, from) == -1);
}

TEST_CASE("coop::os: a closed peer port does not make the socket look dead") {
    // Windows reports an ICMP port-unreachable to the sender's next recvfrom (WSAECONNRESET) unless told not to.
    os::UdpSocket gone;
    REQUIRE(gone.Open(COOP_SOCK_UDP4));
    REQUIRE(gone.Bind(os::Ipv4(127, 0, 0, 1, 0)));
    CoopAddr gone_addr{};
    REQUIRE(gone.Local(gone_addr));
    gone.Close();

    os::UdpSocket me, peer;
    REQUIRE(me.Open(COOP_SOCK_UDP4));
    REQUIRE(me.Bind(os::Ipv4(127, 0, 0, 1, 0)));
    CoopAddr me_addr{};
    REQUIRE(me.Local(me_addr));
    char buf[64];
    CoopAddr from{};
    for (int i = 0; i < 3; ++i) {
        CHECK(me.SendTo("lost", 4, gone_addr) == 4);
        os::SleepUs(20000);   // time for the ICMP answer to come back
        CHECK(me.RecvFrom(buf, sizeof buf, from) == COOP_SOCK_WOULD_BLOCK);
    }

    REQUIRE(peer.Open(COOP_SOCK_UDP4));
    REQUIRE(peer.Bind(os::Ipv4(127, 0, 0, 1, 0)));
    CHECK(peer.SendTo("still", 5, me_addr) == 5);
    int got = COOP_SOCK_WOULD_BLOCK;
    for (int i = 0; i < 200 && got == COOP_SOCK_WOULD_BLOCK; ++i) {
        got = me.RecvFrom(buf, sizeof buf, from);
        if (got == COOP_SOCK_WOULD_BLOCK) os::SleepUs(5000);
    }
    CHECK(got == 5);
}

TEST_CASE("coop::os: resolve a literal address") {
    CoopAddr a{};
    REQUIRE(os::Resolve("127.0.0.1", COOP_SOCK_UDP4, a));
    CHECK(a.family == 4);
    CHECK(a.port == 0);
    CHECK(a.addr[0] == 127);
    CHECK(a.addr[3] == 1);
    CHECK_FALSE(os::Resolve(nullptr, COOP_SOCK_UDP4, a));
    CHECK_FALSE(os::Resolve("127.0.0.1", 99, a));
}

// ---- v1 step 4 (os-wrap): the slots appended after the first table.

TEST_CASE("coop::os: an adapter's table from before the appended slots still installs") {
    RestoreStd restore;
    CoopPlatform old = *os::StdPlatform();
    old.size = static_cast<uint32_t>(offsetof(CoopPlatform, log) + sizeof(CoopPlatform::log));
    old.mutex_try_lock = nullptr;   // past its end: never read, never required
    old.env_get = nullptr;
    old.random_fill = nullptr;
    CHECK_FALSE(COOP_PLATFORM_HAS(&old, mutex_try_lock));
    REQUIRE(os::Install(&old) == os::InstallError::kNone);

    os::Mutex m;
    CHECK(m.try_lock());   // no slot: waits for the lock instead
    m.unlock();
    CHECK_FALSE(os::GetEnv("PATH").has_value());   // no environment
    std::uint8_t a[16] = {}, b[16] = {};
    CHECK_FALSE(os::RandomFill(a, sizeof a));       // the fallback still fills
    CHECK_FALSE(os::RandomFill(b, sizeof b));
    CHECK(std::memcmp(a, b, sizeof a) != 0);
    CHECK(std::count(std::begin(a), std::end(a), std::uint8_t{0}) < 8);

    // A table that reaches an appended required slot must fill it.
    CoopPlatform full = *os::StdPlatform();
    full.random_fill = nullptr;
    CHECK(os::Install(&full) == os::InstallError::kMissingSlot);
    full = *os::StdPlatform();
    full.env_get = nullptr;   // optional
    CHECK(os::Install(&full) == os::InstallError::kNone);
    CHECK_FALSE(os::GetEnv("PATH").has_value());
}

namespace {
struct TryLockState {
    os::Mutex mutex;
    std::atomic<int> got{-1};
};
void TryFromOtherThread(void *arg) {
    auto *s = static_cast<TryLockState *>(arg);
    std::unique_lock<os::Mutex> lock(s->mutex, std::try_to_lock);
    s->got = lock.owns_lock() ? 1 : 0;
}
}  // namespace

TEST_CASE("coop::os: try_lock does not wait for a held lock") {
    TryLockState s;
    REQUIRE(s.mutex.ok());
    {
        std::lock_guard<os::Mutex> held(s.mutex);
        os::Thread t;
        REQUIRE(t.Start(TryFromOtherThread, &s));
        t.Join();
        CHECK(s.got == 0);
    }
    os::Thread t;
    REQUIRE(t.Start(TryFromOtherThread, &s));
    t.Join();
    CHECK(s.got == 1);
}

namespace {
char g_env_value[600];
int FakeEnv(const char *name, char *out, size_t cap) {
    if (std::strcmp(name, "COOP_OS_TEST_SET") != 0) return -1;
    const size_t len = std::strlen(g_env_value);
    if (cap) {
        const size_t n = len < cap - 1 ? len : cap - 1;
        std::memcpy(out, g_env_value, n);
        out[n] = '\0';
    }
    return static_cast<int>(len);
}
}  // namespace

TEST_CASE("coop::os: GetEnv through the table, short and long values") {
    RestoreStd restore;
    CoopPlatform mine = *os::StdPlatform();
    mine.env_get = FakeEnv;
    REQUIRE(os::Install(&mine) == os::InstallError::kNone);
    std::strcpy(g_env_value, "0");
    CHECK(os::GetEnv("COOP_OS_TEST_SET") == std::optional<std::string>("0"));
    std::strcpy(g_env_value, "");
    CHECK(os::GetEnv("COOP_OS_TEST_SET") == std::optional<std::string>(""));   // set but empty
    std::memset(g_env_value, 'x', 500);
    g_env_value[500] = '\0';
    const auto longer = os::GetEnv("COOP_OS_TEST_SET");
    REQUIRE(longer.has_value());
    CHECK(longer->size() == 500);
    CHECK(longer->find_first_not_of('x') == std::string::npos);
    CHECK_FALSE(os::GetEnv("COOP_OS_TEST_UNSET").has_value());
    CHECK_FALSE(os::GetEnv("").has_value());
    CHECK_FALSE(os::GetEnv(nullptr).has_value());
}

TEST_CASE("coop::os: the std backend's environment and random source") {
    // PATH is set on every CI runner and PC.
    const auto path = os::GetEnv("PATH");
    REQUIRE(path.has_value());
    CHECK_FALSE(path->empty());
    CHECK_FALSE(os::GetEnv("COOP_OS_TEST_SURELY_UNSET_7f3a").has_value());

    std::uint8_t a[13] = {}, b[13] = {};   // not a multiple of 4: the tail is filled too
    CHECK(os::RandomFill(a, sizeof a));
    CHECK(os::RandomFill(b, sizeof b));
    CHECK(std::memcmp(a, b, sizeof a) != 0);
    CHECK(os::RandomFill(nullptr, 0));
    CHECK_FALSE(os::RandomFill(nullptr, 4));
}

TEST_CASE("coop::os: SteadyClock is NowUs as a std::chrono clock") {
    static_assert(os::SteadyClock::is_steady);
    const auto before = os::NowUs();
    const auto t = os::SteadyClock::now();
    const auto after = os::NowUs();
    const auto us = static_cast<std::uint64_t>(t.time_since_epoch().count());
    CHECK(us >= before);
    CHECK(us <= after);
    os::SleepUs(2000);
    CHECK(os::SteadyClock::now() - t >= std::chrono::milliseconds(1));
    // The same epoch as std::chrono::steady_clock on the std backend (the core's old clock), so a value kept as
    // milliseconds since the clock's start reads the same before and after the switch.
    const auto std_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto os_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        os::SteadyClock::now().time_since_epoch()).count();
    CHECK(os_ms - std_ms <= 1);
    CHECK(std_ms - os_ms <= 1);
}
