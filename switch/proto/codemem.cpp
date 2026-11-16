// The module's kernel calls for codemem.h (writing the game's code through a writable alias).
// Every call checked against Fritchii's kernel (src/core/hle/kernel): svc/svc_process_memory.cpp
// (0x74 refuses the pseudo-handle, maps SharedCode RW), k_server_session.cpp (copy handles go
// through GetObjectForIpc, which turns 0xFFFF8001 into the real process), svc/svc_ipc.cpp
// (0x23 queues and returns; 0x43 with message 0 receives into TLS), k_page_table_base.cpp
// CanContain (SharedCode: inside the ASLR region, outside heap and alias), svc/svc_info.cpp
// (GetInfo 12/13 is that region). Horizon's mesosphere kernel does the same.

#include "codemem.h"

#include "svc.h"

namespace codemem {
namespace {

Alias g_alias[kMaxAliases];
Fallback g_fallback;

// The IPC request's buffer: 0x23 locks it, so a page of its own (module .bss is CodeData,
// which the kernel lets IPC use).
alignas(0x1000) uint32_t g_request[0x1000 / 4];

constexpr int64_t kReceiveTimeoutNs = 1000000000;  // the request is already queued: returns at once

uint32_t* Tls() {
    uint64_t tls;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tls));
    return reinterpret_cast<uint32_t*>(tls);
}

// A real handle to this process (step 1 in codemem.h). Closes the session pair and the event.
Status OwnProcess(uint32_t& process) {
    Status st;
    uint32_t server = 0, client = 0, event = 0;
    uint32_t r = svc::CreateSession(&server, &client);
    if (r != 0) return {kCreateSession, r, 0};
    HandleCopyRequest(g_request, svc::kCurrentProcess);
    r = svc::SendAsyncRequestWithUserBuffer(&event, g_request, sizeof(g_request), client);
    if (r != 0) {
        st = {kSendRequest, r, 0};
    } else {
        int32_t index = -1;
        r = svc::ReplyAndReceive(&index, &server, 1, 0 /*no reply*/, kReceiveTimeoutNs);
        if (r != 0) {
            st = {kReceive, r, 0};
        } else if (!CopiedHandle(Tls(), process)) {
            st = {kNoHandle, 0, 0};
        }
    }
    // Closing the server drops the request unanswered: the kernel ends it (session closed),
    // signals the event and unlocks g_request.
    svc::CloseHandle(server);
    if (event != 0) svc::CloseHandle(event);
    svc::CloseHandle(client);
    return st;
}

// One region from GetInfo (address type, size type = address type + 1).
uint32_t RegionInfo(uint32_t process, uint32_t address_type, Range& out) {
    uint64_t addr = 0, size = 0;
    uint32_t r = svc::GetInfo(&addr, address_type, process, 0);
    if (r == 0) r = svc::GetInfo(&size, address_type + 1, process, 0);
    out = {addr, addr + size};
    return r;
}

// A free gap of `size` bytes where the kernel accepts SharedCode (step 2).
// (No arrays with initializers here: freestanding, no memcpy/memset.)
Status FindRoom(uint32_t process, uint64_t size, uint64_t& at) {
    Range region, heap, alias;
    uint32_t r = RegionInfo(process, svc::kInfoAslrRegionAddress, region);
    if (r == 0) r = RegionInfo(process, svc::kInfoHeapRegionAddress, heap);
    if (r == 0) r = RegionInfo(process, svc::kInfoAliasRegionAddress, alias);
    if (r != 0) return {kRegionInfo, r, 0};
    uint64_t addr = region.lo;
    for (int i = 0; i < 4096 && addr < region.hi; ++i) {
        svc::MemoryInfo mi{};
        r = svc::QueryMemory(&mi, addr);
        if (r != 0) return {kNoRoom, r, 0};
        if (mi.size == 0 || mi.addr + mi.size <= addr) break;  // no progress: stop
        if (mi.state == 0) {  // Free
            at = FitIn(mi.addr, mi.size, size, region, heap, alias);
            if (at != 0) return {};
        }
        addr = mi.addr + mi.size;
    }
    return {kNoRoom, 0, 0};
}

void CleanAndInvalidate(uint64_t written, uint64_t runs, uint64_t size) {
    for (uint64_t a = written & ~uint64_t{63}; a < written + size; a += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (uint64_t a = runs & ~uint64_t{63}; a < runs + size; a += 64)
        __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
}

void Store(uint64_t to, const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) reinterpret_cast<volatile uint32_t*>(to)[i] = words[i];
}

// The old way, page by page: RW, write, RX. After the first refused RX (0xd401 on Fritchii:
// the page is CodeData now) RX is not asked again; such pages stay RW (runs on an emulator only).
bool WriteByPermission(uint64_t at, const uint32_t* words, uint32_t count) {
    uint64_t addr = at;
    uint32_t left = count;
    while (left != 0) {
        const uint64_t page = addr & ~(kPage - 1);
        uint32_t n = static_cast<uint32_t>((page + kPage - addr) / 4);
        if (n > left) n = left;
        const uint32_t r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, kPage, svc::kPermRW);
        if (r != 0) {
            if (g_fallback.refused == 0) g_fallback.refused = r;
            return false;
        }
        Store(addr, words, n);
        CleanAndInvalidate(addr, addr, n * 4ull);
        if (g_fallback.rx_refused == 0) {
            const uint32_t rx = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, kPage, svc::kPermRX);
            if (rx != 0) g_fallback.rx_refused = rx;
        }
        ++g_fallback.writes;
        addr += n * 4ull;
        words += n;
        left -= n;
    }
    return true;
}

}  // namespace

Status Open(const Range* ranges, uint32_t count) {
    if (count > kMaxAliases) return {kTooMany, 0, 0};
    uint32_t process = 0;
    Status st = OwnProcess(process);
    if (st.step != kOk) return st;
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t size = ranges[i].hi - ranges[i].lo;
        uint64_t at = 0;
        st = FindRoom(process, size, at);
        if (st.step == kOk) {
            const uint32_t r = svc::MapProcessMemory(at, process, ranges[i].lo, size);
            if (r != 0) st = {kMap, r, 0};
        }
        if (st.step != kOk) break;
        g_alias[i] = {ranges[i].lo, at, size};
        ++mapped;
    }
    svc::CloseHandle(process);  // the mappings do not need it
    st.mapped = mapped;
    return st;
}

Alias Get(uint32_t i) { return i < kMaxAliases ? g_alias[i] : Alias{}; }

bool Write(uint64_t at, const uint32_t* words, uint32_t count) {
    if (count == 0 || (at & 3)) return false;
    for (const Alias& a : g_alias) {
        const uint64_t rw = Translate(a, at, count * 4ull);
        if (rw == 0) continue;
        Store(rw, words, count);
        CleanAndInvalidate(rw, at, count * 4ull);
        return true;
    }
    return WriteByPermission(at, words, count);
}

const Fallback& FallbackStats() { return g_fallback; }

}  // namespace codemem
