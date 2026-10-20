#pragma once
// Preserve the upstream x86 representation/semantics; map intrinsics to NEON on ARM64.
#if defined(__aarch64__) || defined(_M_ARM64)
#ifndef SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_ENABLE_NATIVE_ALIASES
#endif
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
// SIMDe 0.8.2 omits this public x86 spelling from its native aliases.
#ifndef _MM_FROUND_NO_EXC
#define _MM_FROUND_NO_EXC SIMDE_MM_FROUND_NO_EXC
#endif
#else
#include <immintrin.h>
#endif
