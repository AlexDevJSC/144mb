/* SPDX-License-Identifier: MIT */
/* FileSystem4 v3 remote Win32 file HANDLE client.
 * The helper holding all real backing FDs runs as a dedicated storage UID.
 * Sender UID is verified from SCM_CREDENTIALS, NOT SO_PEERCRED on the
 * systemd-created listener. Failed session authentication poisons it.
 */
#define _GNU_SOURCE
#include "fs4_remote.h"
#include "compat.h"
#include "winfile.h"
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
       FS4_LOCK=8, FS4_UNLOCK=9, FS4_SEEK=10, FS4_SIZE=11,
       FS4_ATTR_GET=12, FS4_ATTR_SET=13, FS4_FIND_FIRST=14,
       FS4_FIND_NEXT=15, FS4_FIND_CLOSE=16, FS4_DUPLICATE=17,
       FS4_READ_AT=18, FS4_WRITE_AT=19, FS4_HANDLE_INFO=20,
       FS4_NATIVE_QUERY=21, FS4_NATIVE_SET=22, FS4_NATIVE_FLUSH=23,
       FS4_HARDLINK=24 };
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
struct remote_entry {
    uintptr_t guest;
    uint64_t cap;
    bool search, overlapped;
    uint32_t handle_flags; /* no cross-process inheritance yet */
    uintptr_t completion_port, completion_key;
};
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

bool nt_fs4_remote_valid(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    bool ok=nt_fs4_remote_handle(handle) && find_entry(handle)!=NULL;
    pthread_mutex_unlock(&guard);
    return ok;
}

bool nt_fs4_remote_search(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    const struct remote_entry *entry=find_entry(handle);
    bool found=entry && entry->search;
    pthread_mutex_unlock(&guard);
    return found;
}

bool nt_fs4_remote_overlapped(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    const struct remote_entry *entry=find_entry(handle);
    bool allowed=entry && !entry->search && entry->overlapped;
    pthread_mutex_unlock(&guard);
    return allowed;
}

int32_t nt_fs4_remote_get_flags(uintptr_t handle,uint32_t *flags)
{
    if(!flags) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return 0;}
    pthread_mutex_lock(&guard);
    const struct remote_entry *entry=find_entry(handle);
    bool ok=entry && !entry->search;
    if(ok) *flags=entry->handle_flags;
    pthread_mutex_unlock(&guard);
    if(!ok) nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return ok;
}
int32_t nt_fs4_remote_set_flags(uintptr_t handle,uint32_t mask,uint32_t flags)
{
    /* HANDLE_FLAG_INHERIT requires a sealed, authenticated IPC child ticket;
     * never fake successful remote inheritance with a host FD snapshot. */
    if((mask|flags)&~3u || (mask&1u && flags&1u)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    bool ok=entry && !entry->search;
    if(ok) entry->handle_flags=(entry->handle_flags&~mask)|(flags&mask);
    pthread_mutex_unlock(&guard);
    if(!ok) nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return ok;
}
int32_t nt_fs4_remote_associate_iocp(uintptr_t handle,
                                      uintptr_t port,uintptr_t key)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    bool ok=entry && !entry->search && entry->overlapped &&
            !entry->completion_port;
    if(ok) {
        entry->completion_port=port;
        entry->completion_key=key;
    }
    pthread_mutex_unlock(&guard);
    if(!ok) nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return ok;
}
int32_t nt_fs4_remote_completion(uintptr_t handle,
                                  uintptr_t *port,uintptr_t *key)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    bool ok=entry && !entry->search && entry->overlapped;
    if(ok) {
        if(port) *port=entry->completion_port;
        if(key) *key=entry->completion_key;
    }
    pthread_mutex_unlock(&guard);
    if(!ok) nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return ok;
}

size_t nt_fs4_remote_count(void)
{
    size_t count=0;
    pthread_mutex_lock(&guard);
    for(size_t i=0;i<FS4_LIMIT;i++)
        if(entries[i].cap && !entries[i].search)
            count++;
    pthread_mutex_unlock(&guard);
    return count;
}

