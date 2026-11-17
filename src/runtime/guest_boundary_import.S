/* SPDX-License-Identifier: MIT */
/* ABI-neutral gate for Microsoft x64 PE imports (AMD64 SysV host).
 *
 * The PE's original arguments (RCX/RDX/R8/R9, XMM0..15 and all
 * stack arguments) are restored before tail-jumping to the host export.
 * The caller's return slot is temporarily replaced by our epilogue,
 * which restores all MS nonvolatile registers and all XMM registers
 * after calling the SysV phase publisher.
 *
 * The host export therefore observes precisely the same stack, return
 * alignment and shadow space as before wrapping. This is prohibited
 * when userspace CET SHSTK is enabled (see C allocator's fail-closed
 * check); it is not intended to trap arbitrary jumps into host ELF. */
.text
.p2align 4
.globl nt_guest_boundary_import_entry
.type nt_guest_boundary_import_entry, @function
nt_guest_boundary_import_entry:
    .byte 0xf3,0x0f,0x1e,0xfa  /* ENDBR64, for IBT-aware systems */
    pushq %r11
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %rdi
    pushq %rsi
    subq $256, %rsp
    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    movdqu %xmm4, 64(%rsp)
    movdqu %xmm5, 80(%rsp)
    movdqu %xmm6, 96(%rsp)
    movdqu %xmm7, 112(%rsp)
    movdqu %xmm8, 128(%rsp)
    movdqu %xmm9, 144(%rsp)
    movdqu %xmm10, 160(%rsp)
    movdqu %xmm11, 176(%rsp)
    movdqu %xmm12, 192(%rsp)
    movdqu %xmm13, 208(%rsp)
    movdqu %xmm14, 224(%rsp)
    movdqu %xmm15, 240(%rsp)
    movq 320(%rsp), %rdi
    movq 328(%rsp), %rsi
    call nt_guest_boundary_import_enter@PLT
    movq %rax, 272(%rsp)
    movdqu 0(%rsp), %xmm0
    movdqu 16(%rsp), %xmm1
    movdqu 32(%rsp), %xmm2
    movdqu 48(%rsp), %xmm3
    movdqu 64(%rsp), %xmm4
    movdqu 80(%rsp), %xmm5
    movdqu 96(%rsp), %xmm6
    movdqu 112(%rsp), %xmm7
    movdqu 128(%rsp), %xmm8
    movdqu 144(%rsp), %xmm9
    movdqu 160(%rsp), %xmm10
    movdqu 176(%rsp), %xmm11
    movdqu 192(%rsp), %xmm12
    movdqu 208(%rsp), %xmm13
    movdqu 224(%rsp), %xmm14
    movdqu 240(%rsp), %xmm15
    addq $256, %rsp
    popq %rsi
    popq %rdi
    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rcx
    popq %rax
    popq %r11
    leaq nt_guest_boundary_import_return(%rip), %r11
    movq %r11, (%rsp)
    jmp *%r10
.size nt_guest_boundary_import_entry, .-nt_guest_boundary_import_entry

.p2align 4
.globl nt_guest_boundary_import_return
.type nt_guest_boundary_import_return, @function
nt_guest_boundary_import_return:
    .byte 0xf3,0x0f,0x1e,0xfa
    pushq %rax
    pushq %rdx
    pushq %rdi
    pushq %rsi
    pushq %r10
    subq $264, %rsp
    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    movdqu %xmm4, 64(%rsp)
    movdqu %xmm5, 80(%rsp)
    movdqu %xmm6, 96(%rsp)
    movdqu %xmm7, 112(%rsp)
    movdqu %xmm8, 128(%rsp)
    movdqu %xmm9, 144(%rsp)
    movdqu %xmm10, 160(%rsp)
    movdqu %xmm11, 176(%rsp)
    movdqu %xmm12, 192(%rsp)
    movdqu %xmm13, 208(%rsp)
    movdqu %xmm14, 224(%rsp)
    movdqu %xmm15, 240(%rsp)
    call nt_guest_boundary_import_leave@PLT
    movq %rax, 264(%rsp)
    movdqu 0(%rsp), %xmm0
    movdqu 16(%rsp), %xmm1
    movdqu 32(%rsp), %xmm2
    movdqu 48(%rsp), %xmm3
    movdqu 64(%rsp), %xmm4
    movdqu 80(%rsp), %xmm5
    movdqu 96(%rsp), %xmm6
    movdqu 112(%rsp), %xmm7
    movdqu 128(%rsp), %xmm8
    movdqu 144(%rsp), %xmm9
    movdqu 160(%rsp), %xmm10
    movdqu 176(%rsp), %xmm11
    movdqu 192(%rsp), %xmm12
    movdqu 208(%rsp), %xmm13
    movdqu 224(%rsp), %xmm14
    movdqu 240(%rsp), %xmm15
    addq $264, %rsp
    popq %r10
    popq %rsi
    popq %rdi
    popq %rdx
    popq %rax
    jmp *%r10
.size nt_guest_boundary_import_return, .-nt_guest_boundary_import_return

.section .note.GNU-stack,"",@progbits
