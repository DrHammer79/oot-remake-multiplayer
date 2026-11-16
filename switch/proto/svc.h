// Kernel calls the prototype makes itself (no libnx, no SDK headers).
// Numbers and register use as in the Horizon kernel ABI (switchbrew.org/wiki/SVC).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace svc {

// Out-of-line SDK/libnx wrappers let the kernel clobber the scratch registers; say so here
// too, so the calls stay correct on a console, not just under an emulator's HLE.
#define SVC_CLOBBERS "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17"

struct MemoryInfo {
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
};

inline constexpr uint32_t kCurrentProcess = 0xFFFF8001;  // pseudo handle
inline constexpr uint32_t kPermR = 1;
inline constexpr uint32_t kPermRW = 3;
inline constexpr uint32_t kPermRX = 5;

// 0x06 QueryMemory(MemoryInfo* out, u32* page_info, u64 addr)
inline uint32_t QueryMemory(MemoryInfo* out, uint64_t addr) {
    register uint64_t x0 __asm__("x0") = reinterpret_cast<uint64_t>(out);
    register uint64_t x1 __asm__("x1");
    register uint64_t x2 __asm__("x2") = addr;
    __asm__ volatile("svc 0x06" : "+r"(x0), "=r"(x1), "+r"(x2) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

// 0x16 CloseHandle(handle)
inline uint32_t CloseHandle(uint32_t handle) {
    register uint64_t x0 __asm__("x0") = handle;
    __asm__ volatile("svc 0x16" : "+r"(x0) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

// 0x23 SendAsyncRequestWithUserBuffer(u32* out_event, void* message, u64 size, u32 session):
// the request is queued and the call returns; the event fires when it is answered or dropped.
inline uint32_t SendAsyncRequestWithUserBuffer(uint32_t* out_event, void* message, uint64_t size,
                                               uint32_t session) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = reinterpret_cast<uint64_t>(message);
    register uint64_t x2 __asm__("x2") = size;
    register uint64_t x3 __asm__("x3") = session;
    __asm__ volatile("svc 0x23" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    *out_event = static_cast<uint32_t>(x1);
    return static_cast<uint32_t>(x0);
}

// 0x27 OutputDebugString(const char* s, u64 len): the emulator logs it.
inline uint32_t OutputDebugString(const char* s, size_t len) {
    register uint64_t x0 __asm__("x0") = reinterpret_cast<uint64_t>(s);
    register uint64_t x1 __asm__("x1") = len;
    __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

// 0x29 GetInfo(u64* out, u32 type, u32 handle, u64 sub)
inline uint32_t GetInfo(uint64_t* out, uint32_t type, uint32_t handle, uint64_t sub) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = type;
    register uint64_t x2 __asm__("x2") = handle;
    register uint64_t x3 __asm__("x3") = sub;
    __asm__ volatile("svc 0x29" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    *out = x1;
    return static_cast<uint32_t>(x0);
}
inline constexpr uint32_t kInfoAliasRegionAddress = 2;
inline constexpr uint32_t kInfoAliasRegionSize = 3;
inline constexpr uint32_t kInfoHeapRegionAddress = 4;
inline constexpr uint32_t kInfoHeapRegionSize = 5;
inline constexpr uint32_t kInfoAslrRegionAddress = 12;  // where SharedCode (0x74's alias) may go
inline constexpr uint32_t kInfoAslrRegionSize = 13;

// 0x40 CreateSession(u32* server, u32* client, bool is_light, u64 name)
inline uint32_t CreateSession(uint32_t* server, uint32_t* client) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1");
    register uint64_t x2 __asm__("x2") = 0;  // not light
    register uint64_t x3 __asm__("x3") = 0;  // no name
    __asm__ volatile("svc 0x40" : "=r"(x0), "=r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    *server = static_cast<uint32_t>(x1);
    *client = static_cast<uint32_t>(x2);
    return static_cast<uint32_t>(x0);
}

// 0x43 ReplyAndReceive(s32* index, const u32* handles, s32 count, u32 reply_target, s64 timeout_ns):
// the received message lands in this thread's TLS.
inline uint32_t ReplyAndReceive(int32_t* index, const uint32_t* handles, int32_t count,
                                uint32_t reply_target, int64_t timeout_ns) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = reinterpret_cast<uint64_t>(handles);
    register uint64_t x2 __asm__("x2") = static_cast<uint64_t>(count);
    register uint64_t x3 __asm__("x3") = reply_target;
    register uint64_t x4 __asm__("x4") = static_cast<uint64_t>(timeout_ns);
    __asm__ volatile("svc 0x43" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4) : : "memory", SVC_CLOBBERS);
    *index = static_cast<int32_t>(x1);
    return static_cast<uint32_t>(x0);
}

// 0x73 SetProcessMemoryPermission(handle, addr, size, perm)
inline uint32_t SetProcessMemoryPermission(uint32_t handle, uint64_t addr, uint64_t size,
                                           uint32_t perm) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = addr;
    register uint64_t x2 __asm__("x2") = size;
    register uint64_t x3 __asm__("x3") = perm;
    __asm__ volatile("svc 0x73" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

// 0x74 MapProcessMemory(dst, process, src, size): maps `process`'s pages at dst, state SharedCode,
// permission RW. Needs a real process handle (the kernel refuses the pseudo-handle here).
inline uint32_t MapProcessMemory(uint64_t dst, uint32_t process, uint64_t src, uint64_t size) {
    register uint64_t x0 __asm__("x0") = dst;
    register uint64_t x1 __asm__("x1") = process;
    register uint64_t x2 __asm__("x2") = src;
    register uint64_t x3 __asm__("x3") = size;
    __asm__ volatile("svc 0x74" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

// 0x75 UnmapProcessMemory(dst, process, src, size)
inline uint32_t UnmapProcessMemory(uint64_t dst, uint32_t process, uint64_t src, uint64_t size) {
    register uint64_t x0 __asm__("x0") = dst;
    register uint64_t x1 __asm__("x1") = process;
    register uint64_t x2 __asm__("x2") = src;
    register uint64_t x3 __asm__("x3") = size;
    __asm__ volatile("svc 0x75" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory", SVC_CLOBBERS);
    return static_cast<uint32_t>(x0);
}

}  // namespace svc
