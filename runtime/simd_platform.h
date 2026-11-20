// Unified SIMD platform header: native x86-64 AVX2/SSE intrinsics on x86-64, and native ARM64 NEON / ACLE intrinsics
// plus SIMDe (or built-in NEON SSE2 aliases when external SIMDe headers are absent) on AArch64.
#pragma once
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#if __has_include(<arm_acle.h>)
#include <arm_acle.h>
#endif

#if __has_include(<simde/x86/avx2.h>)
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/avx2.h>
#elif __has_include(<simde/x86/sse2.h>)
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/sse2.h>
#else
// Lightweight 1:1 NEON aliases for the SSE2 helpers used in runtime/ when external SIMDe headers are not installed.
typedef uint8x16_t __m128i;

static inline __m128i _mm_loadu_si128(const __m128i *p) { return vld1q_u8((const uint8_t *)p); }
static inline void _mm_storeu_si128(__m128i *p, __m128i v) { vst1q_u8((uint8_t *)p, v); }
static inline __m128i _mm_unpacklo_epi8(__m128i a, __m128i b) { return vzip1q_u8(a, b); }
static inline __m128i _mm_unpackhi_epi8(__m128i a, __m128i b) { return vzip2q_u8(a, b); }
static inline void _mm_pause(void) { __asm__ volatile("yield" ::: "memory"); }
#endif
#endif
