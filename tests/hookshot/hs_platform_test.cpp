// SPDX-License-Identifier: GPL-3.0-or-later
// The Longshot module's CoopPlatform table (hookshot/src/coop_hs_platform.h), filled from a stand-in Hookshot table.
#include <doctest/doctest.h>

#include <coop/os.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../hookshot/compat/coop_runtime.h"
#include "coop_hs_platform.h"

namespace {

// A stand-in Hookshot: 64 bytes of guest memory at 0x1000, and what was asked of it.
constexpr std::uint64_t kBase = 0x1000;
std::uint8_t g_mem[64];
HosCoopCall g_last_call{};
void* g_last_ctx = nullptr;
std::vector<std::string> g_lines;
int g_scratch_n = 0;

bool Inside(std::uint64_t a, std::size_t n) {
    return a >= kBase && n <= sizeof g_mem && a - kBase <= sizeof g_mem - n;
}
int FakeRead(std::uint64_t a, void* out, std::size_t n) {
    if (!Inside(a, n)) return 0;
    std::memcpy(out, g_mem + (a - kBase), n);
    return 1;
}
int FakeWrite(std::uint64_t a, const void* in, std::size_t n) {
    if (!Inside(a, n)) return 0;
    std::memcpy(g_mem + (a - kBase), in, n);
    return 1;
}
int FakeCas32(std::uint64_t a, std::uint32_t expected, std::uint32_t desired, std::uint32_t* seen) {
    if (!Inside(a, 4)) return 0;
    std::memcpy(seen, g_mem + (a - kBase), 4);
    if (*seen == expected) std::memcpy(g_mem + (a - kBase), &desired, 4);
    return 1;
}
int FakeCall(void* ctx, const HosCoopCall* c, std::uint64_t* x0) {
    g_last_ctx = ctx;
    g_last_call = *c;
    if (c->function == 0) return 0;
    *x0 = c->x[0] + c->x[7] + c->stack[3];
    return 1;
}
void FakeLog(const char* line) {
    g_lines.emplace_back(line);
}
std::uint64_t FakePad() {
    return 0x40;
}
int FakeKey(std::uint32_t vk) {
    return vk == 0x75 ? 1 : 0;
}
std::uint64_t FakeScratch(void*, const void*, std::size_t n) {
    g_scratch_n = static_cast<int>(n);
    return 0x2000;
}

HosCoopApi FullApi(const char* dir) {
    HosCoopApi api{};
    api.version = HOS_COOP_API_VERSION;
    api.size = sizeof api;
    api.main_base = 0x80020000;
    api.main_build_id = "e0f3ccfaf50065ae0000000000000000";
    api.game_dir = dir;
    api.read = FakeRead;
    api.write = FakeWrite;
    api.cas32 = FakeCas32;
    api.call = FakeCall;
    api.log = FakeLog;
    api.pad_buttons = FakePad;
    api.key_down = FakeKey;
    api.scratch = FakeScratch;
    return api;
}

// Co-op off and the std table back when a test ends.
struct Restore {
    ~Restore() {
        CoopHsPlatform::Clear();
        coop::os::Install(coop::os::StdPlatform());
    }
};

}  // namespace

TEST_CASE("hookshot platform: Hookshot's table becomes a CoopPlatform that coop::os takes") {
    Restore restore;
    CHECK(CoopHsPlatform::Make(nullptr) == nullptr);
    const HosCoopApi api = FullApi("");
    const CoopPlatform* p = CoopHsPlatform::Make(&api);
    REQUIRE(p != nullptr);
    CHECK(CoopHsPlatform::Get() == p);
    CHECK(p->version == COOP_PLATFORM_VERSION);
    CHECK(p->size == sizeof(CoopPlatform));
    CHECK(p->kind == COOP_PLATFORM_KIND_RECOMP_LONGSHOT);
    CHECK(p->main_base == 0x80020000);
    CHECK(std::string(p->main_build_id) == api.main_build_id);
    CHECK(coop::os::Install(p) == coop::os::InstallError::kNone);
    CHECK(&coop::os::Platform() == p);
    // The OS slots are the std backend's.
    const CoopPlatform* s = coop::os::StdPlatform();
    CHECK(p->sock_open == s->sock_open);
    CHECK(p->thread_start == s->thread_start);
    CHECK(p->mutex_new == s->mutex_new);
    CHECK(p->now_us == s->now_us);
    CHECK(p->mem_alloc == s->mem_alloc);
    CHECK(p->file_read == s->file_read);
    // Not from Hookshot's table.
    CHECK(p->hook_wrap == nullptr);
    CHECK(p->hook_site == nullptr);
    CHECK(p->pad_sticks == nullptr);
    CHECK(p->draw_2d == nullptr);
    CHECK(p->voice_out == nullptr);
    CHECK(p->voice_in == nullptr);
    CHECK(p->ui_caps == 0);
}

