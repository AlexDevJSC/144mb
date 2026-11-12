/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "winsock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { SOCKET_COUNT = 256, WSAEINVAL = 10022, WSAEFAULT = 10014, WSAENOTSOCK = 10038,
       WSAENOPROTOOPT = 10042, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047,
       WSAENOBUFS = 10055, WSAVERNOTSUPPORTED = 10092, WSANOTINITIALISED = 10093 };
_Static_assert(sizeof(struct nt_wsa_data) == 408, "WSADATA AMD64 layout");
_Static_assert(sizeof(struct nt_fd_set) == 520, "Winsock fd_set AMD64 layout");

struct socket_handle { uintptr_t id; int fd; };
static struct socket_handle sockets[SOCKET_COUNT];
static uintptr_t next_socket = 0x78000001;
static pthread_mutex_t socket_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t startup_count;
static _Thread_local int last_socket_error;

static int socket_error(int error)
{
    switch (error) {
    case EINTR: return 10004;
    case EACCES: case EPERM: return 10013;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return 10024;
    case EAGAIN: return 10035;
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case EBADF: case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return 10043;
    case ESOCKTNOSUPPORT: return 10044;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EPFNOSUPPORT: return 10046;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ENETRESET: return 10052;
    case ECONNABORTED: return 10053;
    case ECONNRESET: case EPIPE: return 10054;
    case ENOMEM: case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ESHUTDOWN: return 10058;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    default: return 10107; /* WSASYSCALLFAILURE */
    }
}

static int fail(int error)
{
    last_socket_error = error;
    return -1;
}

int NT_MS_ABI nt_WSAGetLastError(void) { return last_socket_error; }

int NT_MS_ABI nt_WSAStartup(uint16_t version, struct nt_wsa_data *data)
{
    if (data == NULL)
        return WSAEFAULT;
    const uint8_t major = (uint8_t)version, minor = (uint8_t)(version >> 8);
    if (major == 0 || major > 2)
        return WSAVERNOTSUPPORTED;
    pthread_mutex_lock(&socket_lock);
    const bool ok = startup_count != UINT32_MAX;
    if (ok)
        ++startup_count;
    pthread_mutex_unlock(&socket_lock);
    if (!ok)
        return WSAENOBUFS;
    memset(data, 0, sizeof(*data));
    data->version = major == 1 ? (minor == 0 ? 1 : 0x0101) : (minor <= 2 ? version : 0x0202);
    data->highest_version = 0x0202;
    strcpy(data->description, "Winsock on Linux sockets");
    strcpy(data->system_status, "Running");
    return 0;
}

static struct socket_handle *find_socket(uintptr_t id)
{
    for (size_t i = 0; i < SOCKET_COUNT; ++i)
        if (sockets[i].id == id)
            return &sockets[i];
    return NULL;
}

static uintptr_t new_socket(int fd)
{
    pthread_mutex_lock(&socket_lock);
    struct socket_handle *slot = find_socket(0);
    uintptr_t handle = UINTPTR_MAX;
    if (startup_count == 0) {
        fail(WSANOTINITIALISED);
    } else if (slot == NULL || next_socket == UINTPTR_MAX) {
        fail(WSAENOBUFS);
    } else {
        handle = next_socket++;
        *slot = (struct socket_handle){handle, fd};
    }
    pthread_mutex_unlock(&socket_lock);
    if (handle == UINTPTR_MAX)
        close(fd);
    return handle;
}

static int borrow_socket(uintptr_t handle)
{
    pthread_mutex_lock(&socket_lock);
    const struct socket_handle *socket = handle != 0 ? find_socket(handle) : NULL;
    int fd;
    if (startup_count == 0)
        fd = fail(WSANOTINITIALISED);
    else if (socket == NULL)
        fd = fail(WSAENOTSOCK);
    else {
        fd = fcntl(socket->fd, F_DUPFD_CLOEXEC, 3);
        if (fd < 0)
            fail(socket_error(errno));
    }
    pthread_mutex_unlock(&socket_lock);
    return fd;
}

static void retire_socket(int fd)
{
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
}

