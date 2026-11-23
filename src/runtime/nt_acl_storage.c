/* SPDX-License-Identifier: MIT */
/* Pinned-inode NT ACL persistence. This is an internal storage primitive,
 * NOT a permission grant: only a higher-level trusted service can call
 * write after authenticating WRITE_DAC and acquiring its namespace WAL.
 *
 * A checksummed xattr detects interrupted/corrupted records but offers
 * no cryptographic authenticity against a writer with physical backing
 * access. Backing DAC must remain owned by the dedicated storage UID.
 */
#define _GNU_SOURCE
#include "nt_acl_storage.h"
#include "nt_acl_engine.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define NT_ACL_XATTR "user.ntnative.security.sd"
#define FS4_LEGACY_ACL_XATTR "user.ntnative.ntacl-v1"
enum { HEADER_BYTES = 40 };
static const uint8_t magic[8] = {'N','T','S','D','0','0','0','1'};

static void store32(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4;i++)
        p[i]=(uint8_t)(value>>(8u*i));
}
static void store64(uint8_t *p,uint64_t value) {
    for(unsigned i=0;i<8;i++)
        p[i]=(uint8_t)(value>>(8u*i));
}
static uint32_t load32(const uint8_t *p) {
    uint32_t v=0;
    for(unsigned i=0;i<4;i++)
        v|=(uint32_t)p[i]<<(8u*i);
    return v;
}
static uint64_t load64(const uint8_t *p) {
    uint64_t v=0;
    for(unsigned i=0;i<8;i++)
        v|=(uint64_t)p[i]<<(8u*i);
    return v;
}
static uint64_t hash_bytes(uint64_t hash,const uint8_t *p,size_t length) {
    for(size_t i=0;i<length;i++) {
        hash^=p[i];
        hash*=UINT64_C(1099511628211);
    }
    return hash;
}
static uint64_t record_hash(const uint8_t *p,size_t length) {
    uint64_t hash=UINT64_C(14695981039346656037);
    hash=hash_bytes(hash,p+8,24);
    return hash_bytes(hash,p+HEADER_BYTES,length-HEADER_BYTES);
}
static bool valid_inode(int fd,struct stat *st) {
    if (fstat(fd,st)!=0)
        return false;
    if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) {
        errno=EINVAL;
        return false;
    }
    return true;
}
/* O_PATH pins the exact inode without exercising its host read DAC.
 * Linux fgetxattr(O_PATH) returns EBADF; /proc/self/fd/N remains bound to
 * the same file description (not a new guest-controlled path). */
static ssize_t read_acl_xattr(int fd,const char *name,void *out,size_t capacity)
{
    ssize_t result=fgetxattr(fd,name,out,capacity);
    if (result<0 && errno==EBADF) {
        char path[64];
        const int count=snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
        if (count<=0 || (size_t)count>=sizeof(path)) {
            errno=ENAMETOOLONG;return -1;
        }
        result=getxattr(path,name,out,capacity);
    }
    return result;
}

