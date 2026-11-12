/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "virtual_memory.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* This host is explicitly Linux x86-64. Windows page size and reservation
 * granularity are 4 KiB and 64 KiB respectively. Do not pretend that a
 * different host page size can implement the same guest contract. */
enum {
    VM_PAGE = 4096,
    VM_GRANULARITY = 65536,
    VM_COMMIT = 0x1000,
    VM_RESERVE = 0x2000,
    VM_DECOMMIT = 0x4000,
    VM_RELEASE = 0x8000,
    VM_PRIVATE = 0x20000,
    VM_NOACCESS = 1,
    VM_READONLY = 2,
    VM_READWRITE = 4,
    VM_EXECUTE = 0x10,
    VM_EXECUTE_READ = 0x20,
};

_Static_assert(sizeof(struct nt_memory_basic_information) == 48, "MEMORY_BASIC_INFORMATION x64 size");
_Static_assert(offsetof(struct nt_memory_basic_information, region_size) == 24, "MEMORY_BASIC_INFORMATION RegionSize");
_Static_assert(offsetof(struct nt_memory_basic_information, state) == 32, "MEMORY_BASIC_INFORMATION State");
_Static_assert(offsetof(struct nt_memory_basic_information, type) == 40, "MEMORY_BASIC_INFORMATION Type");

struct memory_region {
    struct memory_region *next;
    unsigned char *base;
    size_t size;
    uint32_t allocation_protect;
    /* A zero byte is reserved, otherwise the byte is the committed PAGE_* value. */
    uint8_t *page_protect;
};

static struct memory_region *regions;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static bool compatible_page_size(void)
{
    return sysconf(_SC_PAGESIZE) == VM_PAGE;
}

static bool round_pages(size_t amount, size_t *rounded)
{
    if (amount == 0 || amount > SIZE_MAX - (VM_PAGE - 1))
        return false;
    *rounded = (amount + (VM_PAGE - 1)) & ~(size_t)(VM_PAGE - 1);
    return true;
}

static bool native_protection(uint32_t requested, int *result)
{
    switch (requested) {
    case VM_NOACCESS:     *result = PROT_NONE; break;
    case VM_READONLY:     *result = PROT_READ; break;
    case VM_READWRITE:    *result = PROT_READ | PROT_WRITE; break;
    case VM_EXECUTE:      *result = PROT_EXEC; break;
    case VM_EXECUTE_READ: *result = PROT_READ | PROT_EXEC; break;
    default:
        return false; /* Never grant write+execute in the guest. */
    }
    return true;
}

static struct memory_region *find_locked(uintptr_t address)
{
    for (struct memory_region *item = regions; item != NULL; item = item->next) {
        const uintptr_t base = (uintptr_t)item->base;
        if (address >= base && address - base < item->size)
            return item;
    }
    return NULL;
}

static bool range_inside(const struct memory_region *region, uintptr_t address, size_t length)
{
    const uintptr_t base = (uintptr_t)region->base;
    return length != 0 && address >= base &&
           address - base < region->size &&
           length <= region->size - (address - base);
}

static void *reserve_region(void *requested, size_t size)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (requested != NULL) {
        if ((uintptr_t)requested % VM_GRANULARITY != 0)
            return MAP_FAILED;
        return mmap(requested, size, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
    }
    if (size > SIZE_MAX - VM_GRANULARITY)
        return MAP_FAILED;
    const size_t oversized = size + VM_GRANULARITY;
    void *mapped = mmap(NULL, oversized, PROT_NONE, flags, -1, 0);
    if (mapped == MAP_FAILED)
        return MAP_FAILED;
    const uintptr_t raw = (uintptr_t)mapped;
    const uintptr_t aligned = (raw + VM_GRANULARITY - 1) & ~(uintptr_t)(VM_GRANULARITY - 1);
    const size_t prefix = aligned - raw;
    const size_t suffix = oversized - prefix - size;
    if (prefix != 0)
        (void)munmap(mapped, prefix);
    if (suffix != 0)
        (void)munmap((void *)(aligned + size), suffix);
    return (void *)aligned;
}

static bool update_commit(struct memory_region *region, uintptr_t address, size_t size,
                          uint32_t protection, int native)
{
    if (!range_inside(region, address, size) || address % VM_PAGE != 0)
        return false;
    if (mprotect((void *)address, size, native) != 0)
        return false;
    const size_t index = (address - (uintptr_t)region->base) / VM_PAGE;
    for (size_t page = 0; page < size / VM_PAGE; ++page)
        region->page_protect[index + page] = (uint8_t)protection;
    return true;
}

