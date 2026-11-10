/* SPDX-License-Identifier: MIT */
#include "compat.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

_Static_assert(sizeof(struct nt_context64) == 1232, "AMD64 CONTEXT size");
_Static_assert(_Alignof(struct nt_context64) == 16, "AMD64 CONTEXT alignment");
_Static_assert(offsetof(struct nt_context64, flags) == 0x30, "AMD64 CONTEXT flags");
_Static_assert(offsetof(struct nt_context64, rax) == 0x78, "AMD64 CONTEXT Rax");
_Static_assert(offsetof(struct nt_context64, rsp) == 0x98, "AMD64 CONTEXT Rsp");
_Static_assert(offsetof(struct nt_context64, rip) == 0xf8, "AMD64 CONTEXT Rip");
_Static_assert(offsetof(struct nt_context64, floating) == 0x100, "AMD64 CONTEXT FltSave");
_Static_assert(sizeof(struct nt_runtime_function) == 12, "RUNTIME_FUNCTION size");

/* The launcher sets the immutable image view before running the guest. */
static struct {
    uintptr_t base;
    size_t image_size;
    const struct nt_runtime_function *table;
    size_t count;
} image;

bool nt_runtime_set_runtime_functions(uintptr_t base, size_t image_size,
                                      const struct nt_runtime_function *table, size_t count)
{
    if (base == 0) {
        if (count != 0)
            return false;
        image.base = 0;
        image.image_size = 0;
        image.table = NULL;
        image.count = 0;
        return true;
    }
    if (image_size < 4 || image_size > UINT32_MAX || image_size > UINTPTR_MAX - base ||
        table == NULL || count == 0 || count > 65536)
        return false;
    uint32_t previous_end = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct nt_runtime_function *entry = &table[i];
        if (entry->begin < previous_end || entry->begin >= entry->end || entry->end > image_size ||
            entry->unwind == 0 || (entry->unwind & 3) != 0 || entry->unwind > image_size - 4)
            return false;
        previous_end = entry->end;
    }
    image.base = base;
    image.image_size = image_size;
    image.table = table;
    image.count = count;
    return true;
}

const struct nt_runtime_function *NT_MS_ABI nt_RtlLookupFunctionEntry(
    uint64_t pc, uint64_t *base, void *history)
{
    (void)history;
    if (base == NULL)
        return NULL;
    *base = 0;
    if (pc < image.base || pc - image.base >= image.image_size)
        return NULL;
    const uint64_t rva = pc - image.base;
    size_t left = 0, right = image.count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2;
        const struct nt_runtime_function *entry = &image.table[middle];
        if (rva < entry->begin) {
            right = middle;
        } else if (rva >= entry->end) {
            left = middle + 1;
        } else {
            *base = image.base;
            return entry;
        }
    }
    return NULL;
}

static _Thread_local uintptr_t stack_low, stack_high;

bool nt_runtime_set_unwind_stack(const void *start, size_t length)
{
    const uintptr_t low = (uintptr_t)start;
    if ((low == 0) != (length == 0) || length > UINTPTR_MAX - low)
        return false;
    stack_low = low;
    stack_high = low + length;
    return true;
}

static bool stack_bytes(uint64_t address, uint64_t offset, void *output, size_t length)
{
    if (stack_low == 0 || address < stack_low || address > stack_high ||
        offset > stack_high - address || length > stack_high - address - offset)
        return false;
    memcpy(output, (const void *)(uintptr_t)(address + offset), length);
    return true;
}

static const unsigned char *image_bytes(uint32_t offset, size_t length)
{
    return offset <= image.image_size && length <= image.image_size - offset ?
           (const unsigned char *)(image.base + offset) : NULL;
}

static uint64_t *saved_register(struct nt_context64 *context, unsigned int index)
{
    switch (index) {
    case 3: return &context->rbx;
    case 5: return &context->rbp;
    case 6: return &context->rsi;
    case 7: return &context->rdi;
    case 12: return &context->r12;
    case 13: return &context->r13;
    case 14: return &context->r14;
    case 15: return &context->r15;
    default: return NULL;
    }
}

static uint32_t load_word(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8;
}

static uint32_t load_dword(const unsigned char *bytes)
{
    return load_word(bytes) | load_word(bytes + 2) << 16;
}

static bool begins_epilogue(const unsigned char *code, size_t length)
{
    if (code[0] == 0xc3 || code[0] == 0xc2 || code[0] == 0xe9 || code[0] == 0xeb ||
        (code[0] >= 0x58 && code[0] <= 0x5f))
        return true;
    if (length >= 2 && ((code[0] == 0x41 && code[1] >= 0x58 && code[1] <= 0x5f) ||
                       (code[0] == 0xff && (code[1] & 0x38) == 0x20)))
        return true;
    return length >= 3 && ((code[0] & 0xf8) == 0x48) &&
           (((code[1] == 0x81 || code[1] == 0x83) && code[2] == 0xc4) ||
            (code[1] == 0x8d && (code[2] & 0x38) == 0x20));
}

