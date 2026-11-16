/* SPDX-License-Identifier: GPL-3.0-or-later
   ENet's view of libc in the freestanding ExeFS payload (v1 step 8c; see mem.c). */
#ifndef COOP_LIBC_SYS_TYPES_H
#define COOP_LIBC_SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
/* Only named by enet/unix.h's ENetSocketSet typedef; enet_socketset_select is refused (enet_coop). */
typedef struct { uint64_t fds_bits[16]; } fd_set;
#endif
