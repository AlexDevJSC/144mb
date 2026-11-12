/* SPDX-License-Identifier: MIT */
.text
.globl nt_RtlCaptureContext
.type nt_RtlCaptureContext,@function
nt_RtlCaptureContext:
    /* Windows x64 CONTEXT. RCX points to a writable 16-byte-aligned record. */
    movq %rax, 0x78(%rcx)
    movq %rcx, 0x80(%rcx)
    movq %rdx, 0x88(%rcx)
    movq %rbx, 0x90(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, 0x98(%rcx)
    movq %rbp, 0xa0(%rcx)
    movq %rsi, 0xa8(%rcx)
    movq %rdi, 0xb0(%rcx)
    movq %r8, 0xb8(%rcx)
    movq %r9, 0xc0(%rcx)
    movq %r10, 0xc8(%rcx)
    movq %r11, 0xd0(%rcx)
    movq %r12, 0xd8(%rcx)
    movq %r13, 0xe0(%rcx)
    movq %r14, 0xe8(%rcx)
    movq %r15, 0xf0(%rcx)
    movq (%rsp), %rax
    movq %rax, 0xf8(%rcx)
    pushfq
    popq %rax
    movl %eax, 0x44(%rcx)
    movw %cs, 0x38(%rcx)
    movw %ds, 0x3a(%rcx)
    movw %es, 0x3c(%rcx)
    movw %fs, 0x3e(%rcx)
    movw %gs, 0x40(%rcx)
    movw %ss, 0x42(%rcx)
    stmxcsr 0x34(%rcx)
    fxsave64 0x100(%rcx)
    movl $0x10000f, 0x30(%rcx)
    ret
.size nt_RtlCaptureContext, .-nt_RtlCaptureContext

/* Preserve the Windows caller's state before entering the SysV dispatcher. */
.macro SEH_ENTRY symbol, dispatcher, output=0
.globl \symbol
.type \symbol,@function
\symbol:
    leaq -0x528(%rsp), %rsp
    movq %rcx, 0x20(%rsp)
    movq %rdx, 0x28(%rsp)
    movq %r8, 0x30(%rsp)
    movq %r9, 0x38(%rsp)
    leaq 0x40(%rsp), %rcx
    call nt_RtlCaptureContext
    movq 0x20(%rsp), %rax
    movq %rax, 0xc0(%rsp)
    movq 0x528(%rsp), %rax
    movq %rax, 0x138(%rsp)
    leaq 0x530(%rsp), %rax
    movq %rax, 0xd8(%rsp)
    leaq 0x40(%rsp), %rdi
    movq 0x20(%rsp), %rsi
    movq 0x28(%rsp), %rdx
    movq 0x30(%rsp), %rcx
    movq 0x38(%rsp), %r8
.if \output
    movq 0x550(%rsp), %r9
.else
    xorl %r9d, %r9d
.endif
    call \dispatcher
    ud2
.size \symbol, .-\symbol
.endm

SEH_ENTRY nt_RaiseException, nt_dispatch_software_exception
SEH_ENTRY nt_RtlUnwind, nt_unwind_software_exception
SEH_ENTRY nt_RtlUnwindEx, nt_unwind_software_exception, 1

.globl nt_restore_software_context
.type nt_restore_software_context,@function
nt_restore_software_context:
    movq %rdi, %r10
    fxrstor64 0x100(%r10)
    movl 0x44(%r10), %eax
    andl $0x8d5, %eax
    orl $0x202, %eax
    pushq %rax
    popfq
    movq 0x98(%r10), %rsp
    pushq 0xf8(%r10)
    movq 0x78(%r10), %rax
    movq 0x80(%r10), %rcx
    movq 0x88(%r10), %rdx
    movq 0x90(%r10), %rbx
    movq 0xa0(%r10), %rbp
    movq 0xa8(%r10), %rsi
    movq 0xb0(%r10), %rdi
    movq 0xb8(%r10), %r8
    movq 0xc0(%r10), %r9
    movq 0xd0(%r10), %r11
    movq 0xd8(%r10), %r12
    movq 0xe0(%r10), %r13
    movq 0xe8(%r10), %r14
    movq 0xf0(%r10), %r15
    movq 0xc8(%r10), %r10
    ret
.size nt_restore_software_context, .-nt_restore_software_context
.section .note.GNU-stack,"",@progbits