static struct remote_entry *free_entry(void)
{
    for(size_t i=0;i<FS4_LIMIT;i++)
        if(!entries[i].cap)
            return &entries[i];
    return NULL;
}

static uintptr_t adopt_cap(struct remote_entry *entry,uint64_t cap,
                           bool search,bool overlapped)
{
    if(!entry || !cap || next_guest>=UINT32_MAX) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINTPTR_MAX;
    }
    const uintptr_t guest=(uintptr_t)(UINT64_C(0xf44e000000000000) |
                                        (next_guest++ << 4));
    *entry=(struct remote_entry){.guest=guest,.cap=cap,
                                 .search=search,.overlapped=overlapped};
    return guest;
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
    if (length>FS4_MAX ||
        (size>FS4_MAX && operation!=FS4_LOCK && operation!=FS4_UNLOCK)) {
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
    struct remote_entry *slot=free_entry();
    if (!slot) {
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINTPTR_MAX;
    }
    struct reply response;
    const bool overlapped=(flags & UINT32_C(0x40000000))!=0;
    /* Server owns an ordinary synchronous Win32 handle; async completion
     * is implemented on the guest side over positional IPC. */
    bool ok=request(FS4_OPEN,0,-1,access,share,disposition,
                    flags & ~UINT32_C(0x40000000),
                    path,(uint32_t)n,(uint32_t)n,&response,NULL,0);
    uintptr_t handle=UINTPTR_MAX;
    if (ok && response.handle && next_guest<UINT32_MAX) {
        handle=adopt_cap(slot,response.handle,false,overlapped);
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
    if (entry->handle_flags & 2u) { /* HANDLE_FLAG_PROTECT_FROM_CLOSE */
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
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

int32_t nt_fs4_remote_hardlink(const char *source, const char *dest)
{
    size_t a=0,b=0;
    if (!checked_path(source,&a) || !checked_path(dest,&b))
        return 0;
    /* guest_path() strips the C: prefix for its private grant; the broker
     * intentionally requires explicit C:/ names and rejects bare paths.
     * Reintroduce ONLY this proven private-root prefix on the wire. */
    if (a+3>1024 || b+3>1024 || a+b+7>FS4_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_NAME);
        return 0;
    }
    unsigned char data[FS4_MAX];
    memcpy(data,"C:/",3);
    memcpy(data+3,source,a);
    data[a+3]=0;
    memcpy(data+a+4,"C:/",3);
    memcpy(data+a+7,dest,b);
    const uint32_t length=(uint32_t)(a+b+7);
    pthread_mutex_lock(&guard);
    struct reply response;
    const bool ok=request(FS4_HARDLINK,0,-1,0,0,0,0,
                          data,length,length,&response,NULL,0);
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
    if (offset>INT64_MAX || !length || length>INT64_MAX ||
        length>INT64_MAX-offset) {
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

/* All methods take the same process-local mutex as read/write/close; a
 * search cursor and any file HANDLE cannot race an IPC close or reuse. */
uintptr_t nt_fs4_remote_duplicate(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *original=find_entry(handle), *slot=free_entry();
    if (!original || original->search || !slot) {
        nt_SetLastError(original ? NT_ERROR_NOT_ENOUGH_MEMORY : NT_ERROR_INVALID_HANDLE);
        pthread_mutex_unlock(&guard);
        return UINTPTR_MAX;
    }
    const bool overlapped=original->overlapped;
    struct reply result={0};
    bool ok=request(FS4_DUPLICATE,original->cap,-1,0,0,0,2,NULL,0,0,
                    &result,NULL,0);
    uintptr_t new_handle=UINTPTR_MAX;
    if(ok && result.handle) {
        new_handle=adopt_cap(slot,result.handle,false,overlapped);
        if(new_handle!=UINTPTR_MAX) {
            slot->completion_port=original->completion_port;
            slot->completion_key=original->completion_key;
        }
    }
    pthread_mutex_unlock(&guard);
    return new_handle;
}

static bool path_request(uint16_t operation,const char *path,uint32_t flags,
                         uint32_t access,uint32_t share,struct reply *reply,
                         void *output,size_t capacity)
{
    size_t n=0;
    if(!checked_path(path,&n))
        return false;
    pthread_mutex_lock(&guard);
    bool ok=request(operation,0,-1,access,share,0,flags,path,(uint32_t)n,
                    (uint32_t)n,reply,output,capacity);
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_get_attributes(const char *path,
                                     struct nt_file_attribute_data *info)
{
    if(!info) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return 0;}
    struct reply reply={0};
    struct nt_file_attribute_data result={0};
    if(!path_request(FS4_ATTR_GET,path,0,0,0,&reply,&result,sizeof(result)))
        return 0;
    if(reply.length!=sizeof(result)) {
        nt_SetLastError(NT_ERROR_BROKEN_PIPE);
        return 0;
    }
    *info=result;
    return 1;
}

int32_t nt_fs4_remote_set_attributes(const char *path,uint32_t flags)
{
    struct reply reply={0};
    return path_request(FS4_ATTR_SET,path,flags,0,0,&reply,NULL,0);
}

uintptr_t nt_fs4_remote_find_first(const char *path,int32_t level,
                                   int32_t operation,uint32_t flags,
                                   struct nt_find_data_w *info)
{
    if(!info) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return UINTPTR_MAX;}
    size_t n=0;
    if(!checked_path(path,&n))
        return UINTPTR_MAX;
    pthread_mutex_lock(&guard);
    struct remote_entry *slot=free_entry();
    if(!slot) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        pthread_mutex_unlock(&guard);
        return UINTPTR_MAX;
    }
    struct nt_find_data_w result={0};
    struct reply reply={0};
    bool ok=request(FS4_FIND_FIRST,0,-1,(uint32_t)level,(uint32_t)operation,
                    0,flags,path,(uint32_t)n,(uint32_t)n,&reply,
                    (unsigned char *)&result,sizeof(result));
    uintptr_t handle=UINTPTR_MAX;
    if(ok && reply.length==sizeof(result) && reply.handle) {
        handle=adopt_cap(slot,reply.handle,true,false);
        if(handle!=UINTPTR_MAX)
            *info=result;
    } else if(ok) {
        nt_SetLastError(NT_ERROR_BROKEN_PIPE);
    }
    pthread_mutex_unlock(&guard);
    return handle;
}

int32_t nt_fs4_remote_find_next(uintptr_t handle,struct nt_find_data_w *info)
{
    if(!info) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return 0;}
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if(!entry || !entry->search) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        pthread_mutex_unlock(&guard);
        return 0;
    }
    struct nt_find_data_w result={0};
    struct reply reply={0};
    bool ok=request(FS4_FIND_NEXT,entry->cap,-1,0,0,0,0,NULL,0,0,
                    &reply,(unsigned char *)&result,sizeof(result));
    if(ok && reply.length==sizeof(result))
        *info=result;
    else {
        if(ok)
            nt_SetLastError(NT_ERROR_BROKEN_PIPE);
        ok=false;
    }
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_find_close(uintptr_t handle)
{
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if(!entry || !entry->search) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        pthread_mutex_unlock(&guard);
        return 0;
    }
    struct reply reply={0};
    bool ok=request(FS4_FIND_CLOSE,entry->cap,-1,0,0,0,0,NULL,0,0,
                    &reply,NULL,0);
    *entry=(struct remote_entry){0};
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_handle_info(uintptr_t handle,
                                  struct nt_handle_file_info *info)
{
    if(!info) {nt_SetLastError(NT_ERROR_INVALID_PARAMETER);return 0;}
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if(!entry || entry->search) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        pthread_mutex_unlock(&guard);
        return 0;
    }
    struct nt_handle_file_info result={0};
    struct reply reply={0};
    bool ok=request(FS4_HANDLE_INFO,entry->cap,-1,0,0,0,0,NULL,0,0,
                    &reply,(unsigned char *)&result,sizeof(result));
    if(ok && reply.length==sizeof(result))
        *info=result;
    else {
        if(ok)
            nt_SetLastError(NT_ERROR_BROKEN_PIPE);
        ok=false;
    }
    pthread_mutex_unlock(&guard);
    return ok;
}

