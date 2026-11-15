// SPDX-License-Identifier: GPL-3.0-or-later
// coop::os: the core's OS calls (threads, locks, time, memory, files, sockets, log) over the CoopPlatform table
// (coop_platform.h, docs/platform.md). On PC the std backend fills the table; an ExeFS adapter fills its own.
// No exceptions, no std::function, no virtuals: everything is a call through the installed table.
// v1 step 1: nothing in the core uses it yet (step 4, os-wrap, swaps std::thread/mutex/chrono/fstream for it).
#pragma once

#include <coop_platform.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::os {

// The std backend's table (PC: std::thread, std::mutex, steady_clock, files under `root`, BSD/Winsock sockets).
// Game slots are NULL. `root` is the mod's folder (UTF-8); until one is given, the current folder. The table lives
// for the program; a later call with a root changes it for later file calls.
const CoopPlatform *StdPlatform(const std::string &root = {});

// Why Install refused a table.
enum class InstallError { kNone, kNull, kVersion, kSize, kMissingSlot };

// Uses `platform` for every call below from now on (it must outlive them). Refuses a table of another version, one
// too short for version 1's slots, or one missing a required slot (COOP_PLATFORM_SLOTS), and keeps the old one.
// Before any Install the std backend is used. Call before starting threads that use coop::os.
InstallError Install(const CoopPlatform *platform);
const CoopPlatform &Platform();

// ---- time
std::uint64_t NowUs();
void SleepUs(std::uint64_t us);

// ---- log
void Log(const char *line);
inline void Log(const std::string &line) { Log(line.c_str()); }

// ---- memory
void *Alloc(std::size_t n, std::size_t align = alignof(std::max_align_t));
void Free(void *p);

// ---- files, relative to the mod's folder
bool ReadFile(const char *rel, std::vector<std::uint8_t> &out);
bool WriteFile(const char *rel, const void *data, std::size_t n);
// A path the platform takes: relative, '/' separators, no "..", no ':' or '\\', not empty.
bool IsModPath(const char *rel);

// ---- locks. Works with std::lock_guard / std::scoped_lock (lock/unlock).
class Mutex {
public:
    Mutex();
    ~Mutex();
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    bool ok() const { return handle_ != nullptr; }
    void lock();
    void unlock();

private:
    void *handle_;
    const CoopPlatform *owner_;   // the table that made it frees it, even after a later Install
};

// ---- threads. A plain function and argument (no captures cross the platform boundary). Join before destruction;
// a Thread destroyed while running joins it.
class Thread {
public:
    Thread() = default;
    ~Thread();
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    bool Start(void (*fn)(void *), void *arg, int core = -1, std::size_t stack = 0);
    bool Join();
    bool running() const { return handle_ != nullptr; }

private:
    void *handle_ = nullptr;
    const CoopPlatform *owner_ = nullptr;
};
std::uint64_t ThreadId();

// ---- sockets: non-blocking UDP.
// The address of `host` (a name or a literal) for that socket kind, port 0.
bool Resolve(const char *host, std::uint32_t sock_kind, CoopAddr &out);
CoopAddr Ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { Close(); }
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;
    bool Open(std::uint32_t sock_kind = COOP_SOCK_UDP4);
    bool Bind(const CoopAddr &local);
    bool Local(CoopAddr &out) const;
    int SendTo(const void *data, std::size_t n, const CoopAddr &to);
    // Bytes received, COOP_SOCK_WOULD_BLOCK when nothing waits, -1 on an error.
    int RecvFrom(void *buf, std::size_t cap, CoopAddr &from);
    void Close();
    bool open() const { return handle_ >= 0; }

private:
    int handle_ = -1;
    const CoopPlatform *owner_ = nullptr;
};

}  // namespace coop::os
