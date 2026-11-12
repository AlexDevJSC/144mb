/* SPDX-License-Identifier: MIT */
.text
.globl import_fixture_entry
.seh_proc import_fixture_entry
import_fixture_entry:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    call continuable
    testl %eax, %eax
    jne entry_done
    call catch_exception
    testl %eax, %eax
    jne entry_done
    call unwind_here
    testl %eax, %eax
    jne entry_done
    call unwind_ex_here
    testl %eax, %eax
    jne entry_done
    call consolidate_here
entry_done:
    addq $40, %rsp
    ret
.seh_endproc

.seh_proc continuable
continuable:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movq $0x1234, 40(%rsp)
    movl $0xe1440001, %ecx
    xorl %edx, %edx
    movl $1, %r8d
    leaq 40(%rsp), %r9
continue_begin:
    call *__imp_RaiseException(%rip)
continue_end:
    xorl %eax, %eax
    jmp continue_done
continue_handler:
    movl $31, %eax
continue_done:
    addq $56, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva continue_begin, continue_end, continue_filter, continue_handler
    .text
.seh_endproc

continue_filter:
    movq (%rcx), %rcx
    cmpl $0xe1440001, (%rcx)
    jne invalid_filter
    cmpl $1, 24(%rcx)
    jne invalid_filter
    cmpq $0x1234, 32(%rcx)
    jne invalid_filter
    movl $-1, %eax
    ret
invalid_filter:
    movl $1, %eax
    ret

.seh_proc catch_exception
catch_exception:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
catch_begin:
    call finally_exception
catch_end:
    movl $32, %eax
    jmp catch_done
catch_handler:
    cmpl $0xe1440002, %eax
    jne catch_invalid
    cmpl $1, ran_finally(%rip)
    jne catch_invalid
    xorl %eax, %eax
    jmp catch_done
catch_invalid:
    movl $33, %eax
catch_done:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva catch_begin, catch_end
    .long 1
    .rva catch_handler
    .text
.seh_endproc

.seh_proc finally_exception
finally_exception:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movl $0xe1440002, %ecx
    movl $1, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
finally_begin:
    call *__imp_RaiseException(%rip)
finally_end:
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler,@except,@unwind
    .seh_handlerdata
    .long 1
    .rva finally_begin, finally_end, finally_handler
    .long 0
    .text
.seh_endproc

finally_handler:
    movzbl %cl, %eax
    addl %eax, ran_finally(%rip)
    ret

.seh_proc unwind_here
unwind_here:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq %rsp, %rcx
    leaq unwind_target(%rip), %rdx
    xorl %r8d, %r8d
    movl $0x12345, %r9d
    call *__imp_RtlUnwind(%rip)
    movl $34, %eax
    jmp unwind_done
unwind_target:
    cmpq $0x12345, %rax
    jne unwind_invalid
    xorl %eax, %eax
    jmp unwind_done
unwind_invalid:
    movl $35, %eax
unwind_done:
    addq $40, %rsp
    ret
.seh_endproc

.seh_proc unwind_ex_here
unwind_ex_here:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movq %rsp, %rcx
    leaq unwind_ex_target(%rip), %rdx
    xorl %r8d, %r8d
    movl $0x54321, %r9d
    leaq caller_context(%rip), %rax
    movq %rax, 32(%rsp)
    movq $0, 40(%rsp)
    call *__imp_RtlUnwindEx(%rip)
    movl $36, %eax
    jmp unwind_ex_done
unwind_ex_target:
    cmpq $0x54321, %rax
    jne unwind_ex_invalid
    leaq unwind_ex_target(%rip), %rdx
    cmpq %rdx, caller_context+0xf8(%rip)
    jne unwind_ex_invalid
    cmpq %rsp, caller_context+0x98(%rip)
    jne unwind_ex_invalid
    xorl %eax, %eax
    jmp unwind_ex_done
unwind_ex_invalid:
    movl $37, %eax
unwind_ex_done:
    addq $56, %rsp
    ret
.seh_endproc

.seh_proc consolidate_here
consolidate_here:
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movq %rsp, %rcx
    leaq consolidate_invalid(%rip), %rdx
    leaq consolidate_record(%rip), %r8
    movl $0x80000029, (%r8)
    movl $1, 24(%r8)
    leaq consolidate_callback(%rip), %rax
    movq %rax, 32(%r8)
    movl $0x23456, %r9d
    leaq caller_context(%rip), %rax
    movq %rax, 32(%rsp)
    movq $0, 40(%rsp)
    call *__imp_RtlUnwindEx(%rip)
    jmp consolidate_invalid
consolidate_target:
    cmpq $0x23456, %rax
    jne consolidate_invalid
    leaq consolidate_target(%rip), %rax
    cmpq %rax, caller_context+0xf8(%rip)
    jne consolidate_invalid
    xorl %eax, %eax
    jmp consolidate_done
consolidate_invalid:
    movl $38, %eax
consolidate_done:
    addq $56, %rsp
    ret
.seh_endproc

consolidate_callback:
    cmpl $0x22, 4(%rcx)
    jne consolidate_bad_record
    leaq consolidate_target(%rip), %rax
    ret
consolidate_bad_record:
    leaq consolidate_invalid(%rip), %rax
    ret

.bss
.balign 16
caller_context: .space 1232
consolidate_record: .space 152
ran_finally: .long 0
