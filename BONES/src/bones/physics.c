#include "bones_internal.h"
#include <math.h>

static float clampf(float n, float l, float h) { return n < l ? l : (n > h ? h : n); }
/* Upright cylinder approximation against static AABBs, with Y overlap.
   Split horizontal motion into small bounded advances to avoid pass-through
   at specified walk/sprint speeds. X then Z provides simple wall sliding. */
static bool blocked(const BonesWorld *w, const BonesPlayer *p, float x, float z) {
    uint32_t i;
    for (i = 0; i < w->collider_count; ++i) {
        const BonesCollider *b = &w->colliders[i];
        float cx, cz, dx, dz;
        if (!b->solid || p->feet.y >= b->bounds.max.y ||
            p->feet.y + p->height <= b->bounds.min.y) continue;
        cx = clampf(x, b->bounds.min.x, b->bounds.max.x);
        cz = clampf(z, b->bounds.min.z, b->bounds.max.z);
        dx = x - cx; dz = z - cz;
        if (dx*dx + dz*dz < p->radius*p->radius - 0.000001f) return true;
    }
    return false;
}
static void horizontal_move(const BonesWorld *w, BonesPlayer *p, float dt) {
    float dx = p->velocity.x * dt, dz = p->velocity.z * dt;
    float max_move = fmaxf(fabsf(dx), fabsf(dz));
    int steps = (int)ceilf(max_move / 0.08f), j;
    if (steps < 1) steps = 1;
    if (steps > 16) steps = 16; /* safe given fixed dt and limited input speed */
    dx /= (float)steps; dz /= (float)steps;
    for (j = 0; j < steps; ++j) {
        if (!blocked(w, p, p->feet.x + dx, p->feet.z)) p->feet.x += dx;
        else p->velocity.x = 0.0f;
        if (!blocked(w, p, p->feet.x, p->feet.z + dz)) p->feet.z += dz;
        else p->velocity.z = 0.0f;
    }
}
void bones_physics_step_actor(const BonesWorld *world, BonesPlayer *p, const BonesInput *in, float dt) {
    float forward = (float)(!!in->forward) - (float)(!!in->backward);
    float sideways = (float)(!!in->right) - (float)(!!in->left);
    float len, sy, cy, x, z, want_x, want_z, diff_x, diff_z, amount;
    p->yaw += ((float)(!!in->yaw_right) - (float)(!!in->yaw_left)) * 1.85f * dt
              + clampf(in->mouse_dx, -1000.0f, 1000.0f) * .0023f;
    p->pitch += ((float)(!!in->pitch_up) - (float)(!!in->pitch_down)) * 1.85f * dt
                - clampf(in->mouse_dy, -1000.0f, 1000.0f) * .0023f;
    p->pitch = clampf(p->pitch, -1.30f, 1.30f);
    /* Wrap yaw to avoid numerical loss of precision over long play sessions. */
    if (p->yaw > 3.14159265f || p->yaw < -3.14159265f)
        p->yaw = remainderf(p->yaw, 6.28318530f);
    len = sqrtf(forward * forward + sideways * sideways);
    if (len > 1.0f) { forward /= len; sideways /= len; }
    sy = sinf(p->yaw); cy = cosf(p->yaw);
    x = sy*forward + cy*sideways;
    z = cy*forward - sy*sideways;
    want_x = x * (in->sprint ? 5.5f : 3.0f);
    want_z = z * (in->sprint ? 5.5f : 3.0f);
    diff_x = want_x - p->velocity.x;
    diff_z = want_z - p->velocity.z;
    len = sqrtf(diff_x*diff_x + diff_z*diff_z);
    amount = (forward != 0.0f || sideways != 0.0f ? 20.0f : 26.0f)*dt;
    if (len > amount) { diff_x *= amount/len; diff_z *= amount/len; }
    p->velocity.x += diff_x;
    p->velocity.z += diff_z;
    if (in->jump && !p->jump_held && p->grounded) {
        p->velocity.y = 5.0f; p->grounded = false;
    }
    p->jump_held = in->jump != 0;
    p->velocity.y -= 14.0f * dt;
    p->feet.y += p->velocity.y * dt;
    if (p->feet.y <= 0.0f) {
        p->feet.y = 0.0f; p->velocity.y = 0.0f; p->grounded = true;
    }
    horizontal_move(world, p, dt);
}
void bones_physics_update(BonesEngine *e, const BonesInput *in, float dt) {
    bool was_grounded=e->player.grounded;
    bones_physics_step_actor(&e->world, &e->player, in, dt);
    if (was_grounded && !e->player.grounded) {
        const BonesEvent evt={BONES_EVENT_JUMP,1u,0u};
        (void)bones_emit_event(e,&evt);
    } else if (!was_grounded && e->player.grounded) {
        const BonesEvent evt={BONES_EVENT_LAND,1u,0u};
        (void)bones_emit_event(e,&evt);
    }
}
