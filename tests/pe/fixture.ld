/* SPDX-License-Identifier: MIT */
SECTIONS
{
    .text (__image_base__ + __section_alignment__) : { *(.text) }
}
