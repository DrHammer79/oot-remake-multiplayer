/* SPDX-License-Identifier: GPL-3.0-or-later
   The portable core's interface to the platform it runs on (oot-remake-multiplayer, docs/platform.md).

   v1 'everywhere' step 1 (coop-platform-h): the core (protocol, net, host sync, game logic) asks the platform for
   everything outside itself through one table, CoopPlatform, that the adapter fills: the Longshot/Hookshot module
   today, an ExeFS code mod (emulators, Switch) later. Plain C, so both sides' compilers agree on the layout, and so
   the core can be built as a payload that a recompiler lifts:
   - Only plain structs and C function pointers cross the boundary: no virtuals, std::function or C++ types.
   - Every slot's target is a named, exported function of the adapter (COOP_PLATFORM_SLOTS lists the slots, so a
     build step can write a lifter's entries list from it), and so is every function the core hands back to the
     adapter (thread_start's fn).
   - Versioned like HosCoopApi (hookshot/src/hos_coop_api.h): `version` and `size` first; the table only grows at its
     end, and the core reads a later slot only when `size` covers it (COOP_PLATFORM_HAS).
   No callers yet: coop::os (coop/platform/include/coop/os.hpp) wraps the OS slots with a std backend on PC. */
#ifndef COOP_PLATFORM_H
#define COOP_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_PLATFORM_VERSION 1u

/* Who filled the table (CoopPlatform.kind). */
#define COOP_PLATFORM_KIND_STD 0u              /* PC, no game: the std backend (tools, tests) */
#define COOP_PLATFORM_KIND_RECOMP_LONGSHOT 1u  /* the Longshot/Hookshot module */
#define COOP_PLATFORM_KIND_RECOMP_HOOKSHOT 2u
#define COOP_PLATFORM_KIND_EXEFS_EMU 3u        /* ExeFS code mod on an emulator */
#define COOP_PLATFORM_KIND_EXEFS_SWITCH 4u     /* ExeFS code mod on a console */
#define COOP_PLATFORM_KIND_EMU_HOOK 5u         /* the legacy emulator hook (lichii/hook) */

/* CoopPlatform.ui_caps bits. */
#define COOP_PLATFORM_UI_GAME 1u        /* the core draws with the game's own text/layout through call() */
#define COOP_PLATFORM_UI_OVERLAY_2D 2u  /* draw_2d works */

/* Socket kinds for sock_open, and what sock_recvfrom returns when nothing is waiting. */
#define COOP_SOCK_UDP4 4u
#define COOP_SOCK_UDP6 6u
#define COOP_SOCK_WOULD_BLOCK (-2)

/* One game function call: x0-x7, four words at sp, s0. The same layout as HosCoopCall. */
typedef struct CoopCall {
    uint64_t function;   /* guest address */
    uint64_t x[8];
    uint64_t stack[4];
    float s0;
} CoopCall;

/* A network address. family 4: addr[0..3] is the IPv4 address; family 6: all 16 bytes, scope_id for link-local. */
typedef struct CoopAddr {
    uint16_t family;     /* 4 or 6; 0 = none */
    uint16_t port;       /* host byte order */
    uint32_t scope_id;
    uint8_t addr[16];    /* network byte order */
} CoopAddr;

/* Defined by the UI step (v1 step 6, ui-drawlist); only pointed at until then. */
typedef struct CoopDrawList CoopDrawList;

/* The table. Slots marked OPTIONAL may be NULL (the core then does without); the OS slots are required. Unless a
   slot says otherwise it may be called from any thread. */
