#include "FloatingPoint.h"
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
# include <xmmintrin.h>
# include <pmmintrin.h>
# define TPT_MP_HAVE_SSE_CONTROL 1
#endif

namespace multiplayer
{
void MatchGameFloatingPoint()
{
    // Every room participant flushes denormals to zero, whatever its CPU or
    // whatever the game does locally (the game only kills denormals on some x86
    // builds): a server and its players must compute tiny values the same way.
#ifdef TPT_MP_HAVE_SSE_CONTROL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    // FPCR.FZ (bit 24): denormal inputs and results are treated as zero.
    unsigned long long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (!(fpcr & (1ULL << 24)))
    {
        fpcr |= 1ULL << 24;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    }
#endif
}
}
