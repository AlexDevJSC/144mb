/* SPDX-License-Identifier: MIT */
/* A genuine .pdata/.xdata Win64 SEH scope INSIDE a special APC that
 * interrupted a different PE thread during an import-free CPU spin.
 * The naked RaiseException import and the resource-free C-specific
 * handler must be reconciled without unwinding any other ELF frame. */
.text
.globl special_exception_apc
.seh_proc special_exception_apc
special_exception_apc:
    pushq %rbx
    .seh_pushreg %rbx
    subq $32, %rsp
    .seh_stackalloc 32
    .seh_endprologue
    /* Windows APC receives async_target* in RCX. Keep it in nonvolatile
     * RBX so the CONTEXT restored by SEH preserves the argument. */
    movq %rcx, %rbx
    movl $0xe14400af, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
apc_raise_begin:
    call *__imp_RaiseException(%rip)
apc_raise_end:
    /* Returning here means the frame handler never recovered the
     * exception. Report a non-success count, not a false PASS. */
    movl $2, 4(%rbx)
    jmp apc_done
apc_catch:
    call *__imp_GetCurrentThreadId(%rip)
    movl %eax, 16(%rbx)
    movl $1, 4(%rbx)
apc_done:
    addq $32, %rsp
    popq %rbx
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva apc_raise_begin, apc_raise_end
    .long 1 /* EXCEPTION_EXECUTE_HANDLER */
    .rva apc_catch
    .text
.seh_endproc
