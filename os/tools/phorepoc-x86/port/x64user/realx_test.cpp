// realx_test.cpp -- HOSTED test of xu_realx.cpp (the amd64/x32 TRealX). Built by build_realx_test.sh
// against realx_test_shim/e32math.h (an exact copy of the TRealX class declaration; the real Symbian
// headers do not coexist with libc/libm in one hosted TU). The oracle is the host's x87 long double:
// same 64-bit mantissa, same round-to-nearest-even, so for in-range results sign, exponent and
// mantissa must match BIT FOR BIT; the rounding flag is checked against FE_INEXACT plus a second
// evaluation under FE_TOWARDZERO. Special values / error codes / operand asymmetries are checked
// against what euser/epoc/x86/uc_realx.cia does. Bounded output; last line REALX OK | REALX FAIL n.

#include <e32math.h>
#include <stdio.h>
#include <string.h>
#include <fenv.h>
#include <math.h>
#include <limits.h>

static int gFail = 0;
static int gChecks = 0;
static TInt gPanic = 0;

void TRealXPanic(TInt aErr)
	{
	gPanic = aErr;
	}

static void Fail(const char* aWhat, const TRealX* aA, const TRealX* aB, const TRealX* aR, int aInfo)
	{
	if (++gFail > 12)
		return;
	printf("FAIL %s (info %d)\n", aWhat, aInfo);
	const TRealX* v[3] = { aA, aB, aR };
	for (int i=0; i<3; ++i)
		if (v[i])
			printf("   %c: exp %04x sign %d flag %d mant %08x%08x\n", "abr"[i], v[i]->iExp, v[i]->iSign, v[i]->iFlag, v[i]->iMantHi, v[i]->iMantLo);
	}

#define CHECK(c, what, info) do { ++gChecks; if (!(c)) Fail(what, 0, 0, 0, (int)(info)); } while (0)

static TUint64 gSeed = 0x9E3779B97F4A7C15ULL;
static TUint64 Rnd()
	{
	gSeed ^= gSeed<<13; gSeed ^= gSeed>>7; gSeed ^= gSeed<<17;
	return gSeed * 0x2545F4914F6CDD1DULL;
	}
static int RndRange(int aLo, int aHi)			// inclusive
	{
	return aLo + (int)(Rnd() % (TUint64)(aHi-aLo+1));
	}

static TRealX Mk(TUint aSign, TUint aExp, TUint64 aMant, TUint aFlag = 0)
	{
	return TRealX((aExp<<16) | (aFlag<<8) | aSign, (TUint)(aMant>>32), (TUint)aMant);
	}
static TUint64 MantOf(const TRealX& aX)
	{
	return ((TUint64)aX.iMantHi<<32) | aX.iMantLo;
	}

// A random normalised mantissa; several shapes so that exact results and exact ties actually occur.
static TUint64 RndMant()
	{
	TUint64 m = Rnd();
	switch (Rnd() % 5)
		{
	case 0: m &= ~0ULL << RndRange(1, 62); break;			// trailing zeros
	case 1: m = Rnd() % 1000 + 1; break;					// small integer
	case 2: m |= (1ULL << RndRange(0, 40)) - 1; break;		// trailing ones
	default: break;
		}
	if (m == 0)
		m = 1;
	return m << __builtin_clzll(m);
	}

static long double ToLD(const TRealX& aX)
	{
	struct { TUint64 m; unsigned short se; } enc;
	long double r = 0;
	if (aX.iExp == 0)
		{ enc.m = 0; enc.se = 0; }
	else if (aX.iExp == 0xFFFF)
		{ enc.m = MantOf(aX); enc.se = 0x7FFF; }
	else
		{ enc.m = MantOf(aX); enc.se = (unsigned short)(aX.iExp - 0x4000); }
	enc.se |= (unsigned short)((aX.iSign & 1) << 15);
	memcpy(&r, &enc.m, 8);
	memcpy((char*)&r + 8, &enc.se, 2);
	return r;
	}

// value equality with the oracle: sign, exponent and mantissa
static bool SameAsLD(const TRealX& aX, long double aLd)
	{
	TUint64 m; unsigned short se;
	memcpy(&m, &aLd, 8);
	memcpy(&se, (char*)&aLd + 8, 2);
	if ((aX.iSign & ~1) != 0)
		return false;
	if ((unsigned)(se>>15) != (unsigned)(aX.iSign & 1))
		return false;
	se &= 0x7FFF;
	if (se == 0 && m == 0)
		return aX.iExp == 0;
	if (se == 0x7FFF)
		return aX.iExp == 0xFFFF && MantOf(aX) == m;
	return aX.iExp == (unsigned)se + 0x4000 && MantOf(aX) == m;
	}

typedef long double (*TLdOp)(long double, long double);
static long double LdAdd(long double a, long double b) { volatile long double x = a, y = b; volatile long double r = x + y; return r; }
static long double LdSub(long double a, long double b) { volatile long double x = a, y = b; volatile long double r = x - y; return r; }
static long double LdMul(long double a, long double b) { volatile long double x = a, y = b; volatile long double r = x * y; return r; }
static long double LdDiv(long double a, long double b) { volatile long double x = a, y = b; volatile long double r = x / y; return r; }

