/* EJEMPLO EXTERNO: un futuro modulo Ear puede recibir eventos sin
   depender de fisicas, Win32 ni implementacion visual de Eye.
   Este archivo NO forma parte del ejecutable base. */
#include "../include/bones/bones.h"

typedef struct {
    uint32_t num_saltos;
    uint32_t num_aterrizajes;
} EarExampleState;

static bool ear_begin(void *context, BonesEngine *engine) {
    EarExampleState *state = (EarExampleState *)context;
    (void)engine;
    state->num_saltos = 0;
    state->num_aterrizajes = 0;
    return true;
}
static void ear_event(void *context, const BonesEngine *engine, const BonesEvent *event) {
    EarExampleState *state = (EarExampleState *)context;
    (void)engine;
    if (event->type == BONES_EVENT_JUMP) state->num_saltos++;
    if (event->type == BONES_EVENT_LAND) state->num_aterrizajes++;
    /* Aqui un motor Ear real podria convertir eventos en comandos de audio.
       No debe modificar la posicion ni decidir si el salto fue valido. */
}
BonesSubsystem ear_example_descriptor(EarExampleState *state) {
    BonesSubsystem ear = {0};
    ear.version = BONES_SUBSYSTEM_API_VERSION;
    ear.struct_size = (uint32_t)sizeof(ear);
    ear.slot = BONES_SLOT_EAR;
    ear.context = state;
    ear.startup = ear_begin;
    ear.on_event = ear_event;
    return ear;
}