struct unwind_code {
    unsigned int position, operation, index;
    uint32_t offset;
};

static size_t decode_code(const unsigned char *slot, size_t remaining, struct unwind_code *code)
{
    *code = (struct unwind_code){slot[0], slot[1] & 15u, slot[1] >> 4, 0};
    switch (code->operation) {
    case 0: case 3: return 1;
    case 2: code->offset = code->index * 8 + 8; return 1;
    case 1:
        if (code->index == 0 && remaining >= 2) {
            code->offset = load_word(slot + 2) * 8;
            return 2;
        }
        if (code->index != 1)
            return 0;
        /* Unscaled 32-bit size, shared with the FAR save encodings. */
        /* fall through */
    case 5: case 9:
        if (remaining < 3)
            return 0;
        code->offset = load_dword(slot + 2);
        return 3;
    case 4: case 8:
        if (remaining < 2)
            return 0;
        code->offset = load_word(slot + 2) * (code->operation == 4 ? 8u : 16u);
        return 2;
    default:
        return 0;
    }
}

static bool unwind_one(struct nt_context64 *context, struct nt_unwind_pointers *pointers,
                       struct unwind_code code, uint64_t frame)
{
    uint64_t *reg;
    switch (code.operation) {
    case 0:
        reg = saved_register(context, code.index);
        if (reg == NULL || !stack_bytes(context->rsp, 0, reg, sizeof(*reg)))
            return false;
        pointers->integer[code.index] = (void *)(uintptr_t)context->rsp;
        context->rsp += sizeof(*reg);
        return true;
    case 1: case 2:
        if (context->rsp > stack_high || code.offset > stack_high - context->rsp)
            return false;
        context->rsp += code.offset;
        return true;
    case 3:
        context->rsp = frame;
        return true;
    case 4: case 5:
        reg = saved_register(context, code.index);
        if (reg == NULL || !stack_bytes(frame, code.offset, reg, sizeof(*reg)))
            return false;
        pointers->integer[code.index] = (void *)(uintptr_t)(frame + code.offset);
        return true;
    case 8: case 9:
        if (code.index < 6 ||
            !stack_bytes(frame, code.offset, context->floating + 160 + code.index * 16, 16))
            return false;
        pointers->floating[code.index] = (void *)(uintptr_t)(frame + code.offset);
        return true;
    default:
        return false;
    }
}

static bool unwind_codes(const unsigned char *info, struct nt_context64 *context,
                         struct nt_unwind_pointers *pointers, uint32_t position, uint64_t *frame)
{
    const bool in_prologue = position < info[1];
    const unsigned int count = info[2], frame_reg = info[3] & 15u, frame_offset = (info[3] >> 4) * 16;
    struct unwind_code decoded[255];
    size_t used = 0;
    unsigned int previous = info[1];
    bool has_frame = false;
    for (size_t i = 0; i < count;) {
        struct unwind_code code;
        const size_t consumed = decode_code(info + 4 + i * 2, count - i, &code);
        if (consumed == 0 || code.position > previous ||
            (code.operation == 3 && (frame_reg == 0 || code.index != 0)))
            return false;
        previous = code.position;
        i += consumed;
        if (in_prologue && code.position > position)
            continue;
        decoded[used++] = code;
        has_frame |= code.operation == 3;
    }
    *frame = context->rsp;
    if (has_frame) {
        const uint64_t *value = saved_register(context, frame_reg);
        if (value == NULL || *value < frame_offset)
            return false;
        *frame = *value - frame_offset;
    }
    if (*frame < stack_low || *frame > stack_high)
        return false;
    for (size_t i = 0; i < used; ++i) {
        if (!unwind_one(context, pointers, decoded[i], *frame))
            return false;
    }
    if (!stack_bytes(context->rsp, 0, &context->rip, sizeof(context->rip)))
        return false;
    context->rsp += sizeof(context->rip);
    return true;
}

