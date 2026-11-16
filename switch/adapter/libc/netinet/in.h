/* SPDX-License-Identifier: GPL-3.0-or-later
   ENet's view of libc in the freestanding ExeFS payload (v1 step 8c; see mem.c). */
#ifndef COOP_LIBC_NETINET_IN_H
#define COOP_LIBC_NETINET_IN_H
#include <stdint.h>
/* ENET_HOST_TO_NET_* use these; the payload is little-endian AArch64. */
static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
#endif