void *NT_MS_ABI nt_VirtualAlloc(void *address, size_t size,
                               uint32_t allocation_type, uint32_t protection)
{
    size_t rounded = 0;
    int native = 0;
    if (!round_pages(size, &rounded) ||
        (allocation_type & ~(VM_RESERVE | VM_COMMIT)) != 0 ||
        (allocation_type & (VM_RESERVE | VM_COMMIT)) == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!native_protection(protection, &native)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (!compatible_page_size()) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return NULL;
    }

    /* MEM_COMMIT with a NULL address implicitly reserves a new allocation. */
    if (address == NULL)
        allocation_type |= VM_RESERVE;
    if ((allocation_type & VM_RESERVE) != 0) {
        if (address != NULL && (uintptr_t)address % VM_GRANULARITY != 0) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return NULL;
        }
        struct memory_region *region = calloc(1, sizeof(*region));
        if (region == NULL) {
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        region->page_protect = calloc(rounded / VM_PAGE, 1);
        if (region->page_protect == NULL) {
            free(region);
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        void *base = reserve_region(address, rounded);
        if (base == MAP_FAILED) {
            free(region->page_protect);
            free(region);
            nt_SetLastError(address == NULL ? NT_ERROR_NOT_ENOUGH_MEMORY : NT_ERROR_INVALID_ADDRESS);
            return NULL;
        }
        region->base = base;
        region->size = rounded;
        region->allocation_protect = protection;
        pthread_mutex_lock(&memory_lock);
        if ((allocation_type & VM_COMMIT) != 0 &&
            !update_commit(region, (uintptr_t)base, rounded, protection, native)) {
            pthread_mutex_unlock(&memory_lock);
            munmap(base, rounded);
            free(region->page_protect);
            free(region);
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        region->next = regions;
        regions = region;
        pthread_mutex_unlock(&memory_lock);
        return base;
    }

    /* An existing reservation is required when MEM_COMMIT specifies an address. */
    const uintptr_t start = (uintptr_t)address;
    pthread_mutex_lock(&memory_lock);
    struct memory_region *region = find_locked(start);
    const bool ok = region != NULL && update_commit(region, start, rounded, protection, native);
    pthread_mutex_unlock(&memory_lock);
    if (!ok) {
        nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
        return NULL;
    }
    return address;
}

int32_t NT_MS_ABI nt_VirtualFree(void *address, size_t size, uint32_t free_type)
{
    const uintptr_t start = (uintptr_t)address;
    if (address == NULL || !compatible_page_size() ||
        (free_type != VM_RELEASE && free_type != VM_DECOMMIT) ||
        (free_type == VM_RELEASE && size != 0) ||
        (free_type == VM_DECOMMIT && (size == 0 || start % VM_PAGE != 0))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t rounded = 0;
    if (free_type == VM_DECOMMIT && !round_pages(size, &rounded)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&memory_lock);
    struct memory_region **link = &regions;
    while (*link && !range_inside(*link, start, 1))
        link = &(*link)->next;
    struct memory_region *region = *link;
    if (region == NULL || (free_type == VM_RELEASE && address != region->base) ||
        (free_type == VM_DECOMMIT && !range_inside(region, start, rounded))) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
        return 0;
    }
    if (free_type == VM_RELEASE) {
        if (munmap(region->base, region->size) != 0) {
            pthread_mutex_unlock(&memory_lock);
            nt_SetLastError(NT_ERROR_GEN_FAILURE);
            return 0;
        }
        *link = region->next;
        pthread_mutex_unlock(&memory_lock);
        free(region->page_protect);
        free(region);
        return 1;
    }
    /* Change access first. madvise then ensures recommitted pages are zero. */
    if (mprotect(address, rounded, PROT_NONE) != 0) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    const int discarded = madvise(address, rounded, MADV_DONTNEED);
    const size_t index = (start - (uintptr_t)region->base) / VM_PAGE;
    memset(region->page_protect + index, 0, rounded / VM_PAGE);
    pthread_mutex_unlock(&memory_lock);
    if (discarded != 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

int nt_virtual_protect_managed(void *address, size_t size, uint32_t protection, uint32_t *previous)
{
    if (address == NULL)
        return -1;
    const uintptr_t start = (uintptr_t)address;
    pthread_mutex_lock(&memory_lock);
    struct memory_region *region = find_locked(start);
    if (region == NULL) {
        pthread_mutex_unlock(&memory_lock);
        return -1;
    }
    size_t rounded = 0;
    int native = 0;
    /* VirtualProtect rounds the affected range down/up to page boundaries. */
    const uintptr_t aligned = start & ~(uintptr_t)(VM_PAGE - 1);
    const size_t delta = start - aligned;
    if (size == 0 || size > SIZE_MAX - delta ||
        !round_pages(size + delta, &rounded) ||
        !range_inside(region, aligned, rounded)) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
        return 0;
    }
    if (!native_protection(protection, &native)) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const size_t index = (aligned - (uintptr_t)region->base) / VM_PAGE;
    for (size_t page = 0; page < rounded / VM_PAGE; ++page) {
        if (region->page_protect[index + page] == 0) {
            pthread_mutex_unlock(&memory_lock);
            nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
            return 0;
        }
    }
    const uint32_t old = region->page_protect[index];
    if (mprotect((void *)aligned, rounded, native) != 0) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    for (size_t page = 0; page < rounded / VM_PAGE; ++page)
        region->page_protect[index + page] = (uint8_t)protection;
    pthread_mutex_unlock(&memory_lock);
    *previous = old;
    return 1;
}

size_t NT_MS_ABI nt_VirtualQuery(const void *address, struct nt_memory_basic_information *info,
                                 size_t length)
{
    if (info == NULL || length < sizeof(*info) || address == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uintptr_t at = (uintptr_t)address;
    pthread_mutex_lock(&memory_lock);
    const struct memory_region *region = find_locked(at);
    if (region == NULL) {
        pthread_mutex_unlock(&memory_lock);
        nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
        return 0;
    }
    const size_t page_count = region->size / VM_PAGE;
    size_t first = (at - (uintptr_t)region->base) / VM_PAGE;
    const uint8_t protection = region->page_protect[first];
    size_t last = first + 1;
    while (first > 0 && region->page_protect[first - 1] == protection)
        --first;
    while (last < page_count && region->page_protect[last] == protection)
        ++last;
    struct nt_memory_basic_information result = {0};
    result.base_address = region->base + first * VM_PAGE;
    result.allocation_base = region->base;
    result.allocation_protect = region->allocation_protect;
    result.region_size = (last - first) * VM_PAGE;
    result.state = protection == 0 ? VM_RESERVE : VM_COMMIT;
    result.protect = protection;
    result.type = VM_PRIVATE;
    memcpy(info, &result, sizeof(result));
    pthread_mutex_unlock(&memory_lock);
    return sizeof(result);
}
