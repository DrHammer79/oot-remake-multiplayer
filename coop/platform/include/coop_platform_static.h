/* SPDX-License-Identifier: GPL-3.0-or-later
   The static binding of the platform's required slots (v1 step 8, docs/platform.md "The payload build").

   A payload that a recompiler lifts (the ExeFS code mod, subsdk1) may not call through a function pointer, and
   CoopPlatform is a table of them. So a payload build defines COOP_PLATFORM_STATIC, and coop::os (os.cpp) then calls
   the adapter's coop_plat_<slot> functions below directly, whatever table is installed. The adapter defines every one
   of them (switch/adapter/platform_os.cpp: OS; platform_net.cpp: sockets) and fills its CoopPlatform's required slots
   with the same functions, so the table and the direct calls never disagree. PC and the Longshot/Hookshot module do
   not define COOP_PLATFORM_STATIC and keep the table.

   Plain C. Each declaration has exactly its slot's type (os.cpp checks that at compile time); the meaning of each is
   the slot's, in coop_platform.h. */
#ifndef COOP_PLATFORM_STATIC_H
#define COOP_PLATFORM_STATIC_H
#include "coop_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Network (piece c, v1-exefs-net). */
int coop_plat_sock_open(uint32_t sock_kind);
int coop_plat_sock_bind(int s, const CoopAddr *local);
int coop_plat_sock_local(int s, CoopAddr *out);
int coop_plat_sock_sendto(int s, const void *data, size_t n, const CoopAddr *to);
int coop_plat_sock_recvfrom(int s, void *buf, size_t cap, CoopAddr *from);
int coop_plat_sock_close(int s);
int coop_plat_resolve(const char *host, uint32_t sock_kind, CoopAddr *out);

/* Threads and locks. */
void *coop_plat_thread_start(void (*fn)(void *arg), void *arg, int core, size_t stack);
int coop_plat_thread_join(void *thread);
uint64_t coop_plat_thread_id(void);
void *coop_plat_mutex_new(void);
void coop_plat_mutex_lock(void *mutex);
void coop_plat_mutex_unlock(void *mutex);
void coop_plat_mutex_free(void *mutex);

/* Time. */
uint64_t coop_plat_now_us(void);
void coop_plat_sleep_us(uint64_t us);

/* Memory. */
void *coop_plat_mem_alloc(size_t n, size_t align);
void coop_plat_mem_free(void *p);

/* Files. */
int coop_plat_file_read(const char *rel, void **buf, size_t *n);
int coop_plat_file_write(const char *rel, const void *buf, size_t n);

/* Log. */
void coop_plat_log(const char *line);

/* Appended by v1 step 4 (os-wrap); env_get is optional, so a payload has none (coop::os::GetEnv gives nothing). */
int coop_plat_mutex_try_lock(void *mutex);
int coop_plat_random_fill(void *out, size_t n);
void *coop_plat_cond_new(void);
int coop_plat_cond_wait_us(void *cond, void *mutex, uint64_t us);
void coop_plat_cond_notify_one(void *cond);
void coop_plat_cond_notify_all(void *cond);
void coop_plat_cond_free(void *cond);

#ifdef __cplusplus
}
#endif
#endif
