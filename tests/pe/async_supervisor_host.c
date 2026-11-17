/* SPDX-License-Identifier: MIT */
/* Trusted, explicitly opt-in PE32+ integration fixture. Unlike the old
 * external host this executes the Windows entry inside the real direct
 * fork of nt_runtime_run_fixture, with an authenticated parent broker.
 * The executable is the trusted PE fixture built by this test target. */
#define _GNU_SOURCE
#include "image.h"
#include "dll_graph.h"
#include "ntnative/runtime.h"
#include "ntnative/guest_thread.h"
#include "ntnative/guest_boundary_runtime.h"
#include "compat.h"
#include "winbase.h"
#include "sync.h"
#include "../file_share_cleanup.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct pe_image loaded;
static struct pe_dll_graph *graph;
static struct nt_guest_tls initial_tls;

static uintptr_t resolve(const char *module, const char *name,
                         uint16_t ordinal, void *opaque)
{
    return pe_dll_graph_resolve(module, name, ordinal, opaque);
}

static bool read_fixture(const char *path, unsigned char **bytes, size_t *length)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info;
    const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && info.st_size <= PE_IMAGE_MAX_BYTES;
    if (!valid) { close(fd); return false; }
    *length = (size_t)info.st_size;
    *bytes = malloc(*length);
    if (!*bytes) { close(fd); return false; }
    size_t offset = 0;
    while (offset < *length) {
        ssize_t n = read(fd, *bytes + offset, *length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    close(fd);
    if (offset == *length) return true;
    free(*bytes);
    *bytes = NULL;
    return false;
}

static int NT_MS_ABI guest_entry(void)
{
    struct nt_guest_thread thread = {0};
    if (!nt_runtime_enter_guest_thread(&thread, (uintptr_t)loaded.base, &initial_tls))
        return 90;
    if (!pe_dll_graph_attach_all(graph))
        return 91;
    nt_runtime_notify_tls(1);
    nt_guest_boundary_guest_enter();
    const uintptr_t address = (uintptr_t)loaded.base + loaded.entry_rva;
    int (NT_MS_ABI *main_entry)(void) = NULL;
    memcpy(&main_entry, &address, sizeof(main_entry));
    const int result = main_entry();
    nt_guest_boundary_guest_leave();
    nt_runtime_notify_tls(0);
    pe_dll_graph_detach_all(graph);
    nt_runtime_run_fls_cleanup();
    if (!nt_guest_thread_leave(&thread))
        return 92;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("Requires exactly one BUILD-GENERATED PE fixture\n", stderr);
        return 2;
    }
    unsigned char *bytes = NULL;
    size_t length = 0;
    if (!read_fixture(argv[1], &bytes, &length))
        return 3;
    const char *slash = strrchr(argv[1], '/');
    char directory[4096];
    const size_t bytes_dir = slash ? (size_t)(slash - argv[1]) : 0;
    if (bytes_dir >= sizeof(directory)) return 4;
    if (bytes_dir == 0) {
        memcpy(directory, ".", 2);
    } else {
        memcpy(directory, argv[1], bytes_dir);
        directory[bytes_dir] = 0;
    }
    graph = pe_dll_graph_create(directory);
    if (!graph) return 5;
    pe_dll_graph_install(graph);
    nt_guest_boundary_enable_import_gate();
    char error[192] = {0};
    const bool mapped = pe_image_load(bytes, length, resolve, graph,
                                     &loaded, error, sizeof(error));
    free(bytes);
    if (!mapped) {
        fprintf(stderr, "FAIL async PE image: %s\n", error);
        return 6;
    }
    if (loaded.code_size == 0 ||
        !nt_guest_boundary_set_image_range(
            (uintptr_t)loaded.base + loaded.code_rva,
            (uintptr_t)loaded.base + loaded.code_rva + loaded.code_size))
        return 7;
    char temp[] = "/tmp/nt-async-supervisor-XXXXXX";
    if (!mkdtemp(temp) || !nt_runtime_set_file_root(temp) ||
        !nt_runtime_set_standard_streams(STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO) ||
        !nt_runtime_set_guest_image((uintptr_t)loaded.base,
                                   "C:\\async-test.exe", "\"C:\\async-test.exe\""))
        return 8;
    nt_runtime_set_guest_image_extent(loaded.size);
    nt_runtime_set_guest_image_entrypoint(loaded.entry_rva);
    initial_tls = (struct nt_guest_tls){
        loaded.base + loaded.tls.data_rva, loaded.tls.bytes, loaded.tls.zero_fill
    };
    nt_runtime_set_guest_tls(&initial_tls,
        loaded.base + loaded.tls.callbacks_rva, loaded.tls.callback_count);
    const char *const environment[] = {"PATH=C:\\", NULL};
    nt_runtime_set_guest_environment(environment);
    nt_runtime_enable_supervisor_ipc();
    nt_runtime_enable_supervisor_thread_control();
    uint32_t code = 0xff;
    const bool ran = nt_runtime_run_fixture(guest_entry, &code);
    if (!ran || code) {
        fprintf(stderr, "FAIL Win32 async PE fixture: ran=%d code=%u\n",
                (int)ran, code);
        return 9;
    }
    pe_image_unload(&loaded);
    pe_dll_graph_uninstall();
    pe_dll_graph_destroy(graph);
    if (!nt_runtime_set_file_root(NULL) ||
        !nt_test_cleanup_share_registry(temp) || rmdir(temp))
        return 10;
    puts("PASS real PE32+ Win32 async Suspend/Resume/CONTEXT via authenticated parent broker");
    return 0;
}
