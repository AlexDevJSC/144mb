/* Mouth v0.3 LAN: bounded UDP datagrams and a Bones ModKit host adapter.
   Wire format is deliberately independent of C structure layouts. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/mouth_lan.h"
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define mouth_inet_pton InetPtonA
typedef SOCKET mouth_socket;
#define MOUTH_SOCK_BAD INVALID_SOCKET
#define mouth_sock_close closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define mouth_inet_pton inet_pton
#include <unistd.h>
#include <fcntl.h>
typedef int mouth_socket;
#define MOUTH_SOCK_BAD (-1)
#define mouth_sock_close close
#endif

#define WIRE_HEADER 12u
#define WIRE_HELLO 36u
#define WIRE_WELCOME 36u
#define WIRE_INPUT 22u
#define WIRE_SNAP_BASE 21u
#define WIRE_ACTOR 28u
#define MOUTH_TYPE_HELLO 1u
#define MOUTH_TYPE_WELCOME 2u
#define MOUTH_TYPE_INPUT 3u
#define MOUTH_TYPE_SNAPSHOT 4u
#define MOUTH_TYPE_REJECT 5u
#define MOUTH_TYPE_LEAVE 6u /* Best effort graceful leave; no ACK */
#define MOUTH_INPUT_GRACE_MS 250u

static MouthLan *registered_host; /* One module instance per process. */

static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v) { unsigned i;for(i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8)); }
static void w64(uint8_t *p, uint64_t v) { unsigned i;for(i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8)); }
static uint16_t r16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static uint32_t r32(const uint8_t *p){unsigned i;uint32_t v=0;for(i=0;i<4;i++)v|=(uint32_t)p[i]<<(i*8);return v;}
static uint64_t r64(const uint8_t *p){unsigned i;uint64_t v=0;for(i=0;i<8;i++)v|=(uint64_t)p[i]<<(i*8);return v;}

