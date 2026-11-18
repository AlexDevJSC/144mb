/* SPDX-License-Identifier: MIT */
/* A real, freestanding PE32+ x64 special APC with frame-based SEH.
 * This explicitly tests __C_specific_handler scope unwinding inside
 * a guest APC, NOT merely vectored CONTINUE_EXECUTION. */
.text
.globl import_fixture_entry
.seh_proc import_fixture_entry
import_fixture_entry:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    /* Windows current thread pseudo HANDLE (-2). NtQueueApcThreadEx:
     * RCX=thread, RDX=reserve(1), R8=callback, R9=context,
     * shadow space + first/second stack arguments. */
    movq $-2, %rcx
    movl $1, %edx
    leaq special_frame_apc(%rip), %r8
    xorl %r9d, %r9d
    movq $0, 32(%rsp)
    movq $0, 40(%rsp)
    call *__imp_NtQueueApcThreadEx(%rip)
    testl %eax, %eax
    jne failed_enqueue
    cmpl $1, caught(%rip)
    jne failed_catch
    cmpl $0, returned_from_raise(%rip)
    jne failed_catch
    /* The imported NT queue call was not an alertable wait. */
    xorl %eax, %eax
    jmp entry_done
failed_enqueue:
    movl $91, %eax
    jmp entry_done
failed_catch:
    movl $92, %eax
entry_done:
    addq $56, %rsp
    ret
.seh_endproc

.globl special_frame_apc
.seh_proc special_frame_apc
special_frame_apc:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movl $0xe14400b5, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
special_raise_begin:
    call *__imp_RaiseException(%rip)
special_raise_end:
    /* Returning from a non-resuming, accepted SEH execute handler
     * instead of jumping to it is a failed test contract. */
    incl returned_from_raise(%rip)
    jmp special_done
special_catch:
    incl caught(%rip)
special_done:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva special_raise_begin, special_raise_end
    .long 1  /* unconditional EXCEPTION_EXECUTE_HANDLER */
    .rva special_catch
    .text
.seh_endproc

.bss
.balign 4
caught: .long 0
returned_from_raise: .long 0
