// The game's own nn::os and nn::fs functions the ExeFS adapter calls (platform_os.cpp). rtld links them by name from
// sdk, so each declaration must mangle to exactly the name sdk exports: the comment after each is that name, checked
// against the game's sdk dynsym (1.1.1, a local dump; nothing copied), and tools/switch_mod/imports.txt lists the
// same names so build.py refuses a module that imports anything else (a wrong parameter type = a new name = refused).
// Return values: nn::Result is one 32-bit word in w0, 0 = success; the return type is not part of a mangled name.
// Struct layouts: only what we touch. Sizes we only hold (ThreadType, MutexType) are over-allocated in our memory.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nn {

struct TimeSpan {  // nn::TimeSpan: nanoseconds; 8 bytes, trivially copyable, passed in x0
    long ns;
};

namespace os {

struct ThreadType;  // opaque: we only hold kThreadTypeBytes for it
struct MutexType;   // opaque: we only hold kMutexTypeBytes for it
struct Tick {       // nn::os::Tick: 8 bytes, returned in x0
    long value;
};

// Generous: sdk's ThreadType is 0x1c0 bytes and MutexType 0x20 on the SDKs we know; nothing breaks if ours are bigger.
inline constexpr size_t kThreadTypeBytes = 0x400;
inline constexpr size_t kMutexTypeBytes = 0x40;
inline constexpr int kDefaultPriority = 16;  // nn::os's default; 0 = highest, 31 = lowest for a game
// The co-op's threads: below the game's (16 and up for its main and render threads), so a co-op thread that
// spins can never take the CPU from the game; the cost is that a game busy on every core delays co-op, never
// the other way round. Same default core as the game's own threads (core -1); a caller may ask for one.
inline constexpr int kCoopPriority = 24;

// _ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi
uint32_t CreateThread(ThreadType* thread, void (*fn)(void*), void* arg, void* stack, unsigned long stack_size,
                      int priority);
// _ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii
uint32_t CreateThread(ThreadType* thread, void (*fn)(void*), void* arg, void* stack, unsigned long stack_size,
                      int priority, int ideal_core);
void StartThread(ThreadType* thread);              // _ZN2nn2os11StartThreadEPNS0_10ThreadTypeE
void WaitThread(ThreadType* thread);               // _ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE
void DestroyThread(ThreadType* thread);            // _ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE
void SetThreadName(ThreadType* thread, const char* name);  // _ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc
ThreadType* GetCurrentThread();                    // _ZN2nn2os16GetCurrentThreadEv
uint64_t GetThreadId(const ThreadType* thread);    // _ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE
void SleepThread(TimeSpan time);                   // _ZN2nn2os11SleepThreadENS_8TimeSpanE

void InitializeMutex(MutexType* mutex, bool recursive, int lock_level);  // _ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi
void FinalizeMutex(MutexType* mutex);              // _ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE
void LockMutex(MutexType* mutex);                  // _ZN2nn2os9LockMutexEPNS0_9MutexTypeE
void UnlockMutex(MutexType* mutex);                // _ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE

bool TryLockMutex(MutexType* mutex);               // _ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE

// Condition variables (with an nn::os mutex the caller holds). TimedWait returns ConditionVariableStatus:
// 0 timed out, 1 woken (as Atmosphere's os::ConditionVariableStatus).
struct ConditionVariableType;  // opaque: we only hold kCondTypeBytes for it
inline constexpr size_t kCondTypeBytes = 0x40;
inline constexpr int kCondTimedOut = 0;
// _ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE
void InitializeConditionVariable(ConditionVariableType* cond);
// _ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE
void FinalizeConditionVariable(ConditionVariableType* cond);
// _ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE
void SignalConditionVariable(ConditionVariableType* cond);
// _ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE
void BroadcastConditionVariable(ConditionVariableType* cond);
// _ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE
int TimedWaitConditionVariable(ConditionVariableType* cond, MutexType* mutex, TimeSpan timeout);


Tick GetSystemTick();                              // _ZN2nn2os13GetSystemTickEv
long GetSystemTickFrequency();                     // _ZN2nn2os22GetSystemTickFrequencyEv

}  // namespace os

namespace fs {

struct FileHandle {  // nn::fs::FileHandle: one pointer, passed in x0
    void* handle;
};
struct WriteOption {  // nn::fs::WriteOption
    int flags;
};
inline constexpr int kOpenRead = 1;
inline constexpr int kOpenWrite = 2;
inline constexpr int kWriteFlush = 1;

uint32_t MountSdCard(const char* name);            // _ZN2nn2fs11MountSdCardEPKc
uint32_t OpenFile(FileHandle* out, const char* path, int mode);  // _ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci
void CloseFile(FileHandle file);                   // _ZN2nn2fs9CloseFileENS0_10FileHandleE
uint32_t GetFileSize(long* out, FileHandle file);  // _ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE
// _ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm
uint32_t ReadFile(unsigned long* read, FileHandle file, long offset, void* buf, unsigned long size);
// _ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE
uint32_t WriteFile(FileHandle file, long offset, const void* buf, unsigned long size, const WriteOption& option);
uint32_t FlushFile(FileHandle file);               // _ZN2nn2fs9FlushFileENS0_10FileHandleE
uint32_t CreateFile(const char* path, long size);  // _ZN2nn2fs10CreateFileEPKcl
uint32_t DeleteFile(const char* path);             // _ZN2nn2fs10DeleteFileEPKc
uint32_t RenameFile(const char* from, const char* to);  // _ZN2nn2fs10RenameFileEPKcS2_
uint32_t CreateDirectory(const char* path);        // _ZN2nn2fs15CreateDirectoryEPKc

}  // namespace fs

namespace crypto {
// The system's cryptographic random source (csrng through the SDK), not nn::os::GenerateRandomBytes (TinyMT):
// co-op makes player keys and join codes from it. sdk exports it, so the module links it or does not load.
void GenerateCryptographicallyRandomBytes(void* out, unsigned long size);  // _ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm
}  // namespace crypto
}  // namespace nn
