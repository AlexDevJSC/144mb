/* Mouth Session API v1 semantics, Skin UI only. No socket or fake connected
 * state: the host must feed get_status and world identity to the UI. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_ui2.h"
int main(void){
    Skin2Ui u;char ip[16];uint16_t port=0;
    skin2_init(&u);
    assert(!skin2_lan_request(&u,ip,&port));
    skin2_input(&u,SKIN2_DOWN);skin2_input(&u,SKIN2_DOWN);
    skin2_input(&u,SKIN2_ACCEPT);assert(u.screen==SKIN2_LAN);
    assert(skin2_input(&u,SKIN2_ACCEPT)==SKIN2_NO_ACTION); /* no world */
    assert(!u.lan_identity_ready);
    skin2_lan_status(&u,2,0,14444,1); /* WAITING_FOR_CLIENT, from Mouth */
    assert(skin2_input(&u,SKIN2_ACCEPT)==SKIN2_LAN_CREATE_REQUEST);
    skin2_input(&u,SKIN2_DOWN);skin2_input(&u,SKIN2_ACCEPT);
    assert(u.screen==SKIN2_LAN_JOIN);
    const char *host="192.168.1.25";
    for(const char *p=host;*p;++p)assert(skin2_lan_character(&u,(unsigned char)*p));
    assert(!skin2_lan_character(&u,'x'));
    assert(!skin2_lan_character(&u,0));
    skin2_input(&u,SKIN2_DOWN); /* port */
    assert(skin2_lan_character(&u,8));
    assert(skin2_lan_character(&u,'5'));
    assert(skin2_lan_character(&u,8));
    assert(skin2_lan_character(&u,'4'));
    assert(skin2_lan_character(&u,8));
    /* Valid port = 14444 after normalizing the last digit */
    assert(skin2_lan_character(&u,'4'));
    skin2_input(&u,SKIN2_DOWN);assert(u.selection==2);
    assert(skin2_input(&u,SKIN2_ACCEPT)==SKIN2_LAN_JOIN_REQUEST);
    assert(skin2_lan_request(&u,ip,&port));
    assert(!strcmp(ip,"192.168.1.25") && port==14444u);
    assert(skin2_input(&u,SKIN2_BACK)==SKIN2_NO_ACTION && u.screen==SKIN2_LAN);
    skin2_input(&u,SKIN2_DOWN);skin2_input(&u,SKIN2_DOWN);assert(u.selection==2);
    skin2_input(&u,SKIN2_ACCEPT);assert(u.screen==SKIN2_LAN_STATUS);
    assert(skin2_input(&u,SKIN2_ACCEPT)==SKIN2_LAN_DISCONNECT_REQUEST);
    skin2_lan_status(&u,8,9,14444,1); /* LOST / CONNECTION_LOST */
    assert(u.lan_phase==8u&&u.lan_error==9u);
    skin2_lan_status(&u,0,0,0,1);
    assert(skin2_input(&u,SKIN2_ACCEPT)==SKIN2_NO_ACTION);
    /* Reject invalid address + zero/overflow port + missing identity. */
    skin2_input(&u,SKIN2_BACK);skin2_input(&u,SKIN2_DOWN);skin2_input(&u,SKIN2_ACCEPT);
    assert(u.screen==SKIN2_LAN_JOIN);
    strcpy(u.lan_ipv4,"999.100.1.1");assert(!skin2_lan_request(&u,ip,&port));
    strcpy(u.lan_ipv4,"10.0.0.1");strcpy(u.lan_port,"0");assert(!skin2_lan_request(&u,ip,&port));
    strcpy(u.lan_port,"65536");assert(!skin2_lan_request(&u,ip,&port));
    strcpy(u.lan_port,"65535");assert(skin2_lan_request(&u,ip,&port)&&port==65535u);
    skin2_lan_status(&u,4,0,14444,0);assert(!skin2_lan_request(&u,ip,&port));
    puts("PASS Mouth v0.4 Session API1 UI: host/join/IPv4/port/phase/error/disconnect/world precondition");
    return 0;
}
