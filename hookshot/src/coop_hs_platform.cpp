// SPDX-License-Identifier: GPL-3.0-or-later
// The Longshot/Hookshot module's CoopPlatform table (coop_hs_platform.h) and the compat runtime over it.
#include "coop_hs_platform.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

#include <coop/os.hpp>

#include "../compat/coop_runtime.h"
#include "coop_hookshot.h"

namespace CoopHsPlatform {
namespace {

// CoopCall is HosCoopCall's layout (coop_platform.h), so a call goes through as it is.
static_assert(sizeof(CoopCall) == sizeof(HosCoopCall));
static_assert(offsetof(CoopCall, x) == offsetof(HosCoopCall, x));
static_assert(offsetof(CoopCall, stack) == offsetof(HosCoopCall, stack));
static_assert(offsetof(CoopCall, s0) == offsetof(HosCoopCall, s0));

const HosCoopApi* g_api = nullptr;         // set by Make before co-op's threads start, as the module's g_api was
CoopPlatform g_table{};
std::string g_build;                       // main's build id, hex
std::string g_dir;                         // the game's folder, UTF-8
std::atomic<const CoopPlatform*> g_current{nullptr};

bool HasScratch(const HosCoopApi* api) {
    return api->size >= offsetof(HosCoopApi, scratch) + sizeof(api->scratch) && api->scratch != nullptr;
}

// The slots: named functions (a lifter's entries), each Hookshot's own.
int HsRead(std::uint64_t address, void* out, std::size_t size) {
    return g_api->read(address, out, size);
}
int HsWrite(std::uint64_t address, const void* in, std::size_t size) {
    return g_api->write(address, in, size);
}
int HsCas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t* seen) {
    return g_api->cas32(address, expected, desired, seen);
}
int HsCall(void* ctx, const CoopCall* call, std::uint64_t* x0) {
    HosCoopCall k;
    std::memcpy(&k, call, sizeof k);
    return g_api->call(ctx, &k, x0);
}
std::uint64_t HsScratch(void* ctx, const void* data, std::size_t n) {
    return g_api->scratch(ctx, data, n);
}
// Hookshot hands over player 1's word only.
std::uint64_t HsPadButtons(int player) {
    return player == 0 ? g_api->pad_buttons() : 0;
}
int HsKeyDown(std::uint32_t vk) {
    return g_api->key_down(vk);
}
void HsLog(const char* line) {
    g_api->log(line);
}

}  // namespace

const CoopPlatform* Make(const HosCoopApi* api) {
    if (api == nullptr) {
        return nullptr;
    }
    g_api = api;
    g_build = api->main_build_id != nullptr ? api->main_build_id : "";
    g_dir = api->game_dir != nullptr ? api->game_dir : "";
    // The OS slots: the std backend's, its files under the game's folder.
    CoopPlatform t = *coop::os::StdPlatform(g_dir);
    t.kind = COOP_PLATFORM_KIND_RECOMP_LONGSHOT;
    t.ui_caps = 0;   // the module draws through Hookshot's ImGui itself until the drawlist step (v1 step 6)
    t.main_base = api->main_base;
    t.main_build_id = g_build.c_str();
    t.read = api->read != nullptr ? HsRead : nullptr;
    t.write = api->write != nullptr ? HsWrite : nullptr;
    t.cas32 = api->cas32 != nullptr ? HsCas32 : nullptr;
    t.call = api->call != nullptr ? HsCall : nullptr;
    t.scratch = HasScratch(api) ? HsScratch : nullptr;
    t.pad_buttons = CoopHookshot::HasPadButtons(api) ? HsPadButtons : nullptr;
    t.key_down = CoopHookshot::HasKeyDown(api) ? HsKeyDown : nullptr;
    if (api->log != nullptr) {
        t.log = HsLog;   // hookshot_log.txt; without it the std backend's stderr
    }
    // Not from Hookshot's table: hooks are Hookshot's patches, the pad's sticks come through hos_coop_pad, drawing
    // and voice through the module's own exports.
    g_table = t;
    g_current.store(&g_table, std::memory_order_release);
    return &g_table;
}

const CoopPlatform* Get() {
    return g_current.load(std::memory_order_acquire);
}

void Clear() {
    g_current.store(nullptr, std::memory_order_release);
}

std::filesystem::path GameDir() {
    if (Get() == nullptr || g_dir.empty()) {
        return std::filesystem::path(".");
    }
    // UTF-8 from the bridge: a folder name outside the ANSI code page stays whole.
    return std::filesystem::path(std::u8string(g_dir.begin(), g_dir.end()));
}

}  // namespace CoopHsPlatform

// The emulator's API slice (hookshot/compat), answered from the table.
namespace CoopRuntime {
bool Running() {
    return CoopHsPlatform::Get() != nullptr;
}
std::uint64_t MainBase() {
    const CoopPlatform* p = CoopHsPlatform::Get();
    return p != nullptr ? p->main_base : 0;
}
const std::string& MainBuildHex() {
    return CoopHsPlatform::g_build;
}
std::filesystem::path GameDir() {
    return CoopHsPlatform::GameDir();
}
bool Valid(std::uint64_t address, std::size_t size) {
    // The bridge checks a whole range before it copies: one byte at each end
    // says whether the range is mapped (Hookshot's guest memory has no holes
    // inside a mapped range the hook reads).
    const CoopPlatform* p = CoopHsPlatform::Get();
    std::uint8_t b = 0;
    return p != nullptr && p->read != nullptr && size != 0 && address + size > address &&
           p->read(address, &b, 1) != 0 && p->read(address + size - 1, &b, 1) != 0;
}
bool Read(std::uint64_t address, void* out, std::size_t size) {
    const CoopPlatform* p = CoopHsPlatform::Get();
    return p != nullptr && p->read != nullptr && p->read(address, out, size) != 0;
}
bool Write(std::uint64_t address, const void* in, std::size_t size) {
    const CoopPlatform* p = CoopHsPlatform::Get();
    return p != nullptr && p->write != nullptr && p->write(address, in, size) != 0;
}
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen) {
    const CoopPlatform* p = CoopHsPlatform::Get();
    return p != nullptr && p->cas32 != nullptr && p->cas32(address, expected, desired, &seen) != 0;
}
void Log(const std::string& line) {
    // Only Hookshot's log, as before: nothing while co-op is off or Hookshot gave none.
    const CoopPlatform* p = CoopHsPlatform::Get();
    if (p != nullptr && p->log == CoopHsPlatform::HsLog) {
        p->log(line.c_str());
    }
}
bool PadButtons(std::uint64_t& out) {
    const CoopPlatform* p = CoopHsPlatform::Get();
    if (p == nullptr || p->pad_buttons == nullptr) {
        return false;
    }
    out = p->pad_buttons(0);
    return true;
}
}  // namespace CoopRuntime
