/* SPDX-License-Identifier: MIT */
/* A bounded authorization adapter for already-authenticated Unix peers.
 * This is not a Windows logon or an impersonation implementation.
 * No write of ACLs, no bypass on malformed security descriptors.
 */
#define _GNU_SOURCE
#include "nt_acl_policy.h"
#include "nt_acl_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Windows FILE_GENERIC_READ/WRITE/EXECUTE and FILE_ALL_ACCESS. */
const struct nt_acl_generic_mapping nt_acl_file_generic_mapping={
    .read=UINT32_C(0x00120089),.write=UINT32_C(0x00120116),
    .execute=UINT32_C(0x001200a0),.all=UINT32_C(0x001f01ff)
};
const struct nt_acl_generic_mapping nt_acl_directory_generic_mapping={
    .read=UINT32_C(0x00120089),.write=UINT32_C(0x00120116),
    .execute=UINT32_C(0x001200a0),.all=UINT32_C(0x001f01ff)
};
static void put32(uint8_t *p,uint32_t x) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8u*i));
}
static void add_unix_sid(struct nt_acl_peer_token *out,size_t index,
                         uint32_t type,uint32_t value) {
    uint8_t *sid=out->sid_storage[index];
    sid[0]=1;sid[1]=2;sid[7]=22;
    put32(sid+8,type);
    put32(sid+12,value);
    out->token.sid[index]=sid;
    out->token.length[index]=16;
}
int nt_acl_peer_token_init(struct nt_acl_peer_token *out,
                           uid_t authenticated_uid,gid_t authenticated_gid,
                           const gid_t *supplementary,size_t group_count,
                           const uint8_t *deny_only)
{
    /* Up to 29 distinct supplementary groups plus uid/gid/Everyone.
     * Truncating groups could accidentally omit a DENY principal. */
    if (!out || group_count>29 || (group_count && !supplementary)) {
        errno=EINVAL;
        return -1;
    }
    memset(out,0,sizeof(*out));
    add_unix_sid(out,0,1,(uint32_t)authenticated_uid);
    add_unix_sid(out,1,2,(uint32_t)authenticated_gid);
    size_t next=2;
    for (size_t i=0;i<group_count;i++) {
        if (deny_only && (deny_only[i]&~NT_ACL_SID_DENY_ONLY)) {
            errno=EINVAL;
            memset(out,0,sizeof(*out));
            return -1;
        }
        const uint32_t group=(uint32_t)supplementary[i];
        size_t duplicate=next;
        for (size_t j=1;j<next;j++) {
            uint8_t *sid=out->sid_storage[j];
            if (sid[7]==22 && sid[1]==2 && sid[8]==2 &&
                sid[9]==0 && sid[10]==0 && sid[11]==0 &&
                sid[12]==(uint8_t)group && sid[13]==(uint8_t)(group>>8) &&
                sid[14]==(uint8_t)(group>>16) && sid[15]==(uint8_t)(group>>24)) {
                duplicate=j;break;
            }
        }
        if (duplicate<next) {
            /* A duplicate group must not silently change an authenticated
             * primary/earlier group from enabled to deny-only (or vice
             * versa). Conflicting attributes have no canonical identity. */
            const uint8_t flag=deny_only ? deny_only[i] : 0u;
            if (out->token.sid_flags[duplicate]!=flag) {
                errno=EINVAL;
                memset(out,0,sizeof(*out));
                return -1;
            }
            continue;
        }
        if (next>=31) {
            errno=E2BIG;
            memset(out,0,sizeof(*out));
            return -1;
        }
        add_unix_sid(out,next,2,group);
        out->token.sid_flags[next]=deny_only ? deny_only[i] : 0;
        next++;
    }
    /* S-1-1-0 = Everyone, not a wildcard that accepts unknown SIDs. */
    uint8_t *everyone=out->sid_storage[next];
    everyone[0]=1;
    everyone[1]=1;
    everyone[7]=1;
    out->token.sid[next]=everyone;
    out->token.length[next]=12;
    out->token.count=next+1;
    out->privileges=0;
    return 0;
}

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
/* FILE object GenericMapping is selected by the trusted caller.  The
 * authenticated owner retains the implicit NT READ_CONTROL/WRITE_DAC
 * rights even if the DACL does not explicitly grant them. */
static int requested_concrete(uint32_t *mask,
                              const struct nt_acl_generic_mapping *mapping)
{
    const uint32_t generic=*mask&UINT32_C(0xf0000000);
    *mask&=~UINT32_C(0xf0000000);
    if (generic&UINT32_C(0x80000000)) *mask|=mapping->read;
    if (generic&UINT32_C(0x40000000)) *mask|=mapping->write;
    if (generic&UINT32_C(0x20000000)) *mask|=mapping->execute;
    if (generic&UINT32_C(0x10000000)) *mask|=mapping->all;
    return (*mask&UINT32_C(0xf0000000)) ? -1 : 0;
}
static bool is_authenticated_owner(const uint8_t *sd,size_t length,
                                   const struct nt_acl_peer_token *peer)
{
    if (length<20 || !peer->token.count || !peer->token.sid[0])
        return false;
    const size_t offset=little32(sd+4);
    if (!offset || offset>=length)
        return false;
    const size_t bytes=8u+(size_t)sd[offset+1]*4u;
    return bytes==peer->token.length[0] &&
           bytes<=length-offset &&
           memcmp(sd+offset,peer->token.sid[0],bytes)==0;
}

