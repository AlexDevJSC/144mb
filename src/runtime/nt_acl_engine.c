/* SPDX-License-Identifier: MIT */
/* NTFS security descriptor parsing and ordered ACE evaluation, bounded and
 * independent of chmod. This subset deliberately refuses generic masks,
 * object ACE, audit/SACL, conditional/callback ACE and SID mapping.
 * No filesystem access decision uses this engine until token identity and
 * persisted xattr security descriptors are connected to the service UID.
 */
#include "nt_acl_engine.h"
#include <limits.h>
#include <string.h>

enum { SD_HEADER=20, SD_SELF_RELATIVE=0x8000, SD_DACL_PRESENT=4,
       SD_SACL_PRESENT=0x10, ACL_HEADER=8, ACE_BASE=8,
       ACE_ALLOW=0, ACE_DENY=1, ACE_INHERIT_OBJECT=1,
       ACE_INHERIT_CONTAINER=2, ACE_NO_PROPAGATE=4,
       ACE_INHERIT_ONLY=8, ACE_INHERITED=16,
       ACE_FLAGS_VALID=31 };
struct acl { const uint8_t *start; size_t size; unsigned count; bool null_dacl; };

static uint16_t get16(const uint8_t *b) {
    return (uint16_t)b[0] | (uint16_t)((uint16_t)b[1]<<8);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) |
           ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static void put16(uint8_t *b,uint16_t v) {
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8);
}
static bool sid_valid(const uint8_t *p,size_t size) {
    return size>=8 && p[0]==1 && p[1]<=15 &&
           size>=8u+(size_t)p[1]*4u;
}
static int sd_acl(const uint8_t *sd,size_t length,struct acl *acl) {
    if (!sd || length<SD_HEADER || sd[0]!=1 ||
        sd[1]!=0 || (get16(sd+2)&SD_SELF_RELATIVE)==0)
        return NT_ACL_INVALID;
    uint16_t control=get16(sd+2);
    if (control&SD_SACL_PRESENT)
        return NT_ACL_UNSUPPORTED;
    for (int i=0;i<2;i++) {
        uint32_t offset=get32(sd+4+(size_t)i*4u);
        if (offset && (offset%4u || offset>length ||
                       !sid_valid(sd+offset,length-offset)))
            return NT_ACL_INVALID;
    }
    /* A non-null SACL offset when SACL_PRESENT is clear is malformed. */
    if (get32(sd+12))
        return NT_ACL_INVALID;
    uint32_t dacl=get32(sd+16);
    if (!(control&SD_DACL_PRESENT)) {
        if (dacl)
            return NT_ACL_INVALID;
        *acl=(struct acl){.null_dacl=true};
        return NT_ACL_ALLOWED;
    }
    if (!dacl) {
        *acl=(struct acl){.null_dacl=true};
        return NT_ACL_ALLOWED; /* NULL DACL, as distinct from empty ACL */
    }
    if (dacl%4u || dacl<SD_HEADER || dacl>length ||
        length-dacl<ACL_HEADER)
        return NT_ACL_INVALID;
    const uint8_t *p=sd+dacl;
    if (p[0]!=2 || p[1]!=0 || get16(p+2)<ACL_HEADER ||
        get16(p+2)>length-dacl || get16(p+6)!=0)
        return NT_ACL_INVALID;
    size_t total=get16(p+2);
    size_t cursor=ACL_HEADER;
    unsigned count=get16(p+4);
    for (unsigned i=0;i<count;i++) {
        if (cursor>total || total-cursor<ACE_BASE)
            return NT_ACL_INVALID;
        const uint8_t *ace=p+cursor;
        size_t size=get16(ace+2);
        if (size<ACE_BASE+8 || (size&3u) || size>total-cursor)
            return NT_ACL_INVALID;
        if (ace[0]!=ACE_ALLOW && ace[0]!=ACE_DENY)
            return NT_ACL_UNSUPPORTED;
        if (ace[1]&~ACE_FLAGS_VALID)
            return NT_ACL_UNSUPPORTED;
        if (!sid_valid(ace+ACE_BASE,size-ACE_BASE) ||
            size!=(size_t)ACE_BASE+8u+(size_t)ace[ACE_BASE+1]*4u)
            return NT_ACL_INVALID;
        cursor+=size;
    }
    if (cursor!=total)
        return NT_ACL_INVALID;
    *acl=(struct acl){.start=p,.size=total,.count=count};
    return NT_ACL_ALLOWED;
}

/* A parsed self-relative SD is the minimum condition for persistence.
 * This does not authenticate its owner or prove an access grant. */
int nt_acl_validate_descriptor(const uint8_t *sd,size_t bytes) {
    struct acl acl={0};
    return sd_acl(sd,bytes,&acl);
}

static bool map_generic(uint32_t *rights,
                        const struct nt_acl_generic_mapping *mapping)
{
    const uint32_t all_generic=UINT32_C(0xf0000000);
    if (!(*rights&all_generic))
        return true;
    if (!mapping)
        return false;
    const uint32_t selected=*rights&all_generic;
    *rights&=~all_generic;
    if (selected&UINT32_C(0x80000000)) *rights|=mapping->read;
    if (selected&UINT32_C(0x40000000)) *rights|=mapping->write;
    if (selected&UINT32_C(0x20000000)) *rights|=mapping->execute;
    if (selected&UINT32_C(0x10000000)) *rights|=mapping->all;
    /* A caller must provide concrete rights, not nested GENERIC bits. */
    return !(*rights&all_generic);
}