// Expected TRealX rounding flag for an in-range result
static TUint ExpectedFlag(TLdOp aOp, long double a, long double b)
	{
	feclearexcept(FE_ALL_EXCEPT);
	long double rn = aOp(a, b);
	if (!fetestexcept(FE_INEXACT))
		return 0;
	fesetround(FE_TOWARDZERO);
	long double rz = aOp(a, b);
	fesetround(FE_TONEAREST);
	return (rn == rz) ? 1 : 2;
	}

static void CheckArith(const char* aName, int aWhich, const TRealX& a, const TRealX& b)
	{
	static const TLdOp ops[4] = { LdAdd, LdSub, LdMul, LdDiv };
	TRealX r;
	TInt err;
	switch (aWhich)
		{
	case 0: err = a.Add(r, b); break;
	case 1: err = a.Sub(r, b); break;
	case 2: err = a.Mult(r, b); break;
	default: err = a.Div(r, b); break;
		}
	long double la = ToLD(a), lb = ToLD(b);
	long double expect = ops[aWhich](la, lb);
	TUint flag = ExpectedFlag(ops[aWhich], la, lb);
	++gChecks;
	// The ONE place the asm is not correctly rounded (uc_realx.cia:121-122, 216-225): exponents more
	// than 64 apart always return the larger operand. For an effective subtraction with exponents
	// exactly 65 apart, the larger mantissa 8000000000000000 and the smaller > 8000000000000000 the
	// true result is nearer the predecessor of the larger operand. We follow the asm.
	const TRealX& lg = a.iExp > b.iExp ? a : b;
	const TRealX& sm = a.iExp > b.iExp ? b : a;
	if (aWhich < 2 && lg.iExp - sm.iExp == 65 && MantOf(lg) == 1ULL<<63 && MantOf(sm) != 1ULL<<63
		&& ((a.iSign ^ b.iSign ^ aWhich) & 1))
		{
		if (err != KErrNone || r.iExp != lg.iExp || MantOf(r) != MantOf(lg) || r.iFlag != 2)
			Fail("asm >64 quirk", &a, &b, &r, err);
		}
	else if (err != KErrNone || !SameAsLD(r, expect) || r.iFlag != flag)
		Fail(aName, &a, &b, &r, err*10 + (int)flag);
	// the Eq form and the operator form must agree with the 3-operand form, and not panic
	TRealX r2(a);
	gPanic = 0;
	switch (aWhich)
		{
	case 0: r2 += b; break;
	case 1: r2 -= b; break;
	case 2: r2 *= b; break;
	default: r2 /= b; break;
		}
	++gChecks;
	if (gPanic || memcmp(&r, &r2, sizeof(TRealX)))
		Fail("op= differs", &a, &b, &r2, gPanic);
	}

static void TestArithmetic()
	{
	for (int i=0; i<6000; ++i)
		{
		TUint e1 = 0x7FFF + RndRange(-300, 300);
		TUint e2 = e1 + RndRange(-70, 70);
		TRealX a = Mk(Rnd()&1, e1, RndMant());
		TRealX b = Mk(Rnd()&1, e2, RndMant());
		CheckArith("Add", 0, a, b);
		CheckArith("Sub", 1, a, b);
		}
	for (int i=0; i<4000; ++i)
		{											// near cancellation
		TUint e1 = 0x7FFF + RndRange(-300, 300);
		TUint64 m = RndMant();
		TUint64 m2 = m ^ (Rnd() & ((1ULL << RndRange(0, 40)) - 1));
		if (Rnd() % 8 == 0)
			m2 = m;
		TRealX a = Mk(Rnd()&1, e1, m);
		TRealX b = Mk(Rnd()&1, e1 + RndRange(-1, 1), m2);
		CheckArith("Add/cancel", 0, a, b);
		CheckArith("Sub/cancel", 1, a, b);
		}
	for (int i=0; i<6000; ++i)
		{
		TRealX a = Mk(Rnd()&1, 0x7FFF + RndRange(-3000, 3000), RndMant());
		TRealX b = Mk(Rnd()&1, 0x7FFF + RndRange(-3000, 3000), RndMant());
		CheckArith("Mult", 2, a, b);
		CheckArith("Div", 3, a, b);
		}
	// Mod: exact remainder, sign of the dividend, vs fmodl
	for (int i=0; i<4000; ++i)
		{
		TUint e2 = 0x7FFF + RndRange(-300, 300);
		TRealX a = Mk(Rnd()&1, e2 + RndRange(-2, 63), RndMant());
		TRealX b = Mk(Rnd()&1, e2, RndMant());
		TRealX r;
		TInt err = a.Mod(r, b);
		long double expect = fmodl(ToLD(a), ToLD(b));
		++gChecks;
		if (a.iExp < b.iExp)
			{
			if (err != KErrNone || memcmp(&r, &a, sizeof(TRealX)))
				Fail("Mod small dividend", &a, &b, &r, err);
			}
		else if (err != KErrNone || !SameAsLD(r, expect) || r.iFlag != 0)
			Fail("Mod", &a, &b, &r, err);
		}
	}

static TUint64 BitsOf(double d) { TUint64 u; memcpy(&u, &d, 8); return u; }
static TUint32 BitsOf(float f) { TUint32 u; memcpy(&u, &f, 4); return u; }
static double DoubleOf(TUint64 u) { double d; memcpy(&d, &u, 8); return d; }
static float FloatOf(TUint32 u) { float f; memcpy(&f, &u, 4); return f; }

