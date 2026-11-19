/* SPDX-License-Identifier: MIT */
/* Real Linux xattr + pinned-root ACL gate. No fake guest credentials and
 * no claim that this unit test proves complete PE/FUSE enforcement. */
#define _GNU_SOURCE
#include "nt_acl_policy.h"
#include "nt_acl_storage.h"
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

static void put16(uint8_t *p,uint16_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static void owner_sid(uint8_t *sid,uint32_t user) {
    memset(sid,0,16);sid[0]=1;sid[1]=2;sid[7]=22;
    put32(sid+8,1);put32(sid+12,user);
}
static void group_sid(uint8_t *sid,uint32_t group) {
    memset(sid,0,16);sid[0]=1;sid[1]=2;sid[7]=22;
    put32(sid+8,2);put32(sid+12,group);
}
static const uint8_t everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
static size_t ace(uint8_t *p,uint8_t type,uint32_t access,
                  const uint8_t *sid,size_t size)
{
    p[0]=type;p[1]=0;
    put16(p+2,(uint16_t)(8+size));
    put32(p+4,access);
    memcpy(p+8,sid,size);
    return 8+size;
}
static size_t fixture_sd(uint8_t sd[160],uint32_t uid,uint32_t deny_group)
{
    uint8_t owner[16],group[16];
    owner_sid(owner,uid);group_sid(group,deny_group);
    memset(sd,0,160);
    sd[0]=1;put16(sd+2,0x8004);
    put32(sd+4,20);put32(sd+8,36);put32(sd+16,52);
    memcpy(sd+20,owner,16);memcpy(sd+36,group,16);
    sd[52]=2;put16(sd+56,3);
    size_t next=60;
    next+=ace(sd+next,1,NT_ACL_DATA_WRITE,group,16);
    next+=ace(sd+next,0,NT_ACL_DATA_READ|NT_ACL_DATA_WRITE|
                             NT_ACL_WRITE_DAC,owner,16);
    next+=ace(sd+next,0,NT_ACL_DATA_READ,everyone,12);
    put16(sd+54,(uint16_t)(next-52));
    return next;
}
int main(void)
{
    char temp[]="/tmp/nt144-acl-policy-XXXXXX";
    assert(mkdtemp(temp));
    char folder[512],file[512],alias[512],linkname[512];
    assert(snprintf(folder,sizeof(folder),"%s/nested",temp)<(int)sizeof(folder));
    assert(snprintf(file,sizeof(file),"%s/nested/data",temp)<(int)sizeof(file));
    assert(snprintf(alias,sizeof(alias),"%s/nested/hardlink",temp)<(int)sizeof(alias));
    assert(snprintf(linkname,sizeof(linkname),"%s/nested/symlink",temp)<(int)sizeof(linkname));
    assert(mkdir(folder,0700)==0);
    int root=open(temp,O_PATH|O_DIRECTORY|O_CLOEXEC);
    int file_fd=open(file,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    assert(root>=0 && file_fd>=0);
    uint8_t sd[160];
    size_t size=fixture_sd(sd,1001,3001);
    assert(nt_acl_storage_write_trusted_fd(file_fd,sd,size));
    gid_t groups[2]={3001,4001};
    struct nt_acl_peer_token alice={0},bob={0},supp={0};
    assert(nt_acl_peer_token_init(&alice,1001,2001,NULL,0,NULL)==0);
    assert(nt_acl_peer_token_init(&bob,1002,2002,NULL,0,NULL)==0);
    assert(nt_acl_peer_token_init(&supp,1002,2002,groups,2,NULL)==0);
    assert(alice.token.count==3 && supp.token.count==5);
    assert(nt_acl_policy_check_fd(file_fd,&alice,NT_ACL_DATA_WRITE,
                                   &nt_acl_file_generic_mapping)==NT_ACL_POLICY_ALLOWED);
    assert(nt_acl_policy_check_fd(file_fd,&bob,NT_ACL_DATA_WRITE,
                                   &nt_acl_file_generic_mapping)==NT_ACL_POLICY_DENIED);
    assert(nt_acl_policy_check_fd(file_fd,&bob,NT_ACL_DATA_READ,
                                   &nt_acl_file_generic_mapping)==NT_ACL_POLICY_ALLOWED);
    assert(nt_acl_policy_check_fd(file_fd,&supp,NT_ACL_DATA_WRITE,
                                   &nt_acl_file_generic_mapping)==NT_ACL_POLICY_DENIED);
    assert(nt_acl_policy_check_path(root,"/nested/data",false,&alice,
        NT_ACL_DATA_WRITE,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ALLOWED);
    assert(nt_acl_policy_check_path(root,"C:/nested\\data",false,&bob,
        NT_ACL_DATA_WRITE,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_DENIED);
    /* The parent lacks an NT SD: a caller must fall back to the existing
     * DAC/share/namespace checks, never equate absence to an ACL grant. */
    assert(nt_acl_policy_check_path(root,"C:/nested/data",true,&alice,
        NT_ACL_DATA_WRITE,&nt_acl_directory_generic_mapping)==NT_ACL_POLICY_ABSENT);
    assert(nt_acl_policy_check_path(root,"C:/nested/../data",false,&alice,
        NT_ACL_DATA_READ,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ERROR);
    assert(nt_acl_policy_check_path(root,"C:/nested//data",false,&alice,
        NT_ACL_DATA_READ,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ERROR);
    assert(nt_acl_policy_check_path(root,"C:/nested/data/",false,&alice,
        NT_ACL_DATA_READ,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ERROR);
    /* A hardlink shares the same inode, therefore the same SD. */
    assert(link(file,alias)==0);
    assert(nt_acl_policy_check_path(root,"C:/nested/hardlink",false,&bob,
        NT_ACL_DATA_WRITE,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_DENIED);
    assert(symlink("data",linkname)==0);
    assert(nt_acl_policy_check_path(root,"C:/nested/symlink",false,&alice,
        NT_ACL_DATA_READ,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ERROR);
    uint8_t tiny[128]={0};
    ssize_t stored=fgetxattr(file_fd,"user.ntnative.security.sd",tiny,sizeof(tiny));
    assert(stored>0);
    tiny[stored-1]^=0x80;
    assert(fsetxattr(file_fd,"user.ntnative.security.sd",tiny,(size_t)stored,0)==0);
    assert(nt_acl_policy_check_path(root,"C:/nested/data",false,&alice,
        NT_ACL_DATA_READ,&nt_acl_file_generic_mapping)==NT_ACL_POLICY_ERROR);
    /* Malformed data and caller-provided deny-only group flags fail closed. */
    uint8_t denyflags[2]={0,2};
    assert(nt_acl_peer_token_init(&supp,1002,2002,groups,2,denyflags)==-1);
    assert(nt_acl_peer_token_init(&supp,1002,2002,groups,30,NULL)==-1);
    assert(fremovexattr(file_fd,"user.ntnative.security.sd")==0);
    assert(nt_acl_policy_check_fd(file_fd,&alice,NT_ACL_DATA_WRITE,
         &nt_acl_file_generic_mapping)==NT_ACL_POLICY_ABSENT);
    close(file_fd);close(root);
    assert(unlink(linkname)==0);assert(unlink(alias)==0);assert(unlink(file)==0);
    assert(rmdir(folder)==0);assert(rmdir(temp)==0);
    puts("PASS authenticated UID/group DACL access, inode/alias, path confinement and corruption");
    return 0;
}
