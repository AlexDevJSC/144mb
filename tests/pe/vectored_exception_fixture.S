/* SPDX-License-Identifier: MIT */
/* Win64 .pdata/.xdata backed fixture; VEH is invoked before frame SEH. */
.text
.globl import_fixture_entry
.seh_proc import_fixture_entry
import_fixture_entry:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movl $1, %ecx
    leaq first_handler(%rip), %rdx
    call *__imp_AddVectoredExceptionHandler(%rip)
    testq %rax, %rax
    je failed_first
    movq %rax, first_token(%rip)
    xorl %ecx, %ecx
    leaq second_handler(%rip), %rdx
    call *__imp_AddVectoredExceptionHandler(%rip)
    testq %rax, %rax
    je failed_second
    movq %rax, second_token(%rip)

    movl $0xe1440201, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    call *__imp_RaiseException(%rip)
    cmpl $1, first_calls(%rip)
    jne failed_order
    cmpl $1, second_calls(%rip)
    jne failed_order

    movq first_token(%rip), %rcx
    call *__imp_RemoveVectoredExceptionHandler(%rip)
    testl %eax, %eax
    je failed_remove
    movq first_token(%rip), %rcx
    call *__imp_RemoveVectoredExceptionHandler(%rip)
    testl %eax, %eax
    jne failed_remove

    movl $0xe1440201, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    call *__imp_RaiseException(%rip)
    cmpl $1, first_calls(%rip)
    jne failed_order
    cmpl $2, second_calls(%rip)
    jne failed_order

    movq second_token(%rip), %rcx
    call *__imp_RemoveVectoredExceptionHandler(%rip)
    testl %eax, %eax
    je failed_remove
    xorl %eax, %eax
    jmp finish
failed_first:
    movl $41, %eax
    jmp finish
failed_second:
    movl $42, %eax
    jmp finish
failed_order:
    movl $43, %eax
    jmp finish
failed_remove:
    movl $44, %eax
finish:
    addq $40, %rsp
    ret
.seh_endproc

first_handler:
    incl first_calls(%rip)
    xorl %eax, %eax  /* EXCEPTION_CONTINUE_SEARCH */
    ret

second_handler:
    movq (%rcx), %rax  /* EXCEPTION_POINTERS.ExceptionRecord */
    cmpl $0xe1440201, (%rax)
    jne unexpected
    incl second_calls(%rip)
    movl $-1, %eax /* EXCEPTION_CONTINUE_EXECUTION */
    ret
unexpected:
    xorl %eax, %eax
    ret

.bss
.balign 8
first_token: .quad 0
second_token: .quad 0
first_calls: .long 0
second_calls: .long 0