static int make_header(uint8_t *d,uint32_t cap,uint8_t type,uint32_t size,uint32_t session){
    if(!d||cap<size||size>MOUTH_LAN_PACKET_MAX||size<WIRE_HEADER)return 0;
    d[0]='M';d[1]='1';d[2]='4';d[3]='4';
    d[4]=MOUTH_LAN_VERSION;d[5]=type;
    w16(d+6,(uint16_t)size);w32(d+8,session);
    return 1;
}
static int check_header(const uint8_t *p,uint32_t n,uint8_t type,uint32_t size){
    return p && n>=WIRE_HEADER && n<=MOUTH_LAN_PACKET_MAX &&
        p[0]=='M'&&p[1]=='1'&&p[2]=='4'&&p[3]=='4'&&
        p[4]==MOUTH_LAN_VERSION && p[5]==type && r16(p+6)==n &&
        (!size||size==n);
}
int mouth_lan_encode_hello(uint8_t *d,uint32_t cap,const MouthLanConfig *cfg,uint32_t nonce){
    if(!cfg||!cfg->world_id||!nonce||!make_header(d,cap,MOUTH_TYPE_HELLO,WIRE_HELLO,0))return 0;
    w64(d+12,cfg->world_id);w64(d+20,cfg->seed);
    w32(d+28,cfg->generator_version);w32(d+32,nonce);
    return WIRE_HELLO;
}
int mouth_lan_decode_hello(const uint8_t *p,uint32_t n,MouthLanConfig *cfg,uint32_t *nonce){
    if(!cfg||!nonce||!check_header(p,n,MOUTH_TYPE_HELLO,WIRE_HELLO)||r32(p+8))return 0;
    memset(cfg,0,sizeof(*cfg));
    cfg->world_id=r64(p+12);cfg->seed=r64(p+20);
    cfg->generator_version=r32(p+28);*nonce=r32(p+32);
    return cfg->world_id!=0&&*nonce!=0;
}
int mouth_lan_encode_input(uint8_t *d,uint32_t cap,uint32_t session,uint32_t sequence,
                           int f,int s,int yaw,int sprint,int jump){
    if(!session||!sequence||f< -1||f>1||s< -1||s>1||
       yaw< -300||yaw>300||sprint<0||sprint>1||jump<0||jump>1||
       !make_header(d,cap,MOUTH_TYPE_INPUT,WIRE_INPUT,session))return 0;
    w32(d+12,sequence);d[16]=(uint8_t)(int8_t)f;d[17]=(uint8_t)(int8_t)s;
    w16(d+18,(uint16_t)(int16_t)yaw);d[20]=(uint8_t)sprint;d[21]=(uint8_t)jump;
    return WIRE_INPUT;
}
int mouth_lan_decode_input(const uint8_t *p,uint32_t n,uint32_t session,
                           uint32_t *seq,int *f,int *s,int *yaw,int *sprint,int *jump){
    int a,b,c;
    if(!seq||!f||!s||!yaw||!sprint||!jump||!session||
       !check_header(p,n,MOUTH_TYPE_INPUT,WIRE_INPUT)||r32(p+8)!=session)return 0;
    a=(int)(int8_t)p[16];b=(int)(int8_t)p[17];c=(int)(int16_t)r16(p+18);
    if(!r32(p+12)||a< -1||a>1||b< -1||b>1||c< -300||c>300||p[20]>1||p[21]>1)return 0;
    *seq=r32(p+12);*f=a;*s=b;*yaw=c;*sprint=p[20];*jump=p[21];
    return 1;
}
uint64_t mouth_lan_clock_ms(void){
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
#endif
}
static mouth_socket sock_of(const MouthLan *l){return (mouth_socket)l->socket_handle;}
static int setup_network(MouthLan *l){
#ifdef _WIN32
    WSADATA data;
    if(WSAStartup(MAKEWORD(2,2),&data)!=0)return 0;
    l->winsock_started=1;
#else
    (void)l;
#endif
    return 1;
}
static void stop_network(MouthLan *l){
#ifdef _WIN32
    if(l->winsock_started)WSACleanup();
#else
    (void)l;
#endif
    l->winsock_started=0;
}
static int nonblocking(mouth_socket fd){
#ifdef _WIN32
    u_long yes=1;
    return ioctlsocket(fd,FIONBIO,&yes)==0;
#else
    int flags=fcntl(fd,F_GETFL,0);
    return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0;
#endif
}
static int send_packet(MouthLan *l,const uint8_t *p,int n);
void mouth_lan_close(MouthLan *l){
    if(!l)return;
    if(l->socket_active){
        uint8_t leave[WIRE_HEADER];
        if(l->status==MOUTH_LAN_CONNECTED && l->session_id &&
           make_header(leave,sizeof(leave),MOUTH_TYPE_LEAVE,
                       sizeof(leave),l->session_id))
            (void)send_packet(l,leave,(int)sizeof(leave));
        mouth_sock_close(sock_of(l));l->socket_active=0;
    }
    l->socket_handle=MOUTH_LAN_INVALID_SOCKET;
    stop_network(l);l->services=NULL;l->status=MOUTH_LAN_OFF;
    if(registered_host==l)registered_host=NULL;
}
static int socket_open(MouthLan *l,uint16_t port){
    struct sockaddr_in endpoint;
    mouth_socket fd;
    if(!setup_network(l))return 0;
    fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd==MOUTH_SOCK_BAD){stop_network(l);return 0;}
    memset(&endpoint,0,sizeof(endpoint));
    endpoint.sin_family=AF_INET;endpoint.sin_port=htons(port);
    endpoint.sin_addr.s_addr=htonl(INADDR_ANY);
    if(!nonblocking(fd)||bind(fd,(const struct sockaddr*)&endpoint,
                              (int)sizeof(endpoint))!=0){
        mouth_sock_close(fd);stop_network(l);return 0;
    }
    l->socket_handle=(uintptr_t)fd;l->socket_active=1;
    return 1;
}
static int send_packet(MouthLan *l,const uint8_t *p,int n){
    struct sockaddr_in to;
    int result;
    if(!l->socket_active||!l->peer_addr||!l->peer_port||n<0||n>(int)MOUTH_LAN_PACKET_MAX)return 0;
    memset(&to,0,sizeof(to));to.sin_family=AF_INET;
    to.sin_port=l->peer_port;to.sin_addr.s_addr=l->peer_addr;
    result=(int)sendto(sock_of(l),(const char*)p,n,0,(const struct sockaddr*)&to,(int)sizeof(to));
    if(result!=n)return 0;
    l->packets_sent++;l->bytes_sent+=(uint32_t)n;return 1;
}
static int matching_peer(const MouthLan *l,const struct sockaddr_in *from){
    return from->sin_family==AF_INET && from->sin_port==l->peer_port &&
           from->sin_addr.s_addr==l->peer_addr;
}
static int64_t from_s32(uint32_t v){return v<=INT32_MAX?(int64_t)v:(int64_t)v-INT64_C(4294967296);}
static int acceptable_coord(float f){
    return isfinite(f)&&f>=-200000.0f&&f<=200000.0f;
}
static void send_welcome(MouthLan *l){
    uint8_t p[WIRE_WELCOME];
    if(!make_header(p,sizeof(p),MOUTH_TYPE_WELCOME,sizeof(p),l->session_id))return;
    w64(p+12,l->config.remote_actor_id);w32(p+20,l->config.remote_owner);
    w32(p+24,l->client_nonce);w64(p+28,l->services->tick(l->services->host_context));
    (void)send_packet(l,p,sizeof(p));
}
static void send_reject(MouthLan *l,uint8_t code){
    uint8_t p[13];
    if(make_header(p,sizeof(p),MOUTH_TYPE_REJECT,sizeof(p),0)){p[12]=code;(void)send_packet(l,p,sizeof(p));}
}
static void reset_host_peer(MouthLan *l){
    l->status=MOUTH_LAN_DISCONNECTED;l->session_id=0;
    l->last_error=MOUTH_SESSION_CONNECTION_LOST;
    l->last_input_seq=0;l->forward=0;l->strafe=0;l->yaw_milliradians=0;
    l->sprint=l->jump=0;l->peer_addr=0;l->peer_port=0;
}
static void host_receive(MouthLan *l,const uint8_t *p,uint32_t n,
                         const struct sockaddr_in *from,uint64_t now){
    MouthLanConfig offered;uint32_t nonce,seq;int f,s,yaw,sp,j;
    if(!p||n> MOUTH_LAN_PACKET_MAX||n<WIRE_HEADER){l->rejected_packets++;return;}
    /* Versioned negative acknowledgement for an otherwise recognizable
       HELLO. Does not evict an established peer. */
    if(n>=WIRE_HEADER && p[0]=='M' && p[1]=='1' && p[2]=='4' &&
       p[3]=='4' && p[5]==MOUTH_TYPE_HELLO &&
       p[4]!=MOUTH_LAN_VERSION && l->status!=MOUTH_LAN_CONNECTED){
        l->peer_addr=from->sin_addr.s_addr;l->peer_port=from->sin_port;
        send_reject(l,2);l->peer_addr=0;l->peer_port=0;
        return;
    }
    if(check_header(p,n,MOUTH_TYPE_HELLO,WIRE_HELLO) &&
       mouth_lan_decode_hello(p,n,&offered,&nonce)){
        if(l->status==MOUTH_LAN_CONNECTED && !matching_peer(l,from))return;
        if(offered.world_id!=l->config.world_id||offered.seed!=l->config.seed||
           offered.generator_version!=l->config.generator_version){
            if(l->status!=MOUTH_LAN_CONNECTED){
                l->peer_addr=from->sin_addr.s_addr;l->peer_port=from->sin_port;
                send_reject(l,1);l->peer_addr=0;l->peer_port=0;
            }
            return; /* Invalid HELLO must not disrupt an established peer. */
        }
        l->peer_addr=from->sin_addr.s_addr;l->peer_port=from->sin_port;
        if(l->status==MOUTH_LAN_CONNECTED){
            if(nonce==l->client_nonce){l->last_rx_ms=now;send_welcome(l);}
            return;
        }
        l->client_nonce=nonce;l->session_id=(uint32_t)(
            now ^ (now>>32) ^ (uint64_t)nonce ^ l->config.seed ^ (l->config.seed>>32) ^ 
            (uint64_t)l->peer_addr ^ ((uint32_t)l->peer_port<<16) ^ UINT32_C(0xb1441a44));
        if(!l->session_id)l->session_id=1;
        /* Per-actor Bones command sequence must survive UDP reconnection. */
        l->last_input_seq=0;
        l->forward=l->strafe=0;l->sprint=l->jump=0;l->yaw_milliradians=0;
        l->last_input_ms=l->last_rx_ms=now;l->status=MOUTH_LAN_CONNECTED;
        l->last_error=MOUTH_SESSION_OK;l->ever_connected=1;
        send_welcome(l);return;
    }
    if(l->status==MOUTH_LAN_CONNECTED && matching_peer(l,from) &&
       check_header(p,n,MOUTH_TYPE_LEAVE,WIRE_HEADER) &&
       r32(p+8)==l->session_id){
        reset_host_peer(l);
        return;
    }
    if(l->status!=MOUTH_LAN_CONNECTED||!matching_peer(l,from)||
       !mouth_lan_decode_input(p,n,l->session_id,&seq,&f,&s,&yaw,&sp,&j)){
        l->rejected_packets++;return;
    }
    if(seq<=l->last_input_seq){l->rejected_packets++;return;}
    l->last_input_seq=seq;l->last_rx_ms=l->last_input_ms=now;
    l->forward=(int8_t)f;l->strafe=(int8_t)s;
    l->yaw_milliradians=(int16_t)yaw;l->sprint=(uint8_t)sp;l->jump=(uint8_t)j;
}
static void poll_host(MouthLan *l){
    uint8_t p[MOUTH_LAN_PACKET_MAX];unsigned i;
    uint64_t now=mouth_lan_clock_ms();
    for(i=0;i<32;i++){
        struct sockaddr_in from;
#ifdef _WIN32
        int size=(int)sizeof(from);
#else
        socklen_t size=(socklen_t)sizeof(from);
#endif
        int n=(int)recvfrom(sock_of(l),(char*)p,sizeof(p),0,(struct sockaddr*)&from,&size);
        if(n<=0)break;
        l->packets_received++;l->bytes_received+=(uint32_t)n;
        if(size==sizeof(from))host_receive(l,p,(uint32_t)n,&from,now);
    }
    if(l->status==MOUTH_LAN_CONNECTED && now-l->last_rx_ms>MOUTH_LAN_TIMEOUT_MS)
        reset_host_peer(l);
}
static void send_snapshot(MouthLan *l){
    uint8_t p[WIRE_SNAP_BASE+WIRE_ACTOR*MOUTH_LAN_MAX_ACTORS];
    BonesModActor actor;uint32_t i,n=0;
    const BonesModServices *s=l->services;
    uint64_t tick=s->tick(s->host_context);
    if(l->status!=MOUTH_LAN_CONNECTED||tick==l->last_snapshot_tick)return;
    for(i=0;i<MOUTH_LAN_MAX_ACTORS;i++){
        uint64_t id=i?l->config.remote_actor_id:1u;
        uint32_t off;
        memset(&actor,0,sizeof(actor));actor.size=sizeof(actor);
        if(s->actor_get(s->host_context,id,&actor)!=BONES_MOD_OK)continue;
        if(!acceptable_coord(actor.x)||!acceptable_coord(actor.y)||
           !acceptable_coord(actor.z)||!isfinite(actor.yaw))continue;
        off=WIRE_SNAP_BASE+WIRE_ACTOR*n;
        w64(p+off,actor.id);w32(p+off+8,actor.owner);
        w32(p+off+12,(uint32_t)(int32_t)lroundf(actor.x*1000.0f));
        w32(p+off+16,(uint32_t)(int32_t)lroundf(actor.y*1000.0f));
        w32(p+off+20,(uint32_t)(int32_t)lroundf(actor.z*1000.0f));
        w32(p+off+24,(uint32_t)(int32_t)lroundf(actor.yaw*1000.0f));
        n++;
    }
    if(n!=MOUTH_LAN_MAX_ACTORS)return;
    if(!make_header(p,sizeof(p),MOUTH_TYPE_SNAPSHOT,WIRE_SNAP_BASE+WIRE_ACTOR*n,l->session_id))return;
    w64(p+12,tick);p[20]=(uint8_t)n;
    if(send_packet(l,p,WIRE_SNAP_BASE+WIRE_ACTOR*n))l->last_snapshot_tick=tick;
}
static int32_t host_start(void *user,const BonesModServices *s){
    MouthLan *l=(MouthLan*)user;BonesModActor actor;
    if(!l||!s||s->abi!=BONES_MOD_ABI||s->size!=sizeof(*s)||
       !(s->permissions&BONES_PERMISSION_SEND_COMMANDS)||!s->actor_get||
       !s->submit_move||!s->tick||!s->actor_get||!l->host_mode)return BONES_MOD_EINVAL;
    memset(&actor,0,sizeof(actor));actor.size=sizeof(actor);
    if(s->actor_get(s->host_context,l->config.remote_actor_id,&actor)!=BONES_MOD_OK||
       actor.owner!=l->config.remote_owner||actor.owner==0)return BONES_MOD_MISSING;
    l->services=s;
    if(!socket_open(l,l->config.port)){l->services=NULL;l->last_error=MOUTH_SESSION_SOCKET_ERROR;return BONES_MOD_BAD_STATE;}
    l->status=MOUTH_LAN_CONNECTING;l->last_error=MOUTH_SESSION_OK;return BONES_MOD_OK;
}
static void host_stop(void *user){mouth_lan_close((MouthLan*)user);}
static void host_before(void *user){
    MouthLan *l=(MouthLan*)user;BonesModCommand cmd;uint64_t now;
    if(!l||!l->socket_active||!l->services)return;
    poll_host(l);if(l->status!=MOUTH_LAN_CONNECTED)return;
    now=mouth_lan_clock_ms();memset(&cmd,0,sizeof(cmd));
    cmd.size=sizeof(cmd);cmd.actor_id=l->config.remote_actor_id;
    cmd.tick=l->services->tick(l->services->host_context)+1u;
    cmd.sequence=++l->next_command_seq;cmd.issuer=l->config.remote_owner;
    if(now-l->last_input_ms<=MOUTH_INPUT_GRACE_MS){
        cmd.forward=l->forward;cmd.strafe=l->strafe;
        cmd.yaw_milliradians=l->yaw_milliradians;
        cmd.sprint=l->sprint;cmd.jump=l->jump;
    }
    /* Bones decides actual movement, collision and command acceptance. */
    (void)l->services->submit_move(l->services->host_context,&cmd);
}
static void host_after(void *user){
    MouthLan *l=(MouthLan*)user;
    if(l&&l->socket_active&&l->services &&
       l->services->tick(l->services->host_context)%3u==0)
        send_snapshot(l);
}
int mouth_lan_configure_host(MouthLan *l,const MouthLanConfig *cfg){
    if(!l||!cfg||!cfg->world_id||!cfg->port||!cfg->remote_actor_id||
       !cfg->remote_owner||(registered_host&&registered_host!=l)||
       (registered_host==l&&l->socket_active))return 0;
    memset(l,0,sizeof(*l));l->config=*cfg;l->socket_handle=MOUTH_LAN_INVALID_SOCKET;
    l->host_mode=1;registered_host=l;return 1;
}
int32_t mouth_lan_query(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out)||!registered_host)return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"mouth_lan",sizeof("mouth_lan"));
    out->implementation_version=3;out->role=BONES_ROLE_MOUTH;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|
        BONES_PERMISSION_READ_TIME|BONES_PERMISSION_SEND_COMMANDS;
    out->user_context=registered_host;out->start=host_start;out->stop=host_stop;
    out->before_tick=host_before;out->after_tick=host_after;
    return BONES_MOD_OK;
}
int mouth_lan_client_open(MouthLan *l,const MouthLanConfig *cfg,const char *ip){
    struct in_addr addr;
    if(!l||!cfg||!ip||!cfg->world_id||!cfg->port)return 0;
    memset(l,0,sizeof(*l));l->socket_handle=MOUTH_LAN_INVALID_SOCKET;
    l->config=*cfg;
    if(mouth_inet_pton(AF_INET,ip,&addr)!=1)return 0;
    if(!socket_open(l,0))return 0;
    l->peer_addr=addr.s_addr;l->peer_port=htons(cfg->port);
    l->connect_begin_ms=mouth_lan_clock_ms();
    l->client_nonce=(uint32_t)(l->connect_begin_ms ^ cfg->seed ^ UINT32_C(0x198b144f));
    if(!l->client_nonce)l->client_nonce=1;
    l->status=MOUTH_LAN_CONNECTING;l->last_error=MOUTH_SESSION_OK;return 1;
}
void mouth_lan_client_controls(MouthLan *l,int f,int s,int yaw,int sprint,int jump){
    if(!l||l->host_mode)return;
    if(f>=-1&&f<=1)l->forward=(int8_t)f;
    if(s>=-1&&s<=1)l->strafe=(int8_t)s;
    if(yaw>=-300&&yaw<=300)l->yaw_milliradians=(int16_t)yaw;
    if(sprint>=0&&sprint<=1)l->sprint=(uint8_t)sprint;
    if(jump>=0&&jump<=1)l->jump=(uint8_t)jump;
}
static void client_receive(MouthLan *l,const uint8_t *p,uint32_t n,uint64_t now){
    uint32_t i;uint64_t tick;
    if(l->status==MOUTH_LAN_CONNECTING &&
       check_header(p,n,MOUTH_TYPE_REJECT,13)&&r32(p+8)==0){
        l->last_error=(p[12]==2)?MOUTH_SESSION_PROTOCOL_INCOMPATIBLE:
                      (p[12]==1)?MOUTH_SESSION_WORLD_INCOMPATIBLE:
                      MOUTH_SESSION_PROTOCOL_INCOMPATIBLE;
        l->status=MOUTH_LAN_REJECTED;return;
    }
    if(check_header(p,n,MOUTH_TYPE_WELCOME,WIRE_WELCOME) &&
       l->status==MOUTH_LAN_CONNECTING && r32(p+24)==l->client_nonce &&
       r64(p+12)!=0 && r32(p+20)!=0 && r32(p+8)!=0){
        l->config.remote_actor_id=r64(p+12);l->config.remote_owner=r32(p+20);
        l->session_id=r32(p+8);l->last_rx_ms=now;
        l->last_snapshot_tick=0;l->status=MOUTH_LAN_CONNECTED;
        l->last_error=MOUTH_SESSION_OK;l->ever_connected=1;return;
    }
    if(l->status==MOUTH_LAN_CONNECTED &&
       check_header(p,n,MOUTH_TYPE_LEAVE,WIRE_HEADER) &&
       r32(p+8)==l->session_id){
        l->last_error=MOUTH_SESSION_CONNECTION_LOST;
        l->status=MOUTH_LAN_DISCONNECTED;
        l->actor_count=0;
        return;
    }
    if(l->status!=MOUTH_LAN_CONNECTED ||
       !check_header(p,n,MOUTH_TYPE_SNAPSHOT,0) ||
       r32(p+8)!=l->session_id || n<WIRE_SNAP_BASE)return;
    tick=r64(p+12);
    if(!tick||tick<=l->last_snapshot_tick||p[20]!=MOUTH_LAN_MAX_ACTORS||
       n!=WIRE_SNAP_BASE+WIRE_ACTOR*(uint32_t)p[20])return;
    {
        MouthLanActor decoded[MOUTH_LAN_MAX_ACTORS];
        for(i=0;i<p[20];i++){
            uint32_t off=WIRE_SNAP_BASE+WIRE_ACTOR*i;
            MouthLanActor *a=&decoded[i];
            a->actor_id=r64(p+off);a->owner=r32(p+off+8);
            if(!a->actor_id||!a->owner)return;
            a->x=(float)from_s32(r32(p+off+12))/1000.0f;
            a->y=(float)from_s32(r32(p+off+16))/1000.0f;
            a->z=(float)from_s32(r32(p+off+20))/1000.0f;
            a->yaw=(float)from_s32(r32(p+off+24))/1000.0f;
        }
        if(decoded[0].actor_id==decoded[1].actor_id ||
           decoded[0].actor_id!=1u ||
           decoded[1].actor_id!=l->config.remote_actor_id ||
           decoded[1].owner!=l->config.remote_owner)return;
        memcpy(l->actors,decoded,sizeof(decoded));
    }
    l->actor_count=p[20];l->last_snapshot_tick=tick;l->last_rx_ms=now;
}
void mouth_lan_client_poll(MouthLan *l){
    uint8_t p[MOUTH_LAN_PACKET_MAX];uint64_t now;unsigned i;
    if(!l||l->host_mode||!l->socket_active||l->status==MOUTH_LAN_REJECTED||
       l->status==MOUTH_LAN_DISCONNECTED)return;
    now=mouth_lan_clock_ms();
    for(i=0;i<32;i++){
        struct sockaddr_in from;
#ifdef _WIN32
        int size=(int)sizeof(from);
#else
        socklen_t size=(socklen_t)sizeof(from);
#endif
        int n=(int)recvfrom(sock_of(l),(char*)p,sizeof(p),0,(struct sockaddr*)&from,&size);
        if(n<=0)break;
        l->packets_received++;l->bytes_received+=(uint32_t)n;
        if(size==sizeof(from)&&matching_peer(l,&from))
            client_receive(l,p,(uint32_t)n,now);
        else l->rejected_packets++;
    }
    if(l->status==MOUTH_LAN_CONNECTED &&
       now-l->last_rx_ms>MOUTH_LAN_TIMEOUT_MS){
        l->status=MOUTH_LAN_CONNECTING;l->session_id=0;
        l->actor_count=0;l->next_input_seq=0;l->last_hello_ms=0;
        l->last_error=MOUTH_SESSION_CONNECTION_LOST;
        l->connect_begin_ms=now;
        l->client_nonce^=(uint32_t)now ^ UINT32_C(0x65be2a9d);
        if(!l->client_nonce)l->client_nonce=1;
    }
    if(l->status==MOUTH_LAN_CONNECTING &&
       now-l->connect_begin_ms>=MOUTH_LAN_TIMEOUT_MS){
        l->last_error=l->ever_connected?MOUTH_SESSION_CONNECTION_LOST:
                          MOUTH_SESSION_TIMEOUT;
        l->status=MOUTH_LAN_DISCONNECTED;
        return;
    }
    if(l->status==MOUTH_LAN_CONNECTING){
        if(!l->last_hello_ms||now-l->last_hello_ms>=MOUTH_LAN_HELLO_MS){
            int n=mouth_lan_encode_hello(p,sizeof(p),&l->config,l->client_nonce);
            if(n>0)(void)send_packet(l,p,n);
            l->last_hello_ms=now;
        }
    }else if(l->status==MOUTH_LAN_CONNECTED){
        if(l->next_input_seq==UINT32_MAX){
            l->status=MOUTH_LAN_DISCONNECTED;return;
        }
        int n=mouth_lan_encode_input(p,sizeof(p),l->session_id,++l->next_input_seq,
             l->forward,l->strafe,l->yaw_milliradians,l->sprint,l->jump);
        if(n>0)(void)send_packet(l,p,n);
    }
}


