/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "dll_graph.h"
#include "image.h"
#include "ntnative/runtime.h"
#include "../runtime/winbase.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_DLLS = 64, MAX_DLL_NAME = 128 };
struct graph_module {
    bool present, loading, attaching, attached, registered, pinned;
    unsigned int references;
    uint64_t attach_order;
    uint32_t tls_index; /* Never reused while graph lives; 0 belongs to EXE. */
    char name[MAX_DLL_NAME];
    char dos_path[MAX_DLL_NAME + 4];
    struct pe_image image;
    struct graph_module *dependencies[MAX_DLLS];
    size_t dependency_count;
};
struct pe_dll_graph {
    int directory_fd;
    pthread_mutex_t lock; /* Recursive for DLL imports and DllMain callbacks. */
    struct graph_module modules[MAX_DLLS];
    struct graph_module *loading;
    unsigned int resolve_depth;
    uint64_t next_attach_order;
    uint32_t next_tls_index;
};

static bool valid_module_name(const char *input, char output[MAX_DLL_NAME])
{
    if (input == NULL)
        return false;
    size_t n = strnlen(input, MAX_DLL_NAME);
    if (n == 0 || n >= MAX_DLL_NAME || input[0] == '.' || strstr(input, "..") != NULL)
        return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)input[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.'))
            return false;
    }
    const char *dot = strrchr(input, '.');
    if (dot != NULL && strcasecmp(dot, ".dll") != 0)
        return false;
    if (dot == NULL) {
        if (n > MAX_DLL_NAME - sizeof(".dll"))
            return false;
        memcpy(output, input, n);
        memcpy(output + n, ".dll", sizeof(".dll"));
    } else {
        memcpy(output, input, n + 1);
    }
    return true;
}

static bool reserved_module(const char *name)
{
    static const char *const reserved[] = {
        "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll",
        "gdi32.dll", "advapi32.dll", "ole32.dll", "shell32.dll",
        "comdlg32.dll", "ws2_32.dll", "ucrtbase.dll", "msvcrt.dll"
    };
    if (strncasecmp(name, "api-ms-win-", 11) == 0 ||
        strncasecmp(name, "ext-ms-win-", 11) == 0)
        return true;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        if (strcasecmp(name, reserved[i]) == 0)
            return true;
    return false;
}

static struct graph_module *find_module(struct pe_dll_graph *graph, const char *name)
{
    for (size_t i = 0; i < MAX_DLLS; ++i)
        if (graph->modules[i].present && strcasecmp(graph->modules[i].name, name) == 0)
            return &graph->modules[i];
    return NULL;
}

static struct graph_module *find_base(struct pe_dll_graph *graph, uintptr_t base)
{
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        struct graph_module *module = &graph->modules[i];
        if (module->present && !module->loading && (uintptr_t)module->image.base == base)
            return module;
    }
    return NULL;
}

static void track_dependency(struct pe_dll_graph *graph, struct graph_module *dependency)
{
    struct graph_module *parent = graph->loading;
    if (parent == NULL || dependency == parent)
        return;
    for (size_t i = 0; i < parent->dependency_count; ++i)
        if (parent->dependencies[i] == dependency)
            return;
    if (parent->dependency_count < MAX_DLLS)
        parent->dependencies[parent->dependency_count++] = dependency;
}

/* Linux filenames are case-sensitive while Windows DLL module names
 * (under this intentionally ASCII-only contract) are not. Enumerate the
 * already-opened directory only on ENOENT, require a unique matching name,
 * and still open the result via O_NOFOLLOW beneath the same directory fd.
 * Never follow symlinks, escape to a host directory, or guess between two
 * differently-cased files with the same Windows module name. */
