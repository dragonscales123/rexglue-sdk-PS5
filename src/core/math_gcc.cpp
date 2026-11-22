#include <rex/math.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
static_assert(false, "This file is non-MSVC only");
#endif

namespace rex {

// lzcnt, tzcnt and bit_scan_forward are inline in rex/math.h.

}  // namespace rex
