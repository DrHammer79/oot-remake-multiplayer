/* SPDX-License-Identifier: GPL-3.0-or-later
   The few libc pieces the ExeFS payload needs for ENet (v1 step 8c). The payload is freestanding (no libc): these
   headers are ENet's whole view of libc (ENet's protocol files, built with -I switch/adapter/libc), and this file
   gives the memory functions they and the compiler call. Weak, so a fuller runtime (step 8 f1) replaces them.
   -ffreestanding keeps the compiler from turning these loops back into calls to themselves. */
#include <string.h>

__attribute__((weak)) void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((weak)) void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

__attribute__((weak)) void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
