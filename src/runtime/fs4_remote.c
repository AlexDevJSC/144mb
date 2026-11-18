/* SPDX-License-Identifier: MIT */
/* FileSystem4 v3 remote Win32 file HANDLE client.
 * The helper holding all real backing FDs runs as a dedicated storage UID.
 * Sender UID is verified from SCM_CREDENTIALS, NOT SO_PEERCRED on the
 * systemd-created listener. Failed session authentication poisons it.
 */
#define _GNU_SOURCE
#include "fs4_remote.h"
#include "compat.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

enum { FS4_MAGIC=0x34465348u, FS4_VERSION=3, FS4_MAX=2048,
       FS4_LIMIT=32, FS4_OPEN=1, FS4_READ=2, FS4_WRITE=3,
       FS4_CLOSE=4, FS4_DELETE=5, FS4_RENAME=6, FS4_TRUNCATE=7,
       FS4_LOCK=8, FS4_UNLOCK=9, FS4_SEEK=10, FS4_SIZE=11 };
struct request {
    uint32_t magic;
    uint16_t version, op;
    uint64_t handle;
    int64_t offset;
    uint32_t access, share, disposition, flags, size, reserved;
    uint64_t nonce;
};
struct reply {
    uint32_t magic;
    uint16_t version, op;
    uint32_t error, length;
    uint64_t handle, value;
};
_Static_assert(sizeof(struct request)==56, "FS4 client ABI request");
_Static_assert(sizeof(struct reply)==32, "FS4 client ABI reply");
struct remote_entry { uintptr_t guest; uint64_t cap; };
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t determine=PTHREAD_ONCE_INIT;
static char endpoint[sizeof(((struct sockaddr_un *)0)->sun_path)];
static uid_t expected_uid;
static bool configured, poisoned;
static int socket_fd=-1;
static uint64_t nonce, next_guest=1;
static pid_t owner_pid;
static struct remote_entry entries[FS4_LIMIT];

static void detect_mode(void)
{
    const char *path=getenv("NT_FS4_HANDLE_SOCKET");
    if (!path || !*path)
        return;
    configured=true; /* failure still forbids any POSIX fallback */
    const char *uid=getenv("NT_FS4_SERVICE_UID");
    char *end=NULL;
    long number=uid ? strtol(uid,&end,10) : -1;
    if (!uid || !end || *end || number<=0 || number>INT_MAX ||
        (uid_t)number==getuid() || path[0]!='/' ||
        strlen(path)>=sizeof(endpoint)) {
        poisoned=true;
        return;
    }
    memcpy(endpoint,path,strlen(path)+1);
    expected_uid=(uid_t)number;
    owner_pid=getpid();
}

bool nt_fs4_remote_mode(void)
{
    (void)pthread_once(&determine,detect_mode);
    return configured;
}
bool nt_fs4_remote_handle(uintptr_t handle)
{
    return nt_fs4_remote_mode() &&
           ((uint64_t)handle & UINT64_C(0xffff000000000000)) ==
                               UINT64_C(0xf44e000000000000);
}

static struct remote_entry *find_entry(uintptr_t guest)
{
    for (size_t i=0;i<FS4_LIMIT;i++)
        if (entries[i].guest==guest && entries[i].cap)
            return &entries[i];
    return NULL;
}

static bool receive_reply(struct reply *value, unsigned char *data,
                          size_t capacity, size_t *received)
{
    unsigned char scratch[FS4_MAX];
    struct iovec iov[2]={{value,sizeof(*value)},{scratch,sizeof(scratch)}};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))];
    } control;
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=2,
                       .msg_control=control.bytes,
                       .msg_controllen=sizeof(control.bytes)};
    ssize_t count;
    do { count=recvmsg(socket_fd,&msg,0); }
    while (count<0 && errno==EINTR);
    if (count<(ssize_t)sizeof(*value) ||
        msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC) ||
        count!=(ssize_t)(sizeof(*value)+value->length) ||
        value->length>capacity || value->length>FS4_MAX)
        return false;
    unsigned credentials=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))
        if (c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
            c->cmsg_len>=CMSG_LEN(sizeof(struct ucred))) {
            const struct ucred *peer=(const struct ucred *)CMSG_DATA(c);
            if (peer->uid==expected_uid && peer->uid!=0)
                ++credentials;
        }
    if (credentials!=1)
        return false;
    if (value->length && data)
        memcpy(data,scratch,value->length);
    if (received)
        *received=value->length;
    return true;
}