static int open_dll_case_insensitive(int directory_fd, const char *name)
{
    int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 || errno != ENOENT)
        return fd;

    int listing_fd = openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (listing_fd < 0)
        return -1;
    DIR *listing = fdopendir(listing_fd);
    if (listing == NULL) {
        const int error = errno;
        close(listing_fd);
        errno = error;
        return -1;
    }
    char candidate[MAX_DLL_NAME] = {0};
    bool ambiguous = false;
    struct dirent *item;
    while ((item = readdir(listing)) != NULL) {
        const size_t length = strnlen(item->d_name, MAX_DLL_NAME);
        if (length == 0 || length >= MAX_DLL_NAME || strcasecmp(item->d_name, name) != 0)
            continue;
        char validated[MAX_DLL_NAME];
        if (!valid_module_name(item->d_name, validated))
            continue;
        if (candidate[0] != 0) {
            ambiguous = true;
            break;
        }
        memcpy(candidate, item->d_name, length + 1);
    }
    closedir(listing);
    if (ambiguous) {
        errno = EEXIST;
        return -1;
    }
    if (candidate[0] == 0) {
        errno = ENOENT;
        return -1;
    }
    return openat(directory_fd, candidate, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

/* Only a single pre-opened root can supply guest libraries; Windows guest
 * names can never escape into arbitrary host folders or follow symlinks. */
static bool read_dll(struct pe_dll_graph *graph, const char *name,
                     unsigned char **bytes, size_t *size)
{
    const int fd = open_dll_case_insensitive(graph->directory_fd, name);
    if (fd < 0)
        return false;
    struct stat info;
    const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                         info.st_size > 0 && info.st_size <= PE_IMAGE_MAX_BYTES;
    if (!regular) {
        close(fd);
        return false;
    }
    *size = (size_t)info.st_size;
    unsigned char *data = malloc(*size);
    if (data == NULL) {
        close(fd);
        return false;
    }
    size_t done = 0;
    while (done < *size) {
        const ssize_t got = read(fd, data + done, *size - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        done += (size_t)got;
    }
    close(fd);
    if (done != *size) {
        free(data);
        return false;
    }
    *bytes = data;
    return true;
}

static bool attach_module(struct pe_dll_graph *graph, struct graph_module *module)
{
    if (!module->present || module->loading)
        return false;
    if (module->attached)
        return true;
    if (module->attaching)
        return false; /* A dependency cycle cannot be initialized safely. */
    module->attaching = true;
    for (size_t i = 0; i < module->dependency_count; ++i)
        if (!attach_module(graph, module->dependencies[i])) {
            module->attaching = false;
            return false;
        }
    const uintptr_t address = (uintptr_t)module->image.base;
    const uintptr_t entry = module->image.entry_rva != 0 ?
                            address + module->image.entry_rva : 0;
    if (!nt_runtime_guest_register_dll(address, module->image.size, entry, module->dos_path)) {
        module->attaching = false;
        return false;
    }
    module->registered = true;
    if (!pe_image_run_tls_callbacks(&module->image, 1) ||
        !pe_image_run_library_entry(&module->image, 1)) {
        nt_runtime_guest_unregister_dll(address);
        module->registered = false;
        module->attaching = false;
        return false;
    }
    module->attached = true;
    module->attach_order = ++graph->next_attach_order;
    module->attaching = false;
    return true;
}

static struct graph_module *load_module(struct pe_dll_graph *graph,
                                         const char *requested, bool explicit_reference)
{
    char name[MAX_DLL_NAME];
    if (!valid_module_name(requested, name) || reserved_module(name))
        return NULL;
    struct graph_module *module = find_module(graph, name);
    if (module != NULL) {
        if (module->loading)
            return NULL; /* No partially constructed IATs in cycles. */
        if (explicit_reference && module->references != UINT32_MAX)
            ++module->references;
        else if (!explicit_reference)
            module->pinned = true;
        track_dependency(graph, module);
        return module;
    }
    for (size_t i = 0; i < MAX_DLLS; ++i)
        if (!graph->modules[i].present) {
            module = &graph->modules[i];
            break;
        }
    if (module == NULL)
        return NULL;
    *module = (struct graph_module){.present = true, .loading = true,
                                     .pinned = !explicit_reference,
                                     .references = explicit_reference ? 1u : 0u};
    memcpy(module->name, name, strlen(name) + 1);
    snprintf(module->dos_path, sizeof(module->dos_path), "C:\\%s", name);

    unsigned char *bytes = NULL;
    size_t length = 0;
    if (!read_dll(graph, name, &bytes, &length)) {
        *module = (struct graph_module){0};
        return NULL;
    }
    struct graph_module *parent = graph->loading;
    graph->loading = module;
    char reason[160] = {0};
    const bool loaded = pe_image_load_library(bytes, length,
             pe_dll_graph_resolve, graph, &module->image, reason, sizeof(reason));
    graph->loading = parent;
    free(bytes);
    if (!loaded) {
        fprintf(stderr, "Guest PE DLL %s: %s\n", name, reason);
        *module = (struct graph_module){0};
        return NULL;
    }
    /* A pre-entry import can reserve its own static TLS slot. Loading a
     * new TLS DLL after guest threads have started requires stop-the-world
     * thread initialization and is deliberately unsupported here. Pin all
     * such startup DLLs so another thread can never retain a stale pointer
     * to a freed TLS template or callback. */
    if (module->image.tls.present) {
        if (nt_guest_thread_teb() != NULL || graph->next_tls_index >= 64) {
            fprintf(stderr, "Guest PE DLL %s: static TLS only supported for startup imports\n", name);
            pe_image_unload(&module->image);
            *module = (struct graph_module){0};
            return NULL;
        }
        module->tls_index = graph->next_tls_index++;
        memcpy(module->image.base + module->image.tls.index_rva,
               &module->tls_index, sizeof(module->tls_index));
        module->pinned = true;
    }
    module->loading = false;
    track_dependency(graph, module);
    return module;
}

uintptr_t pe_dll_graph_resolve(const char *module_name, const char *name,
                               uint16_t ordinal, void *context)
{
    struct pe_dll_graph *graph = context;
    nt_guest_proc built_in = name != NULL ? nt_runtime_resolve(module_name, name) :
                              nt_runtime_resolve_ordinal(module_name, ordinal);
    if (built_in != NULL) {
        uintptr_t result = 0;
        memcpy(&result, &built_in, sizeof(result));
        return result;
    }
    if (graph == NULL)
        return 0;
    pthread_mutex_lock(&graph->lock);
    uintptr_t result = 0;
    if (graph->resolve_depth++ < MAX_DLLS) {
        struct graph_module *module = load_module(graph, module_name, false);
        if (module != NULL)
            result = 0; /* Query exports only after mapping is complete. */
        if (module != NULL)
            (void)pe_image_resolve_export(&module->image, name, ordinal,
                                         pe_dll_graph_resolve, graph, &result);
    }
    --graph->resolve_depth;
    pthread_mutex_unlock(&graph->lock);
    return result;
}

static uintptr_t backend_load(void *context, const char *name)
{
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    struct graph_module *module = load_module(graph, name, true);
    uintptr_t result = module != NULL ? (uintptr_t)module->image.base : 0;
    if (result != 0 && nt_guest_thread_teb() != NULL && !attach_module(graph, module))
        result = 0;
    pthread_mutex_unlock(&graph->lock);
    return result;
}

static uintptr_t backend_find(void *context, const char *name)
{
    struct pe_dll_graph *graph = context;
    char normalized[MAX_DLL_NAME];
    if (!valid_module_name(name, normalized))
        return 0;
    pthread_mutex_lock(&graph->lock);
    struct graph_module *module = find_module(graph, normalized);
    const uintptr_t result = module != NULL && !module->loading ?
                             (uintptr_t)module->image.base : 0;
    pthread_mutex_unlock(&graph->lock);
    return result;
}

static uintptr_t backend_getproc(void *context, uintptr_t base, const char *name, uint16_t ordinal)
{
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    struct graph_module *module = find_base(graph, base);
    uintptr_t address = 0;
    if (module != NULL)
        (void)pe_image_resolve_export(&module->image, name, ordinal,
                                     pe_dll_graph_resolve, graph, &address);
    pthread_mutex_unlock(&graph->lock);
    return address;
}

static bool backend_release(void *context, uintptr_t base)
{
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    struct graph_module *module = find_base(graph, base);
    const bool valid = module != NULL;
    if (valid && module->references > 0)
        --module->references;
    if (valid && module->references == 0 && !module->pinned) {
        if (module->attached) {
            (void)pe_image_run_tls_callbacks(&module->image, 0);
            (void)pe_image_run_library_entry(&module->image, 0);
            module->attached = false;
        }
        if (module->registered) {
            nt_runtime_guest_unregister_dll(base);
            module->registered = false;
        }
        pe_image_unload(&module->image);
        *module = (struct graph_module){0};
    }
    pthread_mutex_unlock(&graph->lock);
    return valid;
}

static uintptr_t backend_at(void *context, uintptr_t address)
{
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    uintptr_t base = 0;
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        const struct graph_module *module = &graph->modules[i];
        if (module->present && !module->loading && module->image.base != NULL &&
            address >= (uintptr_t)module->image.base &&
            address - (uintptr_t)module->image.base < module->image.size) {
            base = (uintptr_t)module->image.base;
            break;
        }
    }
    pthread_mutex_unlock(&graph->lock);
    return base;
}

static const char *backend_filename(void *context, uintptr_t base)
{
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    struct graph_module *module = find_base(graph, base);
    /* Names live in fixed slots until process shutdown or unload. */
    const char *name = module != NULL ? module->dos_path : NULL;
    pthread_mutex_unlock(&graph->lock);
    return name;
}

static void backend_thread_event(void *context, uint32_t reason)
{
    if (reason != 2 && reason != 3)
        return;
    struct pe_dll_graph *graph = context;
    pthread_mutex_lock(&graph->lock);
    uint64_t boundary = reason == 2 ? 0 : UINT64_MAX;
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        struct graph_module *selected = NULL;
        for (size_t j = 0; j < MAX_DLLS; ++j) {
            struct graph_module *candidate = &graph->modules[j];
            if (!candidate->present || !candidate->attached ||
                (reason == 2 ? candidate->attach_order <= boundary :
                               candidate->attach_order >= boundary))
                continue;
            if (selected == NULL ||
                (reason == 2 ? candidate->attach_order < selected->attach_order :
                               candidate->attach_order > selected->attach_order))
                selected = candidate;
        }
        if (selected == NULL)
            break;
        boundary = selected->attach_order;
        (void)pe_image_run_tls_callbacks(&selected->image, reason);
        (void)pe_image_run_library_entry(&selected->image, reason);
    }
    pthread_mutex_unlock(&graph->lock);
}

