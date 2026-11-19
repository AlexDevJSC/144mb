/* SPDX-License-Identifier: MIT */
#include "fs4_acl.h"

#include <errno.h>
#include <string.h>

enum {
    SD_HEADER = 20,
    SD_SELF_RELATIVE = 0x8000,
    SD_DACL_PRESENT = 0x0004,
    SD_DACL_AUTO_INHERITED = 0x0400,
    SD_SACL_PRESENT = 0x0010,
    ACL_HEADER = 8,
    ACE_HEADER = 8
};

struct acl_view {
    const unsigned char *bytes;
    size_t length, owner, group, dacl, dacl_end;
    uint16_t ace_count;
};

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}
static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (8u * i));
}
static bool region(size_t offset, size_t amount, size_t limit)
{
    return offset <= limit && amount <= limit - offset;
}
static bool nonoverlap(size_t a, size_t alen, size_t b, size_t blen)
{
    return a + alen <= b || b + blen <= a;
}
static size_t valid_sid(const unsigned char *p, size_t available)
{
    if (available < 8 || p[0] != 1 || p[1] > 15)
        return 0;
    const size_t span = 8u + (size_t)p[1] * 4u;
    return span <= available ? span : 0;
}

/* The supported ACL subset is deliberately conservative: allow/deny
 * ACEs with standard SID tails only. Object, callback, conditional,
 * resource and opaque ACEs need a Windows token/claim/object model.
 * Never silently ignore a type that could deny current rights. */
static bool parse(const void *blob, size_t length, struct acl_view *v)
{
    if (!blob || length < SD_HEADER || length > FS4_SD_LIMIT) {
        errno = EINVAL;
        return false;
    }
    const unsigned char *bytes = blob;
    const uint16_t control = le16(bytes + 2);
    if (bytes[0] != 1 || bytes[1] != 0 ||
        (control & (SD_SELF_RELATIVE | SD_DACL_PRESENT)) !=
                  (SD_SELF_RELATIVE | SD_DACL_PRESENT) ||
        (control & SD_SACL_PRESENT) || le32(bytes + 12) != 0) {
        errno = ENOTSUP;
        return false;
    }
    const size_t owner=le32(bytes + 4), group=le32(bytes + 8);
    const size_t dacl=le32(bytes + 16);
    if (!owner || !group || !dacl ||
        (owner % 4u) || (group % 4u) || (dacl % 4u) ||
        owner < SD_HEADER || group < SD_HEADER || dacl < SD_HEADER ||
        !region(owner, 8, length) ||
        !region(group, 8, length) ||
        !region(dacl, ACL_HEADER, length)) {
        errno = EINVAL; /* NULL DACL is not an acceptable grant. */
        return false;
    }
    const size_t olen=valid_sid(bytes + owner, length - owner);
    const size_t glen=valid_sid(bytes + group, length - group);
    const uint16_t acl_size=le16(bytes + dacl + 2);
    const uint16_t count=le16(bytes + dacl + 4);
    if (!olen || !glen || acl_size < ACL_HEADER ||
        !region(dacl, acl_size, length) || count > FS4_MAX_ACES ||
        (bytes[dacl] != 2 && bytes[dacl] != 4) ||
        bytes[dacl + 1] != 0 ||
        bytes[dacl + 6] != 0 || bytes[dacl + 7] != 0 ||
        !nonoverlap(owner,olen,group,glen) ||
        !nonoverlap(owner,olen,dacl,acl_size) ||
        !nonoverlap(group,glen,dacl,acl_size)) {
        errno = EINVAL;
        return false;
    }
    size_t pos=dacl + ACL_HEADER;
    const size_t end=dacl + acl_size;
    for (uint16_t i=0;i<count;i++) {
        if (!region(pos, ACE_HEADER, end)) {
            errno = EINVAL;
            return false;
        }
        const uint16_t ace_length=le16(bytes + pos + 2);
        if ((bytes[pos] != FS4_ACE_ALLOW && bytes[pos] != FS4_ACE_DENY) ||
            (bytes[pos + 1] & ~31u) ||
            ace_length < ACE_HEADER + 8u ||
            (ace_length & 3u) || !region(pos,ace_length,end)) {
            errno = bytes[pos] > FS4_ACE_DENY ? ENOTSUP : EINVAL;
            return false;
        }
        const size_t sid_len=valid_sid(bytes + pos + ACE_HEADER,
                                        ace_length - ACE_HEADER);
        if (!sid_len || sid_len + ACE_HEADER != ace_length) {
            errno = EINVAL;
            return false;
        }
        pos+=ace_length;
    }
    if (pos != end) {
        errno = EINVAL; /* No hidden padding/undocumented ACEs. */
        return false;
    }
    if (v) {
        *v=(struct acl_view){.bytes=bytes,.length=length,
                             .owner=owner,.group=group,
                             .dacl=dacl,.dacl_end=end,.ace_count=count};
    }
    return true;
}

