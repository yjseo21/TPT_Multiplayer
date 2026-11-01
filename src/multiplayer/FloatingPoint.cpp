#include "FloatingPoint.h"
#include "Config.h"
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
# include <xmmintrin.h>
# include <pmmintrin.h>
# define TPT_MP_HAVE_SSE_CONTROL 1
#endif

namespace multiplayer
{
void MatchGameFloatingPoint()
{
#ifdef TPT_MP_HAVE_SSE_CONTROL
    if constexpr (X86_KILL_DENORMALS)
    {
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    }
#endif
}
}