static bool attach(void)
{
    if (!nt_fs4_remote_mode() || poisoned || getpid()!=owner_pid)
        return false;
    if (socket_fd>=0)
        return true;
    struct stat st;
    if (lstat(endpoint,&st)!=0 || !S_ISSOCK(st.st_mode))
        return false;
    socket_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (socket_fd<0)
        return false;
    int yes=1;
    if (setsockopt(socket_fd,SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes))!=0)
        return false;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    memcpy(address.sun_path,endpoint,strlen(endpoint)+1);
    if (connect(socket_fd,(struct sockaddr *)&address,sizeof(address))!=0)
        return false;
    const char hello[]="{\"version\":3,\"op\":\"session\"}";
    if (send(socket_fd,hello,sizeof(hello)-1,MSG_NOSIGNAL)!=(ssize_t)(sizeof(hello)-1))
        return false;
    struct reply answer;
    size_t payload=0;
    if (!receive_reply(&answer,NULL,0,&payload) ||
        answer.magic!=FS4_MAGIC || answer.version!=FS4_VERSION ||
        answer.op!=0 || answer.error || answer.length || !answer.value)
        return false;
    nonce=answer.value;
    return true;
}

static bool request(uint16_t operation,uint64_t handle,int64_t offset,
                    uint32_t access,uint32_t share,uint32_t disposition,
                    uint32_t flags,const void *data,uint32_t length,
                    uint32_t size,struct reply *result,
                    unsigned char *output,size_t capacity)
{
    if (poisoned || !attach()) {
        poisoned=true;
        nt_SetLastError(NT_ERROR_BROKEN_PIPE);
        return false;
    }
    if (length>FS4_MAX || size>FS4_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    struct request q={.magic=FS4_MAGIC,.version=FS4_VERSION,.op=operation,
                      .handle=handle,.offset=offset,.access=access,
                      .share=share,.disposition=disposition,.flags=flags,
                      .size=size,.nonce=nonce};
    struct iovec iov[2]={{&q,sizeof(q)},{(void *)data,length}};
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=length ? 2u : 1u};
    ssize_t sent=sendmsg(socket_fd,&msg,MSG_NOSIGNAL);
    size_t got=0;
    if (sent!=(ssize_t)(sizeof(q)+length) ||
        !receive_reply(result,output,capacity,&got) ||
        result->magic!=FS4_MAGIC || result->version!=FS4_VERSION ||
        result->op!=operation || result->length!=got) {
        poisoned=true;
        nt_SetLastError(NT_ERROR_BROKEN_PIPE);
        return false;
    }
    if (result->error) {
        nt_SetLastError(result->error);
        return false;
    }
    return true;
}

static bool checked_path(const char *path,size_t *n)
{
    if (!path) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    *n=strnlen(path,1025);
    if (*n==0 || *n>1024 || *n>FS4_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_NAME);
        return false;
    }
    return true;
}

uintptr_t nt_fs4_remote_open(const char *path,uint32_t access,uint32_t share,
                             uint32_t disposition,uint32_t flags)
{
    size_t n=0;
    if (!checked_path(path,&n))
        return UINTPTR_MAX;
    pthread_mutex_lock(&guard);
    struct remote_entry *slot=NULL;
    for (size_t i=0;i<FS4_LIMIT;i++)
        if (!entries[i].cap && !slot)
            slot=&entries[i];
    if (!slot) {
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINTPTR_MAX;
    }
    struct reply response;
    bool ok=request(FS4_OPEN,0,-1,access,share,disposition,flags,
                    path,(uint32_t)n,(uint32_t)n,&response,NULL,0);
    uintptr_t handle=UINTPTR_MAX;
    if (ok && response.handle && next_guest<UINT32_MAX) {
        handle=(uintptr_t)(UINT64_C(0xf44e000000000000) | (next_guest++ << 4));
        slot->cap=response.handle;
        slot->guest=handle;
    } else if (ok) {
        struct reply ignored;
        (void)request(FS4_CLOSE,response.handle,-1,0,0,0,0,
                      NULL,0,0,&ignored,NULL,0);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    }
    pthread_mutex_unlock(&guard);
    return handle;
}

