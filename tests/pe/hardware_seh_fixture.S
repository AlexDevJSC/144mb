/* SPDX-License-Identifier: MIT */
/* Real CPU exceptions in a mapped freestanding PE64 (NOT RaiseException).
 * The Linux kernel delivers SIGILL, SIGFPE and SIGSEGV; a dedicated
 * trusted signal bridge constructs a Windows CONTEXT and dispatches
 * the original Win64 .pdata/.xdata __C_specific_handler scopes. */
.text
.globl import_fixture_entry
.seh_proc import_fixture_entry
import_fixture_entry:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    call test_illegal
    testl %eax, %eax
    jne done
    call test_divide
    testl %eax, %eax
    jne done
    call test_access
done:
    addq $40, %rsp
    ret
.seh_endproc

.seh_proc test_illegal
test_illegal:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
illegal_begin:
    ud2
illegal_end:
    movl $91, %eax
    jmp illegal_done
illegal_catch:
    incl illegal_catches(%rip)
    cmpl $1, illegal_catches(%rip)
    jne illegal_failed
    xorl %eax, %eax
    jmp illegal_done
illegal_failed:
    movl $92, %eax
illegal_done:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva illegal_begin, illegal_end, illegal_filter, illegal_catch
    .text
.seh_endproc

illegal_filter:
    movq (%rcx), %rax
    cmpl $0xc000001d, (%rax)
    jne no_match
    movl $1, %eax
    ret

.seh_proc test_divide
test_divide:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movl $1, %eax
    xorl %ecx, %ecx
divide_begin:
    divl %ecx
divide_end:
    movl $93, %eax
    jmp divide_done
divide_catch:
    incl divide_catches(%rip)
    cmpl $1, divide_catches(%rip)
    jne divide_failed
    xorl %eax, %eax
    jmp divide_done
divide_failed:
    movl $94, %eax
divide_done:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva divide_begin, divide_end, divide_filter, divide_catch
    .text
.seh_endproc

divide_filter:
    movq (%rcx), %rax
    cmpl $0xc0000094, (%rax)
    jne no_match
    movl $1, %eax
    ret

.seh_proc test_access
test_access:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    xorl %eax, %eax
access_begin:
    movq (%rax), %rax
access_end:
    movl $95, %eax
    jmp access_done
access_catch:
    incl access_catches(%rip)
    cmpl $1, access_catches(%rip)
    jne access_failed
    xorl %eax, %eax
    jmp access_done
access_failed:
    movl $96, %eax
access_done:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva access_begin, access_end, access_filter, access_catch
    .text
.seh_endproc

access_filter:
    movq (%rcx), %rax
    cmpl $0xc0000005, (%rax)
    jne no_match
    cmpl $2, 24(%rax)
    jne no_match
    cmpq $0, 32(%rax)  /* ExceptionInformation[0] = READ */
    jne no_match
    cmpq $0, 40(%rax)  /* ExceptionInformation[1] = NULL */
    jne no_match
    movl $1, %eax
    ret

no_match:
    xorl %eax, %eax
    ret

.bss
.balign 4
illegal_catches: .long 0
divide_catches: .long 0
access_catches: .long 0
