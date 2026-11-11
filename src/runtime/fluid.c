/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "winbase.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct {
#define FLUID(type, name, signature, arguments) type (*name) signature;
#define FLUID_VOID(name, signature, arguments) FLUID(void, name, signature, arguments)
#include "fluid_api.h"
#undef FLUID_VOID
#undef FLUID
    int (*fluid_synth_sfload)(void *, const char *, int);
} fluid;
static bool available;
static pthread_once_t load_once = PTHREAD_ONCE_INIT;

static bool load_symbol(void *library, const char *name, void *target, size_t size)
{
    void *symbol = dlsym(library, name);
    if (symbol == NULL || size != sizeof(symbol))
        return false;
    memcpy(target, &symbol, size);
    return true;
}

static void load_fluid(void)
{
    void *library = dlopen("libfluidsynth.so.3", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
        return;
#define FLUID(type, name, signature, arguments) \
    if (!load_symbol(library, #name, &fluid.name, sizeof(fluid.name))) goto missing;
#define FLUID_VOID(name, signature, arguments) FLUID(void, name, signature, arguments)
#include "fluid_api.h"
    FLUID(int, fluid_synth_sfload, (), ())
#undef FLUID_VOID
#undef FLUID
    available = true;
    return;
missing:
    dlclose(library);
}

bool nt_runtime_fluidsynth_available(void)
{
    return pthread_once(&load_once, load_fluid) == 0 && available;
}

/* The resolver publishes this module only after all symbols are bound. */
#define FLUID(type, name, signature, arguments) \
    type NT_MS_ABI nt_##name signature { return fluid.name arguments; }
#define FLUID_VOID(name, signature, arguments) \
    void NT_MS_ABI nt_##name signature { fluid.name arguments; }
#include "fluid_api.h"
#undef FLUID_VOID
#undef FLUID

int NT_MS_ABI nt_fluid_synth_sfload(void *synth, const char *path, int reset)
{
    const int fd = nt_runtime_open_guest_file_readonly(path);
    if (fd < 0)
        return -1;
    char source[64];
    snprintf(source, sizeof(source), "/proc/self/fd/%d", fd);
    const int result = fluid.fluid_synth_sfload(synth, source, reset);
    close(fd);
    return result;
}
