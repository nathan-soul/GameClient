// MinGW-w64 compatibility header for Generals Game Code
// Resolves typedef conflicts between ReactOS ATL (tiff.h) and Valve SDK (steamtypes.h)
// Both define uint32/int32 at global scope, which C++ doesn't allow.
//
// This header is included FIRST via -include flag, before the PCH.

#ifndef MINGW_COMPAT_H
#define MINGW_COMPAT_H

// If tiff.h hasn't been included yet, we pre-define these types
// to prevent tiff.h from creating conflicting definitions later.
// tiff.h checks TIFF_INT32_T/TIFF_UINT32_T before typedefing,
// but we use a different mechanism: we ensure steamtypes.h's
// definitions are compatible with tiff.h's.

// This is a no-op - the actual fix is in tiff.h which now guards
// its conflicting typedefs with TIFF_SKIP_CONFLICTING_TYPES.

#endif // MINGW_COMPAT_H