typedef struct CoopPlatform {
    uint32_t version;    /* COOP_PLATFORM_VERSION */
    uint32_t size;       /* sizeof(CoopPlatform) as the adapter was built */
    uint32_t kind;       /* COOP_PLATFORM_KIND_* */
    uint32_t ui_caps;    /* COOP_PLATFORM_UI_* */

    /* Identity: the same meaning everywhere (the game's main NSO). 0 / NULL with no game. */
    uint64_t main_base;
    const char *main_build_id;   /* hex, 32 digits */

    /* Game memory (guest addresses; plain pointers in process). OPTIONAL. 0 when any byte is unmapped (nothing
       copied): in process the adapter checks regions so a bad address is a refusal, never a fault. */
    int (*read)(uint64_t address, void *out, size_t size);
    int (*write)(uint64_t address, const void *in, size_t size);
    int (*cas32)(uint64_t address, uint32_t expected, uint32_t desired, uint32_t *seen);

    /* Game calls, OPTIONAL: only on the game's thread, inside a hook with its ctx (as HosCoopApi's). */
    int (*call)(void *ctx, const CoopCall *call, uint64_t *x0);
    uint64_t (*scratch)(void *ctx, const void *data, size_t n);

    /* Hooks the core asks for by id (from the build table), OPTIONAL: 1 installed, 0 missing. */
    int (*hook_wrap)(uint32_t wrap_id, uint64_t main_offset, const uint32_t first_words[4]);
    int (*hook_site)(uint32_t site_id, uint64_t main_offset, const uint32_t words[2]);

    /* Input, OPTIONAL. Buttons are Switch NpadButton bits; sticks are lx, ly, rx, ry. key_down: a Windows
       virtual-key code, 0 everywhere without a keyboard. */
    uint64_t (*pad_buttons)(int player);
    void (*pad_sticks)(int player, int32_t out[4]);
    int (*key_down)(uint32_t vk);

    /* UI, OPTIONAL (ui_caps says which). */
    void (*draw_2d)(const CoopDrawList *list);

    /* Voice, OPTIONAL: frames written / read, 0 = none. */
    int (*voice_out)(const int16_t *pcm, size_t frames, int channels);
    int (*voice_in)(int16_t *pcm, size_t frames);

    /* Network: non-blocking UDP, so ENet runs on it. Handles are >= 0; -1 is an error. */
    int (*sock_open)(uint32_t sock_kind);                                            /* COOP_SOCK_UDP4/6 */
    int (*sock_bind)(int s, const CoopAddr *local);                                  /* port 0 = any; 0 ok */
    int (*sock_local)(int s, CoopAddr *out);                                         /* the bound address; 0 ok */
    int (*sock_sendto)(int s, const void *data, size_t n, const CoopAddr *to);       /* bytes sent */
    int (*sock_recvfrom)(int s, void *buf, size_t cap, CoopAddr *from);              /* bytes, or WOULD_BLOCK */
    int (*sock_close)(int s);                                                        /* 0 ok */
    int (*resolve)(const char *host, uint32_t sock_kind, CoopAddr *out);             /* 0 ok; port left 0 */

    /* Threads and locks. thread_start returns a handle (NULL on failure) to pass to thread_join exactly once; core
       -1 = any, stack 0 = the platform's default. thread_id is the caller's, for "game thread only" checks. */
    void *(*thread_start)(void (*fn)(void *arg), void *arg, int core, size_t stack);
    int (*thread_join)(void *thread);
    uint64_t (*thread_id)(void);
    void *(*mutex_new)(void);   /* NULL on failure; not recursive */
    void (*mutex_lock)(void *mutex);
    void (*mutex_unlock)(void *mutex);
    void (*mutex_free)(void *mutex);

    /* Time: a steady clock (never goes back), microseconds from any start. */
    uint64_t (*now_us)(void);
    void (*sleep_us)(uint64_t us);

    /* Memory: align is a power of two, at most 4096. mem_free(NULL) does nothing. */
    void *(*mem_alloc)(size_t n, size_t align);
    void (*mem_free)(void *p);

    /* Files, relative to the mod's own folder ('/' separators; no absolute paths, no ".."). file_read hands back a
       buffer from mem_alloc (release it with mem_free); file_write replaces the file whole (never half written). 0 ok. */
    int (*file_read)(const char *rel, void **buf, size_t *n);
    int (*file_write)(const char *rel, const void *buf, size_t n);

    /* One line of the mod's log (no newline needed). */
    void (*log)(const char *line);
} CoopPlatform;

/* 1 when the table is long enough to have `field` (a later version's slot in an older adapter's table). */
#define COOP_PLATFORM_HAS(p, field) \
    ((p)->size >= offsetof(CoopPlatform, field) + sizeof(((const CoopPlatform *)0)->field))

/* Every function slot, in order: X(name, required). For a lifter's entries list and for the checks in coop::os. */
#define COOP_PLATFORM_SLOTS(X) \
    X(read, 0) X(write, 0) X(cas32, 0) X(call, 0) X(scratch, 0) X(hook_wrap, 0) X(hook_site, 0) \
    X(pad_buttons, 0) X(pad_sticks, 0) X(key_down, 0) X(draw_2d, 0) X(voice_out, 0) X(voice_in, 0) \
    X(sock_open, 1) X(sock_bind, 1) X(sock_local, 1) X(sock_sendto, 1) X(sock_recvfrom, 1) X(sock_close, 1) \
    X(resolve, 1) X(thread_start, 1) X(thread_join, 1) X(thread_id, 1) X(mutex_new, 1) X(mutex_lock, 1) \
    X(mutex_unlock, 1) X(mutex_free, 1) X(now_us, 1) X(sleep_us, 1) X(mem_alloc, 1) X(mem_free, 1) X(file_read, 1) \
    X(file_write, 1) X(log, 1)

/* The core's entry points that an adapter calls. Declared for review of the shape; defined by later steps (core-lib-
   split / hookshot-on-platform), so nothing calls them yet. */
int coop_core_start(const CoopPlatform *platform);                   /* build check, scan, asks for hooks */
void coop_core_tick(void *ctx, int site);                            /* frame sites; runs the queued game calls */
int coop_core_wrap(uint32_t wrap_id, int phase, uint64_t *x, void *ctx);
int coop_core_event(uint32_t event, uint64_t a, uint64_t b);         /* world/actor events */
uint32_t coop_core_hold(uint64_t component, uint32_t bits);
uint64_t coop_core_pad(uint64_t buttons, int32_t stick[4]);
void coop_core_stop(void);

#ifdef __cplusplus
}
#endif
#endif
