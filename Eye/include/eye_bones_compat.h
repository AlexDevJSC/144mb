#ifndef EYE_BONES_COMPAT_H
#define EYE_BONES_COMPAT_H

/* Contract observed in BONES_Contrato_Modulos_v0_2.txt (20/10/2026).
 * Actual Bones source is not bundled: this is a compile-time guard for the
 * final integration, NOT a tested integration of full Bones C files. */
#define EYE_EXPECT_BONES_API_VERSION 2u
#define EYE_EXPECT_BONES_SUBSYSTEM_API_VERSION 1u

/* Include "bones/bones.h" BEFORE this header in the final Bones build. */
#ifdef BONES_API_VERSION
#if BONES_API_VERSION != EYE_EXPECT_BONES_API_VERSION
#error "Eye 0.3 expects BONES_API_VERSION=2; update adapter contract."
#endif
#endif
#ifdef BONES_SUBSYSTEM_API_VERSION
#if BONES_SUBSYSTEM_API_VERSION != EYE_EXPECT_BONES_SUBSYSTEM_API_VERSION
#error "Eye 0.3 expects BONES_SUBSYSTEM_API_VERSION=1."
#endif
#endif

#endif