int32_t nt_fs4_remote_io_at(uintptr_t handle,void *data,uint32_t length,
                            uint32_t *done,bool write,uint64_t offset)
{
    if(done) *done=0;
    if(!done || (length && !data) || offset>INT64_MAX ||
       (uint64_t)length>INT64_MAX-offset) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if(!entry || entry->search) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        pthread_mutex_unlock(&guard);
        return 0;
    }
    bool ok=true;
    uint32_t total=0;
    do {
        uint32_t amount=length-total;
        if(amount>FS4_MAX) amount=FS4_MAX;
        struct reply reply={0};
        unsigned char *at=data ? (unsigned char *)data+total : NULL;
        ok=request(write ? FS4_WRITE_AT : FS4_READ_AT,entry->cap,
                   (int64_t)(offset+total),0,0,0,0,
                   write ? at : NULL,write ? amount : 0,
                   amount,&reply,write ? NULL : at,write ? 0 : amount);
        if(!ok) break;
        if(reply.value>amount || (!write && reply.length!=reply.value)) {
            poisoned=true;
            nt_SetLastError(NT_ERROR_BROKEN_PIPE);
            ok=false;break;
        }
        total+=(uint32_t)reply.value;
        if(reply.value<amount || length==0)
            break;
    } while(total<length);
    *done=total;
    pthread_mutex_unlock(&guard);
    return ok;
}