int NT_MS_ABI nt_WSACleanup(void)
{
    int closing[SOCKET_COUNT];
    size_t count = 0;
    pthread_mutex_lock(&socket_lock);
    if (startup_count == 0) {
        pthread_mutex_unlock(&socket_lock);
        return fail(WSANOTINITIALISED);
    }
    if (--startup_count == 0) {
        for (size_t i = 0; i < SOCKET_COUNT; ++i) {
            if (sockets[i].id != 0) {
                closing[count++] = sockets[i].fd;
                sockets[i].id = 0;
            }
        }
    }
    pthread_mutex_unlock(&socket_lock);
    for (size_t i = 0; i < count; ++i)
        retire_socket(closing[i]);
    return 0;
}

uintptr_t NT_MS_ABI nt_socket(int domain, int type, int protocol)
{
    pthread_mutex_lock(&socket_lock);
    const bool ready = startup_count != 0;
    pthread_mutex_unlock(&socket_lock);
    if (!ready) {
        fail(WSANOTINITIALISED);
        return UINTPTR_MAX;
    }
    const int family = domain == 2 ? AF_INET : domain == 23 ? AF_INET6 : -1;
    const int style = type == 1 ? SOCK_STREAM : type == 2 ? SOCK_DGRAM : -1;
    if (family < 0 || style < 0) {
        fail(family < 0 ? WSAEAFNOSUPPORT : 10044);
        return UINTPTR_MAX;
    }
    const int fd = socket(family, style | SOCK_CLOEXEC, protocol);
    if (fd < 0) {
        fail(socket_error(errno));
        return UINTPTR_MAX;
    }
    return new_socket(fd);
}

int NT_MS_ABI nt_closesocket(uintptr_t handle)
{
    pthread_mutex_lock(&socket_lock);
    struct socket_handle *socket = handle != 0 ? find_socket(handle) : NULL;
    const int error = startup_count == 0 ? WSANOTINITIALISED : socket == NULL ? WSAENOTSOCK : 0;
    const int fd = error == 0 ? socket->fd : -1;
    if (fd >= 0)
        socket->id = 0;
    pthread_mutex_unlock(&socket_lock);
    if (fd < 0)
        return fail(error);
    retire_socket(fd);
    return 0;
}

static int finish(int fd, int result)
{
    const int error = errno;
    close(fd);
    return result < 0 ? fail(socket_error(error)) : result;
}

static int decode_address(const void *address, int length, struct sockaddr_storage *native)
{
    if (address == NULL || length < 2)
        return fail(WSAEFAULT);
    uint16_t family;
    memcpy(&family, address, sizeof(family));
    const size_t size = family == 2 ? sizeof(struct sockaddr_in) :
                        family == 23 ? sizeof(struct sockaddr_in6) : 0;
    if (size == 0)
        return fail(WSAEAFNOSUPPORT);
    if ((size_t)length < size)
        return fail(WSAEFAULT);
    memset(native, 0, sizeof(*native));
    memcpy(native, address, size);
    native->ss_family = family == 2 ? AF_INET : AF_INET6;
    return (int)size;
}

static bool encode_address(void *address, int *length, struct sockaddr_storage *native, socklen_t size)
{
    if (*length < 0 || (uint32_t)*length < size) {
        fail(WSAEFAULT);
        return false;
    }
    native->ss_family = native->ss_family == AF_INET ? 2 : 23;
    memcpy(address, native, size);
    *length = (int)size;
    return true;
}

static int set_address(uintptr_t handle, const void *address, int length, bool connect_socket)
{
    struct sockaddr_storage native;
    const int size = decode_address(address, length, &native);
    if (size < 0)
        return -1;
    const int fd = borrow_socket(handle);
    if (fd < 0)
        return -1;
    const struct sockaddr *addr = (const struct sockaddr *)&native;
    const int status = connect_socket ? connect(fd, addr, (socklen_t)size) : bind(fd, addr, (socklen_t)size);
    if (connect_socket && status < 0 && errno == EINPROGRESS) {
        close(fd);
        return fail(10035);
    }
    return finish(fd, status);
}

int NT_MS_ABI nt_bind(uintptr_t handle, const void *address, int length)
{
    return set_address(handle, address, length, false);
}