static void RoundTrip64(TUint64 aBits)
	{
	TRealX x;
	TInt e1 = x.Set(DoubleOf(aBits));
	TReal64 back = 0;
	TInt e2 = x.GetTReal(back);
	TUint64 frac = aBits & 0x000FFFFFFFFFFFFFULL;
	bool special = ((aBits>>52) & 0x7FF) == 0x7FF;
	TInt want = special ? (frac ? KErrArgument : KErrOverflow) : KErrNone;
	++gChecks;
	if (BitsOf(back) != aBits || e1 != want || e2 != want || x.iFlag != 0)
		Fail("TReal64 round trip", &x, 0, 0, e1*100 + e2);
	if (!special)
		{
		volatile long double ld = DoubleOf(aBits);
		++gChecks;
		if (!SameAsLD(x, ld))
			Fail("TReal64 -> TRealX value", &x, 0, 0, 0);
		TRealX y(DoubleOf(aBits));
		TRealX z; z = DoubleOf(aBits);
		CHECK(!memcmp(&x, &y, 12) && !memcmp(&x, &z, 12), "TReal64 ctor/assign", 0);
		CHECK(BitsOf((TReal64)x) == aBits, "operator TReal64", 0);
		}
	}

static void RoundTrip32(TUint32 aBits)
	{
	TRealX x;
	TInt e1 = x.Set(FloatOf(aBits));
	TReal32 back = 0;
	TInt e2 = x.GetTReal(back);
	TUint32 frac = aBits & 0x007FFFFF;
	bool special = ((aBits>>23) & 0xFF) == 0xFF;
	TInt want = special ? (frac ? KErrArgument : KErrOverflow) : KErrNone;
	++gChecks;
	if (BitsOf(back) != aBits || e1 != want || e2 != want || x.iFlag != 0)
		Fail("TReal32 round trip", &x, 0, 0, e1*100 + e2);
	if (!special)
		{
		volatile long double ld = FloatOf(aBits);
		++gChecks;
		if (!SameAsLD(x, ld))
			Fail("TReal32 -> TRealX value", &x, 0, 0, 0);
		TRealX y(FloatOf(aBits));
		TRealX z; z = FloatOf(aBits);
		CHECK(!memcmp(&x, &y, 12) && !memcmp(&x, &z, 12), "TReal32 ctor/assign", 0);
		CHECK(BitsOf((TReal32)x) == aBits, "operator TReal32", 0);
		}
	}

