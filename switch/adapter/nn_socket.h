// SPDX-License-Identifier: GPL-3.0-or-later
// The game's socket library (nn::socket in the sdk module), as the ExeFS adapter imports it: rtld links these by
// their mangled names, so the declarations must match the sdk's exactly (types decide the mangling). Only the
// BSD-style overloads that take ::sockaddr; the prototype's Socket/Connect/Send/Close resolved this way (RUN PASS
// v1-proto-a). Never call Initialize: the game has the library up (prototype run, "already initialized").
// Kept apart from nn_imports.h (the OS imports, v1-exefs-os) so the two pieces never edit one file; fold later.
#ifndef COOP_SWITCH_NN_SOCKET_H
#define COOP_SWITCH_NN_SOCKET_H

#include <stddef.h>
#include <stdint.h>

// The Switch's (FreeBSD's) layouts. Only pointers to them cross into the sdk.
struct sockaddr {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    unsigned int ai_addrlen;
    char *ai_canonname;
    sockaddr *ai_addr;
    addrinfo *ai_next;
};

namespace nn::socket {
bool IsInitialized();                                                      // _ZN2nn6socket13IsInitializedEv
int Socket(int domain, int type, int protocol);                            // _ZN2nn6socket6SocketEiii
int Bind(int fd, const ::sockaddr *addr, unsigned int len);                // _ZN2nn6socket4BindEiPK8sockaddrj
int GetSockName(int fd, ::sockaddr *addr, unsigned int *len);              // _ZN2nn6socket11GetSockNameEiP8sockaddrPj
long SendTo(int fd, const void *data, unsigned long size, int flags, const ::sockaddr *to,
            unsigned int to_len);                                          // _ZN2nn6socket6SendToEiPKvmiPK8sockaddrj
long RecvFrom(int fd, void *buf, unsigned long size, int flags, ::sockaddr *from,
              unsigned int *from_len);                                     // _ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj
int Close(int fd);                                                         // _ZN2nn6socket5CloseEi
int GetAddrInfo(const char *node, const char *service, const ::addrinfo *hints,
                ::addrinfo **res);  // _ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_
void FreeAddrInfo(::addrinfo *res);                                        // _ZN2nn6socket12FreeAddrInfoEP8addrinfo
}  // namespace nn::socket

#endif