static nt_status native_transfer(uint16_t op,uintptr_t handle,
                                  const void *input,uint32_t input_length,
                                  void *output,uint32_t output_capacity,
                                  uint32_t kind,uint32_t *received)
{
    if(received) *received=0;
    pthread_mutex_lock(&guard);
    struct remote_entry *entry=find_entry(handle);
    if(!entry || entry->search) {
        pthread_mutex_unlock(&guard);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return NT_STATUS_INVALID_HANDLE;
    }
    struct reply reply={0};
    bool ok=request(op,entry->cap,-1,kind,0,0,0,input,input_length,
                    op==FS4_NATIVE_QUERY ? output_capacity : input_length,
                    &reply,output,output_capacity);
    if(!ok) {
        pthread_mutex_unlock(&guard);
        return NT_STATUS_UNSUCCESSFUL;
    }
    if(received) *received=reply.length;
    const nt_status status=(nt_status)(uint32_t)reply.value;
    pthread_mutex_unlock(&guard);
    return status;
}
nt_status nt_fs4_remote_native_query(uintptr_t handle,void *buffer,
                                    uint32_t capacity,uint32_t kind,
                                    uint32_t *received)
{
    if(!buffer || (kind!=5 && kind!=14)) return NT_STATUS_INVALID_PARAMETER;
    return native_transfer(FS4_NATIVE_QUERY,handle,NULL,0,buffer,
                           capacity,kind,received);
}
nt_status nt_fs4_remote_native_set(uintptr_t handle,const void *buffer,
                                  uint32_t size,uint32_t kind)
{
    if(!buffer || (kind==13 ? size!=1 :
                   (kind==14 || kind==20) ? size!=8 : true))
        return NT_STATUS_INVALID_PARAMETER;
    return native_transfer(FS4_NATIVE_SET,handle,buffer,size,NULL,0,kind,NULL);
}
nt_status nt_fs4_remote_native_flush(uintptr_t handle)
{
    return native_transfer(FS4_NATIVE_FLUSH,handle,NULL,0,NULL,0,0,NULL);
}