static void TestIeee()
	{
	static const TUint64 k64[] = {
		0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x8000000000000001ULL,
		0x000FFFFFFFFFFFFFULL, 0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL,
		0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0xFFF8000000000000ULL,
		0x7FF0000000000001ULL, 0x7FF4000000000000ULL, 0x3FF0000000000000ULL, 0x0000000080000000ULL,
		0x0000000100000000ULL, 0x00000000FFFFFFFFULL, 0x0008000000000000ULL };
	for (unsigned i=0; i<sizeof(k64)/sizeof(k64[0]); ++i)
		RoundTrip64(k64[i]);
	for (int i=0; i<5000; ++i)
		{
		TUint64 b = Rnd();
		if (i % 5 == 0)
			b &= 0x800FFFFFFFFFFFFFULL;							// denormals
		if (i % 5 == 1)
			b = (b & 0x800FFFFFFFFFFFFFULL) >> RndRange(0, 51) | (b & 0x8000000000000000ULL);
		RoundTrip64(b);
		}
	static const TUint32 k32[] = { 0, 0x80000000u, 1, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu,
		0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x3F800000u, 0x00400000u };
	for (unsigned i=0; i<sizeof(k32)/sizeof(k32[0]); ++i)
		RoundTrip32(k32[i]);
	for (int i=0; i<5000; ++i)
		{
		TUint32 b = (TUint32)Rnd();
		if (i % 5 == 0)
			b &= 0x807FFFFFu;
		if (i % 5 == 1)
			b = ((b & 0x007FFFFFu) >> RndRange(0, 22)) | (b & 0x80000000u);
		RoundTrip32(b);
		}

	// Narrowing with rounding (flag 0): the hardware long double -> double/float conversion is the oracle
	for (int i=0; i<8000; ++i)
		{
		int sel = i % 4;
		TUint e = sel == 0 ? 0x7FFF + RndRange(-1030, 1030)
				: sel == 1 ? 0x7C00 + RndRange(-60, 4)				// double denormal band
				: sel == 2 ? 0x7F80 + RndRange(-30, 4)				// float denormal band
				:			 0x7FFF + RndRange(-140, 140);
		TRealX x = Mk(Rnd()&1, e, RndMant());
		volatile long double ld = ToLD(x);
		volatile double wd = (double)ld;
		volatile float wf = (float)ld;
		TReal64 gd = 0; TReal32 gf = 0;
		TInt ed = x.GetTReal(gd);
		TInt ef = x.GetTReal(gf);
		TInt wantd = isinf(wd) ? KErrOverflow : (wd == 0 ? KErrUnderflow : KErrNone);
		TInt wantf = isinf(wf) ? KErrOverflow : (wf == 0 ? KErrUnderflow : KErrNone);
		++gChecks;
		if (BitsOf(gd) != BitsOf((double)wd) || ed != wantd)
			Fail("GetTReal(TReal64) rounding", &x, 0, 0, ed);
		++gChecks;
		if (BitsOf(gf) != BitsOf((float)wf) || ef != wantf)
			Fail("GetTReal(TReal32) rounding", &x, 0, 0, ef);
		}

	// The TRealX rounding flags break an apparent tie (uc_realx.cia:1759-1764, 1861-1866)
	TReal64 d; TReal32 f;
	Mk(0, 0x7FFF, 0x8000000000000400ULL, 0).GetTReal(d); CHECK(BitsOf(d) == 0x3FF0000000000000ULL, "tie64 even", 0);
	Mk(0, 0x7FFF, 0x8000000000000400ULL, 1).GetTReal(d); CHECK(BitsOf(d) == 0x3FF0000000000001ULL, "tie64 flag1", 0);
	Mk(0, 0x7FFF, 0x8000000000000C00ULL, 0).GetTReal(d); CHECK(BitsOf(d) == 0x3FF0000000000002ULL, "tie64 odd", 0);
	Mk(0, 0x7FFF, 0x8000000000000C00ULL, 2).GetTReal(d); CHECK(BitsOf(d) == 0x3FF0000000000001ULL, "tie64 flag2", 0);
	Mk(0, 0x7FFF, 0x8000008000000000ULL, 0).GetTReal(f); CHECK(BitsOf(f) == 0x3F800000u, "tie32 even", 0);
	Mk(0, 0x7FFF, 0x8000008000000000ULL, 1).GetTReal(f); CHECK(BitsOf(f) == 0x3F800001u, "tie32 flag1", 0);
	Mk(0, 0x7FFF, 0x8000018000000000ULL, 0).GetTReal(f); CHECK(BitsOf(f) == 0x3F800002u, "tie32 odd", 0);
	Mk(0, 0x7FFF, 0x8000018000000000ULL, 2).GetTReal(f); CHECK(BitsOf(f) == 0x3F800001u, "tie32 flag2", 0);
	// overflow by rounding; total underflow; zero is not an underflow
	CHECK(Mk(1, 0x83FE, ~0ULL).GetTReal(d) == KErrOverflow && BitsOf(d) == 0xFFF0000000000000ULL, "round to inf 64", 0);
	CHECK(Mk(0, 0x807E, ~0ULL).GetTReal(f) == KErrOverflow && BitsOf(f) == 0x7F800000u, "round to inf 32", 0);
	CHECK(Mk(1, 0x7000, 1ULL<<63).GetTReal(d) == KErrUnderflow && BitsOf(d) == 0x8000000000000000ULL, "underflow 64", 0);
	CHECK(Mk(1, 0x7000, 1ULL<<63).GetTReal(f) == KErrUnderflow && BitsOf(f) == 0x80000000u, "underflow 32", 0);
	CHECK(Mk(1, 0, 0).GetTReal(d) == KErrNone && BitsOf(d) == 0x8000000000000000ULL, "-0 64", 0);
	CHECK(Mk(1, 0, 0).GetTReal(f) == KErrNone && BitsOf(f) == 0x80000000u, "-0 32", 0);
	}