int nt_acl_policy_check_fd(int fd,const struct nt_acl_peer_token *peer,
                           uint32_t requested,
                           const struct nt_acl_generic_mapping *mapping)
{
    if (!peer || peer->token.count<3 || peer->token.count>32 ||
        fd<0 || !mapping) {
        errno=EINVAL;return NT_ACL_POLICY_ERROR;
    }
    uint8_t *descriptor=malloc(NT_ACL_SD_MAX_BYTES);
    if (!descriptor) {errno=ENOMEM;return NT_ACL_POLICY_ERROR;}
    size_t needed=0;
    int status=nt_acl_storage_read_fd(fd,descriptor,NT_ACL_SD_MAX_BYTES,&needed);
    if (status==NT_ACL_STORAGE_ABSENT) {
        free(descriptor);return NT_ACL_POLICY_ABSENT;
    }
    if (status!=NT_ACL_STORAGE_PRESENT) {
        int saved=errno;
        free(descriptor);
        errno=saved;
        return NT_ACL_POLICY_ERROR;
    }
    uint32_t concrete=requested;
    if (requested_concrete(&concrete,mapping)!=0) {
        free(descriptor);
        errno=ENOTSUP;
        return NT_ACL_POLICY_ERROR;
    }
    const uint32_t implicit=is_authenticated_owner(
        descriptor,needed,peer) ?
        NT_ACL_READ_CONTROL|NT_ACL_WRITE_DAC : 0u;
    const uint32_t remaining=concrete & ~implicit;
    uint32_t granted=0;
    const int result=remaining ?
        nt_acl_access_check_mapped(descriptor,needed,&peer->token,
                                    remaining,mapping,&granted) :
        NT_ACL_ALLOWED;
    free(descriptor);
    if (result==NT_ACL_ALLOWED && (!remaining || granted==remaining))
        return NT_ACL_POLICY_ALLOWED;
    if (result==NT_ACL_DENIED) {errno=EACCES;return NT_ACL_POLICY_DENIED;}
    errno=result==NT_ACL_UNSUPPORTED ? ENOTSUP : EUCLEAN;
    return NT_ACL_POLICY_ERROR;
}

static int confined_policy_fd(int root_fd,const char *path,bool parent)
{
    if (!path || root_fd<0) {errno=EINVAL;return -1;}
    const char *s=path;
    if (s[0]=='C' && s[1]==':' && s[2]=='/') s+=3;
    else if (*s=='/') s++;
    else {errno=EINVAL;return -1;}
    char safe[PATH_MAX];
    const size_t n=strnlen(s,sizeof(safe));
    if (n>=sizeof(safe)) {errno=ENAMETOOLONG;return -1;}
    size_t used=0;
    for (size_t i=0;i<n;) {
        const size_t start=i;
        while (i<n && s[i]!='/' && s[i]!='\\') {
            const unsigned char ch=(unsigned char)s[i];
            if (ch<32 || ch==127 || ch==':') {
                errno=EINVAL;return -1;
            }
            i++;
        }
        const size_t width=i-start;
        if (!width || (width==1 && s[start]=='.') ||
            (width==2 && s[start]=='.' && s[start+1]=='.')) {
            errno=EINVAL;return -1;
        }
        if (used) {
            if (used+1>=sizeof(safe)) {errno=ENAMETOOLONG;return -1;}
            safe[used++]='/';
        }
        if (used+width>=sizeof(safe)) {
            errno=ENAMETOOLONG;return -1;
        }
        memcpy(safe+used,s+start,width);
        used+=width;
        if (i<n) {
            i++;
            if (i==n) {errno=EINVAL;return -1;} /* trailing separator */
        }
    }
    safe[used]=0;
    if (parent && used) {
        char *slash=strrchr(safe,'/');
        if (slash) *slash=0;
        else used=0;
    }
    const char *relative=(parent && !used) || !safe[0] ? "." : safe;
    struct open_how how={
        .flags=O_PATH|O_CLOEXEC|O_NOFOLLOW,
        .resolve=RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV
    };
    return (int)syscall(SYS_openat2,root_fd,relative,&how,sizeof(how));
}

int nt_acl_policy_check_path(int root_fd,const char *path,bool parent,
                             const struct nt_acl_peer_token *peer,
                             uint32_t requested,
                             const struct nt_acl_generic_mapping *mapping)
{
    const int fd=confined_policy_fd(root_fd,path,parent);
    if (fd<0) return NT_ACL_POLICY_ERROR;
    const int allowed=nt_acl_policy_check_fd(fd,peer,requested,mapping);
    const int error=errno;
    close(fd);
    errno=error;
    return allowed;
}
