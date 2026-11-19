/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fs4_acl_storage.h"
#include "fs4_acl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"FAIL FS4 DACL storage line %d: %s errno=%d\n", \
            __LINE__,#expr,errno);exit(1);} } while(0)

static void w16(unsigned char *p,unsigned v)
{
    p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);
}
static void w32(unsigned char *p,uint32_t v)
{
    for (int i=0;i<4;i++)
        p[i]=(unsigned char)(v>>(i*8));
}
static void build_dacl(unsigned char descriptor[FS4_SD_LIMIT],size_t *length)
{
    memset(descriptor,0,FS4_SD_LIMIT);
    descriptor[0]=1;
    w16(descriptor+2,0x8004);
    w32(descriptor+4,20);
    w32(descriptor+8,36);
    w32(descriptor+16,52);
    fs4_acl_unix_sid(descriptor+20,1,1000);
    fs4_acl_unix_sid(descriptor+36,2,1000);
    descriptor[52]=2;
    w16(descriptor+54,28);
    w16(descriptor+56,1);
    descriptor[60]=FS4_ACE_ALLOW;
    descriptor[61]=FS4_ACE_OBJECT_INHERIT | FS4_ACE_CONTAINER_INHERIT;
    w16(descriptor+62,20);
    w32(descriptor+64,0x1u);
    const unsigned char everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
    memcpy(descriptor+68,everyone,sizeof(everyone));
    *length=80;
}
int main(void)
{
    char root[]="/tmp/fs4-dacl-storage-XXXXXX";
    CHECK(mkdtemp(root)!=NULL);
    int directory=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(directory>=0);
    int child=openat(directory,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);
    if (child<0 && (errno==EOPNOTSUPP || errno==EINVAL || errno==EISDIR)) {
        /* Staging is mandatory for full namespace integration; do not
         * substitute a named file and claim pre-publication testing. */
        puts("SKIP DACL O_TMPFILE inheritance: host backing does not support staging");
        close(directory);
        CHECK(rmdir(root)==0);
        return 0;
    }
    CHECK(child>=0);
    unsigned char parent[FS4_SD_LIMIT], observed[FS4_SD_LIMIT]={0};
    size_t n=0,needed=0;
    build_dacl(parent,&n);
    CHECK(fs4_acl_validate(parent,n));
    CHECK(fs4_acl_storage_read(directory,observed,sizeof(observed),&needed)
          ==FS4_ACL_ABSENT);
    CHECK(needed==0);
    CHECK(fs4_acl_storage_write_trusted(directory,parent,n));
    CHECK(fs4_acl_storage_read(directory,observed,4,&needed)==-1);
    CHECK(errno==ERANGE && needed==n);
    CHECK(fs4_acl_storage_read(directory,observed,sizeof(observed),&needed)
          ==FS4_ACL_PRESENT);
    CHECK(needed==n && memcmp(observed,parent,n)==0);
    bool inherited=false;
    CHECK(fs4_acl_storage_inherit_trusted(directory,child,500,600,false,
                                          &inherited));
    CHECK(inherited);
    size_t child_size=0;
    CHECK(fs4_acl_storage_read(child,observed,sizeof(observed),&child_size)
          ==FS4_ACL_PRESENT);
    CHECK(fs4_acl_validate(observed,child_size));
    unsigned char child_owner[16];
    fs4_acl_unix_sid(child_owner,1,500);
    CHECK(memcmp(observed+20,child_owner,16)==0);
    const unsigned char everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
    struct fs4_acl_token token={0};
    token.sid[0]=everyone;token.sid_length[0]=sizeof(everyone);token.count=1;
    CHECK(fs4_acl_check(observed,child_size,&token,0x1u));
    CHECK(linkat(child,"",directory,"published.dat",AT_EMPTY_PATH)==0);
    int reopened=openat(directory,"published.dat",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    CHECK(reopened>=0);
    memset(observed,0,sizeof(observed));
    CHECK(fs4_acl_storage_read(reopened,observed,sizeof(observed),&needed)
          ==FS4_ACL_PRESENT);
    CHECK(needed==child_size);
    close(reopened);

    /* Host-side corruption is not a permission grant. No silent default
     * projection or inherited ACL may be fabricated after malformed xattr. */
    CHECK(fsetxattr(directory,"user.ntnative.ntacl-v1","BROKEN",6,0)==0);
    CHECK(fsync(directory)==0);
    CHECK(fs4_acl_storage_read(directory,observed,sizeof(observed),&needed)==-1);
    CHECK(errno==EUCLEAN);
    inherited=true;
    CHECK(!fs4_acl_storage_inherit_trusted(directory,child,100,200,false,
                                           &inherited));
    CHECK(!inherited);
    CHECK(errno==EUCLEAN);
    CHECK(fsetxattr(directory,"user.ntnative.ntacl-v1",parent,n,0)==0);
    CHECK(fsync(directory)==0);
    CHECK(unlinkat(directory,"published.dat",0)==0);
    close(child);
    close(directory);
    CHECK(rmdir(root)==0);
    puts("PASS FS4 internal DACL xattr write/fsync, readback, pre-publication inheritance and EUCLEAN corruption");
    return 0;
}
