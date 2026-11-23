/* PORTABLE SSE RECIPROCALS (PORT_SSE_PORTABLE, 1 unless the port says 0): rcpss, rcpps, rsqrtss and
   rsqrtps as exact divisions and square roots, the same on every x86 CPU. The instructions are 12-bit
   approximations whose tables differ between Intel and AMD, so a recorded run moved between PCs
   (L.A. Rush, frame 204: an Intel CPU against AMD ones). The exact result is within the instructions'
   own bound (|error| <= 1.5 * 2^-12), so a program written for either CPU accepts it.
   cpu.h redirects the intrinsics (_mm_rcp_ss, _mm_rcp_ps, _mm_rsqrt_ss, _mm_rsqrt_ps) here, so the
   generated code and a port's own rewrites that use them both get these; cpu_rcp1 and cpu_rsqrt1 are
   the scalar forms (a reference build's or a rewrite's own use). The hardware's special cases:
   a NaN gives the same NaN made quiet; a zero or denormal input counts as zero (rcp: an infinity of
   its sign; rsqrt: -0 and +0 give -inf and +inf); rcp of an infinity is a zero of its sign, and of
   |x| >= 2^126 too (the result is tiny and flushed, as the hardware's approximation is); rsqrt of a negative number or -inf is the default NaN
   (0xffc00000), of +inf a +0. */
#pragma once
#include <math.h>

static inline uint32_t sse_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float sse_bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static inline float cpu_rcp1(float x)
{
    uint32_t u = sse_fbits(x), s = u & 0x80000000u, a = u & 0x7fffffffu;
    if (a > 0x7f800000u) return sse_bitsf(u | 0x00400000u);     /* NaN: made quiet */
    if (a < 0x00800000u) return sse_bitsf(s | 0x7f800000u);     /* zero, denormal: infinity */
    if (a == 0x7f800000u) return sse_bitsf(s);                  /* infinity: zero */
    if (a >= 0x7e800000u) return sse_bitsf(s);                  /* |x| >= 2^126: tiny, flushed (the hardware's too) */
    return (float)(1.0 / (double)x);
}

static inline float cpu_rsqrt1(float x)
{
    uint32_t u = sse_fbits(x), s = u & 0x80000000u, a = u & 0x7fffffffu;
    if (a > 0x7f800000u) return sse_bitsf(u | 0x00400000u);
    if (a < 0x00800000u) return sse_bitsf(s | 0x7f800000u);     /* -0 / +0, denormals: -inf / +inf */
    if (s) return sse_bitsf(0xffc00000u);                       /* negative, -inf: the default NaN */
    if (a == 0x7f800000u) return 0.0f;
    return (float)(1.0 / sqrt((double)x));
}

static inline __m128 cpu_rcp_ss(__m128 v) { return _mm_move_ss(v, _mm_set_ss(cpu_rcp1(_mm_cvtss_f32(v)))); }
static inline __m128 cpu_rsqrt_ss(__m128 v) { return _mm_move_ss(v, _mm_set_ss(cpu_rsqrt1(_mm_cvtss_f32(v)))); }
static inline __m128 cpu_rcp_ps(__m128 v)
{
    float f[4];
    _mm_storeu_ps(f, v);
    for (int i = 0; i < 4; i++) f[i] = cpu_rcp1(f[i]);
    return _mm_loadu_ps(f);
}
static inline __m128 cpu_rsqrt_ps(__m128 v)
{
    float f[4];
    _mm_storeu_ps(f, v);
    for (int i = 0; i < 4; i++) f[i] = cpu_rsqrt1(f[i]);
    return _mm_loadu_ps(f);
}

#if PORT_SSE_PORTABLE
#undef _mm_rcp_ss
#undef _mm_rcp_ps
#undef _mm_rsqrt_ss
#undef _mm_rsqrt_ps
#define _mm_rcp_ss(v) cpu_rcp_ss(v)
#define _mm_rcp_ps(v) cpu_rcp_ps(v)
#define _mm_rsqrt_ss(v) cpu_rsqrt_ss(v)
#define _mm_rsqrt_ps(v) cpu_rsqrt_ps(v)
#endif
