/* PORTABLE x87 TRANSCENDENTALS (PORT_X87_PORTABLE, fpu.c): fsin, fcos, fsincos, fptan, fpatan,
   f2xm1, fyl2x and fyl2xp1 by the host's basic arithmetic alone (+ - * / sqrt, which IEEE fixes
   bit for bit), so a run gives the same results on every x86 CPU. The host's own instructions
   differ between Intel and AMD in the last bits (all eight, measured 2026-11-18: an i3-10100F and an
   i9-10900K against a Ryzen AI 9 HX 370), which made recorded runs depend on the PC.
   Computed in the full 64-bit significand with round-to-nearest (the host's control word is set for
   it and put back): within a few units in the last place of the 80-bit result, as the hardware is.
   Zeros, infinities, NaNs and the undefined ranges keep the hardware path's answer, which the x87
   defines exactly. Series coefficients and split constants: mpmath at 400 bits (see the CHANGELOG). */

static const f80 X87S_PIO2_1 = 0xc90fdaa200000000p-63L, X87S_PIO2_2 = 0x85a308d300000000p-97L,
                 X87S_PIO2_3 = 0x98cc517000000000p-132L, X87S_PIO2_4 = 0xdc1cd129024e088ap-167L;
static const f80 X87S_PIO2_HI = 0xc90fdaa22168c234p-63L, X87S_PIO2_LO = 0xc4c6628b80dc1cd1p-127L;
static const f80 X87S_PI_HI = 0xc90fdaa22168c234p-62L, X87S_PI_LO = 0xc4c6628b80dc1cd1p-126L;
static const f80 X87S_2OPI = 0xa2f9836e4e44152ap-64L;          /* 2/pi, for the quadrant only */
static const f80 X87S_LN2_HI = 0xb17217f7d1cf79abp-64L, X87S_LN2_LO = 0xc9e3b39803f2f6afp-128L;
static const f80 X87S_2OLN2_HI = 0xb8aa3b295c17f0bbp-62L, X87S_2OLN2_LO = 0xbe87fed0691d3e88p-126L;
static const f80 X87S_SQRTH = 0xb504f333f9de6484p-64L;

static const f80 X87S_S[] = {-0xaaaaaaaaaaaaaaabp-66L, 0x8888888888888889p-70L, -0xd00d00d00d00d00dp-76L, 0xb8ef1d2ab6399c7dp-82L, -0xd7322b3faa271c7fp-89L, 0xb092309d43684be5p-96L, -0xd73f9f399dc0f88fp-104L, 0xca963b81856a5359p-112L, -0x97a4da340a0ab926p-120L, 0xb8dc77b6e7ab8c5fp-129L, -0xbb0da098b1c0ceccp-138L};
static const f80 X87S_C[] = {-0x8000000000000000p-64L, 0xaaaaaaaaaaaaaaabp-68L, -0xb60b60b60b60b60bp-73L, 0xd00d00d00d00d00dp-79L, -0x93f27dbbc4fae397p-85L, 0x8f76c77fc6c4bdaap-92L, -0xc9cba54603e4e906p-100L, 0xd73f9f399dc0f88fp-108L, -0xb413c31dcbecbbdep-116L, 0xf2a15d201011283dp-125L, -0x8671cb6dbfc294a3p-133L, 0xf96780cb97abbe65p-143L};
static const f80 X87S_A[] = {-0xaaaaaaaaaaaaaaabp-65L, 0xcccccccccccccccdp-66L, -0x9249249249249249p-66L, 0xe38e38e38e38e38ep-67L, -0xba2e8ba2e8ba2e8cp-67L, 0x9d89d89d89d89d8ap-67L, -0x8888888888888889p-67L, 0xf0f0f0f0f0f0f0f1p-68L, -0xd79435e50d79435ep-68L, 0xc30c30c30c30c30cp-68L, -0xb21642c8590b2164p-68L, 0xa3d70a3d70a3d70ap-68L, -0x97b425ed097b425fp-68L, 0x8d3dcb08d3dcb08dp-68L};
static const f80 X87S_H[] = {0xaaaaaaaaaaaaaaabp-65L, 0xcccccccccccccccdp-66L, 0x9249249249249249p-66L, 0xe38e38e38e38e38ep-67L, 0xba2e8ba2e8ba2e8cp-67L, 0x9d89d89d89d89d8ap-67L, 0x8888888888888889p-67L, 0xf0f0f0f0f0f0f0f1p-68L, 0xd79435e50d79435ep-68L, 0xc30c30c30c30c30cp-68L, 0xb21642c8590b2164p-68L, 0xa3d70a3d70a3d70ap-68L, 0x97b425ed097b425fp-68L};
static const f80 X87S_E[] = {0x8000000000000000p-64L, 0xaaaaaaaaaaaaaaabp-66L, 0xaaaaaaaaaaaaaaabp-68L, 0x8888888888888889p-70L, 0xb60b60b60b60b60bp-73L, 0xd00d00d00d00d00dp-76L, 0xd00d00d00d00d00dp-79L, 0xb8ef1d2ab6399c7dp-82L, 0x93f27dbbc4fae397p-85L, 0xd7322b3faa271c7fp-89L, 0x8f76c77fc6c4bdaap-92L, 0xb092309d43684be5p-96L, 0xc9cba54603e4e906p-100L, 0xd73f9f399dc0f88fp-104L, 0xd73f9f399dc0f88fp-108L, 0xca963b81856a5359p-112L, 0xb413c31dcbecbbdep-116L, 0x97a4da340a0ab926p-120L, 0xf2a15d201011283dp-125L};

