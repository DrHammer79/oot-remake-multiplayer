// SPDX-License-Identifier: GPL-3.0-or-later
// coop::os built as a payload builds it (COOP_PLATFORM_STATIC, coop_platform_static.h): every OS call goes to the
// adapter's coop_plat_<slot> directly, before and after Install. Its own program (coop_platform_static_tests), since
// os.cpp is compiled here a second time with the flag; the fakes below stand in for the adapter.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <coop/os.hpp>
#include <coop_platform_static.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Calls {
    std::vector<std::string> names;
    std::uint64_t now = 1000;
    std::string written;
    std::string log;
    int frees = 0;
    void Clear() { *this = Calls{}; }
    bool Has(const char *name) const {
        for (const auto &n : names)
            if (n == name) return true;
        return false;
    }
};
Calls g;

void Note(const char *name) { g.names.emplace_back(name); }

int g_mutex_dummy = 0;
int g_thread_dummy = 0;

// A table whose OS slots are NOT the coop_plat_* functions: a payload build must not call them.
int g_table_calls = 0;
std::uint64_t TableNow() { return ++g_table_calls, 42; }

}  // namespace

extern "C" {
int coop_plat_sock_open(uint32_t sock_kind) { return Note("sock_open"), sock_kind == COOP_SOCK_UDP4 ? 7 : -1; }
int coop_plat_sock_bind(int, const CoopAddr *) { return Note("sock_bind"), 0; }
int coop_plat_sock_local(int, CoopAddr *out) { return Note("sock_local"), out->port = 99, 0; }
int coop_plat_sock_sendto(int, const void *, size_t n, const CoopAddr *) { return Note("sock_sendto"), static_cast<int>(n); }
int coop_plat_sock_recvfrom(int, void *, size_t, CoopAddr *) { return Note("sock_recvfrom"), COOP_SOCK_WOULD_BLOCK; }
int coop_plat_sock_close(int) { return Note("sock_close"), 0; }
int coop_plat_resolve(const char *, uint32_t, CoopAddr *out) { return Note("resolve"), out->family = 4, 0; }
void *coop_plat_thread_start(void (*fn)(void *), void *arg, int, size_t) {
    Note("thread_start");
    fn(arg);  // runs it at once: enough to see the call
    return &g_thread_dummy;
}
int coop_plat_thread_join(void *t) { return Note("thread_join"), t == &g_thread_dummy ? 0 : -1; }
uint64_t coop_plat_thread_id(void) { return Note("thread_id"), 5; }
void *coop_plat_mutex_new(void) { return Note("mutex_new"), &g_mutex_dummy; }
void coop_plat_mutex_lock(void *) { Note("mutex_lock"); }
void coop_plat_mutex_unlock(void *) { Note("mutex_unlock"); }
void coop_plat_mutex_free(void *) { Note("mutex_free"); }
uint64_t coop_plat_now_us(void) { return Note("now_us"), g.now; }
void coop_plat_sleep_us(uint64_t us) {
    Note("sleep_us");
    g.now += us;
}
void *coop_plat_mem_alloc(size_t n, size_t) { return Note("mem_alloc"), std::malloc(n ? n : 1); }
void coop_plat_mem_free(void *p) {
    Note("mem_free");
    if (p) ++g.frees;
    std::free(p);
}
int coop_plat_file_read(const char *rel, void **buf, size_t *n) {
    Note("file_read");
    if (std::strcmp(rel, "a/b.txt") != 0) return -1;
    *buf = std::malloc(3);
    std::memcpy(*buf, "abc", 3);
    *n = 3;
    return 0;
}
int coop_plat_file_write(const char *, const void *buf, size_t n) {
    Note("file_write");
    g.written.assign(static_cast<const char *>(buf), n);
    return 0;
}
void coop_plat_log(const char *line) { Note("log"), g.log = line; }
}

TEST_CASE("payload build: before Install, Platform() is the adapter's functions with no game slots") {
    const CoopPlatform &p = coop::os::Platform();
    CHECK(p.version == COOP_PLATFORM_VERSION);
    CHECK(p.size == sizeof(CoopPlatform));
    CHECK(p.now_us == &coop_plat_now_us);
    CHECK(p.file_write == &coop_plat_file_write);
    CHECK(p.sock_open == &coop_plat_sock_open);
    CHECK(p.read == nullptr);
    CHECK(p.call == nullptr);
}