static bool backend_prepare_thread(void *context, struct nt_guest_thread *thread)
{
    struct pe_dll_graph *graph = context;
    if (graph == NULL || thread == NULL)
        return false;
    pthread_mutex_lock(&graph->lock);
    bool okay = true;
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        const struct graph_module *module = &graph->modules[i];
        if (module->present && !module->loading && module->tls_index != 0 &&
            !nt_guest_thread_set_static_tls(thread, module->tls_index,
                module->image.base + module->image.tls.data_rva,
                module->image.tls.bytes, module->image.tls.zero_fill)) {
            okay = false;
            break;
        }
    }
    pthread_mutex_unlock(&graph->lock);
    return okay;
}

static const struct nt_external_module_backend host_module_backend = {
    .load = backend_load, .find = backend_find, .getproc = backend_getproc,
    .release = backend_release, .at = backend_at, .filename = backend_filename,
    .thread_event = backend_thread_event,
    .prepare_thread = backend_prepare_thread
};

void pe_dll_graph_install(struct pe_dll_graph *graph)
{
    struct nt_external_module_backend backend = host_module_backend;
    backend.context = graph;
    nt_runtime_set_external_module_backend(graph != NULL ? &backend : NULL);
}

void pe_dll_graph_uninstall(void)
{
    nt_runtime_set_external_module_backend(NULL);
}