bool fs4_acl_validate(const void *descriptor, size_t length)
{
    return parse(descriptor,length,NULL);
}

static bool subject_matches(const struct fs4_acl_token *token,
                            const unsigned char *sid, size_t sid_len)
{
    for (size_t i=0;i<token->count;i++)
        if (token->sid_length[i]==sid_len && token->sid[i] &&
            memcmp(token->sid[i],sid,sid_len)==0)
            return true;
    return false;
}

bool fs4_acl_check(const void *descriptor, size_t length,
                   const struct fs4_acl_token *token, uint32_t rights)
{
    struct acl_view acl;
    if (!token || token->count > FS4_TOKEN_SIDS || !rights) {
        errno = EINVAL;
        return false;
    }
    if (!parse(descriptor,length,&acl))
        return false;
    const unsigned char *d=acl.bytes;
    size_t pos=acl.dacl + ACL_HEADER;
    uint32_t remaining=rights;
    for (unsigned i=0;i<acl.ace_count;i++) {
        const uint16_t span=le16(d + pos + 2);
        const uint8_t flags=d[pos + 1], type=d[pos];
        const uint32_t mask=le32(d + pos + 4);
        const bool matched=!(flags & FS4_ACE_INHERIT_ONLY) &&
            subject_matches(token,d + pos + ACE_HEADER,span - ACE_HEADER);
        if (matched && type==FS4_ACE_DENY && (mask & remaining)) {
            errno = EACCES;
            return false;
        }
        if (matched && type==FS4_ACE_ALLOW)
            remaining &= ~mask;
        if (!remaining)
            return true;
        pos+=span;
    }
    errno = EACCES;
    return false;
}

void fs4_acl_unix_sid(unsigned char out[16],uint32_t type,uint32_t id)
{
    memset(out,0,16);
    out[0]=1;
    out[1]=2; /* S-1-22-<type>-<id> */
    out[7]=22;
    put32(out + 8,type);
    put32(out + 12,id);
}

static bool creator_rewrite(const unsigned char *sid,size_t span,
                            uint32_t *target_type)
{
    /* S-1-3-0 CREATOR_OWNER and S-1-3-1 CREATOR_GROUP.
     * Replace placeholders on an inherited ACE, not the parent ACL. */
    static const unsigned char prefix[8]={1,1,0,0,0,0,0,3};
    if (span!=12 || memcmp(sid,prefix,sizeof(prefix)))
        return false;
    const uint32_t id=le32(sid + 8);
    if (id>1) return false;
    *target_type=id==0 ? 1u : 2u;
    return true;
}

