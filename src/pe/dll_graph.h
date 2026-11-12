/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_PE_DLL_GRAPH_H
#define NTNATIVE_PE_DLL_GRAPH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pe_dll_graph;

/* Directory must have been provisioned by the Linux launcher. Guest DLL
 * lookups are bounded to single regular filenames beneath this directory. */
struct pe_dll_graph *pe_dll_graph_create(const char *directory);
void pe_dll_graph_destroy(struct pe_dll_graph *graph);

/* Install/uninstall this graph as the provider of mapped guest DLLs for the
 * existing NTDLL/KERNEL32/KERNELBASE export registry. */
void pe_dll_graph_install(struct pe_dll_graph *graph);
void pe_dll_graph_uninstall(void);

uintptr_t pe_dll_graph_resolve(const char *module, const char *name,
                               uint16_t ordinal, void *graph);

/* Must be invoked from a guest thread after it has a valid GS/TEB and PEB;
 * the loader recursively attaches dependencies before their importers. */
bool pe_dll_graph_attach_all(struct pe_dll_graph *graph);
void pe_dll_graph_detach_all(struct pe_dll_graph *graph);
#endif