static f80 x87s_horner(const f80 *k, int n, f80 z)
{
    f80 p = k[n - 1];
    for (int i = n - 2; i >= 0; i--)
        p = k[i] + z * p;
    return p;
}

/* sin and cos of r, |r| <= pi/4 (a little more is fine) */
static f80 x87s_ksin(f80 r) { f80 z = r * r; return r + r * z * x87s_horner(X87S_S, 11, z); }
static f80 x87s_kcos(f80 r) { f80 z = r * r; return 1.0L + z * x87s_horner(X87S_C, 12, z); }

/* x = k*pi/2 + r, |r| <= pi/4 about: the quadrant k & 3 */
static int x87s_reduce(f80 x, f80 *r)
{
    f80 ax = fabsl(x);
    if (ax <= 0xc90fdaa22168c234p-64L) {             /* |x| <= pi/4 */
        *r = x;
        return 0;
    }
    if (ax < 0x1p31L) {
        f80 k = nearbyintl(x * X87S_2OPI);           /* (the host rounds to nearest here) */
        *r = (((x - k * X87S_PIO2_1) - k * X87S_PIO2_2) - k * X87S_PIO2_3) - k * X87S_PIO2_4;
        return (int)((long long)k & 3);
    }
    /* far out (up to 2^63): the remainder by the 128-bit pi/2's high part, exact (fprem1), as the
       hardware reduces with its own 66-bit pi; the quotient's low bits give the quadrant */
    f80 y = X87S_PIO2_HI, rem = x;
    uint16_t sw;
    do
        __asm__("fprem1; fnstsw %1" : "+t"(rem), "=am"(sw) : "u"(y));
    while (sw & 0x0400);
    int q = ((sw >> 9) & 1) | ((sw >> 14) & 1) << 1;       /* the quotient's Q0 = C1, Q1 = C3 */
    *r = rem;
    return q & 3;
}

static f80 x87s_sin(f80 x)
{
    f80 r;
    switch (x87s_reduce(x, &r)) {
    case 0: return x87s_ksin(r);
    case 1: return x87s_kcos(r);
    case 2: return -x87s_ksin(r);
    default: return -x87s_kcos(r);
    }
}

static f80 x87s_cos(f80 x)
{
    f80 r;
    switch (x87s_reduce(x, &r)) {
    case 0: return x87s_kcos(r);
    case 1: return -x87s_ksin(r);
    case 2: return -x87s_kcos(r);
    default: return x87s_ksin(r);
    }
}

static f80 x87s_tan(f80 x)
{
    f80 r;
    int q = x87s_reduce(x, &r);
    f80 s = x87s_ksin(r), c = x87s_kcos(r);
    return q & 1 ? -c / s : s / c;
}

/* atan2(y, x) for finite, nonzero x and y, as fpatan (st1 = y, st0 = x) */
static f80 x87s_atan2(f80 y, f80 x)
{
    f80 ax = fabsl(x), ay = fabsl(y), t;
    int swap = ay > ax;
    t = swap ? ax / ay : ay / ax;                    /* 0 < t <= 1 */
    f80 m = 1.0L;
    while (t > 0.2L) {                               /* atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))) */
        t = t / (1.0L + sqrtl(1.0L + t * t));
        m *= 2.0L;
    }
    f80 z = t * t;
    f80 a = m * (t + t * z * x87s_horner(X87S_A, 14, z));
    if (swap)
        a = (X87S_PIO2_HI - a) + X87S_PIO2_LO;
    if (x < 0)
        a = (X87S_PI_HI - a) + X87S_PI_LO;
    return y < 0 ? -a : a;
}

/* e^z - 1 for |z| <= ln 2 */
static f80 x87s_expm1(f80 z)
{
    if (fabsl(z) > 0.35L) {                          /* e^z - 1 = e (e + 2), e = e^(z/2) - 1 */
        f80 e = x87s_expm1(z * 0.5L);
        return e * (e + 2.0L);
    }
    return z + z * z * x87s_horner(X87S_E, 19, z);
}

/* 2^x - 1 */
static f80 x87s_f2xm1(f80 x) { return x87s_expm1(x * X87S_LN2_HI + x * X87S_LN2_LO); }

/* 2 atanh(s) / ln 2 = log2((1 + s) / (1 - s)) */
static f80 x87s_log2_atanh(f80 s)
{
    f80 z = s * s;
    f80 a = s + s * z * x87s_horner(X87S_H, 13, z);
    return a * X87S_2OLN2_HI + a * X87S_2OLN2_LO;
}

/* log2(x) for finite x > 0 */
static f80 x87s_log2(f80 x)
{
    int e;
    f80 m = frexpl(x, &e);                           /* x = m 2^e, 1/2 <= m < 1 (exact) */
    if (m < X87S_SQRTH) {
        m *= 2.0L;
        e--;
    }
    return (f80)e + x87s_log2_atanh((m - 1.0L) / (m + 1.0L));
}

/* log2(1 + x) for x > -1 */
static f80 x87s_log2p1(f80 x) { return x87s_log2_atanh(x / (2.0L + x)); }