int NT_MS_ABI nt_connect(uintptr_t handle, const void *address, int length)
{
    return set_address(handle, address, length, true);
}

int NT_MS_ABI nt_getsockname(uintptr_t handle, void *address, int *length)
{
    if (address == NULL || length == NULL)
        return fail(WSAEFAULT);
    const int fd = borrow_socket(handle);
    if (fd < 0)
        return -1;
    struct sockaddr_storage native;
    socklen_t size = sizeof(native);
    if (getsockname(fd, (struct sockaddr *)&native, &size) != 0)
        return finish(fd, -1);
    close(fd);
    return encode_address(address, length, &native, size) ? 0 : -1;
}

int NT_MS_ABI nt_listen(uintptr_t handle, int backlog)
{
    const int fd = borrow_socket(handle);
    return fd >= 0 ? finish(fd, listen(fd, backlog)) : -1;
}

uintptr_t NT_MS_ABI nt_accept(uintptr_t handle, void *address, int *length)
{
    if (address != NULL && (length == NULL || *length < 0)) {
        fail(WSAEFAULT);
        return UINTPTR_MAX;
    }
    const int fd = borrow_socket(handle);
    if (fd < 0)
        return UINTPTR_MAX;
    const int status = fcntl(fd, F_GETFL);
    struct sockaddr_storage native;
    socklen_t size = sizeof(native);
    const int client = status >= 0 ? accept4(fd, (struct sockaddr *)&native, &size,
        SOCK_CLOEXEC | ((status & O_NONBLOCK) != 0 ? SOCK_NONBLOCK : 0)) : -1;
    if (finish(fd, client) < 0)
        return UINTPTR_MAX;
    if (address != NULL && !encode_address(address, length, &native, size)) {
        close(client);
        return UINTPTR_MAX;
    }
    return new_socket(client);
}

static int transfer(uintptr_t handle, const void *input, void *output, int length, int flags, bool writing)
{
    if (length < 0 || (length != 0 && (writing ? input == NULL : output == NULL)))
        return fail(WSAEFAULT);
    const int allowed = writing ? 5 : 11;
    if ((flags & ~allowed) != 0)
        return fail(WSAEOPNOTSUPP);
    int native = (flags & 1) != 0 ? MSG_OOB : 0;
    if ((flags & 2) != 0)
        native |= MSG_PEEK;
    if ((flags & 4) != 0)
        native |= MSG_DONTROUTE;
    if ((flags & 8) != 0)
        native |= MSG_WAITALL;
    const int fd = borrow_socket(handle);
    if (fd < 0)
        return -1;
    const ssize_t result = writing ? send(fd, input, (size_t)length, native | MSG_NOSIGNAL) :
                                     recv(fd, output, (size_t)length, native);
    return finish(fd, (int)result);
}

int NT_MS_ABI nt_send(uintptr_t handle, const char *buffer, int length, int flags)
{
    return transfer(handle, buffer, NULL, length, flags, true);
}

int NT_MS_ABI nt_recv(uintptr_t handle, char *buffer, int length, int flags)
{
    return transfer(handle, NULL, buffer, length, flags, false);
}

int NT_MS_ABI nt_ioctlsocket(uintptr_t handle, int32_t command, uint32_t *value)
{
    if (value == NULL)
        return fail(WSAEFAULT);
    if ((uint32_t)command != UINT32_C(0x8004667e) && (uint32_t)command != UINT32_C(0x4004667f))
        return fail(WSAEINVAL);
    const int fd = borrow_socket(handle);
    if (fd < 0)
        return -1;
    int arg = *value != 0;
    const bool query = (uint32_t)command == UINT32_C(0x4004667f);
    const int result = ioctl(fd, query ? FIONREAD : FIONBIO, &arg);
    if (result == 0 && query)
        *value = (uint32_t)arg;
    return finish(fd, result);
}

