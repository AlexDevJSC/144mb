#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../BONES/include/bones/modkit.h"
#include "../../BONES/modules/ear/ear_audio_control.h"
extern int32_t bones_mod_ear_query(uint32_t,BonesModDescriptor*);
extern int ear_mod_test_render(int16_t*,size_t);
static uint64_t test_tick(void *c){(void)c;return 100;}
static int32_t test_actor(void *c,uint64_t id,BonesModActor *a){
    (void)c;
    if(id!=11||!a||a->size!=sizeof(*a))return BONES_MOD_MISSING;
    memset(a,0,sizeof(*a));a->size=sizeof(*a);a->id=id;return BONES_MOD_OK;
}
static BonesModServices services(void){
    BonesModServices s={0};s.size=sizeof(s);s.abi=BONES_MOD_ABI;
    s.permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    s.actor_get=test_actor;s.tick=test_tick;return s;
}
static BonesModDescriptor query(void){
    BonesModDescriptor d={0};d.size=sizeof(d);
    assert(bones_mod_ear_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.role==BONES_ROLE_EAR&&d.implementation_version==4);
    assert(d.on_event&&d.after_tick&&d.start&&d.stop);
    assert(!d.present&&!d.build_scene&&d.required_roles==0);
    return d;
}
static EarAudioStatus get(void){
    EarAudioStatus s={0};s.size=sizeof(s);
    assert(ear_audio_get_status(&s));assert(s.api_version==EAR_AUDIO_CONTROL_API);
    return s;
}
static int any_sound(const int16_t *a,size_t n){size_t i;for(i=0;i<n;i++)if(a[i])return 1;return 0;}
static void emit(BonesModDescriptor *d,uint32_t type,uint64_t tick,uint64_t actor){
    BonesModEvent e={0};e.size=sizeof(e);e.type=type;e.tick=tick;e.source_id=actor;
    d->on_event(d->user_context,&e);
}
static int16_t pcm[8192];
int main(void){
    BonesModDescriptor d=query();BonesModServices s=services();EarAudioStatus st;uint64_t count;
    uint64_t init_attempts;
    EarAudioStatus bad={0};bad.size=1;assert(!ear_audio_get_status(&bad));
    assert(ear_audio_set_music_volume(.23f));assert(ear_audio_set_sfx_volume(.67f));
    assert(!ear_audio_set_music_volume(-1));assert(!ear_audio_set_sfx_volume(1.01f));
    assert(!ear_audio_set_music_volume(NAN));
    assert(ear_audio_enable_music(0));assert(!ear_audio_enable_music(2));
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    st=get();assert(st.module_running&&st.music_enabled==0);
    assert(strcmp(ear_audio_error_string(st.error),"unknown audio error")!=0);
    assert(strcmp(ear_audio_error_string(999),"unknown audio error")==0);
    assert(st.music_volume==.23f&&st.sfx_volume==.67f);
#if defined(EAR_FAKE_OUTPUT) && !defined(EAR_FAKE_NO_DEVICE)
    assert(st.device_ready&&st.error==EAR_AUDIO_ERROR_NONE);
    assert(st.buffers_submitted==4&&st.frames_generated==4*512);
#else
    assert(!st.device_ready&&st.error!=EAR_AUDIO_ERROR_NONE);
#endif
    assert(ear_mod_test_render(pcm,8192));assert(!any_sound(pcm,8192));
    assert(ear_audio_enable_music(1));
    assert(ear_mod_test_render(pcm,8192));assert(any_sound(pcm,8192));
    emit(&d,BONES_EVT_JUMP,100,11);emit(&d,BONES_EVT_JUMP,100,11);
    emit(&d,BONES_EVT_LAND,101,11);emit(&d,BONES_EVT_LAND,101,11);
    emit(&d,BONES_EVT_LAND,102,99);emit(&d,BONES_EVT_CUSTOM,102,11);
    st=get();assert(st.events_played==2&&st.events_duplicate==2&&st.events_invalid==1);
    init_attempts=st.open_attempts;
    assert(ear_audio_enable_music(0));
    assert(ear_mod_test_render(pcm,8192));assert(any_sound(pcm,8192)); /* SFX independent */
#if defined(EAR_FAKE_OUTPUT) && !defined(EAR_FAKE_NO_DEVICE)
    count=st.buffers_submitted;
    assert(ear_audio_pause());st=get();assert(st.paused&&st.device_ready);
    d.after_tick(d.user_context);st=get();assert(st.buffers_submitted==count);
    assert(ear_audio_resume());d.after_tick(d.user_context);
    st=get();assert(!st.paused&&st.buffers_submitted>count);
    assert(st.buffers_completed>0&&st.open_attempts==init_attempts);
    assert(ear_audio_retry_output());st=get();assert(st.open_attempts==init_attempts);
#else
    assert(!ear_audio_pause());st=get();assert(st.paused);
    assert(!ear_audio_resume());st=get();assert(!st.paused);
    d.after_tick(d.user_context);st=get();assert(!st.buffers_submitted);
    assert(!ear_audio_retry_output());st=get();assert(st.open_attempts==init_attempts+1);
    assert(st.error!=EAR_AUDIO_ERROR_NONE);
#endif
    d.stop(d.user_context);d.stop(d.user_context);
    st=get();assert(!st.module_running&&!st.device_ready);
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    st=get();assert(st.module_running&&!st.music_enabled);
    d.stop(d.user_context);
    (void)count;
    printf("PASS Ear audio v0.4: ABI, configuration, music mute, SFX, dedupe, pump, recovery, lifecycle\n");
    return 0;
}
