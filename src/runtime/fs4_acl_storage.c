/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fs4_acl_storage.h"
#include "fs4_acl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define FS4_ACL_XATTR "user.ntnative.ntacl-v1"

/* No raw descriptor bytes escape into guest memory before validation.
 * A corrupt xattr cannot become an accidental default-open DACL. */
int fs4_acl_storage_read(int fd, void *out, size_t capacity, size_t *needed)
{
    if (fd<0 || !needed) {
        errno=EINVAL;
        return -1;
    }
    unsigned char checked[FS4_SD_LIMIT];
    ssize_t n;
    do {
        n=fgetxattr(fd,FS4_ACL_XATTR,checked,sizeof(checked));
    } while (n<0 && errno==EINTR);
    if (n<0) {
        if (errno==ENODATA) {
            *needed=0;
            return FS4_ACL_ABSENT;
        }
        if (errno==ERANGE)
            errno=EUCLEAN; /* oversized descriptor: never downgrade */
        return -1;
    }
    if (!fs4_acl_validate(checked,(size_t)n)) {
        errno=EUCLEAN;
        return -1;
    }
    *needed=(size_t)n;
    if (!out || capacity<(size_t)n) {
        errno=ERANGE;
        return -1;
    }
    memcpy(out,checked,(size_t)n);
    return FS4_ACL_PRESENT;
}

bool fs4_acl_storage_write_trusted(int fd,const void *blob,size_t size)
{
    if (fd<0 || !fs4_acl_validate(blob,size))
        return false;
    struct stat info;
    if (fstat(fd,&info)!=0)
        return false;
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        errno=EOPNOTSUPP;
        return false;
    }
    /* Atomic Linux replacement of one xattr: no intermediate partially
     * written descriptor is exposed. fsync is REQUIRED before success.
     * A crash between setxattr and fsync can still have unknown durability. */
    if (fsetxattr(fd,FS4_ACL_XATTR,blob,size,0)!=0)
        return false;
    return fsync(fd)==0;
}

bool fs4_acl_storage_inherit_trusted(int parent_fd,int child_fd,
                                    uint32_t owner_uid,uint32_t group_gid,
                                    bool child_directory,bool *inherited)
{
    if (!inherited || parent_fd<0 || child_fd<0) {
        errno=EINVAL;
        return false;
    }
    *inherited=false;
    unsigned char parent[FS4_SD_LIMIT], child[FS4_SD_LIMIT];
    size_t length=0;
    const int state=fs4_acl_storage_read(parent_fd,parent,sizeof(parent),
                                         &length);
    if (state<0)
        return false;
    if (state==FS4_ACL_ABSENT)
        return true;
    size_t child_size=0;
    if (!fs4_acl_inherit(parent,length,owner_uid,group_gid,
                         child_directory,child,sizeof(child),&child_size))
        return false;
    if (!fs4_acl_storage_write_trusted(child_fd,child,child_size))
        return false;
    *inherited=true;
    return true;
}
