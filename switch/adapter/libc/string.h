/* SPDX-License-Identifier: GPL-3.0-or-later
   ENet's view of libc in the freestanding ExeFS payload (v1 step 8c; see mem.c). */
#ifndef COOP_LIBC_STRING_H
#define COOP_LIBC_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#ifdef __cplusplus
}
#endif
#endif
