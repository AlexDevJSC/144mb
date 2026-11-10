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
.section .note.GNU-stack,"",@progbits
