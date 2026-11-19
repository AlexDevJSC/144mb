/* SPDX-License-Identifier: MIT */
#include "fs4_acl.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr,"FAIL fs4 ACL line %d: %s (errno=%d)\n", \
            __LINE__,#expr,errno); exit(1); } } while (0)

static void w16(unsigned char *p, unsigned v)
{
    p[0]=(unsigned char)v;
    p[1]=(unsigned char)(v>>8);
}
static void w32(unsigned char *p, uint32_t v)
{
    for (int i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i));
}
static uint16_t r16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1]<<8));
}
static uint32_t r32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static unsigned char owner[16], group[16], everyone[12], custom[20];
static void identities(void)
{
    fs4_acl_unix_sid(owner,1,1000);
    fs4_acl_unix_sid(group,2,1000);
    memset(everyone,0,sizeof(everyone));
    everyone[0]=1;everyone[1]=1;everyone[7]=1;
    memset(custom,0,sizeof(custom));
    custom[0]=1;custom[1]=3;custom[7]=5;
    w32(custom+8,21);w32(custom+12,42);w32(custom+16,7);
}
static size_t sd_init(unsigned char b[FS4_SD_LIMIT])
{
    memset(b,0,FS4_SD_LIMIT);
    b[0]=1;
    w16(b+2,0x8004);
    w32(b+4,20);w32(b+8,36);w32(b+16,52);
    memcpy(b+20,owner,16);
    memcpy(b+36,group,16);
    b[52]=2;
    w16(b+54,8);w16(b+56,0);
    return 60;
}
static size_t add_ace(unsigned char *sd,size_t length,unsigned type,
                      unsigned flags,uint32_t mask,
                      const unsigned char *sid,size_t n)
{
    CHECK(length + 8 + n <= FS4_SD_LIMIT);
    sd[length]=(unsigned char)type;
    sd[length+1]=(unsigned char)flags;
    w16(sd+length+2,(unsigned)(8+n));
    w32(sd+length+4,mask);
    memcpy(sd+length+8,sid,n);
    w16(sd+54,(unsigned)(length+8+n-52));
    w16(sd+56,r16(sd+56)+1);
    return length+8+n;
}
static struct fs4_acl_token token(const unsigned char *a,size_t an,
                                  const unsigned char *b,size_t bn)
{
    struct fs4_acl_token t={0};
    t.sid[0]=a;t.sid_length[0]=an;t.count=1;
    if(b){t.sid[1]=b;t.sid_length[1]=bn;t.count=2;}
    return t;
}

static void checks(void)
{
    unsigned char sd[FS4_SD_LIMIT];
    size_t n=sd_init(sd);
    n=add_ace(sd,n,FS4_ACE_DENY,0,0x0002u,custom,sizeof(custom));
    n=add_ace(sd,n,FS4_ACE_ALLOW,0,0x0003u,everyone,sizeof(everyone));
    CHECK(fs4_acl_validate(sd,n));
    struct fs4_acl_token unknown=token(everyone,sizeof(everyone),NULL,0);
    struct fs4_acl_token identified=token(everyone,sizeof(everyone),
                                           custom,sizeof(custom));
    CHECK(fs4_acl_check(sd,n,&unknown,0x3u));
    CHECK(fs4_acl_check(sd,n,&identified,0x1u));
    CHECK(!fs4_acl_check(sd,n,&identified,0x2u));
    CHECK(!fs4_acl_check(sd,n,&unknown,0x4u));
    CHECK(!fs4_acl_check(sd,n,&unknown,0));
    CHECK(!fs4_acl_check(sd,n,NULL,0x1u));
    CHECK(fs4_acl_validate(sd,n)); /* failed check never mutates policy */
    sd[60]=0x9; /* unknown callback ACE MUST NOT be accepted */
    CHECK(!fs4_acl_validate(sd,n));
    CHECK(!fs4_acl_check(sd,n,&unknown,0x1u));
    sd[60]=0;
    sd[62]=0xFF;sd[63]=0x7F; /* truncated/overflow ACE */
    CHECK(!fs4_acl_validate(sd,n));
    sd[62]=28;sd[63]=0;
    CHECK(fs4_acl_validate(sd,n));
    sd[61]=0x80; /* unsupported audit-only flag in DACL */
    CHECK(!fs4_acl_validate(sd,n));
    sd[61]=0;
    sd[4]=21; /* owner pointer misaligned */
    CHECK(!fs4_acl_validate(sd,n));
    sd[4]=20;
    sd[16]=0; /* NULL DACL could grant all; fail closed */
    CHECK(!fs4_acl_validate(sd,n));
    sd[16]=52;
    sd[20]=2; /* invalid SID revision */
    CHECK(!fs4_acl_validate(sd,n));
    sd[20]=1;
    sd[21]=16; /* SID subauthority overrun */
    CHECK(!fs4_acl_validate(sd,n));
    sd[21]=2;
    CHECK(fs4_acl_validate(sd,n));
}