bool fs4_acl_inherit(const void *parent,size_t parent_length,
                     uint32_t owner_uid,uint32_t group_gid,bool child_dir,
                     void *out,size_t capacity,size_t *written)
{
    struct acl_view inherited;
    if (!written || !out || !parse(parent,parent_length,&inherited))
        return false;
    const unsigned char *source=inherited.bytes;
    unsigned char scratch[FS4_SD_LIMIT]={0};
    /* Head + 2 synthetic Unix SIDs + DACL head. All lengths/alignment
     * are fixed; inherited ACEs are copied into remaining capacity. */
    const size_t dacl=SD_HEADER + 16u + 16u;
    size_t pos=dacl + ACL_HEADER;
    uint16_t count=0;
    const unsigned char *acl=source + inherited.dacl;
    const uint8_t revision=acl[0];
    for (size_t cursor=inherited.dacl + ACL_HEADER;
         cursor<inherited.dacl_end; ) {
        const uint16_t span=le16(source + cursor + 2);
        const unsigned char flags=source[cursor + 1];
        const bool propagate=child_dir ?
            (flags & (FS4_ACE_OBJECT_INHERIT | FS4_ACE_CONTAINER_INHERIT)) != 0 :
            (flags & FS4_ACE_OBJECT_INHERIT) != 0;
        if (propagate) {
            uint8_t new_flags=flags | FS4_ACE_INHERITED;
            if (!child_dir) {
                new_flags &= (uint8_t)~(FS4_ACE_OBJECT_INHERIT |
                                        FS4_ACE_CONTAINER_INHERIT |
                                        FS4_ACE_NO_PROPAGATE |
                                        FS4_ACE_INHERIT_ONLY);
            } else {
                if (flags & FS4_ACE_NO_PROPAGATE)
                    new_flags &= (uint8_t)~(FS4_ACE_OBJECT_INHERIT |
                                            FS4_ACE_CONTAINER_INHERIT |
                                            FS4_ACE_NO_PROPAGATE);
                if ((flags & FS4_ACE_CONTAINER_INHERIT) == 0)
                    new_flags |= FS4_ACE_INHERIT_ONLY;
                else
                    new_flags &= (uint8_t)~FS4_ACE_INHERIT_ONLY;
            }
            uint32_t new_type=0;
            /* An OI-only ACE on an intermediate directory is
             * INHERIT_ONLY. Keep CREATOR_OWNER/GROUP as a placeholder
             * there: it must bind to the final file's actual owner,
             * not the intermediate directory's owner. */
            const bool creator=!(new_flags & FS4_ACE_INHERIT_ONLY) &&
                creator_rewrite(source + cursor + ACE_HEADER,
                                span - ACE_HEADER,&new_type);
            const size_t new_span=span + (creator ? 4u : 0u);
            if (!region(pos,new_span,FS4_SD_LIMIT) ||
                new_span > UINT16_MAX) {
                errno=ENOSPC;
                return false;
            }
            memcpy(scratch + pos,source + cursor,span);
            put16(scratch + pos + 2,(uint16_t)new_span);
            if (creator)
                fs4_acl_unix_sid(scratch + pos + ACE_HEADER,new_type,
                                 new_type==1u ? owner_uid : group_gid);
            scratch[pos + 1]=new_flags;
            pos+=new_span;
            ++count;
        }
        cursor+=span;
    }
    scratch[0]=1;
    put16(scratch + 2,SD_SELF_RELATIVE | SD_DACL_PRESENT |
                        (count ? SD_DACL_AUTO_INHERITED : 0u));
    put32(scratch + 4,SD_HEADER);
    put32(scratch + 8,SD_HEADER + 16u);
    put32(scratch + 16,(uint32_t)dacl);
    fs4_acl_unix_sid(scratch + SD_HEADER,1u,owner_uid);
    fs4_acl_unix_sid(scratch + SD_HEADER + 16u,2u,group_gid);
    scratch[dacl]=revision;
    put16(scratch + dacl + 2,(uint16_t)(pos - dacl));
    put16(scratch + dacl + 4,count);
    if (!fs4_acl_validate(scratch,pos)) {
        errno=EUCLEAN;
        return false;
    }
    *written=pos;
    if (capacity<pos) {
        errno=ENOSPC;
        return false;
    }
    memcpy(out,scratch,pos);
    return true;
}
