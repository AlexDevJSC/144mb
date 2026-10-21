#ifndef BONES_MODKIT_HOST_H
#define BONES_MODKIT_HOST_H
/* Host integration source API. Plugins only include modkit.h. */
#include "modkit.h"
#include "foundation.h"
typedef struct BonesModHost BonesModHost;
BonesModHost *bones_mod_host_create(BonesFoundation *foundation);
/* Static modules: query callbacks are linked in, no DLL distributed. */
int32_t bones_mod_host_add(BonesModHost *, BonesModQuery query);
/* Optional developer-mode DLL loader: NOT part of final size-constrained build.
   The host owns the library handle until bones_mod_host_destroy. */
int32_t bones_mod_host_load(BonesModHost *, const char *path);
int32_t bones_mod_host_start(BonesModHost *);
int32_t bones_mod_host_step(BonesModHost *, const BonesInput *local);
int32_t bones_mod_host_frame(BonesModHost *);
void bones_mod_host_stop(BonesModHost *);
void bones_mod_host_destroy(BonesModHost *);
uint32_t bones_mod_host_count(const BonesModHost *);
int32_t bones_mod_host_error(const BonesModHost *);
/* Only generated build-time registry knows installed specialist modules. */
int32_t bones_mod_register_builtins(BonesModHost *);
#endif
