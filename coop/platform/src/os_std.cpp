// SPDX-License-Identifier: GPL-3.0-or-later
// The std backend of CoopPlatform (coop/os.hpp StdPlatform): PC only, no game.
#include "coop/os.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace coop::os {
namespace {

namespace fs = std::filesystem;

// ---- time
uint64_t StdNowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
void StdSleepUs(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

// ---- log
void StdLog(const char *line) {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
}

// ---- memory
void *StdAlloc(size_t n, size_t align) {
    if (align == 0 || (align & (align - 1)) != 0 || align > 4096) return nullptr;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (n == 0) n = 1;
#ifdef _WIN32
    return _aligned_malloc(n, align);
#else
    void *p = nullptr;
    return posix_memalign(&p, align, n) == 0 ? p : nullptr;
#endif
}
void StdFree(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// ---- threads and locks (core and stack size are hints std::thread cannot take)
void *StdThreadStart(void (*fn)(void *), void *arg, int, size_t) {
    if (!fn) return nullptr;
    try {
        return new std::thread(fn, arg);
    } catch (...) {
        return nullptr;
    }
}
int StdThreadJoin(void *thread) {
    auto *t = static_cast<std::thread *>(thread);
    if (!t) return -1;
    int rc = 0;
    if (t->joinable()) {
        try {
            t->join();
        } catch (...) {
            rc = -1;
        }
    }
    delete t;
    return rc;
}
uint64_t StdThreadId() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }
void *StdMutexNew() {
    try {
        return new std::mutex;
    } catch (...) {
        return nullptr;
    }
}
void StdMutexLock(void *m) { static_cast<std::mutex *>(m)->lock(); }
void StdMutexUnlock(void *m) { static_cast<std::mutex *>(m)->unlock(); }
void StdMutexFree(void *m) { delete static_cast<std::mutex *>(m); }

// ---- files
std::mutex g_root_mutex;
std::string g_root;

bool FullPath(const char *rel, fs::path &out) {
    if (!IsModPath(rel)) return false;
    std::string root;
    {
        std::lock_guard<std::mutex> lock(g_root_mutex);
        root = g_root;
    }
    const std::string text(rel);
    const fs::path rel_path(std::u8string(text.begin(), text.end()));
    out = root.empty() ? rel_path : fs::path(std::u8string(root.begin(), root.end())) / rel_path;
    return true;
}

int StdFileRead(const char *rel, void **buf, size_t *n) {
    if (!buf || !n) return -1;
    *buf = nullptr;
    *n = 0;
    fs::path path;
    if (!FullPath(rel, path)) return -1;
    std::ifstream in(path, std::ios::binary);
    if (!in) return -1;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return -1;
    in.seekg(0, std::ios::beg);
    void *data = StdAlloc(static_cast<size_t>(size), alignof(std::max_align_t));
    if (!data) return -1;
    if (size > 0 && !in.read(static_cast<char *>(data), size)) {
        StdFree(data);
        return -1;
    }
    *buf = data;
    *n = static_cast<size_t>(size);
    return 0;
}

int StdFileWrite(const char *rel, const void *data, size_t n) {
    fs::path path;
    if (!FullPath(rel, path) || (n && !data)) return -1;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return -1;
        if (n) out.write(static_cast<const char *>(data), static_cast<std::streamsize>(n));
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            return -1;
        }
    }
    fs::rename(tmp, path, ec);   // replaces an existing file (MoveFileEx REPLACE_EXISTING / rename(2))
    if (ec) {
        fs::remove(tmp, ec);
        return -1;
    }
    return 0;
}

