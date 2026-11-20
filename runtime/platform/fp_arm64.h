// FP control on AArch64: FPCR (rounding mode bits 23:22, FZ bit 24, DN bit 25, AHP bit 26).
// Inline on purpose: lifted FP code (simd.h) reads and writes it on every operation that depends on the guest FPCR.
#pragma once
#include <stdint.h>

#define PLAT_FP_RC_MASK 0x00c00000u   // rounding mode: 0 nearest, 0x00400000 +inf, 0x00800000 -inf, 0x00c00000 zero
#define PLAT_FP_FTZ 0x01000000u       // FPCR.FZ flushes both denormal inputs and tiny results
#define PLAT_FP_DAZ 0x00000000u

static inline uint32_t plat_fp_get(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (uint32_t)v;
}

static inline void plat_fp_set(uint32_t v) {
    __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)v) : "memory");
}
