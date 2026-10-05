/* LAME 3.100 build configuration for the bundled static libmp3lame
   (replaces the autoconf-generated config.h on every platform). */
#ifndef AF_LAME_CONFIG_H
#define AF_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define TAKEHIRO_IEEE754_HACK 1
#define PACKAGE "lame"
#define LAME_LIBRARY_BUILD 1

#define SIZEOF_DOUBLE 8
#define SIZEOF_FLOAT 4
#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
#define SIZEOF_UNSIGNED_INT 4
#define SIZEOF_UNSIGNED_SHORT 2

#include <stdint.h>
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;

#if defined(_MSC_VER)
#pragma warning(disable : 4305 4244 4267 4996)
#if !defined(__cplusplus) && !defined(inline)
#define inline __inline
#endif
/* VbrTag.c includes a debug-only header that the release tarball lacks. */
#ifdef _DEBUG
#undef _DEBUG
#endif
#endif

#endif