// ---- sockets: a small table maps our int handles to native sockets (a Windows SOCKET does not fit an int).
#ifdef _WIN32
using Native = SOCKET;
const Native kBad = INVALID_SOCKET;
void CloseNative(Native s) { closesocket(s); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool NetStart() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using Native = int;
const Native kBad = -1;
void CloseNative(Native s) { close(s); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool NetStart() { return true; }
#endif

constexpr int kMaxSockets = 64;
std::mutex g_sock_mutex;
Native g_socks[kMaxSockets];
bool g_socks_ready = false;

Native Lookup(int s) {
    std::lock_guard<std::mutex> lock(g_sock_mutex);
    if (!g_socks_ready || s < 0 || s >= kMaxSockets) return kBad;
    return g_socks[s];
}

bool ToNative(const CoopAddr &a, sockaddr_storage &out, socklen_t &len) {
    std::memset(&out, 0, sizeof out);
    if (a.family == 4) {
        auto *in = reinterpret_cast<sockaddr_in *>(&out);
        in->sin_family = AF_INET;
        in->sin_port = htons(a.port);
        std::memcpy(&in->sin_addr, a.addr, 4);
        len = sizeof(sockaddr_in);
        return true;
    }
    if (a.family == 6) {
        auto *in6 = reinterpret_cast<sockaddr_in6 *>(&out);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a.port);
        in6->sin6_scope_id = a.scope_id;
        std::memcpy(&in6->sin6_addr, a.addr, 16);
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

bool FromNative(const sockaddr *sa, CoopAddr &out) {
    out = CoopAddr{};
    if (sa->sa_family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(sa);
        out.family = 4;
        out.port = ntohs(in->sin_port);
        std::memcpy(out.addr, &in->sin_addr, 4);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
        out.family = 6;
        out.port = ntohs(in6->sin6_port);
        out.scope_id = in6->sin6_scope_id;
        std::memcpy(out.addr, &in6->sin6_addr, 16);
        return true;
    }
    return false;
}

int StdSockOpen(uint32_t kind) {
    if ((kind != COOP_SOCK_UDP4 && kind != COOP_SOCK_UDP6) || !NetStart()) return -1;
    Native s = socket(kind == COOP_SOCK_UDP4 ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return -1;
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    const bool ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (!ok) {
        CloseNative(s);
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_sock_mutex);
    if (!g_socks_ready) {
        for (Native &slot : g_socks) slot = kBad;
        g_socks_ready = true;
    }
    for (int i = 0; i < kMaxSockets; ++i) {
        if (g_socks[i] == kBad) {
            g_socks[i] = s;
            return i;
        }
    }
    CloseNative(s);
    return -1;
}

int StdSockBind(int s, const CoopAddr *local) {
    const Native n = Lookup(s);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (n == kBad || !local || !ToNative(*local, sa, len)) return -1;
    return bind(n, reinterpret_cast<const sockaddr *>(&sa), len) == 0 ? 0 : -1;
}

int StdSockLocal(int s, CoopAddr *out) {
    const Native n = Lookup(s);
    if (n == kBad || !out) return -1;
    sockaddr_storage sa;
    socklen_t len = sizeof sa;
    if (getsockname(n, reinterpret_cast<sockaddr *>(&sa), &len) != 0) return -1;
    return FromNative(reinterpret_cast<const sockaddr *>(&sa), *out) ? 0 : -1;
}

int StdSockSendTo(int s, const void *data, size_t n, const CoopAddr *to) {
    const Native sock = Lookup(s);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (sock == kBad || !to || (n && !data) || n > 65507 || !ToNative(*to, sa, len)) return -1;
    const auto sent = sendto(sock, static_cast<const char *>(data), static_cast<int>(n), 0,
                             reinterpret_cast<const sockaddr *>(&sa), len);
    return sent < 0 ? -1 : static_cast<int>(sent);
}

int StdSockRecvFrom(int s, void *buf, size_t cap, CoopAddr *from) {
    const Native sock = Lookup(s);
    if (sock == kBad || (cap && !buf)) return -1;
    sockaddr_storage sa;
    socklen_t len = sizeof sa;
    const int want = cap > 65535 ? 65535 : static_cast<int>(cap);
    const auto got = recvfrom(sock, static_cast<char *>(buf), want, 0, reinterpret_cast<sockaddr *>(&sa), &len);
    if (got < 0) return WouldBlock() ? COOP_SOCK_WOULD_BLOCK : -1;
    if (from) FromNative(reinterpret_cast<const sockaddr *>(&sa), *from);
    return static_cast<int>(got);
}

int StdSockClose(int s) {
    Native n = kBad;
    {
        std::lock_guard<std::mutex> lock(g_sock_mutex);
        if (!g_socks_ready || s < 0 || s >= kMaxSockets || g_socks[s] == kBad) return -1;
        n = g_socks[s];
        g_socks[s] = kBad;
    }
    CloseNative(n);
    return 0;
}

int StdResolve(const char *host, uint32_t kind, CoopAddr *out) {
    if (!host || !out || (kind != COOP_SOCK_UDP4 && kind != COOP_SOCK_UDP6) || !NetStart()) return -1;
    addrinfo hints{};
    hints.ai_family = kind == COOP_SOCK_UDP4 ? AF_INET : AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *list = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &list) != 0 || !list) return -1;
    const bool ok = FromNative(list->ai_addr, *out);
    freeaddrinfo(list);
    if (!ok) return -1;
    out->port = 0;
    return 0;
}

CoopPlatform MakeStd() {
    CoopPlatform p{};
    p.version = COOP_PLATFORM_VERSION;
    p.size = sizeof(CoopPlatform);
    p.kind = COOP_PLATFORM_KIND_STD;
    p.sock_open = StdSockOpen;
    p.sock_bind = StdSockBind;
    p.sock_local = StdSockLocal;
    p.sock_sendto = StdSockSendTo;
    p.sock_recvfrom = StdSockRecvFrom;
    p.sock_close = StdSockClose;
    p.resolve = StdResolve;
    p.thread_start = StdThreadStart;
    p.thread_join = StdThreadJoin;
    p.thread_id = StdThreadId;
    p.mutex_new = StdMutexNew;
    p.mutex_lock = StdMutexLock;
    p.mutex_unlock = StdMutexUnlock;
    p.mutex_free = StdMutexFree;
    p.now_us = StdNowUs;
    p.sleep_us = StdSleepUs;
    p.mem_alloc = StdAlloc;
    p.mem_free = StdFree;
    p.file_read = StdFileRead;
    p.file_write = StdFileWrite;
    p.log = StdLog;
    return p;
}

}  // namespace

const CoopPlatform *StdPlatform(const std::string &root) {
    static const CoopPlatform table = MakeStd();
    if (!root.empty()) {
        std::lock_guard<std::mutex> lock(g_root_mutex);
        g_root = root;
    }
    return &table;
}

}  // namespace coop::os
