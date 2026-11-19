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
    puts("PASS self-relative SD, ordered ACE, deny-only SID and DACL inheritance");
    return 0;
}