static bool token_has_sid(const struct nt_acl_token *token,
                          const uint8_t *sid,size_t sid_size,
                          bool for_allow) {
    for (size_t i=0;i<token->count;i++)
        if ((!for_allow ||
             !(token->sid_flags[i] & NT_ACL_SID_DENY_ONLY)) &&
            token->sid[i] && token->length[i]==sid_size &&
            memcmp(token->sid[i],sid,sid_size)==0)
            return true;
    return false;
}

int nt_acl_access_check_mapped(const uint8_t *sd,size_t bytes,
                               const struct nt_acl_token *token,
                               uint32_t requested,
                               const struct nt_acl_generic_mapping *mapping,
                               uint32_t *granted) {
    if (granted) *granted=0;
    if (!token || token->count>32 || !granted)
        return NT_ACL_INVALID;
    /* Generic mapping is entirely object-specific, never implicit. */
    if (!map_generic(&requested,mapping))
        return NT_ACL_UNSUPPORTED;
    for (size_t i=0;i<token->count;i++)
        if (!token->sid[i] || !sid_valid(token->sid[i],token->length[i]) ||
            token->length[i]!=8u+(size_t)token->sid[i][1]*4u ||
            (token->sid_flags[i] & (uint8_t)~NT_ACL_SID_DENY_ONLY))
            return NT_ACL_INVALID;
    struct acl acl={0};
    int valid=sd_acl(sd,bytes,&acl);
    if (valid<0)
        return valid;
    if (acl.null_dacl) {
        *granted=requested;
        return NT_ACL_ALLOWED;
    }
    uint32_t remaining=requested;
    size_t pos=ACL_HEADER;
    for (unsigned i=0;i<acl.count;i++) {
        const uint8_t *ace=acl.start+pos;
        size_t length=get16(ace+2);
        uint32_t rights=get32(ace+4);
        if (!map_generic(&rights,mapping))
            return NT_ACL_UNSUPPORTED;
        if (!(ace[1]&ACE_INHERIT_ONLY) &&
            token_has_sid(token,ace+ACE_BASE,length-ACE_BASE,
                          ace[0]==ACE_ALLOW)) {
            if (ace[0]==ACE_DENY && (remaining & rights))
                return NT_ACL_DENIED;
            if (ace[0]==ACE_ALLOW)
                remaining &= ~rights;
        }
        pos+=length;
    }
    if (remaining)
        return NT_ACL_DENIED;
    *granted=requested;
    return NT_ACL_ALLOWED;
}

int nt_acl_access_check(const uint8_t *sd,size_t bytes,
                        const struct nt_acl_token *token,uint32_t requested,
                        uint32_t *granted) {
    return nt_acl_access_check_mapped(sd,bytes,token,requested,NULL,granted);
}

/* Copy inheritable access ACEs into an ACL_REVISION-2 buffer. It is NOT a
 * descriptor or a persistence operation. Object-only ACEs inherited by a
 * directory become inherit-only, so they can reach directory descendants
 * without incorrectly applying to the intermediate container. */
int nt_acl_inherit_dacl(const uint8_t *parent_sd,size_t bytes,
                        bool directory,uint8_t *out,size_t capacity,
                        size_t *needed) {
    if (!needed)
        return NT_ACL_INVALID;
    *needed=0;
    struct acl acl={0};
    int valid=sd_acl(parent_sd,bytes,&acl);
    if (valid<0)
        return valid;
    if (acl.null_dacl)
        return NT_ACL_UNSUPPORTED;
    size_t total=ACL_HEADER;
    unsigned inherited=0;
    size_t pos=ACL_HEADER;
    for (unsigned i=0;i<acl.count;i++) {
        const uint8_t *ace=acl.start+pos;
        const size_t size=get16(ace+2);
        bool apply=directory ?
            (ace[1]&(ACE_INHERIT_CONTAINER|ACE_INHERIT_OBJECT))!=0 :
            (ace[1]&ACE_INHERIT_OBJECT)!=0;
        if (apply) {
            if (size>UINT16_MAX-total)
                return NT_ACL_UNSUPPORTED;
            total+=size;
            ++inherited;
        }
        pos+=size;
    }
    *needed=total;
    if (!out || capacity<total)
        return NT_ACL_DENIED; /* output buffer too small, size reported */
    memset(out,0,total);
    out[0]=2; /* ACL_REVISION */
    put16(out+2,(uint16_t)total);
    put16(out+4,(uint16_t)inherited);
    pos=ACL_HEADER;
    size_t next=ACL_HEADER;
    for (unsigned i=0;i<acl.count;i++) {
        const uint8_t *ace=acl.start+pos;
        size_t size=get16(ace+2);
        uint8_t flags=ace[1];
        bool apply=directory ?
            (flags&(ACE_INHERIT_CONTAINER|ACE_INHERIT_OBJECT))!=0 :
            (flags&ACE_INHERIT_OBJECT)!=0;
        if (apply) {
            memcpy(out+next,ace,size);
            out[next+1]=flags|ACE_INHERITED;
            if (directory && !(flags&ACE_INHERIT_CONTAINER))
                out[next+1]|=ACE_INHERIT_ONLY;
            else
                out[next+1]&=(uint8_t)~ACE_INHERIT_ONLY;
            if (flags&ACE_NO_PROPAGATE)
                out[next+1]&=(uint8_t)~(ACE_INHERIT_OBJECT|
                                       ACE_INHERIT_CONTAINER|ACE_NO_PROPAGATE);
            else if (!directory)
                out[next+1]&=(uint8_t)~(ACE_INHERIT_OBJECT|
                                       ACE_INHERIT_CONTAINER|ACE_NO_PROPAGATE);
            next+=size;
        }
        pos+=size;
    }
    return NT_ACL_ALLOWED;
}