int nt_acl_storage_read_fd(int fd,uint8_t *out,size_t capacity,
                           size_t *needed)
{
    if (!needed || fd<0) {errno=EINVAL;return NT_ACL_STORAGE_ERROR;}
    *needed=0;
    struct stat st;
    if (!valid_inode(fd,&st)) {
        if (!errno) errno=EINVAL;
        return NT_ACL_STORAGE_ERROR;
    }
    /* The retired pre-v3 engine stored a DIFFERENT, unchecksummed
     * descriptor. Never silently ignore it as ABSENT or prefer whichever
     * xattr happened to be read first. An audited migration must first
     * validate and reconcile old records; until then fail closed. */
    const ssize_t old=read_acl_xattr(fd,FS4_LEGACY_ACL_XATTR,NULL,0);
    if (old>=0) {
        errno=EUCLEAN;
        return NT_ACL_STORAGE_ERROR;
    }
    if (errno!=ENODATA && errno!=ENOTSUP && errno!=EOPNOTSUPP)
        return NT_ACL_STORAGE_ERROR;
    ssize_t size=read_acl_xattr(fd,NT_ACL_XATTR,NULL,0);
    if (size<0) {
        if (errno==ENODATA)
            return NT_ACL_STORAGE_ABSENT;
        return NT_ACL_STORAGE_ERROR;
    }
    if ((size_t)size<HEADER_BYTES+20 ||
        (size_t)size>HEADER_BYTES+NT_ACL_SD_MAX_BYTES) {
        errno=EUCLEAN;
        return NT_ACL_STORAGE_ERROR;
    }
    uint8_t *record=malloc((size_t)size);
    if (!record) {errno=ENOMEM;return NT_ACL_STORAGE_ERROR;}
    const ssize_t got=read_acl_xattr(fd,NT_ACL_XATTR,record,(size_t)size);
    int result=NT_ACL_STORAGE_ERROR;
    if (got!=size || memcmp(record,magic,sizeof(magic))!=0 ||
        load32(record+8)!=(uint32_t)(size-HEADER_BYTES) ||
        load32(record+12)!=0 ||
        load64(record+16)!=(uint64_t)st.st_dev ||
        load64(record+24)!=(uint64_t)st.st_ino ||
        load64(record+32)!=record_hash(record,(size_t)size) ||
        nt_acl_validate_descriptor(record+HEADER_BYTES,
                                   (size_t)size-HEADER_BYTES)!=NT_ACL_ALLOWED ||
        /* A NULL/absent DACL means unrestricted access on Windows. The
         * service intentionally never persists an unbounded grant. */
        (record[HEADER_BYTES+2]&4u)==0 ||
        load32(record+HEADER_BYTES+16)==0) {
        errno=EUCLEAN; /* malformed, substituted inode, or unknown ACE */
    } else {
        *needed=(size_t)size-HEADER_BYTES;
        if (!out || capacity<*needed) {
            result=NT_ACL_STORAGE_SHORT_BUFFER;
            errno=ERANGE;
        } else {
            memcpy(out,record+HEADER_BYTES,*needed);
            result=NT_ACL_STORAGE_PRESENT;
        }
    }
    free(record);
    return result;
}
bool nt_acl_storage_write_trusted_fd(int fd,const uint8_t *descriptor,
                                     size_t bytes)
{
    if (fd<0 || !descriptor || bytes<20 ||
        bytes>NT_ACL_SD_MAX_BYTES ||
        nt_acl_validate_descriptor(descriptor,bytes)!=NT_ACL_ALLOWED ||
        (descriptor[2]&4u)==0 ||
        load32(descriptor+16)==0) {
        errno=EINVAL;
        return false;
    }
    struct stat st;
    if (!valid_inode(fd,&st)) {
        if (!errno) errno=EINVAL;
        return false;
    }
    /* Corrupt existing SD state must block replacement until explicitly
     * repaired by a separate, audited recovery operation. */
    size_t existing=0;
    int before=nt_acl_storage_read_fd(fd,NULL,0,&existing);
    if (before==NT_ACL_STORAGE_ERROR)
        return false;
    uint8_t *record=malloc(HEADER_BYTES+bytes);
    if (!record) {errno=ENOMEM;return false;}
    memcpy(record,magic,sizeof(magic));
    store32(record+8,(uint32_t)bytes);
    store32(record+12,0);
    store64(record+16,(uint64_t)st.st_dev);
    store64(record+24,(uint64_t)st.st_ino);
    memcpy(record+HEADER_BYTES,descriptor,bytes);
    store64(record+32,record_hash(record,HEADER_BYTES+bytes));
    bool success=fsetxattr(fd,NT_ACL_XATTR,record,
                            HEADER_BYTES+bytes,0)==0;
    int error=errno;
    if (success) {
        success=fsync(fd)==0;
        error=errno;
    }
    if (success) {
        /* Verify the pinned inode did not change and no concurrent
         * host writer swapped the metadata. A caller still MUST hold
         * its WAL/mutation gate over this entire invocation. */
        uint8_t *check=malloc(bytes);
        if (!check) {
            success=false;
            error=ENOMEM;
        } else {
            size_t found=0;
            const int status=nt_acl_storage_read_fd(fd,check,bytes,&found);
            success=status==NT_ACL_STORAGE_PRESENT &&
                    found==bytes && memcmp(check,descriptor,bytes)==0;
            if (!success) error=EUCLEAN;
            free(check);
        }
    }
    free(record);
    if (!success) errno=error ? error : EIO;
    return success;
}