static void inherit(void)
{
    unsigned char parent[FS4_SD_LIMIT],file[FS4_SD_LIMIT],directory[FS4_SD_LIMIT];
    unsigned char creator_owner[12]={1,1,0,0,0,0,0,3,0,0,0,0};
    size_t n=sd_init(parent);
    n=add_ace(parent,n,FS4_ACE_ALLOW,
              FS4_ACE_OBJECT_INHERIT|FS4_ACE_CONTAINER_INHERIT,
              0x1u,everyone,sizeof(everyone));
    n=add_ace(parent,n,FS4_ACE_DENY,
              FS4_ACE_CONTAINER_INHERIT|FS4_ACE_NO_PROPAGATE,
              0x2u,custom,sizeof(custom));
    n=add_ace(parent,n,FS4_ACE_ALLOW,
              FS4_ACE_OBJECT_INHERIT|FS4_ACE_CONTAINER_INHERIT,
              0x8u,creator_owner,sizeof(creator_owner));
    n=add_ace(parent,n,FS4_ACE_ALLOW,FS4_ACE_OBJECT_INHERIT,
              0x4u,custom,sizeof(custom));
    CHECK(fs4_acl_validate(parent,n));
    size_t nf=0,nd=0;
    CHECK(fs4_acl_inherit(parent,n,1234,5678,false,file,sizeof(file),&nf));
    CHECK(fs4_acl_inherit(parent,n,1234,5678,true,directory,
                          sizeof(directory),&nd));
    CHECK(fs4_acl_validate(file,nf));
    CHECK(fs4_acl_validate(directory,nd));
    CHECK(r16(file+56)==3); /* CI-only deny does NOT inherit to files */
    CHECK(r16(directory+56)==4);
    CHECK((r16(file+2) & 0x0400) != 0);
    unsigned char new_owner[16], new_group[16];
    fs4_acl_unix_sid(new_owner,1,1234);
    fs4_acl_unix_sid(new_group,2,5678);
    CHECK(memcmp(file+20,new_owner,16)==0);
    CHECK(memcmp(file+36,new_group,16)==0);
    const struct fs4_acl_token t=token(everyone,sizeof(everyone),
                                      new_owner,sizeof(new_owner));
    CHECK(fs4_acl_check(file,nf,&t,0x9u)); /* CREATOR_OWNER substituted */
    CHECK(!fs4_acl_check(file,nf,&t,0x2u));
    const size_t first_dir=(size_t)r32(directory+16)+8u;
    CHECK((directory[first_dir+1] & FS4_ACE_INHERITED)!=0);
    const size_t ci_deny=first_dir+r16(directory+first_dir+2);
    CHECK((directory[ci_deny+1] &
          (FS4_ACE_NO_PROPAGATE | FS4_ACE_OBJECT_INHERIT |
           FS4_ACE_CONTAINER_INHERIT))==0);
    CHECK((directory[ci_deny+1] & FS4_ACE_INHERITED)!=0);
    size_t next=0;
    unsigned char subdir[FS4_SD_LIMIT];
    CHECK(fs4_acl_inherit(directory,nd,1250,9000,true,subdir,sizeof(subdir),&next));
    CHECK(r16(subdir+56)==3); /* NP deny does not propagate; OI-only remains inherit-only */
    CHECK(!fs4_acl_inherit(parent,n,1234,5678,false,file,20,&nf));
    CHECK(errno==ENOSPC);
    CHECK(fs4_acl_validate(parent,n)); /* inherited copy did not mutate parent */
}

static void adversarial_inputs(void)
{
    unsigned char garbage[FS4_SD_LIMIT]={0};
    for (size_t n=0;n<FS4_SD_LIMIT;n++) {
        garbage[n]=(unsigned char)((n*131u+7u)&0xffu);
        CHECK(!fs4_acl_validate(garbage,n));
    }
    CHECK(!fs4_acl_validate(NULL,32));
    CHECK(!fs4_acl_validate(garbage,FS4_SD_LIMIT+1u));
}

int main(void)
{
    identities();
    checks();
    inherit();
    adversarial_inputs();
    puts("PASS FS4 NT DACL policy: arbitrary SID match, DENY/ALLOW, strict bounds and OI/CI inheritance");
    return 0;
}
