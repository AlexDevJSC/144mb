/* SPDX-License-Identifier: MIT */
/* Private inode-backed SD record regression; not a test of Win32 ACL
 * authorization or end-to-end permission enforcement. */
#define _GNU_SOURCE
#include "nt_acl_storage.h"
#include "nt_acl_engine.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static void put16(uint8_t *p,uint16_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);
}
static void put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)
        p[i]=(uint8_t)(v>>(8*i));
}
static void sd_allow_everyone(uint8_t sd[48]) {
    memset(sd,0,48);
    sd[0]=1;
    put16(sd+2,0x8004);
    put32(sd+16,20);
    sd[20]=2;
    put16(sd+22,28);
    put16(sd+24,1);
    sd[28]=0; /* ACCESS_ALLOWED_ACE */
    put16(sd+30,20);
    put32(sd+32,1);
    sd[36]=1;sd[37]=1;sd[43]=1; /* S-1-1-0 */
}
int main(void) {
    char root[]="/tmp/nt144-acl-store-XXXXXX";
    assert(mkdtemp(root)!=NULL);
    char file[512],second[512];
    assert(snprintf(file,sizeof(file),"%s/file",root) <
           (int)sizeof(file));
    assert(snprintf(second,sizeof(second),"%s/second",root) <
           (int)sizeof(second));
    int fd=open(file,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    assert(fd>=0);
    int foreign=open(second,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    assert(foreign>=0);
    uint8_t sd[48],received[64];
    sd_allow_everyone(sd);
    assert(nt_acl_validate_descriptor(sd,sizeof(sd))==NT_ACL_ALLOWED);
    size_t needed=1234;
    assert(nt_acl_storage_read_fd(fd,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_ABSENT && needed==0);
    assert(nt_acl_storage_write_trusted_fd(fd,sd,sizeof(sd)));
    assert(nt_acl_storage_read_fd(fd,NULL,0,&needed)==
           NT_ACL_STORAGE_SHORT_BUFFER && needed==48);
    assert(nt_acl_storage_read_fd(fd,received,47,&needed)==
           NT_ACL_STORAGE_SHORT_BUFFER && needed==48);
    assert(nt_acl_storage_read_fd(fd,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_PRESENT && needed==48);
    assert(memcmp(received,sd,48)==0);
    int reopened=open(file,O_RDONLY|O_CLOEXEC);
    assert(reopened>=0);
    assert(nt_acl_storage_read_fd(reopened,received,48,&needed)==
           NT_ACL_STORAGE_PRESENT);
    close(reopened);
    /* Same xattr bytes on another inode have a DIFFERENT identity. */
    ssize_t stored=fgetxattr(fd,"user.ntnative.security.sd",NULL,0);
    assert(stored>0 && stored<4096);
    uint8_t record[4096];
    assert(fgetxattr(fd,"user.ntnative.security.sd",record,
                     sizeof(record))==stored);
    assert(fsetxattr(foreign,"user.ntnative.security.sd",record,
                     (size_t)stored,0)==0);
    assert(nt_acl_storage_read_fd(foreign,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_ERROR && errno==EUCLEAN);
    assert(!nt_acl_storage_write_trusted_fd(foreign,sd,sizeof(sd)) &&
           errno==EUCLEAN);
    /* A changed hash must be rejected, not projected as a wider DACL. */
    record[stored-1]^=0x40;
    assert(fsetxattr(fd,"user.ntnative.security.sd",record,
                     (size_t)stored,0)==0);
    assert(nt_acl_storage_read_fd(fd,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_ERROR && errno==EUCLEAN);
    assert(!nt_acl_storage_write_trusted_fd(fd,sd,sizeof(sd)) &&
           errno==EUCLEAN);
    /* Host-admin repair in this isolated test ONLY; guest clients cannot
     * edit user.ntnative.* through FUSE. */
    assert(fremovexattr(fd,"user.ntnative.security.sd")==0);
    assert(nt_acl_storage_write_trusted_fd(fd,sd,sizeof(sd)));
    assert(nt_acl_storage_read_fd(fd,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_PRESENT);
    sd[28]=9; /* unsupported ACE may never become a stored ACL */
    assert(!nt_acl_storage_write_trusted_fd(fd,sd,sizeof(sd)) &&
           errno==EINVAL);
    assert(nt_acl_storage_read_fd(fd,received,sizeof(received),&needed)==
           NT_ACL_STORAGE_PRESENT);
    close(foreign);
    close(fd);
    assert(unlink(second)==0);
    assert(unlink(file)==0);
    assert(rmdir(root)==0);
    puts("PASS NT SD inode xattr: bounded validation, persistence, identity and fail-closed corruption");
    return 0;
}