TEST_CASE("payload build: time, log and memory call coop_plat_* directly") {
    g.Clear();
    CHECK(coop::os::NowUs() == 1000);
    coop::os::SleepUs(500);
    CHECK(coop::os::NowUs() == 1500);
    coop::os::Log("hello");
    coop::os::Log(static_cast<const char *>(nullptr));
    void *m = coop::os::Alloc(16);
    REQUIRE(m != nullptr);
    coop::os::Free(m);
    CHECK(g.Has("now_us"));
    CHECK(g.Has("sleep_us"));
    CHECK(g.log.empty());  // the null line logs as ""
    CHECK(g.Has("mem_alloc"));
    CHECK(g.frees == 1);
}

TEST_CASE("payload build: files go through coop_plat_file_* and keep the path checks") {
    g.Clear();
    std::vector<std::uint8_t> got;
    CHECK(coop::os::ReadFile("a/b.txt", got));
    CHECK(got == std::vector<std::uint8_t>{'a', 'b', 'c'});
    CHECK(g.frees == 1);  // the adapter's buffer goes back through mem_free
    CHECK_FALSE(coop::os::ReadFile("../x", got));
    CHECK_FALSE(coop::os::ReadFile("missing", got));
    CHECK(coop::os::WriteFile("out/x.bin", "hi", 2));
    CHECK(g.written == "hi");
    CHECK_FALSE(coop::os::WriteFile("c:/x", "hi", 2));
}

namespace {
int g_ran = 0;
void Run(void *arg) { g_ran += *static_cast<int *>(arg); }
}  // namespace

TEST_CASE("payload build: threads, mutexes and sockets call coop_plat_* directly") {
    g.Clear();
    int three = 3;
    {
        coop::os::Thread t;
        CHECK(t.Start(&Run, &three));
        CHECK(g_ran == 3);
        CHECK(t.Join());
    }
    CHECK(coop::os::ThreadId() == 5);
    {
        coop::os::Mutex m;
        CHECK(m.ok());
        std::lock_guard<coop::os::Mutex> lock(m);
    }
    CHECK(g.Has("mutex_lock"));
    CHECK(g.Has("mutex_unlock"));
    CHECK(g.Has("mutex_free"));
    CoopAddr a{};
    CHECK(coop::os::Resolve("localhost", COOP_SOCK_UDP4, a));
    CHECK(a.family == 4);
    coop::os::UdpSocket s;
    CHECK(s.Open());
    CHECK(s.Bind(coop::os::Ipv4(127, 0, 0, 1, 0)));
    CoopAddr local{};
    CHECK(s.Local(local));
    CHECK(local.port == 99);
    CHECK(s.SendTo("abcd", 4, a) == 4);
    char buf[8];
    CHECK(s.RecvFrom(buf, sizeof buf, a) == COOP_SOCK_WOULD_BLOCK);
    s.Close();
    CHECK(g.Has("sock_close"));
}

TEST_CASE("payload build: an installed table gives game slots, but the OS calls stay direct") {
    static CoopPlatform t;                  // (outlives the test: Install keeps the pointer)
    t = coop::os::Platform();               // the adapter's functions...
    t.now_us = &TableNow;                   // ...but one OS slot pointing elsewhere
    t.kind = COOP_PLATFORM_KIND_EXEFS_SWITCH;
    t.main_base = 0x8000000;
    REQUIRE(coop::os::Install(&t) == coop::os::InstallError::kNone);
    g.Clear();
    g_table_calls = 0;
    CHECK(coop::os::NowUs() == 1000);
    CHECK(g_table_calls == 0);
    CHECK(coop::os::Platform().kind == COOP_PLATFORM_KIND_EXEFS_SWITCH);
    CHECK(coop::os::Platform().main_base == 0x8000000);
    // Install still refuses a bad table and keeps the last good one.
    CoopPlatform bad = t;
    bad.log = nullptr;
    CHECK(coop::os::Install(&bad) == coop::os::InstallError::kMissingSlot);
    CHECK(coop::os::Platform().kind == COOP_PLATFORM_KIND_EXEFS_SWITCH);
}
