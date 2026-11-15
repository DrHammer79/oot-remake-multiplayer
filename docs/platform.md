# The platform interface (v1 'everywhere', step 1)

For v1 the co-op core (protocol, net, host sync, the game logic) runs on more than one platform: the Longshot/Hookshot
module today, an ExeFS code mod on emulators and consoles later. The core reaches everything outside itself through
one plain C table, `CoopPlatform`, that the platform's adapter fills.

- `coop/platform/include/coop_platform.h`: the table. Versioned like `HosCoopApi` (`hookshot/src/hos_coop_api.h`):
  `version` and `size` first, the table only grows at its end, and the core reads a later slot only when
  `COOP_PLATFORM_HAS(p, slot)` says the adapter's table reaches it. Game slots (memory, calls, hooks, input, UI, voice)
  are optional; the OS slots (sockets, threads, locks, time, memory, files, log) are required.
  `CoopCall` has `HosCoopCall`'s layout.
- Built to be lifted: only plain structs and C function pointers cross the boundary (no virtuals, `std::function` or
  C++ types). `COOP_PLATFORM_SLOTS` lists every slot, so a build step can write a recompiler's entries list from it;
  the functions the core hands to the adapter (`thread_start`'s) need entries too.
- `coop/platform/include/coop/os.hpp` (`coop::os`): the core's threads, mutexes, time, aligned memory, files (relative to
  the mod's folder, written whole through a temporary file, no `..` or absolute paths), non-blocking UDP sockets and
  log, all through the installed table. `coop::os::StdPlatform()` is the PC backend (std::thread, std::mutex,
  steady_clock, std::filesystem, Winsock/BSD sockets); `coop::os::Install()` takes an adapter's table and refuses one of
  another version, too short, or missing a required slot. No exceptions cross it.
- Library `coop_platform` (`coop/CMakeLists.txt`); tests in `tests/platform/` (the C and C++ layouts agree, the
  install checks, threads, files, UDP over loopback).
- Not yet used: step 4 (os-wrap) moves the core's `std::thread`/`std::mutex`/`chrono`/`fstream` onto `coop::os`, and
  step 5 makes the Longshot module fill `CoopPlatform` from `HosCoopApi`. The `coop_core_*` entry points are declared
  for review of their shape and defined by those later steps.
- Changes from the design sketch: `thread_start` returns a handle and `thread_join` takes it; `thread_id`,
  `mutex_free`, `sleep_us` and `sock_local` were added; the memory slots are `mem_alloc`/`mem_free` (a CRT may define
  `free` as a macro); sockets are opened by kind (`COOP_SOCK_UDP4`/`6`) and `sock_recvfrom` answers
  `COOP_SOCK_WOULD_BLOCK` when nothing waits.
