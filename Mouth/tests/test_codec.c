#include "../include/mouth_lan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    MouthLanConfig c={0},d={0};
    uint8_t p[MOUTH_LAN_PACKET_MAX]={0},copy[MOUTH_LAN_PACKET_MAX];
    uint32_t nonce=0,seq=0;
    int f=0,s=0,y=0,sp=0,j=0,n,k;
    c.world_id=UINT64_C(0x1122334455667788);
    c.seed=UINT64_C(0xffeeddccbbaa0099);
    c.generator_version=7;c.port=14444;
    n=mouth_lan_encode_hello(p,sizeof(p),&c,UINT32_C(0x12345678));
    assert(n==36);
    assert(p[0]=='M'&&p[1]=='1'&&p[2]=='4'&&p[3]=='4');
    assert(p[4]==MOUTH_LAN_VERSION&&p[5]==1&&p[6]==36&&p[7]==0);
    assert(!memcmp(p+12,"\x88\x77\x66\x55\x44\x33\x22\x11",8));
    assert(!memcmp(p+20,"\x99\x00\xaa\xbb\xcc\xdd\xee\xff",8));
    assert(mouth_lan_decode_hello(p,(uint32_t)n,&d,&nonce)==1);
    assert(d.world_id==c.world_id&&d.seed==c.seed&&d.generator_version==7);
    assert(nonce==UINT32_C(0x12345678));
    assert(!mouth_lan_decode_hello(p,(uint32_t)n-1,&d,&nonce));
    memcpy(copy,p,(size_t)n);copy[4]^=1;
    assert(!mouth_lan_decode_hello(copy,(uint32_t)n,&d,&nonce));
    memcpy(copy,p,(size_t)n);copy[6]^=1;
    assert(!mouth_lan_decode_hello(copy,(uint32_t)n,&d,&nonce));
    memcpy(copy,p,(size_t)n);copy[8]=1;
    assert(!mouth_lan_decode_hello(copy,(uint32_t)n,&d,&nonce));
    assert(!mouth_lan_encode_hello(p,35,&c,1));
    assert(!mouth_lan_encode_hello(p,sizeof(p),&c,0));
    n=mouth_lan_encode_input(p,sizeof(p),UINT32_C(0x00aa1234),1,-1,1,-300,1,0);
    assert(n==22);
    assert(mouth_lan_decode_input(p,(uint32_t)n,UINT32_C(0x00aa1234),
                                  &seq,&f,&s,&y,&sp,&j));
    assert(seq==1&&f==-1&&s==1&&y==-300&&sp==1&&j==0);
    assert(!mouth_lan_decode_input(p,(uint32_t)n,0x00aa1235,&seq,&f,&s,&y,&sp,&j));
    assert(!mouth_lan_decode_input(p,(uint32_t)n-1,0x00aa1234,&seq,&f,&s,&y,&sp,&j));
    p[16]=2;
    assert(!mouth_lan_decode_input(p,(uint32_t)n,0x00aa1234,&seq,&f,&s,&y,&sp,&j));
    for(k=0;k<22;k++){
        assert(mouth_lan_encode_input(p,sizeof(p),UINT32_C(0x00aa1234),1,-1,1,-300,1,0)==n);
        memcpy(copy,p,(size_t)n);
        copy[k]^=0xff;
        if(k==0||k==4||k==5||k==6||k==7||k==8||k==16)
            assert(!mouth_lan_decode_input(copy,(uint32_t)n,0x00aa1234,
                                            &seq,&f,&s,&y,&sp,&j));
    }
    assert(!mouth_lan_encode_input(p,sizeof(p),1,1,2,0,0,0,0));
    assert(!mouth_lan_encode_input(p,sizeof(p),1,1,0,0,301,0,0));
    assert(!mouth_lan_encode_input(p,sizeof(p),1,1,0,0,0,2,0));
    assert(!mouth_lan_encode_input(p,sizeof(p),0,1,0,0,0,0,0));
    assert(!mouth_lan_encode_input(p,sizeof(p),1,0,0,0,0,0,0));
    puts("PASS Mouth LAN: LE codecs, sizes, version, session, range validation.");
    return 0;
}