/* === MOUTH SESSION API v1: UI-independent session boundary =============== */
MouthSessionResult mouth_session_create(MouthLan *lan,
                                         const MouthLanConfig *config) {
    if(!lan||!config||!config->world_id||!config->port||
       !config->remote_actor_id||!config->remote_owner||
       config->remote_actor_id==1u) return MOUTH_SESSION_INVALID_ARGUMENT;
    if(lan->socket_active||(registered_host&&registered_host!=lan))
        return MOUTH_SESSION_BUSY;
    if(!mouth_lan_configure_host(lan,config))return MOUTH_SESSION_BUSY;
    lan->last_error=MOUTH_SESSION_OK;
    return MOUTH_SESSION_OK;
}
MouthSessionResult mouth_session_join(MouthLan *lan,
                                       const MouthLanConfig *config,
                                       const char *host_ipv4) {
    struct in_addr parsed;
    if(!lan||!config||!config->world_id||!config->port)
        return MOUTH_SESSION_INVALID_ARGUMENT;
    if(lan->socket_active||(registered_host&&registered_host==lan))
        return MOUTH_SESSION_BUSY;
    if(!host_ipv4 || mouth_inet_pton(AF_INET,host_ipv4,&parsed)!=1 ||
       parsed.s_addr==htonl(INADDR_ANY) ||
       parsed.s_addr==htonl(INADDR_BROADCAST))
        return MOUTH_SESSION_INVALID_ADDRESS;
    if(!mouth_lan_client_open(lan,config,host_ipv4)){
        lan->last_error=MOUTH_SESSION_SOCKET_ERROR;
        return MOUTH_SESSION_SOCKET_ERROR;
    }
    return MOUTH_SESSION_OK;
}
MouthSessionResult mouth_session_disconnect(MouthLan *lan) {
    if(!lan)return MOUTH_SESSION_INVALID_ARGUMENT;
    mouth_lan_close(lan);
    lan->actor_count=0;lan->ever_connected=0;
    lan->session_id=0;lan->last_error=MOUTH_SESSION_OK;
    return MOUTH_SESSION_OK;
}
MouthSessionResult mouth_session_get_status(const MouthLan *lan,
                                             MouthSessionInfo *out) {
    if(!lan||!out||out->size!=sizeof(*out))
        return MOUTH_SESSION_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    out->size=sizeof(*out);
    out->api_version=MOUTH_SESSION_API_VERSION;
    out->error=lan->last_error;
    out->is_host=lan->host_mode;
    out->socket_open=lan->socket_active;
    out->port=lan->config.port;
    out->remote_actor_id=lan->config.remote_actor_id;
    out->remote_owner=lan->config.remote_owner;
    out->confirmed_tick=lan->last_snapshot_tick;
    out->bytes_sent=lan->bytes_sent;
    out->bytes_received=lan->bytes_received;
    out->rejected_packets=lan->rejected_packets;
    if(lan->status==MOUTH_LAN_REJECTED)out->phase=MOUTH_SESSION_INCOMPATIBLE;
    else if(lan->status==MOUTH_LAN_CONNECTED)
        out->phase=MOUTH_SESSION_CONNECTED;
    else if(lan->status==MOUTH_LAN_DISCONNECTED && !lan->host_mode)
        out->phase=lan->ever_connected?MOUTH_SESSION_LOST:MOUTH_SESSION_TIMED_OUT;
    else if(lan->host_mode) {
        if(lan->socket_active)out->phase=MOUTH_SESSION_WAITING_FOR_CLIENT;
        else if(registered_host==lan)out->phase=MOUTH_SESSION_HOST_PREPARED;
        else out->phase=MOUTH_SESSION_OFFLINE;
    }else if(lan->status==MOUTH_LAN_CONNECTING)
        out->phase=lan->ever_connected?MOUTH_SESSION_RECONNECTING:
                                         MOUTH_SESSION_JOINING;
    else out->phase=MOUTH_SESSION_OFFLINE;
    return MOUTH_SESSION_OK;
}
MouthSessionResult mouth_session_poll(MouthLan *lan) {
    if(!lan)return MOUTH_SESSION_INVALID_ARGUMENT;
    if(lan->host_mode) return MOUTH_SESSION_NOT_READY; /* ModKit owns host ticks */
    if(!lan->socket_active)return MOUTH_SESSION_NOT_READY;
    mouth_lan_client_poll(lan);
    return lan->last_error;
}
MouthSessionResult mouth_session_copy_snapshot(
    const MouthLan *lan, MouthLanActor out[MOUTH_LAN_MAX_ACTORS],
    uint64_t *out_tick) {
    if(!lan||!out||!out_tick)return MOUTH_SESSION_INVALID_ARGUMENT;
    if(lan->host_mode||lan->status!=MOUTH_LAN_CONNECTED||
       lan->actor_count!=MOUTH_LAN_MAX_ACTORS||!lan->last_snapshot_tick)
        return MOUTH_SESSION_NOT_READY;
    memcpy(out,lan->actors,sizeof(lan->actors));
    *out_tick=lan->last_snapshot_tick;
    return MOUTH_SESSION_OK;
}