TEST_CASE("hookshot platform: the game slots are Hookshot's (memory, calls, scratch, pad, keys, log)") {
    Restore restore;
    const HosCoopApi api = FullApi("");
    const CoopPlatform* p = CoopHsPlatform::Make(&api);
    REQUIRE(p != nullptr);
    std::memset(g_mem, 0, sizeof g_mem);
    const std::uint32_t v = 0x11223344;
    CHECK(p->write(kBase + 8, &v, 4) == 1);
    std::uint32_t back = 0;
    CHECK(p->read(kBase + 8, &back, 4) == 1);
    CHECK(back == v);
    CHECK(p->read(kBase + 62, &back, 4) == 0);   // past the end: refused, as Hookshot's
    std::uint32_t seen = 0;
    CHECK(p->cas32(kBase + 8, v, 7, &seen) == 1);
    CHECK(seen == v);
    CHECK(p->read(kBase + 8, &back, 4) == 1);
    CHECK(back == 7);
    // A call goes through whole: registers, stack words, s0 and the hook's ctx.
    CoopCall c{};
    c.function = 0x80020000 + 0x6e70f8;
    c.x[0] = 1;
    c.x[7] = 20;
    c.stack[3] = 300;
    c.s0 = 1.5f;
    int ctx = 0;
    std::uint64_t x0 = 0;
    CHECK(p->call(&ctx, &c, &x0) == 1);
    CHECK(x0 == 321);
    CHECK(g_last_ctx == &ctx);
    CHECK(g_last_call.function == c.function);
    CHECK(g_last_call.stack[3] == 300);
    CHECK(g_last_call.s0 == 1.5f);
    c.function = 0;
    CHECK(p->call(&ctx, &c, &x0) == 0);   // no function there
    CHECK(p->scratch(&ctx, "abc", 3) == 0x2000);
    CHECK(g_scratch_n == 3);
    CHECK(p->pad_buttons(0) == 0x40);
    CHECK(p->pad_buttons(1) == 0);   // Hookshot hands over player 1's word only
    CHECK(p->key_down(0x75) == 1);
    CHECK(p->key_down(0x76) == 0);
    g_lines.clear();
    p->log("hello");
    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0] == "hello");
}

TEST_CASE("hookshot platform: an older Hookshot's table leaves its missing slots NULL") {
    Restore restore;
    HosCoopApi api = FullApi("");
    api.size = static_cast<std::uint32_t>(offsetof(HosCoopApi, pad_buttons));   // the first version
    const CoopPlatform* p = CoopHsPlatform::Make(&api);
    REQUIRE(p != nullptr);
    CHECK(p->read != nullptr);
    CHECK(p->call != nullptr);
    CHECK(p->pad_buttons == nullptr);
    CHECK(p->key_down == nullptr);
    CHECK(p->scratch == nullptr);
    api.size = static_cast<std::uint32_t>(offsetof(HosCoopApi, scratch));   // keys, no scratch (before patch 0004)
    p = CoopHsPlatform::Make(&api);
    CHECK(p->key_down != nullptr);
    CHECK(p->scratch == nullptr);
    // Set by size but left NULL: still none.
    api = FullApi("");
    api.pad_buttons = nullptr;
    api.log = nullptr;
    p = CoopHsPlatform::Make(&api);
    CHECK(p->pad_buttons == nullptr);
    CHECK(p->log == coop::os::StdPlatform()->log);   // a required slot: the std backend's
    CHECK(coop::os::Install(p) == coop::os::InstallError::kNone);
}

TEST_CASE("hookshot platform: the compat runtime answers from the table, nothing while co-op is off") {
    Restore restore;
    CoopHsPlatform::Clear();
    std::uint8_t b = 0;
    std::uint64_t pad = 0;
    CHECK_FALSE(CoopRuntime::Running());
    CHECK(CoopRuntime::MainBase() == 0);
    CHECK_FALSE(CoopRuntime::Read(kBase, &b, 1));
    CHECK_FALSE(CoopRuntime::PadButtons(pad));
    CHECK(CoopRuntime::GameDir() == std::filesystem::path("."));
    const HosCoopApi api = FullApi("C:/Games/OoT");
    REQUIRE(CoopHsPlatform::Make(&api) != nullptr);
    CHECK(CoopRuntime::Running());
    CHECK(CoopRuntime::MainBase() == 0x80020000);
    CHECK(CoopRuntime::MainBuildHex() == api.main_build_id);
    CHECK(CoopRuntime::GameDir() == std::filesystem::path(u8"C:/Games/OoT"));
    std::memset(g_mem, 0, sizeof g_mem);
    const std::uint16_t w = 0xbeef;
    CHECK(CoopRuntime::Write(kBase + 4, &w, 2));
    std::uint16_t r = 0;
    CHECK(CoopRuntime::Read(kBase + 4, &r, 2));
    CHECK(r == 0xbeef);
    CHECK(CoopRuntime::Valid(kBase, 64));
    CHECK_FALSE(CoopRuntime::Valid(kBase, 65));
    CHECK_FALSE(CoopRuntime::Valid(kBase, 0));
    std::uint32_t seen = 0;
    CHECK(CoopRuntime::Cas32(kBase, 0, 9, seen));
    CHECK(CoopRuntime::PadButtons(pad));
    CHECK(pad == 0x40);
    g_lines.clear();
    CoopRuntime::Log("line");
    CHECK(g_lines.size() == 1);
    // Off again (a refused start): nothing answers.
    CoopHsPlatform::Clear();
    CHECK_FALSE(CoopRuntime::Running());
    CHECK_FALSE(CoopRuntime::Read(kBase + 4, &r, 2));
    CoopRuntime::Log("not logged");
    CHECK(g_lines.size() == 1);
}

TEST_CASE("hookshot platform: files go under the game's folder") {
    Restore restore;
    const auto dir = std::filesystem::temp_directory_path() / "coop_hs_platform_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::u8string dir8 = dir.u8string();
    const std::string utf8(dir8.begin(), dir8.end());
    const HosCoopApi api = FullApi(utf8.c_str());
    REQUIRE(coop::os::Install(CoopHsPlatform::Make(&api)) == coop::os::InstallError::kNone);
    CHECK(coop::os::WriteFile("coop_test.txt", "hi", 2));
    CHECK(std::filesystem::exists(dir / "coop_test.txt"));
    std::vector<std::uint8_t> back;
    CHECK(coop::os::ReadFile("coop_test.txt", back));
    CHECK(back.size() == 2);
    std::filesystem::remove_all(dir, ec);
}
