/* SPDX-License-Identifier: GPL-3.0-or-later
   enet_coop: ENet's socket, time and address layer on CoopPlatform (v1 step 8c). ENet's protocol files
   (callbacks.c, compress.c, host.c, list.c, packet.c, peer.c, protocol.c) are built unchanged; this replaces its
   unix.c / win32.c, so ENet runs wherever the platform has sock_* slots: the ExeFS payload (nn::socket) today.
   PC builds keep stock ENet (target enet); enet_coop is its own target and never links beside it.

   With COOP_PLATFORM_STATIC (the payload, step 8 decision 1) the calls go straight to coop_plat_<slot>; otherwise
   to the table given to coop_enet_use_platform (host tests: the std backend).

   Differences from unix.c, all inside ENet's contract:
   - enet_socket_wait sleeps one slice (at most 1 ms) and then reports "maybe readable": the slots cannot poll,
     and enet_host_service treats a receive of 0 as nothing and loops until its timeout.
   - enet_socket_send returns 0 (dropped, ENet resends reliable data) when the platform refuses a datagram: the
     slot does not say why, and one failed UDP send must not end the whole service call.
   - Stream sockets, listen/accept/connect/shutdown and enet_socketset_select are refused (-1): ENet's UDP host
     never uses them. Socket options are accepted and ignored: platform sockets are already non-blocking.
   - Reverse lookup (enet_address_get_host) gives the dotted address. */
#ifndef COOP_ENET_COOP_H
#define COOP_ENET_COOP_H

#include "coop_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef COOP_PLATFORM_STATIC
/* The table enet_coop calls through (kept, not copied). Before enet_initialize; NULL detaches. */
void coop_enet_use_platform(const CoopPlatform *platform);
#endif

#ifdef __cplusplus
}
#endif

#endif