int32_t nt_fs4_remote_close(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if (!entry) {
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    struct reply response;
    bool ok=request(FS4_CLOSE,entry->cap,-1,0,0,0,0,
                    NULL,0,0,&response,NULL,0);
    entry->cap=0;
    entry->guest=0;
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_io(uintptr_t handle,void *data,uint32_t length,
                         uint32_t *done,bool writing)
{
    if (done)
        *done=0;
    if ((length && !data) || !done) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if (!entry) {
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    bool ok=true;
    uint32_t total=0;
    while (total<length || (length==0 && total==0)) {
        uint32_t amount=length-total;
        if (amount>FS4_MAX)
            amount=FS4_MAX;
        struct reply response;
        unsigned char *buffer=data ? (unsigned char *)data+total : NULL;
        ok=request(writing?FS4_WRITE:FS4_READ,entry->cap,-1,0,0,0,0,
                   writing ? buffer : NULL,writing?amount:0,amount,
                   &response,writing?NULL:buffer,writing?0:amount);
        if (!ok)
            break;
        if (response.value>amount) {
            poisoned=true;
            nt_SetLastError(NT_ERROR_BROKEN_PIPE);
            ok=false;
            break;
        }
        total+=(uint32_t)response.value;
        if (length==0 || response.value<amount)
            break;
    }
    *done=total;
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_delete(const char *path)
{
    size_t length=0;
    if (!checked_path(path,&length))
        return 0;
    pthread_mutex_lock(&guard);
    struct reply response;
    bool ok=request(FS4_DELETE,0,-1,0,0,0,0,path,(uint32_t)length,
                    (uint32_t)length,&response,NULL,0);
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_rename(const char *source,const char *dest,uint32_t flags)
{
    size_t a=0,b=0;
    if (!checked_path(source,&a) || !checked_path(dest,&b))
        return 0;
    if (a+b+1>FS4_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_NAME);
        return 0;
    }
    unsigned char data[FS4_MAX];
    memcpy(data,source,a);
    data[a]=0;
    memcpy(data+a+1,dest,b);
    pthread_mutex_lock(&guard);
    struct reply response;
    bool ok=request(FS4_RENAME,0,-1,0,0,0,flags,data,(uint32_t)(a+b+1),
                    (uint32_t)(a+b+1),&response,NULL,0);
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_seek(uintptr_t handle,int64_t offset,uint32_t method,
                           int64_t *position)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    struct reply response;
    bool ok=entry && request(FS4_SEEK,entry->cap,offset,0,0,0,method,
                             NULL,0,0,&response,NULL,0);
    if (!entry)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    if (ok && position)
        *position=(int64_t)response.value;
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_size(uintptr_t handle,int64_t *size)
{
    if (!size) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return 0;}
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    struct reply response;
    bool ok=entry && request(FS4_SIZE,entry->cap,-1,0,0,0,0,
                             NULL,0,0,&response,NULL,0);
    if (!entry)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    if (ok) *size=(int64_t)response.value;
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_truncate(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    struct reply response;
    bool ok=entry && request(FS4_TRUNCATE,entry->cap,-1,0,0,0,0,
                             NULL,0,0,&response,NULL,0);
    if (!entry)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_lock(uintptr_t handle,uint64_t offset,uint64_t length,
                           bool unlock)
{
    if (offset>INT64_MAX || !length) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    struct reply response;
    bool ok=entry && request(unlock?FS4_UNLOCK:FS4_LOCK,entry->cap,
                             (int64_t)offset,0,0,0,(uint32_t)(length>>32),
                             NULL,0,(uint32_t)length,&response,NULL,0);
    if (!entry)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    pthread_mutex_unlock(&guard);
    return ok;
}
