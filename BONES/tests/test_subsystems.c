/* Integration tests: modular lifecycle, event bus, dependency graph, render. */
#include "../include/bones/bones.h"
#include "../src/adapters/eye_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Static storage: prototype has no dynamic-allocation requirement. */
static BonesEngine engine;
static BonesEye eye;

typedef struct {
    unsigned init_n, stop_n, before_n, after_n, events_n, scenes_n;
    unsigned jump_n, land_n;
    unsigned trace_n, shutdown_trace_n;
    unsigned trace[16], shutdown_trace[16];
    unsigned fail_slot;
} Trace;
typedef struct { Trace *trace; unsigned id; } Probe;
static Trace trace;
static Probe probes[BONES_SLOT_COUNT];

static bool startup(void *ctx, BonesEngine *e) {
    Probe *p=(Probe *)ctx;
    (void)e;
    if (p->id==p->trace->fail_slot) return false;
    p->trace->trace[p->trace->trace_n++]=p->id;
    p->trace->init_n++;
    return true;
}
static void shutdown(void *ctx, BonesEngine *e) {
    Probe *p=(Probe *)ctx;
    (void)e;
    p->trace->shutdown_trace[p->trace->shutdown_trace_n++]=p->id;
    p->trace->stop_n++;
}
static void before_tick(void *ctx, BonesEngine *e, const BonesInput *input) {
    Probe *p=(Probe *)ctx;
    (void)input;
    assert(e->subsystem_started && e->tick < 200);
    p->trace->before_n++;
}
static void after_tick(void *ctx, BonesEngine *e) {
    Probe *p=(Probe *)ctx;
    assert(e->tick>0);
    p->trace->after_n++;
}
static void on_event(void *ctx, const BonesEngine *e, const BonesEvent *evt) {
    Probe *p=(Probe *)ctx;
    assert(e->tick>0);
    p->trace->events_n++;
    if(evt->type==BONES_EVENT_JUMP) p->trace->jump_n++;
    if(evt->type==BONES_EVENT_LAND) p->trace->land_n++;
}
static void build_scene(void *ctx, const BonesEngine *e, BonesScene *s) {
    Probe *p=(Probe *)ctx;
    (void)e;
    assert(s->quad_count>0);
    p->trace->scenes_n++;
}
static void wrong_present(void *ctx, const BonesScene *scene) { (void)ctx; (void)scene; }
static BonesSubsystem probe_desc(BonesSubsystemSlot slot) {
    BonesSubsystem m={0};
    probes[slot].trace=&trace; probes[slot].id=(unsigned)slot;
    m.version=BONES_SUBSYSTEM_API_VERSION;
    m.struct_size=(uint32_t)sizeof(m);
    m.slot=slot;
    m.context=&probes[slot];
    m.startup=startup; m.shutdown=shutdown;
    m.before_tick=before_tick; m.after_tick=after_tick;
    m.on_event=on_event; m.build_scene=build_scene;
    return m;
}
static void reset(void) {
    bones_init(&engine,42);
    memset(&trace,0,sizeof(trace));
    trace.fail_slot=999;
}
static void test_all_modules(void) {
    BonesInput in={0};
    BonesSubsystem m, eye_module;
    unsigned i;
    uint64_t first_hash, second_hash;
    reset();
    bones_eye_init(&eye,42);
    eye_module=bones_eye_subsystem(&eye);
    eye_module.requires=BONES_SLOT_BIT(BONES_SLOT_SKIN);
    /* Attach deliberately scrambled. Startup must not follow attachment order. */
    assert(bones_attach_subsystem(&engine,&eye_module));
    m=probe_desc(BONES_SLOT_MOUTH); m.requires=BONES_SLOT_BIT(BONES_SLOT_SKIN);
    assert(bones_attach_subsystem(&engine,&m));
    m=probe_desc(BONES_SLOT_SKIN); m.requires=BONES_SLOT_BIT(BONES_SLOT_BRAIN);
    assert(bones_attach_subsystem(&engine,&m));
    m=probe_desc(BONES_SLOT_EAR); assert(bones_attach_subsystem(&engine,&m));
    m=probe_desc(BONES_SLOT_BRAIN); assert(bones_attach_subsystem(&engine,&m));
    assert(engine.subsystem_count==5);
    assert(!bones_attach_subsystem(&engine,&eye_module));
    assert(bones_start(&engine) && bones_start(&engine));
    assert(trace.init_n==4); /* Eye adapter intentionally has no startup callback. */
    assert(trace.trace_n==4);
    assert(trace.trace[0]==BONES_SLOT_BRAIN);
    assert(trace.trace[1]==BONES_SLOT_EAR);
    assert(trace.trace[2]==BONES_SLOT_SKIN);
    assert(trace.trace[3]==BONES_SLOT_MOUTH);
    assert(engine.active_subsystems==5);
    assert(bones_render_frame(&engine));
    first_hash=p144_renderer_image_hash(&eye.renderer);
    assert(bones_render_frame(&engine));
    second_hash=p144_renderer_image_hash(&eye.renderer);
    assert(first_hash==second_hash);
    assert(trace.scenes_n==8); /* 4 scene hooks x 2 frames */
    in.jump=1;
    assert(bones_step(&engine,&in,BONES_TICK_DT));
    assert(trace.before_n==4 && trace.after_n==4);
    assert(trace.jump_n==4);
    in.jump=0;
    for(i=0;i<120;i++) assert(bones_step(&engine,&in,BONES_TICK_DT));
    assert(trace.land_n==4);
    assert(!bones_attach_subsystem(&engine,&m));
    bones_stop(&engine);
    assert(trace.stop_n==4);
    assert(trace.shutdown_trace[0]==BONES_SLOT_MOUTH);
    assert(trace.shutdown_trace[1]==BONES_SLOT_SKIN);
    assert(trace.shutdown_trace[2]==BONES_SLOT_EAR);
    assert(trace.shutdown_trace[3]==BONES_SLOT_BRAIN);
    bones_stop(&engine);
    assert(trace.stop_n==4);
    printf("PASS all slots, lifecycle, dependencies, Eye renderer hash=%016llx\n",
           (unsigned long long)first_hash);
}
static void test_invalid_and_optional(void) {
    BonesSubsystem a,b;
    BonesInput input={0};
    reset();
    assert(bones_step(&engine,&input,BONES_TICK_DT)); /* all optional */
    assert(!bones_render_frame(&engine)); /* Eye absent */
    bones_stop(&engine);
    reset();
    a=probe_desc(BONES_SLOT_EAR);
    a.version=0; assert(!bones_attach_subsystem(&engine,&a));
    a.version=BONES_SUBSYSTEM_API_VERSION; a.struct_size=0;
    assert(!bones_attach_subsystem(&engine,&a));
    a.struct_size=(uint32_t)sizeof(a);
    a.requires=BONES_SLOT_BIT(BONES_SLOT_EAR); /* self dependency */
    assert(!bones_attach_subsystem(&engine,&a));
    a.requires=(1u<<12); assert(!bones_attach_subsystem(&engine,&a));
    a.requires=0; a.present=wrong_present;
    assert(!bones_attach_subsystem(&engine,&a)); /* only EYE may present */
    a.present=NULL;
    assert(bones_attach_subsystem(&engine,&a));
    assert(!bones_subsystem_attached(&engine,BONES_SLOT_EYE));
    assert(bones_subsystem_attached(&engine,BONES_SLOT_EAR));
    assert(!bones_subsystem_attached(&engine,(BonesSubsystemSlot)25));
    /* Missing required subsystem: do not invoke any startup. */
    b=probe_desc(BONES_SLOT_SKIN); b.requires=BONES_SLOT_BIT(BONES_SLOT_BRAIN);
    assert(bones_attach_subsystem(&engine,&b));
    assert(!bones_start(&engine));
    assert(trace.init_n==0 && engine.tick==0);
    b=probe_desc(BONES_SLOT_BRAIN); b.requires=BONES_SLOT_BIT(BONES_SLOT_SKIN);
    assert(bones_attach_subsystem(&engine,&b));
    assert(!bones_start(&engine)); /* dependency cycle */
    assert(trace.init_n==0);
    /* Can inspect and rebuild engine cleanly on failure. */
    reset();
    a=probe_desc(BONES_SLOT_EAR); a.requires=BONES_SLOT_BIT(BONES_SLOT_BRAIN);
    assert(bones_attach_subsystem(&engine,&a));
    b=probe_desc(BONES_SLOT_BRAIN);
    assert(bones_attach_subsystem(&engine,&b));
    trace.fail_slot=BONES_SLOT_EAR;
    assert(!bones_start(&engine));
    assert(trace.init_n==1 && trace.stop_n==1);
    assert(trace.shutdown_trace[0]==BONES_SLOT_BRAIN);
    assert(engine.active_subsystems==0 && !engine.subsystem_started);
    trace.fail_slot=999;
    assert(bones_start(&engine));
    bones_stop(&engine);
}
static void test_events(void) {
    BonesEvent ev={BONES_EVENT_CUSTOM,77,88};
    BonesInput input={0};
    unsigned i;
    reset();
    for(i=0;i<BONES_MAX_EVENTS;i++) assert(bones_emit_event(&engine,&ev));
    assert(!bones_emit_event(&engine,&ev));
    assert(engine.event_overflow);
    assert(!bones_emit_event(&engine,&(BonesEvent){0,0,0}));
    assert(bones_step(&engine,&input,BONES_TICK_DT));
    assert(engine.event_count==0 && engine.event_overflow==1);
    bones_clear_event_overflow(&engine);
    assert(engine.event_overflow==0);
    assert(bones_emit_event(&engine,&ev));
    bones_stop(&engine);
    assert(engine.event_count==0);
}
int main(void) {
    test_all_modules(); test_invalid_and_optional(); test_events();
    puts("PASS: five-slot module bus, validation, lifecycle rollback, events, Eye adapter");
    return 0;
}