static void TestIntegers()
	{
	static const TInt kInts[] = { 0, 1, -1, 2, -2, 3, 7, 0x7FFFFFFF, INT_MIN, INT_MIN+1, 0x40000000, 65536, -65535 };
	for (int i=0; i<2000; ++i)
		{
		TInt v = i < (int)(sizeof(kInts)/sizeof(kInts[0])) ? kInts[i] : (TInt)(Rnd() >> RndRange(32, 63));
		if (Rnd()&1)
			v = (TInt)(0u - (TUint)v);
		TRealX x(v), y, z;
		CHECK(y.Set(v) == KErrNone, "Set(TInt) ret", v);
		z = v;
		CHECK(SameAsLD(x, (long double)v) && x.iFlag == 0, "TRealX(TInt)", v);
		CHECK(!memcmp(&x, &y, 12) && !memcmp(&x, &z, 12), "TInt forms agree", v);
		CHECK((TInt)x == v, "TInt round trip", v);

		TUint u = (TUint)v;
		TRealX xu(u), yu, zu;
		CHECK(yu.Set(u) == KErrNone, "Set(TUint) ret", v);
		zu = u;
		CHECK(SameAsLD(xu, (long double)u), "TRealX(TUint)", v);
		CHECK(!memcmp(&xu, &yu, 12) && !memcmp(&xu, &zu, 12), "TUint forms agree", v);
		CHECK((TUint)xu == u, "TUint round trip", v);

		TInt64 w = (TInt64)(Rnd() >> RndRange(0, 63));
		if (i == 0) w = 0;
		if (i == 1) w = LLONG_MIN;
		if (i == 2) w = LLONG_MAX;
		if (i == 3) w = LLONG_MIN + 1;
		else if (Rnd()&1)
			w = (TInt64)(0ULL - (TUint64)w);
		TRealX xw(w), yw, zw;
		CHECK(yw.Set(w) == KErrNone, "Set(TInt64) ret", i);
		zw = w;
		CHECK(SameAsLD(xw, (long double)w) && xw.iFlag == 0, "TRealX(TInt64)", i);
		CHECK(!memcmp(&xw, &yw, 12) && !memcmp(&xw, &zw, 12), "TInt64 forms agree", i);
		CHECK((TInt64)xw == w, "TInt64 round trip", i);
		}
	// truncation toward zero of in-range non-integers
	for (int i=0; i<4000; ++i)
		{
		TRealX x = Mk(Rnd()&1, 0x7FFF + RndRange(-5, 62), RndMant());
		long double ld = ToLD(x);
		TInt64 w = (TInt64)ld;								// C truncates toward zero
		CHECK((TInt64)x == w, "TInt64 truncation", i);
		if (ld > -2147483648.0L && ld < 2147483648.0L)
			CHECK((TInt)x == (TInt)w, "TInt truncation", i);
		if (ld >= 0 && ld < 4294967296.0L)
			CHECK((TUint)x == (TUint)w, "TUint truncation", i);
		else if (ld < 0)
			CHECK((TUint)x == 0, "TUint of negative", i);
		}
	// saturation (uc_realx.cia:1526-1677)
	TRealX inf, ninf, nan;
	inf.SetInfinite(EFalse); ninf.SetInfinite(ETrue); nan.SetNaN();
	CHECK((TInt)Mk(0, 0x801E, 1ULL<<63) == 0x7FFFFFFF, "TInt +2^31", 0);
	CHECK((TInt)Mk(1, 0x801E, 1ULL<<63) == INT_MIN, "TInt -2^31", 0);
	CHECK((TInt)Mk(1, 0x8050, ~0ULL) == INT_MIN, "TInt -big", 0);
	CHECK((TInt)Mk(0, 0x801D, ~0ULL) == 0x7FFFFFFF, "TInt 2^31-eps truncates", 0);
	CHECK((TInt)inf == 0x7FFFFFFF && (TInt)ninf == INT_MIN && (TInt)nan == 0, "TInt specials", 0);
	CHECK((TInt)Mk(1, 0x7FFE, ~0ULL) == 0 && (TInt)Mk(0, 0, 0) == 0 && (TInt)Mk(0, 1, 1ULL<<63) == 0, "TInt small", 0);
	CHECK((TUint)Mk(0, 0x801E, ~0ULL) == 0xFFFFFFFFu, "TUint max", 0);
	CHECK((TUint)Mk(0, 0x801F, 1ULL<<63) == 0xFFFFFFFFu, "TUint 2^32", 0);
	CHECK((TUint)Mk(1, 0x801F, 1ULL<<63) == 0 && (TUint)Mk(1, 0x7FFF, 1ULL<<63) == 0, "TUint negative", 0);
	CHECK((TUint)inf == 0xFFFFFFFFu && (TUint)ninf == 0 && (TUint)nan == 0, "TUint specials", 0);
	CHECK((TInt64)Mk(0, 0x803E, 1ULL<<63) == LLONG_MAX, "TInt64 +2^63", 0);
	CHECK((TInt64)Mk(1, 0x803E, 1ULL<<63) == LLONG_MIN, "TInt64 -2^63", 0);
	CHECK((TInt64)Mk(1, 0x9000, ~0ULL) == LLONG_MIN && (TInt64)Mk(0, 0x9000, ~0ULL) == LLONG_MAX, "TInt64 big", 0);
	CHECK((TInt64)inf == LLONG_MAX && (TInt64)ninf == LLONG_MIN && (TInt64)nan == 0, "TInt64 specials", 0);
	CHECK((TInt64)Mk(0, 0x803D, ~0ULL) == LLONG_MAX, "TInt64 2^63-1", 0);
	}

static TRealX::TRealXOrder LdOrder(long double a, long double b)
	{
	return a < b ? TRealX::ELessThan : a > b ? TRealX::EGreaterThan : a == b ? TRealX::EEqual : TRealX::EUnordered;
	}

static void TestCompare()
	{
	TRealX vals[16];
	vals[0] = Mk(0, 0, 0); vals[1] = Mk(1, 0, 0);
	vals[2].SetInfinite(EFalse); vals[3].SetInfinite(ETrue); vals[4].SetNaN();
	vals[5] = Mk(0, 0xFFFF, 0x8000000000000001ULL);			// an SNaN
	vals[6] = Mk(0, 0xFFFE, ~0ULL); vals[7] = Mk(1, 0xFFFE, ~0ULL);
	vals[8] = Mk(0, 1, 1ULL<<63); vals[9] = Mk(1, 1, 1ULL<<63);
	vals[10] = TRealX(1); vals[11] = TRealX(-1);
	for (int i=12; i<16; ++i)
		vals[i] = Mk(Rnd()&1, 0x7FFF + RndRange(-2, 2), RndMant());
	for (int i=0; i<16; ++i)
		for (int j=0; j<16; ++j)
			{
			const TRealX& a = vals[i]; const TRealX& b = vals[j];
			// the extreme exponents are outside x87 range: scale those towards the middle, order is kept
			TRealX sa = a, sb = b;
			if (sa.iExp == 0xFFFE) sa.iExp = 0xB000; if (sa.iExp == 1) sa.iExp = 0x5000;
			if (sb.iExp == 0xFFFE) sb.iExp = 0xB000; if (sb.iExp == 1) sb.iExp = 0x5000;
			TRealX::TRealXOrder want = LdOrder(ToLD(sa), ToLD(sb));
			TRealX::TRealXOrder got = a.Compare(b);
			++gChecks;
			if (got != want)
				Fail("Compare", &a, &b, 0, got*10 + want);
			CHECK(!!(a == b) == (want == TRealX::EEqual) && !!(a != b) == (want != TRealX::EEqual)
				&& !!(a < b) == (want == TRealX::ELessThan) && !!(a > b) == (want == TRealX::EGreaterThan)
				&& !!(a <= b) == (want == TRealX::ELessThan || want == TRealX::EEqual)
				&& !!(a >= b) == (want == TRealX::EGreaterThan || want == TRealX::EEqual), "comparison operators", i*16+j);
			}
	for (int i=0; i<4000; ++i)
		{
		TRealX a = Mk(Rnd()&1, 0x7FFF + RndRange(-2, 2), RndMant());
		TRealX b = Mk(Rnd()&1, 0x7FFF + RndRange(-2, 2), RndMant());
		if (i % 7 == 0)
			{ b = a; b.iFlag = 2; }							// flags do not take part
		++gChecks;
		if (a.Compare(b) != LdOrder(ToLD(a), ToLD(b)))
			Fail("Compare random", &a, &b, 0, 0);
		}
	}

