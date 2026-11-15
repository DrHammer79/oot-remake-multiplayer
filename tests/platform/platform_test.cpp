// SPDX-License-Identifier: GPL-3.0-or-later
// coop_platform.h and coop::os with the std backend (docs/platform.md).
#include <doctest/doctest.h>

#include <coop/os.hpp>
#include <coop_platform.h>

#include "hos_coop_api.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

extern "C" {
size_t coop_platform_c_size(void);
size_t coop_platform_c_offset_log(void);
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
    CHECK(slots == 34);
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