struct pe_dll_graph *pe_dll_graph_create(const char *directory)
{
    if (directory == NULL)
        return NULL;
    const int fd = open(directory, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct pe_dll_graph *graph = calloc(1, sizeof(*graph));
    if (graph == NULL) {
        close(fd);
        return NULL;
    }
    pthread_mutexattr_t attrs;
    if (pthread_mutexattr_init(&attrs) != 0) {
        close(fd);
        free(graph);
        return NULL;
    }
    const int type_error = pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_RECURSIVE);
    const int lock_error = type_error == 0 ? pthread_mutex_init(&graph->lock, &attrs) : type_error;
    pthread_mutexattr_destroy(&attrs);
    if (lock_error != 0) {
        close(fd);
        free(graph);
        return NULL;
    }
    graph->directory_fd = fd;
    graph->next_tls_index = 1; /* EXE static TLS is always index zero. */
    return graph;
}

bool pe_dll_graph_attach_all(struct pe_dll_graph *graph)
{
    if (graph == NULL || nt_guest_thread_teb() == NULL)
        return false;
    pthread_mutex_lock(&graph->lock);
    bool okay = true;
    for (size_t i = 0; i < MAX_DLLS; ++i)
        if (graph->modules[i].present && !attach_module(graph, &graph->modules[i])) {
            okay = false;
            break;
        }
    pthread_mutex_unlock(&graph->lock);
    return okay;
}

void pe_dll_graph_detach_all(struct pe_dll_graph *graph)
{
    if (graph == NULL)
        return;
    pthread_mutex_lock(&graph->lock);
    uint64_t boundary = UINT64_MAX;
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        struct graph_module *selected = NULL;
        for (size_t j = 0; j < MAX_DLLS; ++j) {
            struct graph_module *candidate = &graph->modules[j];
            if (!candidate->present || !candidate->attached ||
                candidate->attach_order >= boundary)
                continue;
            if (selected == NULL || candidate->attach_order > selected->attach_order)
                selected = candidate;
        }
        if (selected == NULL)
            break;
        boundary = selected->attach_order;
        (void)pe_image_run_tls_callbacks(&selected->image, 0);
        (void)pe_image_run_library_entry(&selected->image, 0);
        selected->attached = false;
    }
    for (size_t i = 0; i < MAX_DLLS; ++i) {
        struct graph_module *module = &graph->modules[i];
        if (module->present && module->registered) {
            nt_runtime_guest_unregister_dll((uintptr_t)module->image.base);
            module->registered = false;
        }
    }
    pthread_mutex_unlock(&graph->lock);
}

void pe_dll_graph_destroy(struct pe_dll_graph *graph)
{
    if (graph == NULL)
        return;
    pe_dll_graph_detach_all(graph);
    for (size_t i = 0; i < MAX_DLLS; ++i)
        if (graph->modules[i].present && !graph->modules[i].loading)
            pe_image_unload(&graph->modules[i].image);
    pthread_mutex_destroy(&graph->lock);
    close(graph->directory_fd);
    free(graph);
}