int NT_MS_ABI nt_setsockopt(uintptr_t handle, int level, int option, const void *value, int length)
{
    if (value == NULL || length < 4)
        return fail(WSAEFAULT);
    int native_level = level, native_option = -1;
    int integer;
    memcpy(&integer, value, sizeof(integer));
    const void *data = &integer;
    socklen_t size = sizeof(integer);
    struct timeval timeout;
    struct linger linger;
    if (level == 0xffff) {
        native_level = SOL_SOCKET;
        switch (option) {
        case 0x0004: native_option = SO_REUSEADDR; break;
        case 0x0008: native_option = SO_KEEPALIVE; break;
        case 0x0010: native_option = SO_DONTROUTE; break;
        case 0x0020: native_option = SO_BROADCAST; break;
        case 0x0080: {
            uint16_t words[2];
            memcpy(words, value, sizeof(words));
            linger = (struct linger){words[0], words[1]};
            native_option = SO_LINGER;
            data = &linger;
            size = sizeof(linger);
            break;
        }
        case 0x0100: native_option = SO_OOBINLINE; break;
        case 0x1001: native_option = SO_SNDBUF; break;
        case 0x1002: native_option = SO_RCVBUF; break;
        case 0x1005: case 0x1006:
            if (integer < 0)
                return fail(WSAEINVAL);
            timeout = (struct timeval){integer / 1000, (integer % 1000) * 1000};
            native_option = option == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO;
            data = &timeout;
            size = sizeof(timeout);
            break;
        }
    } else if (level == IPPROTO_TCP && option == 1) {
        native_option = TCP_NODELAY;
    } else if (level == IPPROTO_IPV6 && option == 27) {
        native_option = IPV6_V6ONLY;
    }
    if (native_option < 0)
        return fail(WSAENOPROTOOPT);
    const int fd = borrow_socket(handle);
    return fd >= 0 ? finish(fd, setsockopt(fd, native_level, native_option, data, size)) : -1;
}

int NT_MS_ABI nt_select(int ignored, struct nt_fd_set *readable, struct nt_fd_set *writable,
                       struct nt_fd_set *exceptions, const struct nt_socket_time *timeout)
{
    (void)ignored;
    struct nt_fd_set *const sets[] = {readable, writable, exceptions};
    const short events[] = {POLLIN, POLLOUT, POLLPRI};
    const short ready[] = {POLLIN | POLLHUP | POLLERR, POLLOUT | POLLHUP | POLLERR, POLLPRI};
    if (timeout != NULL && (timeout->seconds < 0 || timeout->microseconds < 0 || timeout->microseconds >= 1000000))
        return fail(WSAEINVAL);
    struct pollfd fds[3 * NT_FD_SETSIZE];
    size_t count = 0;
    for (size_t group = 0; group < 3; ++group) {
        const struct nt_fd_set *set = sets[group];
        if (set == NULL)
            continue;
        if (set->count > NT_FD_SETSIZE) {
            fail(WSAEINVAL);
            goto failure;
        }
        for (size_t i = 0; i < set->count; ++i) {
            const int fd = borrow_socket(set->handles[i]);
            if (fd < 0)
                goto failure;
            fds[count++] = (struct pollfd){.fd = fd, .events = events[group]};
        }
    }
    if (count == 0)
        return fail(WSAEINVAL);
    const struct timespec delay = {timeout != NULL ? timeout->seconds : 0,
                                   timeout != NULL ? (long)timeout->microseconds * 1000 : 0};
    if (ppoll(fds, count, timeout != NULL ? &delay : NULL, NULL) < 0) {
        fail(socket_error(errno));
        goto failure;
    }
    size_t at = 0;
    int total = 0;
    for (size_t group = 0; group < 3; ++group) {
        struct nt_fd_set *set = sets[group];
        if (set == NULL)
            continue;
        uint32_t written = 0;
        for (size_t i = 0; i < set->count; ++i) {
            if ((fds[at].revents & ready[group]) != 0)
                set->handles[written++] = set->handles[i];
            close(fds[at++].fd);
        }
        set->count = written;
        total += (int)written;
    }
    return total;
failure:
    for (size_t i = 0; i < count; ++i)
        close(fds[i].fd);
    return -1;
}

uint32_t NT_MS_ABI nt_htonl(uint32_t value) { return htonl(value); }
uint16_t NT_MS_ABI nt_htons(uint16_t value) { return htons(value); }