static bool Is(const TRealX& aX, TUint aSign, TUint aExp, TUint64 aMant, TUint aFlag = 0)
	{
	return (TUint)(TUint8)aX.iSign == aSign && aX.iExp == aExp && MantOf(aX) == aMant && aX.iFlag == aFlag;
	}

static void TestSpecials()
	{
	const TUint64 msb = 1ULL<<63, indef = 0xC000000000000000ULL;
	TRealX r, zero, nzero = Mk(1, 0, 0), one(1), mone(-1), inf, ninf, nan;
	inf.SetInfinite(EFalse); ninf.SetInfinite(ETrue); nan.SetNaN();
	TRealX big = Mk(0, 0xFFFE, ~0ULL), tiny = Mk(0, 1, msb), tiny2 = Mk(0, 1, msb|1);

	CHECK(Is(zero, 0, 0, 0) && zero.IsZero() && nzero.IsZero() && zero.IsFinite() && !zero.IsNaN() && !zero.IsInfinite(), "zero", 0);
	CHECK(Is(nan, 1, 0xFFFF, indef) && nan.IsNaN() && !nan.IsInfinite() && !nan.IsFinite() && !nan.IsZero(), "SetNaN", 0);
	CHECK(Is(inf, 0, 0xFFFF, msb) && Is(ninf, 1, 0xFFFF, msb) && inf.IsInfinite() && !inf.IsNaN() && !inf.IsFinite(), "SetInfinite", 0);
	r = one; r.SetZero(ETrue); CHECK(Is(r, 1, 0, 0), "SetZero(-)", 0);
	CHECK(Is(one, 0, 0x7FFF, msb) && Is(mone, 1, 0x7FFF, msb) && one.IsFinite(), "one", 0);
	CHECK(Is(-one, 1, 0x7FFF, msb) && Is(+mone, 1, 0x7FFF, msb) && Is(-zero, 1, 0, 0) && Is(-nan, 0, 0xFFFF, indef), "unary", 0);

	// signed zeros (uc_realx.cia:86-87)
	CHECK(zero.Add(r, zero) == KErrNone && Is(r, 0, 0, 0), "+0 + +0", 0);
	CHECK(nzero.Add(r, nzero) == KErrNone && Is(r, 1, 0, 0), "-0 + -0", 0);
	CHECK(zero.Add(r, nzero) == KErrNone && Is(r, 0, 0, 0) && nzero.Add(r, zero) == KErrNone && Is(r, 0, 0, 0), "+0 + -0", 0);
	CHECK(one.Add(r, mone) == KErrNone && Is(r, 0, 0, 0) && mone.Sub(r, mone) == KErrNone && Is(r, 0, 0, 0), "x - x = +0", 0);
	CHECK(mone.Mult(r, zero) == KErrNone && Is(r, 1, 0, 0) && zero.Mult(r, one) == KErrNone && Is(r, 0, 0, 0), "0 * x", 0);
	CHECK(nzero.Div(r, one) == KErrNone && r.IsZero() && r.iSign == 1 && zero.Div(r, mone) == KErrNone && r.IsZero() && r.iSign == 1, "0 / x", 0);
	CHECK(one.Div(r, ninf) == KErrNone && Is(r, 1, 0, 0), "x / inf", 0);
	// operand asymmetry of x + 0 (uc_realx.cia:294-308): *this comes back with its flags, aVal without
	TRealX flagged = Mk(0, 0x8000, msb|5, 1);
	CHECK(flagged.Add(r, zero) == KErrNone && Is(r, 0, 0x8000, msb|5, 1), "this + 0 keeps flag", 0);
	CHECK(zero.Add(r, flagged) == KErrNone && Is(r, 0, 0x8000, msb|5, 0), "0 + aVal clears flag", 0);
	// exponents more than 64 apart (uc_realx.cia:216-225)
	CHECK(big.Add(r, one) == KErrNone && Is(r, 0, 0xFFFE, ~0ULL, 1), "big + 1", 0);
	CHECK(big.Sub(r, one) == KErrNone && Is(r, 0, 0xFFFE, ~0ULL, 2), "big - 1", 0);
	CHECK(one.Sub(r, big) == KErrNone && Is(r, 1, 0xFFFE, ~0ULL, 2), "1 - big", 0);
	CHECK(Mk(0, 0x8041, msb).Sub(r, Mk(0, 0x8000, ~0ULL)) == KErrNone && Is(r, 0, 0x8041, msb, 2), "2^66 - (2^1-eps): asm keeps 2^66", 0);
	// ... and on that path only, the IA-32 code lets the iFlag of *this leak in when *this is the larger
	CHECK(Mk(0, 0x8050, msb|1, 2).Add(r, one) == KErrNone && Is(r, 0, 0x8050, msb|1, 3), ">64 apart: this.iFlag leaks", 0);

	// overflow / underflow / divide by zero
	CHECK(big.Add(r, big) == KErrOverflow && Is(r, 0, 0xFFFF, msb), "add overflow", 0);
	CHECK(big.Sub(r, -big) == KErrOverflow && Is(r, 0, 0xFFFF, msb), "sub overflow", 0);
	CHECK((-big).Mult(r, big) == KErrOverflow && Is(r, 1, 0xFFFF, msb), "mult overflow", 0);
	CHECK(big.Div(r, tiny) == KErrOverflow && Is(r, 0, 0xFFFF, msb), "div overflow", 0);
	CHECK(tiny.Mult(r, -tiny) == KErrUnderflow && Is(r, 1, 0, 0), "mult underflow", 0);
	CHECK(tiny.Div(r, big) == KErrUnderflow && Is(r, 0, 0, 0), "div underflow", 0);
	CHECK(tiny2.Sub(r, tiny) == KErrUnderflow && Is(r, 0, 0, 0), "sub underflow", 0);
	CHECK(tiny.Sub(r, tiny2) == KErrUnderflow && Is(r, 1, 0, 0), "sub underflow -", 0);
	CHECK(one.Div(r, zero) == KErrDivideByZero && Is(r, 0, 0xFFFF, msb), "1/0", 0);
	CHECK(mone.Div(r, zero) == KErrDivideByZero && Is(r, 1, 0xFFFF, msb) && one.Div(r, nzero) == KErrDivideByZero && Is(r, 1, 0xFFFF, msb), "-1/0", 0);
	// largest finite results just in range
	CHECK(Mk(0, 0xFFFE, msb).Add(r, Mk(0, 0xFFFD, msb)) == KErrNone && Is(r, 0, 0xFFFE, 0xC000000000000000ULL), "near-max add", 0);
	CHECK(Mk(0, 0xFFFE, ~0ULL).Add(r, Mk(0, 0xFFBE, msb)) == KErrOverflow && Is(r, 0, 0xFFFF, msb), "round up into overflow", 0);

	// invalid operations -> 'real indefinite' + KErrArgument
	CHECK(inf.Add(r, ninf) == KErrArgument && Is(r, 1, 0xFFFF, indef), "inf + -inf", 0);
	CHECK(inf.Sub(r, inf) == KErrArgument && Is(r, 1, 0xFFFF, indef), "inf - inf", 0);
	CHECK(zero.Mult(r, inf) == KErrArgument && Is(r, 1, 0xFFFF, indef) && ninf.Mult(r, zero) == KErrArgument && Is(r, 1, 0xFFFF, indef), "0 * inf", 0);
	CHECK(zero.Div(r, nzero) == KErrArgument && Is(r, 1, 0xFFFF, indef), "0 / 0", 0);
	CHECK(inf.Div(r, ninf) == KErrArgument && Is(r, 1, 0xFFFF, indef), "inf / inf", 0);
	CHECK(one.Mod(r, zero) == KErrArgument && Is(r, 1, 0xFFFF, indef) && zero.Mod(r, zero) == KErrArgument, "x % 0", 0);
	CHECK(inf.Mod(r, one) == KErrArgument && Is(r, 1, 0xFFFF, indef) && inf.Mod(r, inf) == KErrArgument, "inf % x", 0);
	// infinity arithmetic -> infinity + KErrOverflow
	CHECK(inf.Add(r, inf) == KErrOverflow && Is(r, 0, 0xFFFF, msb) && ninf.Add(r, one) == KErrOverflow && Is(r, 1, 0xFFFF, msb), "inf + x", 0);
	CHECK(one.Sub(r, inf) == KErrOverflow && Is(r, 1, 0xFFFF, msb), "x - inf", 0);
	CHECK(inf.Mult(r, ninf) == KErrOverflow && Is(r, 1, 0xFFFF, msb) && mone.Mult(r, inf) == KErrOverflow && Is(r, 1, 0xFFFF, msb), "inf * x", 0);
	CHECK(ninf.Div(r, one) == KErrOverflow && Is(r, 1, 0xFFFF, msb) && inf.Div(r, zero) == KErrOverflow && Is(r, 0, 0xFFFF, msb), "inf / x", 0);
	CHECK(mone.Mod(r, inf) == KErrNone && Is(r, 1, 0x7FFF, msb), "x % inf", 0);

	// NaN propagation (uc_realx.cia:41-81): larger significand wins, quietened, KErrArgument
	TRealX snan = Mk(0, 0xFFFF, msb|0x1234), qnanBig = Mk(1, 0xFFFF, indef|0x99);
	CHECK(snan.Add(r, one) == KErrArgument && Is(r, 0, 0xFFFF, indef|0x1234), "snan + 1", 0);
	CHECK(one.Mult(r, snan) == KErrArgument && Is(r, 0, 0xFFFF, indef|0x1234), "1 * snan", 0);
	CHECK(inf.Div(r, snan) == KErrArgument && Is(r, 0, 0xFFFF, indef|0x1234), "inf / snan", 0);
	CHECK(snan.Add(r, qnanBig) == KErrArgument && Is(r, 1, 0xFFFF, indef|0x99), "snan + qnan", 0);
	CHECK(qnanBig.Add(r, snan) == KErrArgument && Is(r, 1, 0xFFFF, indef|0x99), "qnan + snan", 0);
	CHECK(snan.Mod(r, one) == KErrArgument && r.IsNaN() && one.Mod(r, snan) == KErrArgument && r.IsNaN(), "nan % x", 0);
	CHECK(zero.Div(r, nan) == KErrArgument && r.IsNaN() && nan.Sub(r, inf) == KErrArgument && r.IsNaN(), "nan misc", 0);

	// Mod edge cases (uc_realx.cia:757-759, 835-848)
	CHECK(Mk(0, 0x7FFF+70, msb).Mod(r, TRealX(3)) == KErrTotalLossOfPrecision && r.IsZero(), "mod loss of precision", 0);
	CHECK(Mk(1, 0x7FFF+63, msb|1).Mod(r, TRealX(2)) == KErrNone && Is(r, 1, 0x7FFF, msb), "-(2^63+1) % 2", 0);
	CHECK(TRealX(6).Mod(r, TRealX(-3)) == KErrNone && Is(r, 0, 0, 0) && TRealX(-6).Mod(r, TRealX(3)) == KErrNone && Is(r, 1, 0, 0), "exact mod -> signed 0", 0);
	CHECK(Mk(0, 5, msb|1).Mod(r, Mk(0, 5, msb)) == KErrUnderflow && r.IsZero(), "mod underflow", 0);
	CHECK(zero.Mod(r, one) == KErrNone && Is(r, 0, 0, 0), "0 % x", 0);

	// operator forms: result first, then panic MATHX <error> (TRealXPanic is a recorder here)
	gPanic = 0; r = one + one; CHECK(gPanic == 0 && Is(r, 0, 0x8000, msb), "1+1", gPanic);
	gPanic = 0; r = big + big; CHECK(gPanic == KErrOverflow && r.IsInfinite(), "operator+ panic", gPanic);
	gPanic = 0; r = big - (-big); CHECK(gPanic == KErrOverflow, "operator- panic", gPanic);
	gPanic = 0; r = tiny * tiny; CHECK(gPanic == KErrUnderflow && r.IsZero(), "operator* panic", gPanic);
	gPanic = 0; r = one / zero; CHECK(gPanic == KErrDivideByZero && r.IsInfinite(), "operator/ panic", gPanic);
	gPanic = 0; r = one % zero; CHECK(gPanic == KErrArgument && r.IsNaN(), "operator% panic", gPanic);
	gPanic = 0; r = one; r += nan; CHECK(gPanic == KErrArgument && r.IsNaN(), "operator+= panic", gPanic);
	gPanic = 0; r = inf; r %= one; CHECK(gPanic == KErrArgument, "operator%= panic", gPanic);
	gPanic = 0; r = inf; ++r; CHECK(gPanic == KErrOverflow && r.IsInfinite(), "++inf panic", gPanic);
	gPanic = 0;
	TRealX c(5);
	TRealX old = c++; CHECK(Is(old, 0, 0x8001, 0xA000000000000000ULL) && (TInt)c == 6, "post++", 0);
	old = c--; CHECK((TInt)old == 6 && (TInt)c == 5, "post--", 0);
	CHECK((TInt)++c == 6 && (TInt)--c == 5 && (TInt)--c == 4, "pre++/--", 0);
	c = 1; --c; CHECK(Is(c, 0, 0, 0), "1-- = +0", 0);
	c = -1; ++c; CHECK(Is(c, 0, 0, 0), "-1++ = +0", 0);
	CHECK(gPanic == 0, "no stray panic", gPanic);
	r = TRealX(7); CHECK(r.AddEq(one) == KErrNone && (TInt)r == 8 && r.SubEq(TRealX(3)) == KErrNone && (TInt)r == 5
		&& r.MultEq(TRealX(4)) == KErrNone && (TInt)r == 20 && r.DivEq(TRealX(8)) == KErrNone && Is(r, 0, 0x8000, 0xA000000000000000ULL)
		&& r.ModEq(one) == KErrNone && Is(r, 0, 0x7FFE, msb), "Eq forms", 0);
	CHECK(one.DivEq(zero) == KErrDivideByZero, "DivEq by zero", 0);
	// the raw-word constructor
	r = TRealX(0x80010201u, 0x89ABCDEFu, 0x01234567u);
	CHECK(r.iExp == 0x8001 && r.iFlag == 2 && r.iSign == 1 && r.iMantHi == 0x89ABCDEFu && r.iMantLo == 0x01234567u, "raw ctor", 0);
	}

int main()
	{
	if (sizeof(long double) < 10 || sizeof(TRealX) != 12)
		{
		printf("REALX FAIL host has no x87 long double / bad layout\n");
		return 1;
		}
	fesetround(FE_TONEAREST);
	TestSpecials();
	TestIeee();
	TestIntegers();
	TestCompare();
	TestArithmetic();
	printf("checks: %d\n", gChecks);
	if (gFail)
		printf("REALX FAIL %d\n", gFail);
	else
		printf("REALX OK\n");
	return gFail ? 1 : 0;
	}
