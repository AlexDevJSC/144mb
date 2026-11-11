/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_WINSOCK_H
#define NTNATIVE_WINSOCK_H
#include "compat.h"

enum { NT_FD_SETSIZE = 64 };
struct nt_wsa_data {
    uint16_t version, highest_version, maximum_sockets, maximum_datagram;
    char *vendor;
    char description[257], system_status[129];
};
struct nt_fd_set { uint32_t count; uintptr_t handles[NT_FD_SETSIZE]; };
struct nt_socket_time { int32_t seconds, microseconds; };

int NT_MS_ABI nt_WSAStartup(uint16_t version, struct nt_wsa_data *data);
int NT_MS_ABI nt_WSACleanup(void);
int NT_MS_ABI nt_WSAGetLastError(void);
uintptr_t NT_MS_ABI nt_socket(int domain, int type, int protocol);
int NT_MS_ABI nt_closesocket(uintptr_t handle);
int NT_MS_ABI nt_bind(uintptr_t handle, const void *address, int length);
int NT_MS_ABI nt_connect(uintptr_t handle, const void *address, int length);
int NT_MS_ABI nt_getsockname(uintptr_t handle, void *address, int *length);
int NT_MS_ABI nt_listen(uintptr_t handle, int backlog);
uintptr_t NT_MS_ABI nt_accept(uintptr_t handle, void *address, int *length);
int NT_MS_ABI nt_send(uintptr_t handle, const char *buffer, int length, int flags);
int NT_MS_ABI nt_recv(uintptr_t handle, char *buffer, int length, int flags);
int NT_MS_ABI nt_ioctlsocket(uintptr_t handle, int32_t command, uint32_t *value);
int NT_MS_ABI nt_setsockopt(uintptr_t handle, int level, int option, const void *value, int length);
int NT_MS_ABI nt_select(int ignored, struct nt_fd_set *readable, struct nt_fd_set *writable,
                       struct nt_fd_set *exceptions, const struct nt_socket_time *timeout);
uint32_t NT_MS_ABI nt_htonl(uint32_t value);
uint16_t NT_MS_ABI nt_htons(uint16_t value);

#endif
