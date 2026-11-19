/* SPDX-License-Identifier: MIT */
#include "nt_acl_engine.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
static const uint8_t alice[16]={1,2,0,0,0,0,0,5,21,0,0,0,42,0,0,0};
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n) {
    for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8));
}
static void sd_init(uint8_t *sd,size_t total,unsigned count) {
    memset(sd,0,total);
    sd[0]=1; put16(sd+2,0x8004); put32(sd+16,20);
    sd[20]=2; put16(sd+22,(uint16_t)(total-20));
    put16(sd+24,(uint16_t)count);
}
static size_t add_ace(uint8_t *p,uint8_t type,uint8_t flags,uint32_t rights,
                      const uint8_t *sid,size_t sid_length) {
    p[0]=type;p[1]=flags;put16(p+2,(uint16_t)(8+sid_length));
    put32(p+4,rights);memcpy(p+8,sid,sid_length);return 8+sid_length;
}
int main(void) {
    struct nt_acl_token token={.sid={alice,everyone},.length={16,12},.count=2};
    uint8_t sd[128]={0};uint32_t granted=0;
    sd_init(sd,68,2);
    add_ace(sd+28,1,3,2,everyone,sizeof(everyone));
    add_ace(sd+48,0,3,3,everyone,sizeof(everyone));
    assert(nt_acl_access_check(sd,68,&token,3,&granted)==NT_ACL_DENIED);
    assert(granted==0);
    assert(nt_acl_access_check(sd,68,&token,1,&granted)==NT_ACL_ALLOWED);
    assert(granted==1);
    /* Move allow ahead of deny: order matters, as in Windows AccessCheck. */
    uint8_t copy[20];memcpy(copy,sd+28,20);
    memcpy(sd+28,sd+48,20);memcpy(sd+48,copy,20);
    assert(nt_acl_access_check(sd,68,&token,3,&granted)==NT_ACL_ALLOWED);
    assert(granted==3);
    /* Inherit-only ACE does not grant the rights on the parent. */
    sd[28+1]=1|8;
    assert(nt_acl_access_check(sd,68,&token,3,&granted)==NT_ACL_DENIED);
    sd[28+1]=1;
    sd[48+1]=1; /* also object-only; intermediate directories inherit-only */
    uint8_t inherited[96]={0};size_t needed=0;
    assert(nt_acl_inherit_dacl(sd,68,true,inherited,sizeof(inherited),&needed)==NT_ACL_ALLOWED);
    assert(needed==48 && inherited[0]==2 && inherited[4]==2);
    assert((inherited[8+1]&16)!=0);
    assert((inherited[28+1]&8)!=0); /* OI-only deny becomes inherit-only */
    assert(nt_acl_inherit_dacl(sd,68,false,inherited,sizeof(inherited),&needed)==NT_ACL_ALLOWED);
    assert(needed==48 && (inherited[28+1]&8)==0);
    assert(nt_acl_inherit_dacl(sd,68,false,NULL,0,&needed)==NT_ACL_DENIED);
    assert(needed==48);
    /* Empty DACL denies; NULL DACL allows; SACL is unsupported. */
    sd_init(sd,28,0);
    assert(nt_acl_access_check(sd,28,&token,1,&granted)==NT_ACL_DENIED);
    put32(sd+16,0);
    assert(nt_acl_access_check(sd,28,&token,1,&granted)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(sd,28,&token,0x80000000u,&granted)==NT_ACL_UNSUPPORTED);
    put16(sd+2,0x8010);
    assert(nt_acl_access_check(sd,28,&token,1,&granted)==NT_ACL_UNSUPPORTED);
    sd_init(sd,48,1);
    add_ace(sd+28,0,0,1,everyone,sizeof(everyone));
    sd[28+8+1]=16; /* malformed SID: too many subauthorities */
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_INVALID);
    sd[28+8+1]=1;
    put16(sd+28+2,5000); /* malicious ACE size */
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_INVALID);
    /* A deny-only group is eligible for DENY, never for ALLOW. Even
     * a matching SID does not promote it to an enabled grant group. */
    sd_init(sd,48,1);
    add_ace(sd+28,0,0,1,everyone,sizeof(everyone));
    token.sid_flags[1]=NT_ACL_SID_DENY_ONLY;
    granted=99;
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_DENIED);
    assert(granted==0);
    token.sid_flags[1]=0;
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_ALLOWED);
    assert(granted==1);
    token.sid_flags[1]=NT_ACL_SID_DENY_ONLY;
    sd[28]=1; /* DENY ACE still matches deny-only SID */
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_DENIED);
    assert(granted==0);
    token.sid_flags[1]=2; /* future/unknown attributes must fail closed */
    assert(nt_acl_access_check(sd,48,&token,1,&granted)==NT_ACL_INVALID);
    assert(granted==0);
    /* GENERIC_* is object-specific. An authenticated caller MUST pass a
     * mapping; the legacy API rejects generic masks instead of guessing. */
    token.sid_flags[1]=0;
    sd_init(sd,48,1);
    add_ace(sd+28,0,0,UINT32_C(0x80000000),everyone,sizeof(everyone));
    struct nt_acl_generic_mapping mapping={
        .read=1,.write=2,.execute=4,.all=7
    };
    assert(nt_acl_access_check(sd,48,&token,UINT32_C(0x80000000),
                               &granted)==NT_ACL_UNSUPPORTED);
    assert(nt_acl_access_check_mapped(sd,48,&token,UINT32_C(0x80000000),
                                     &mapping,&granted)==NT_ACL_ALLOWED);
    assert(granted==1);
    assert(nt_acl_access_check_mapped(sd,48,&token,UINT32_C(0x40000000),
                                     &mapping,&granted)==NT_ACL_DENIED);
    assert(granted==0);
    const struct nt_acl_generic_mapping bad_mapping={
        .read=UINT32_C(0x80000000)
    };
    assert(nt_acl_access_check_mapped(sd,48,&token,UINT32_C(0x80000000),
                                     &bad_mapping,&granted)==NT_ACL_UNSUPPORTED);
    assert(granted==0);

    /* A valid SID-shaped blob embedded in the DACL must never be
     * accepted as the descriptor owner, even though its SID header parses. */
    sd_init(sd,48,1);
    add_ace(sd+28,0,0,1,everyone,sizeof(everyone));
    put32(sd+4,36); /* owner aliasing the ACE's Everyone SID */
    assert(nt_acl_validate_descriptor(sd,48)==NT_ACL_INVALID);
    put32(sd+4,0);
    assert(nt_acl_validate_descriptor(sd,48)==NT_ACL_ALLOWED);

    /* A complete child SD carries real authenticated owner+group SIDs
     * and translates CREATOR_OWNER inside inherited ACEs. Its SID can
     * be longer than the placeholder, so ACE+ACL sizes must be rebuilt. */
    static const uint8_t creator_owner[12]=
        {1,1,0,0,0,0,0,3,0,0,0,0};
    static const uint8_t creator_group[12]=
        {1,1,0,0,0,0,0,3,1,0,0,0};
    sd_init(sd,68,2);
    add_ace(sd+28,0,3,1,creator_owner,sizeof(creator_owner));
    add_ace(sd+48,1,3,2,creator_group,sizeof(creator_group));
    uint8_t child[128]={0};
    size_t output_size=0;
    assert(nt_acl_validate_descriptor(sd,68)==NT_ACL_ALLOWED);
    assert(nt_acl_inherit_descriptor(sd,68,false,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     NULL,0,&output_size)==NT_ACL_DENIED);
    assert(output_size==100);
    assert(nt_acl_inherit_descriptor(sd,68,false,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     child,output_size-1,&output_size)==NT_ACL_DENIED);
    assert(nt_acl_inherit_descriptor(sd,68,false,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     child,sizeof(child),&output_size)==NT_ACL_ALLOWED);
    assert(output_size==100 && nt_acl_validate_descriptor(child,output_size)==NT_ACL_ALLOWED);
    assert((child[2]&4)!=0 && (child[3]&0x80)!=0);
    assert(nt_acl_access_check(child,output_size,&token,1,&granted)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(child,output_size,&token,2,&granted)==NT_ACL_DENIED);
    /* NT ACE inheritance on intermediate directories must preserve the
     * propagation flags while setting INHERITED_ACE. */
    assert(nt_acl_inherit_descriptor(sd,68,true,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     child,sizeof(child),&output_size)==NT_ACL_ALLOWED);
    assert((child[20+16+12+8+1]&16)!=0);
    /* Same descriptor is not a valid output for in-place rewriting. */
    assert(nt_acl_inherit_descriptor(sd,68,true,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     sd,sizeof(sd),&output_size)==NT_ACL_INVALID);
    /* Partial overlap is just as dangerous as identical pointers. It
     * must leave every original descriptor byte untouched. */
    uint8_t saved_parent[68];
    memcpy(saved_parent,sd,sizeof(saved_parent));
    assert(nt_acl_inherit_descriptor(sd,68,true,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     sd+4,sizeof(sd)-4,
                                     &output_size)==NT_ACL_INVALID);
    assert(memcmp(sd,saved_parent,sizeof(saved_parent))==0);
    /* Foreign / unknown ACEs must fail closed with no child written. */
    sd[28]=9;
    assert(nt_acl_inherit_descriptor(sd,68,false,alice,sizeof(alice),
                                     everyone,sizeof(everyone),child,
                                     sizeof(child),&output_size)==NT_ACL_UNSUPPORTED);
    /* The verified-host identity adapter never admits guest-declared
     * domain SIDs or impersonates a different UNIX peer. */
    struct nt_acl_unix_token host_a={0}, host_b={0};
    assert(nt_acl_unix_token_init(&host_a,1001,50));
    assert(nt_acl_unix_token_init(&host_b,1002,50));
    assert(host_a.token.count==3 && host_a.token.length[0]==16);
    sd_init(sd,52,1);
    add_ace(sd+28,0,0,1,host_a.sid_bytes[0],16);
    assert(nt_acl_access_check(sd,52,&host_a.token,1,&granted)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(sd,52,&host_b.token,1,&granted)==NT_ACL_DENIED);
    sd_init(sd,48,1);
    add_ace(sd+28,0,0,1,host_a.sid_bytes[2],12);
    assert(nt_acl_access_check(sd,48,&host_a.token,1,&granted)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(sd,48,&host_b.token,1,&granted)==NT_ACL_ALLOWED);
    assert(!nt_acl_unix_token_init(NULL,1001,50));
    /* Arbitrary binary SIDs can carry the maximum 15 sub-authorities:
     * neither child inheritance nor AccessCheck may silently truncate
     * or substitute an unrecognized domain principal. */
    uint8_t foreign_sid[68]={1,15};
    foreign_sid[7]=5;
    for(unsigned i=0;i<15;i++) put32(foreign_sid+8+i*4,i+10);
    struct nt_acl_token foreign_token={
        .sid={foreign_sid},.length={sizeof(foreign_sid)},.count=1
    };
    sd_init(sd,104,1);
    add_ace(sd+28,0,3,1,foreign_sid,sizeof(foreign_sid));
    assert(nt_acl_validate_descriptor(sd,104)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(sd,104,&foreign_token,1,&granted)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(sd,104,&host_a.token,1,&granted)==NT_ACL_DENIED);
    uint8_t extended_child[160]={0};
    assert(nt_acl_inherit_descriptor(sd,104,false,
                                     alice,sizeof(alice),
                                     everyone,sizeof(everyone),
                                     extended_child,sizeof(extended_child),
                                     &output_size)==NT_ACL_ALLOWED);
    assert(output_size==132);
    assert(nt_acl_validate_descriptor(extended_child,output_size)==NT_ACL_ALLOWED);
    assert(nt_acl_access_check(extended_child,output_size,
                               &foreign_token,1,&granted)==NT_ACL_ALLOWED);
    foreign_sid[1]=16;
    assert(nt_acl_access_check(sd,104,&foreign_token,1,
                               &granted)==NT_ACL_INVALID);
    /* Virtual chmod is allowed only on an EXACT canonical three-ACE
     * owner/group/Everyone allow DACL, never on arbitrary named or DENY
     * ACEs which would be silently removed by mode conversion. */
    uint8_t mode_sd[128]={0},mode_new[128]={0};
    static const uint8_t gsid[16]={1,2,0,0,0,0,0,22,2,0,0,0,25,0,0,0};
    mode_sd[0]=1;put16(mode_sd+2,0x8004);
    put32(mode_sd+4,20);put32(mode_sd+8,36);put32(mode_sd+16,52);
    memcpy(mode_sd+20,alice,16);memcpy(mode_sd+36,gsid,16);
    mode_sd[52]=2;put16(mode_sd+54,76);put16(mode_sd+56,3);
    add_ace(mode_sd+60,0,0,0x81,alice,16);
    add_ace(mode_sd+84,0,0,0x81,gsid,16);
    add_ace(mode_sd+108,0,0,0,everyone,12);
    uint32_t mode=0;
    assert(nt_acl_validate_descriptor(mode_sd,sizeof(mode_sd))==NT_ACL_ALLOWED);
    assert(nt_acl_simple_mode(mode_sd,sizeof(mode_sd),&mode)==NT_ACL_ALLOWED);
    assert(mode==0440);
    assert(nt_acl_chmod_simple_descriptor(mode_sd,sizeof(mode_sd),0640,
                                          mode_new,sizeof(mode_new),
                                          &output_size)==NT_ACL_ALLOWED);
    assert(output_size==128);
    assert(nt_acl_simple_mode(mode_new,sizeof(mode_new),&mode)==NT_ACL_ALLOWED);
    assert(mode==0640);
    assert((mode_new[2] & 0x10)!=0); /* DACL_PROTECTED, no host chmod */
    assert(nt_acl_chmod_simple_descriptor(mode_sd,sizeof(mode_sd),01000,
                                          mode_new,sizeof(mode_new),
                                          &output_size)==NT_ACL_INVALID);
    mode_sd[60]=1; /* explicit DENY must not be erased */
    assert(nt_acl_chmod_simple_descriptor(mode_sd,sizeof(mode_sd),0777,
                                          mode_new,sizeof(mode_new),
                                          &output_size)==NT_ACL_UNSUPPORTED);
    mode_sd[60]=0;
    mode_sd[92]^=1; /* named foreign SID, not canonical owner */
    assert(nt_acl_chmod_simple_descriptor(mode_sd,sizeof(mode_sd),0644,
                                          mode_new,sizeof(mode_new),
                                          &output_size)==NT_ACL_UNSUPPORTED);
    puts("PASS self-relative SD, long SID, DACL inheritance, mapped rights and fail-closed virtual chmod");
    return 0;
}
