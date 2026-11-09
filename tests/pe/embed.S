/* SPDX-License-Identifier: MIT */
.section .rodata
.globl nt_pe_fixture_bytes
.type nt_pe_fixture_bytes, @object
nt_pe_fixture_bytes:
    .incbin NT_PE_FIXTURE_PATH
.size nt_pe_fixture_bytes, . - nt_pe_fixture_bytes
.Lfixture_end:

.balign 8
.globl nt_pe_fixture_size
.type nt_pe_fixture_size, @object
nt_pe_fixture_size:
    .quad .Lfixture_end - nt_pe_fixture_bytes
.size nt_pe_fixture_size, . - nt_pe_fixture_size

.section .note.GNU-stack,"",@progbits