void *NT_MS_ABI nt_RtlVirtualUnwind(uint32_t type, uint64_t base, uint64_t pc,
                                   const struct nt_runtime_function *function,
                                   struct nt_context64 *context, void **handler_data,
                                   uint64_t *frame, struct nt_unwind_pointers *pointers)
{
    uint64_t found;
    if (context == NULL || handler_data == NULL || frame == NULL || function == NULL ||
        (type & ~3u) != 0 || nt_RtlLookupFunctionEntry(pc, &found, NULL) != function || found != base ||
        stack_low == 0 || context->rsp < stack_low || context->rsp >= stack_high) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const uint32_t rva = (uint32_t)(pc - base);
    const unsigned char *instruction = image_bytes(rva, function->end - rva);
    const unsigned char *info = image_bytes(function->unwind, 4);
    if (instruction == NULL || info == NULL || (info[0] & 7) != 1 || (info[0] >> 3 & ~3u) != 0 ||
        begins_epilogue(instruction, function->end - rva)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return NULL;
    }
    const size_t codes = ((size_t)info[2] + 1) & ~(size_t)1;
    const size_t extra = (info[0] >> 3) != 0 ? 4 : 0;
    if (image_bytes(function->unwind, 4 + 2 * codes + extra) == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    struct nt_context64 next = *context;
    struct nt_unwind_pointers restored = {0};
    uint64_t next_frame;
    const uint32_t position = rva - function->begin;
    if (!unwind_codes(info, &next, &restored, position, &next_frame)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    void *handler = NULL, *data = NULL;
    if ((info[0] >> 3 & type) != 0 && position >= info[1]) {
        const unsigned char *entry = info + 4 + 2 * codes;
        const unsigned char *target = image_bytes(load_dword(entry), 1);
        if (target == NULL) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return NULL;
        }
        handler = (void *)target;
        data = (void *)(entry + 4);
    }
    *context = next;
    *handler_data = data;
    *frame = next_frame;
    if (pointers != NULL)
        *pointers = restored;
    return handler;
}

static _Atomic(nt_exception_filter) exception_filter;
static _Thread_local void *current_record, *current_context;
static _Thread_local nt_signal_handler fault_handlers[12];

void **NT_MS_ABI nt_current_exception(void)
{
    return &current_record;
}

void **NT_MS_ABI nt_current_exception_context(void)
{
    return &current_context;
}

nt_exception_filter NT_MS_ABI nt_SetUnhandledExceptionFilter(nt_exception_filter filter)
{
    return atomic_exchange(&exception_filter, filter);
}

int32_t NT_MS_ABI nt_UnhandledExceptionFilter(struct nt_exception_pointers *pointers)
{
    if (pointers == NULL || pointers->record == NULL || pointers->context == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (nt_IsDebuggerPresent())
        return 0;
    const nt_exception_filter filter = atomic_load(&exception_filter);
    if (filter != NULL) {
        void *saved_record = current_record, *saved_context = current_context;
        current_record = pointers->record;
        current_context = pointers->context;
        const int32_t result = filter(pointers);
        current_record = saved_record;
        current_context = saved_context;
        if (result == -1 || result == 1)
            return result;
    }
    /* The worker profile has no interactive crash reporter. */
    return 1;
}

nt_signal_handler NT_MS_ABI nt_signal(int signal, nt_signal_handler handler)
{
    if ((signal != 4 && signal != 8 && signal != 11) ||
        (handler != NULL && (uintptr_t)handler != 1 && (uintptr_t)handler <= UINT16_MAX) ||
        (uintptr_t)handler == UINTPTR_MAX) {
        errno = EINVAL;
        return (nt_signal_handler)UINTPTR_MAX;
    }
    const nt_signal_handler previous = fault_handlers[signal];
    fault_handlers[signal] = handler;
    return previous;
}

int32_t NT_MS_ABI nt_seh_filter_exe(uint32_t code, struct nt_exception_pointers *pointers)
{
    static const struct { uint32_t status; int signal, detail; } faults[] = {
        {0xc0000005, 11, 0}, {0xc000001d, 4, 0}, {0xc0000096, 4, 0},
        {0xc000008d, 8, 0x82}, {0xc000008e, 8, 0x83}, {0xc000008f, 8, 0x86},
        {0xc0000090, 8, 0x81}, {0xc0000091, 8, 0x84}, {0xc0000092, 8, 0x8a},
        {0xc0000093, 8, 0x85}, {0xc00002b4, 8, 0x8e}, {0xc00002b5, 8, 0x8d}
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        if (faults[i].status != code)
            continue;
        const int signal = faults[i].signal;
        const nt_signal_handler handler = fault_handlers[signal];
        if (handler == NULL)
            return 0; /* EXCEPTION_CONTINUE_SEARCH */
        if ((uintptr_t)handler == 1)
            return -1; /* EXCEPTION_CONTINUE_EXECUTION */
        fault_handlers[signal] = NULL;
        void *saved_record = current_record, *saved_context = current_context;
        current_record = pointers != NULL ? pointers->record : NULL;
        current_context = pointers != NULL ? pointers->context : NULL;
        if (signal == 8) {
            typedef void (NT_MS_ABI *floating_handler)(int, int);
            ((floating_handler)(nt_guest_proc)handler)(signal, faults[i].detail);
        } else {
            handler(signal);
        }
        current_record = saved_record;
        current_context = saved_context;
        return -1;
    }
    return 0;
}
