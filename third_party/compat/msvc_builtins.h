/* Minimal GCC builtin shims so upstream C sources compile with MSVC unmodified. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static __inline int bta_builtin_clz(unsigned int x)
{
    unsigned long idx;
    /* __builtin_clz(0) is undefined in GCC; return 32 for safety. */
    return _BitScanReverse(&idx, x) ? (int)(31 - idx) : 32;
}
#define __builtin_clz(x) bta_builtin_clz((unsigned int)(x))
#define __attribute__(x)
#endif
