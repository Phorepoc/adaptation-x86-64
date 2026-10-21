// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xu_realx.cpp -- Phorepoc amd64/x32 euser: sibling of euser/epoc/x86/uc_realx.cia (TRealX).
// The IA-32 original is 3348 lines of register-level asm; nothing in it needs to BE asm, so this is
// plain integer C++ (64-bit mantissa, unsigned __int128 for the wide intermediates), ABI-neutral
// (-mx32 and -m64), no floating-point arithmetic anywhere: TReal32/TReal64 are only ever bit patterns.
// It is a TRANSLATION, not a re-design: every special case, return code, rounding-flag rule and
// operand asymmetry below follows the asm (line references are to the x86 uc_realx.cia). The C++ half
// (uc_realx.cpp: TRealXPanic) and the inline comparison operators (e32math.inl) are unchanged upstream.
//
// Conventions inherited from the asm, worth knowing when reading:
//   * every binary primitive works on "reg" (= aVal, loaded into ecx,edx:ebx, rounding flags cleared)
//     and "src" (= *this, read through esi). Where the asm hands an operand back "unaltered" it is the
//     in-memory one, WITH its old iFlag; "reg" comes back with iFlag=0. That asymmetry is kept.
//   * a zero result only has iExp=0; the asm does not always clear the mantissa (0/x, Mod underflow,
//     Mod total-loss-of-precision). Kept: zero-ness is decided by iExp everywhere.
//   * the ONE deliberate deviation: the IA-32 add leaves bit 1 of the sign byte set after an effective
//     subtraction (cl bit 1 = xor of the signs, uc_realx.cia:100-106, never cleared before the store),
//     and mult/div xor whole sign bytes. Here iSign is always written as 0 or 1 (bit 0 is all any
//     consumer, asm included, ever tests).
// Hosted test: realx_test.cpp / build_realx_test.sh (vs x87 long double, bit-exact).

#include <e32math.h>

void TRealXPanic(TInt aErr);						// uc_realx.cpp

typedef unsigned __int128 TUint128;
typedef char TRealXMustBe12Bytes[(sizeof(TRealX)==12) ? 1 : -1];

static const TUint64 KMantMsb = 0x8000000000000000ULL;		// mantissa of infinity / of 1.0
static const TUint64 KMantIndefinite = 0xC000000000000000ULL;
static const TUint32 KExpSpecial = 0xFFFF;

// An unpacked TRealX: what the asm carries in ecx (exponent:flags:sign) and edx:ebx (mantissa).
struct SRealX
	{
	TUint64 iMant;
	TUint32 iExp;
	TUint32 iSign;		// 0 or 1
	TUint32 iFlag;
	};

static inline void Load(SRealX& aR, const TRealX& aX)
	{
	aR.iMant = (TUint64(aX.iMantHi)<<32) | aX.iMantLo;
	aR.iExp = aX.iExp;
	aR.iSign = TUint32(aX.iSign) & 1;
	aR.iFlag = aX.iFlag;
	}

static inline void Store(TRealX& aX, const SRealX& aR)
	{
	aX.iMantLo = TUint32(aR.iMant);
	aX.iMantHi = TUint32(aR.iMant>>32);
	aX.iExp = TUint16(aR.iExp);
	aX.iSign = TInt8(aR.iSign);
	aX.iFlag = TUint8(aR.iFlag);
	}

static inline TBool IsNaNVal(const SRealX& aR)
	{
	return aR.iExp==KExpSpecial && aR.iMant!=KMantMsb;
	}

static inline void SetVal(SRealX& aR, TUint32 aExp, TUint32 aSign, TUint64 aMant)
	{
	aR.iExp = aExp;
	aR.iSign = aSign;
	aR.iFlag = 0;
	aR.iMant = aMant;
	}

// TRealXRealIndefinite (uc_realx.cia:31)
static TInt RealIndefinite(SRealX& aReg)
	{
	SetVal(aReg, KExpSpecial, 1, KMantIndefinite);
	return KErrArgument;
	}

// TRealXBinOpNaN (uc_realx.cia:41): at least one operand is a NaN. The NaN with the larger
// significand wins (reg on a tie), and is quietened.
static TInt BinOpNaN(SRealX& aReg, const SRealX& aSrc)
	{
	SRealX other = aSrc;
	if (!IsNaNVal(aReg))
		{
		other = aReg;
		aReg = aSrc;
		}
	if (IsNaNVal(other) && aReg.iMant < other.iMant)
		aReg = other;
	aReg.iMant |= 0x4000000000000000ULL;			// SNaN -> QNaN
	return KErrArgument;
	}

// roundfp (uc_realx.cia:175): round the 64-bit mantissa on a 32-bit rounding word plus the sticky
// information in aReg.iFlag (1 = true value is above mantissa:word, 2 = below).
static TInt RoundAdd(SRealX& aReg, TUint32 aRound)
	{
	TBool up = EFalse;
	if (aRound > 0x80000000u)
		up = ETrue;
	else if (aRound == 0x80000000u)
		{
		if (aReg.iFlag & 1)
			up = ETrue;
		else if (aReg.iFlag & 2)
			up = EFalse;
		else
			up = TBool(aReg.iMant & 1);				// round to even
		}
	if (up)
		{
		if (++aReg.iMant == 0)
			{
			aReg.iMant = KMantMsb;
			++aReg.iExp;
			}
		}
	if (aReg.iExp >= KExpSpecial)
		{
		SetVal(aReg, KExpSpecial, aReg.iSign, KMantMsb);
		return KErrOverflow;
		}
	if (up)
		aReg.iFlag = 2;
	else if (aRound)
		aReg.iFlag = 1;								// else leave the sticky flag as it is
	return KErrNone;
	}

// TRealXAdd (uc_realx.cia:88): aReg = aSrc + aReg.
// +0 + +0 = +0, -0 + -0 = -0, +0 + -0 = +0, X + -X = +0.
static TInt XAdd(SRealX& aReg, const SRealX& aSrc)
	{
	aReg.iFlag = 0;
	if (aReg.iExp == KExpSpecial)
		{
		if (IsNaNVal(aReg))
			return BinOpNaN(aReg, aSrc);
		if (aSrc.iExp == KExpSpecial)
			{
			if (IsNaNVal(aSrc))
				return BinOpNaN(aReg, aSrc);
			if (aSrc.iSign != aReg.iSign)
				return RealIndefinite(aReg);		// inf - inf
			}
		return KErrOverflow;						// reg [infinity] unaltered
		}
	if (aSrc.iExp == KExpSpecial)
		{
		if (IsNaNVal(aSrc))
			return BinOpNaN(aReg, aSrc);
		aReg = aSrc;								// source [infinity] unaltered
		return KErrOverflow;
		}
	if (aSrc.iExp == 0)
		{
		if (aReg.iExp == 0)
			{
			aReg.iSign &= aSrc.iSign;				// both zero: negative iff both negative
			aReg.iFlag = 0;
			}
		return KErrNone;							// else reg unaltered
		}
	if (aReg.iExp == 0)
		{
		aReg = aSrc;								// source unaltered, flags included
		return KErrNone;
		}

	const TBool subtract = (aReg.iSign != aSrc.iSign);
	TUint64 big = aReg.iMant;
	TUint64 small = aSrc.iMant;
	TUint32 flag = 0;
	TInt shift = TInt(aReg.iExp) - TInt(aSrc.iExp);
	if (shift < 0)
		{											// source is the larger: its exponent and sign
		shift = -shift;
		big = aSrc.iMant;
		small = aReg.iMant;
		aReg.iExp = aSrc.iExp;
		aReg.iSign = aSrc.iSign;
		flag = aSrc.iFlag;							// (only survives on the >64 path, as in the asm)
		}
	if (shift > 64)
		{											// addfp2: the larger operand is the answer
		flag |= 1;
		if (subtract)
			flag ^= 3;
		aReg.iMant = big;
		aReg.iFlag = flag;
		return KErrNone;
		}
	// small:0 >> shift = mantissa : 32-bit rounding word : sticky
	const TUint128 wide = (TUint128(small)<<64) >> shift;
	small = TUint64(wide>>64);
	TUint32 round = TUint32(wide>>32);
	flag = TUint32(wide) ? 1 : 0;

	if (!subtract)
		{
		TUint64 sum = big + small;
		if (sum < big)
			{										// carry: shift it back in, through the rounding word
			if (round & 1)
				flag |= 1;
			round = (round>>1) | (TUint32(sum)<<31);
			sum = (sum>>1) | KMantMsb;
			++aReg.iExp;
			}
		aReg.iMant = sum;
		aReg.iFlag = flag;
		return RoundAdd(aReg, round);
		}

	// subfp1: 96-bit subtract big:0 - small:round
	flag <<= 1;										// bits lost from the subtrahend: result is rounded UP
	const TUint128 a = TUint128(big)<<32;
	const TUint128 b = (TUint128(small)<<32) | round;
	TUint128 diff;
	if (a >= b)
		diff = a - b;
	else
		{
		diff = b - a;
		aReg.iSign ^= 1;
		flag >>= 1;
		}
	if (diff == 0)
		{
		SetVal(aReg, 0, 0, 0);						// exactly zero: +0
		return KErrNone;
		}
	TUint64 mant = TUint64(diff>>32);
	round = TUint32(diff);
	TInt exp = TInt(aReg.iExp);
	if ((mant>>32) == 0)
		{
		if (mant)
			{
			mant = (mant<<32) | round;
			exp -= 32;
			}
		else
			{
			mant = TUint64(round)<<32;
			exp -= 64;
			}
		round = 0;
		}
	const TInt norm = __builtin_clzll(mant);
	if (norm)
		{
		mant = (mant<<norm) | (round>>(32-norm));
		round <<= norm;
		exp -= norm;
		}
	if (exp <= 0)
		{											// subfpundflw
		SetVal(aReg, 0, aReg.iSign, 0);
		return KErrUnderflow;
		}
	aReg.iMant = mant;
	aReg.iExp = TUint32(exp);
	aReg.iFlag = flag;
	return RoundAdd(aReg, round);
	}

// TRealXSubtract (uc_realx.cia:352): aReg = aSrc - aReg
static TInt XSub(SRealX& aReg, const SRealX& aSrc)
	{
	aReg.iSign ^= 1;
	return XAdd(aReg, aSrc);
	}

// Common tail of multiply and divide: signed 32-bit exponent range check, after rounding.
static TInt FinishMulDiv(SRealX& aReg, TInt aExp, TUint64 aMant, TUint32 aFlag)
	{
	if (aExp >= TInt(KExpSpecial))
		{
		SetVal(aReg, KExpSpecial, aReg.iSign, KMantMsb);
		return KErrOverflow;
		}
	if (aExp <= 0)
		{
		SetVal(aReg, 0, aReg.iSign, 0);
		return KErrUnderflow;
		}
	aReg.iExp = TUint32(aExp);
	aReg.iMant = aMant;
	aReg.iFlag = aFlag;
	return KErrNone;
	}

// TRealXMultiply (uc_realx.cia:361): aReg = aSrc * aReg
static TInt XMul(SRealX& aReg, const SRealX& aSrc)
	{
	aReg.iFlag = 0;
	aReg.iSign ^= aSrc.iSign;
	if (aReg.iExp == KExpSpecial)
		{
		if (IsNaNVal(aReg))
			return BinOpNaN(aReg, aSrc);
		if (aSrc.iExp == KExpSpecial)
			{
			if (IsNaNVal(aSrc))
				return BinOpNaN(aReg, aSrc);
			return KErrOverflow;					// inf * inf
			}
		if (aSrc.iExp == 0)
			return RealIndefinite(aReg);			// inf * 0
		return KErrOverflow;
		}
	if (aSrc.iExp == KExpSpecial)
		{
		if (IsNaNVal(aSrc))
			return BinOpNaN(aReg, aSrc);
		if (aReg.iExp == 0)
			return RealIndefinite(aReg);			// 0 * inf
		SetVal(aReg, KExpSpecial, aReg.iSign, aSrc.iMant);
		return KErrOverflow;
		}
	if (aSrc.iExp == 0 || aReg.iExp == 0)
		{
		SetVal(aReg, 0, aReg.iSign, 0);
		return KErrNone;
		}

	TInt exp = TInt(aReg.iExp) + TInt(aSrc.iExp) - 0x7FFE;
	TUint128 prod = TUint128(aReg.iMant) * aSrc.iMant;
	if (!TUint64(prod>>127))
		{											// only one shift can ever be needed
		prod <<= 1;
		--exp;
		}
	TUint64 mant = TUint64(prod>>64);
	const TUint64 low = TUint64(prod);
	TUint32 flag = 0;
	if (low > KMantMsb || (low == KMantMsb && (mant & 1)))
		{
		if (++mant == 0)
			{
			mant = KMantMsb;
			++exp;
			}
		flag = 2;
		}
	else if (low)
		flag = 1;
	return FinishMulDiv(aReg, exp, mant, flag);
	}

// floor((aRem * 2^64) / aDiv) for aRem < aDiv, remainder back in aRem. Restoring division rather
// than an __int128 divide: euser links without libgcc (no __udivti3).
static TUint64 DivideStep64(TUint64& aRem, TUint64 aDiv)
	{
	TUint64 q = 0;
	TUint64 r = aRem;
	for (TInt i=0; i<64; ++i)
		{
		const TUint64 carry = r>>63;
		r <<= 1;
		q <<= 1;
		if (carry || r >= aDiv)
			{
			r -= aDiv;
			q |= 1;
			}
		}
	aRem = r;
	return q;
	}

// TRealXDivide (uc_realx.cia:556): aReg = aSrc / aReg
static TInt XDiv(SRealX& aReg, const SRealX& aSrc)
	{
	aReg.iFlag = 0;
	aReg.iSign ^= aSrc.iSign;
	if (aSrc.iExp == KExpSpecial)
		{
		if (IsNaNVal(aSrc))
			return BinOpNaN(aReg, aSrc);
		if (aReg.iExp == KExpSpecial)
			{
			if (IsNaNVal(aReg))
				return BinOpNaN(aReg, aSrc);
			return RealIndefinite(aReg);			// inf / inf
			}
		SetVal(aReg, KExpSpecial, aReg.iSign, KMantMsb);
		return KErrOverflow;						// inf / finite (zero included)
		}
	if (aReg.iExp == KExpSpecial)
		{
		if (IsNaNVal(aReg))
			return BinOpNaN(aReg, aSrc);
		SetVal(aReg, 0, aReg.iSign, 0);				// finite / inf = 0
		return KErrNone;
		}
	if (aReg.iExp == 0)
		{
		if (aSrc.iExp == 0)
			return RealIndefinite(aReg);			// 0 / 0
		SetVal(aReg, KExpSpecial, aReg.iSign, KMantMsb);
		return KErrDivideByZero;
		}
	if (aSrc.iExp == 0)
		{
		aReg.iExp = 0;								// divfpdd0: 'and ecx, 1' only - the divisor's
		return KErrNone;							// mantissa stays in the zero result
		}

	TInt exp = TInt(aSrc.iExp) - TInt(aReg.iExp) + 0x7FFE;
	const TUint64 divisor = aReg.iMant;
	TUint64 rem = aSrc.iMant;
	TBool intBit = EFalse;
	if (rem >= divisor)
		{
		rem -= divisor;
		intBit = ETrue;
		++exp;
		}
	TUint64 mant = DivideStep64(rem, divisor);		// 64 quotient bits after the integer bit
	TBool roundBit;
	if (intBit)
		{
		roundBit = TBool(mant & 1);
		mant = (mant>>1) | KMantMsb;
		}
	else
		{											// one more quotient bit is the rounding bit
		const TUint64 carry = rem>>63;
		rem <<= 1;
		roundBit = (carry || rem >= divisor);
		if (roundBit)
			rem -= divisor;
		}
	TUint32 flag = 0;
	if (roundBit && (rem || (mant & 1)))
		{
		if (++mant == 0)
			{
			mant = KMantMsb;
			++exp;
			}
		flag = 2;
		}
	else if (roundBit || rem)
		flag = 1;
	return FinishMulDiv(aReg, exp, mant, flag);
	}

// TRealXModulo (uc_realx.cia:743): aReg = aSrc % aReg, exact, sign of the dividend.
static TInt XMod(SRealX& aReg, const SRealX& aSrc)
	{
	aReg.iSign = aSrc.iSign;
	aReg.iFlag = 0;
	if (aSrc.iExp == KExpSpecial)
		{
		if (IsNaNVal(aSrc))
			return BinOpNaN(aReg, aSrc);
		if (aReg.iExp == KExpSpecial && IsNaNVal(aReg))
			return BinOpNaN(aReg, aSrc);
		return RealIndefinite(aReg);				// inf % anything
		}
	if (aReg.iExp == KExpSpecial)
		{
		if (IsNaNVal(aReg))
			return BinOpNaN(aReg, aSrc);
		aReg = aSrc;								// finite % inf: dividend unaltered
		return KErrNone;
		}
	if (aReg.iExp == 0)
		return RealIndefinite(aReg);				// x % 0, 0 % 0 included
	if (aSrc.iExp < aReg.iExp)
		{
		aReg = aSrc;								// dividend unaltered (covers dividend = 0)
		return KErrNone;
		}
	TInt count = TInt(aSrc.iExp - aReg.iExp);
	if (count >= 64)
		{
		aReg.iExp = 0;								// modfplp: divisor mantissa left in place
		return KErrTotalLossOfPrecision;
		}
	const TUint64 divisor = aReg.iMant;
	TUint128 acc = aSrc.iMant;						// 65-bit accumulator
	for (;;)
		{
		if (acc >= divisor)
			acc -= divisor;
		if (--count < 0)
			break;
		acc <<= 1;
		}
	TUint64 mant = TUint64(acc);
	if (mant == 0)
		{
		SetVal(aReg, 0, aReg.iSign, 0);
		return KErrNone;
		}
	TUint32 exp = aReg.iExp;						// remainder is scaled by the DIVISOR's exponent
	if ((mant>>32) == 0)
		{
		mant <<= 32;
		if (exp <= 32)
			{
			aReg.iMant = mant;						// modfpund: mantissa not cleared
			aReg.iExp = 0;
			return KErrUnderflow;
			}
		exp -= 32;
		}
	const TUint32 norm = TUint32(__builtin_clzll(mant));
	mant <<= norm;
	aReg.iMant = mant;
	if (exp <= norm)
		{
		aReg.iExp = 0;
		return KErrUnderflow;
		}
	aReg.iExp = exp - norm;
	return KErrNone;
	}

// TRealXCompare (uc_realx.cia:3250): infinities order normally, +0 == -0
static TRealX::TRealXOrder XCompare(const SRealX& aOp1, const SRealX& aOp2)
	{
	if (IsNaNVal(aOp1) || IsNaNVal(aOp2))
		return TRealX::EUnordered;
	if (aOp1.iExp == 0)
		{
		if (aOp2.iExp == 0)
			return TRealX::EEqual;
		return aOp2.iSign ? TRealX::EGreaterThan : TRealX::ELessThan;
		}
	TBool absGreater = ETrue;						// also the answer when op2=0 or the signs differ
	if (aOp2.iExp != 0 && aOp1.iSign == aOp2.iSign)
		{
		if (aOp1.iExp != aOp2.iExp)
			absGreater = (aOp1.iExp > aOp2.iExp);
		else if (aOp1.iMant != aOp2.iMant)
			absGreater = (aOp1.iMant > aOp2.iMant);
		else
			return TRealX::EEqual;
		}
	return (absGreater != TBool(aOp1.iSign)) ? TRealX::EGreaterThan : TRealX::ELessThan;
	}

/******************************************************************************
 * Integer -> TRealX (uc_realx.cia:895-1220). Always exact, always KErrNone.
 ******************************************************************************/
static void FromUint64(SRealX& aR, TUint64 aAbs, TUint32 aSign)
	{
	if (aAbs == 0)
		{
		SetVal(aR, 0, 0, 0);						// integer zero is +0
		return;
		}
	const TInt norm = __builtin_clzll(aAbs);
	SetVal(aR, TUint32(0x7FFF + 63 - norm), aSign, aAbs<<norm);
	}

static inline void FromInt(TRealX& aX, TInt aInt)
	{
	SRealX r;
	if (aInt < 0)
		FromUint64(r, TUint64(0u - TUint32(aInt)), 1);
	else
		FromUint64(r, TUint64(TUint32(aInt)), 0);
	Store(aX, r);
	}

static inline void FromUint(TRealX& aX, TUint aInt)
	{
	SRealX r;
	FromUint64(r, TUint64(TUint32(aInt)), 0);
	Store(aX, r);
	}

static inline void FromInt64(TRealX& aX, const TInt64& aInt)
	{
	SRealX r;
	if (aInt < 0)
		FromUint64(r, 0ULL - TUint64(aInt), 1);
	else
		FromUint64(r, TUint64(aInt), 0);
	Store(aX, r);
	}

/******************************************************************************
 * IEEE -> TRealX (ConvertTReal32ToTRealX :1229, ConvertTReal64ToTRealX :1271). Exact; denormals
 * are normalised; NaN payloads are kept left-justified under the integer bit.
 ******************************************************************************/
union UReal32 { TReal32 iReal; TUint32 iBits; };
union UReal64 { TReal64 iReal; TUint64 iBits; };

static TInt FromReal32Bits(TRealX& aX, TUint32 aBits)
	{
	SRealX r;
	const TUint32 sign = aBits>>31;
	const TUint32 exp = (aBits>>23) & 0xFF;
	TUint32 frac = aBits & 0x007FFFFF;
	TInt ret = KErrNone;
	if (exp == 0xFF)
		{
		SetVal(r, KExpSpecial, sign, TUint64((frac<<8) | 0x80000000u)<<32);
		ret = frac ? KErrArgument : KErrOverflow;
		}
	else if (exp)
		SetVal(r, exp + 0x7F80, sign, TUint64((frac<<8) | 0x80000000u)<<32);
	else if (frac)
		{
		frac <<= 9;									// denormal: bit 31 is worth 2^-127
		const TInt norm = __builtin_clz(frac);
		SetVal(r, TUint32(0x7F80 - norm), sign, TUint64(frac<<norm)<<32);
		}
	else
		SetVal(r, 0, sign, 0);
	Store(aX, r);
	return ret;
	}

static TInt FromReal64Bits(TRealX& aX, TUint64 aBits)
	{
	SRealX r;
	const TUint32 sign = TUint32(aBits>>63);
	const TUint32 exp = TUint32(aBits>>52) & 0x7FF;
	TUint64 frac = aBits & 0x000FFFFFFFFFFFFFULL;
	TInt ret = KErrNone;
	if (exp == 0x7FF)
		{
		SetVal(r, KExpSpecial, sign, (frac<<11) | KMantMsb);
		ret = frac ? KErrArgument : KErrOverflow;
		}
	else if (exp)
		SetVal(r, exp + 0x7C00, sign, (frac<<11) | KMantMsb);
	else if (frac)
		{
		frac <<= 12;								// denormal: bit 63 is worth 2^-1023
		const TInt norm = __builtin_clzll(frac);
		SetVal(r, TUint32(0x7C00 - norm), sign, frac<<norm);
		}
	else
		SetVal(r, 0, sign, 0);
	Store(aX, r);
	return ret;
	}

/******************************************************************************
 * TRealX -> IEEE (TRealXGetTReal32 :1714, TRealXGetTReal64 :1799). Round to nearest even, with the
 * TRealX's own rounding flags breaking an apparent tie; denormal results are generated and are
 * NOT an error; KErrUnderflow only when a nonzero value becomes zero.
 ******************************************************************************/
static TInt ToReal32Bits(const TRealX& aX, TUint32& aBits)
	{
	const TUint32 sign = (TUint32(aX.iSign) & 1)<<31;
	TUint32 hi = aX.iMantHi;
	TUint32 lo = aX.iMantLo;
	if (aX.iExp == KExpSpecial)
		{
		const TUint32 frac = (hi>>8) & 0x007FFFFF;	// a NaN whose payload is all below bit 8 of
		aBits = sign | 0x7F800000 | frac;			// iMantHi comes out as infinity, as in the asm
		return frac ? KErrArgument : KErrOverflow;
		}
	TInt exp = TInt(aX.iExp) - 0x7F80;
	if (exp >= 0xFF)
		{
		aBits = sign | 0x7F800000;
		return KErrOverflow;
		}
	TUint32 ext = 0;
	if (exp <= 0)
		{
		if (exp <= -24)
			{
			aBits = sign;
			return aX.iExp ? KErrUnderflow : KErrNone;
			}
		const TInt n = 1 - exp;						// 1..24 right shifts to form the denormal
		ext = lo<<(32-n);
		lo = (lo>>n) | (hi<<(32-n));
		hi = (hi>>n) | 0x80000000u;					// stand-in integer bit: a round-up carry out of
		exp = 0;									// the fraction then lands on exponent 1, mantissa 0
		}
	const TUint32 roundBits = hi & 0xFF;
	TBool up = EFalse;
	if (roundBits > 0x80)
		up = ETrue;
	else if (roundBits == 0x80)
		{
		if (lo || ext || (aX.iFlag & 1))
			up = ETrue;
		else if (aX.iFlag & 2)
			up = EFalse;
		else
			up = TBool(hi & 0x100);
		}
	if (up)
		{
		hi += 0x100;
		if (hi < 0x100)
			{
			hi = (hi>>1) | 0x80000000u;
			if (++exp == 0xFF)
				{
				aBits = sign | 0x7F800000;
				return KErrOverflow;
				}
			}
		}
	const TUint32 res = (TUint32(exp)<<23) | ((hi & 0x7FFFFF00)>>8);
	if (res == 0)
		{
		aBits = sign;
		return KErrUnderflow;
		}
	aBits = sign | res;
	return KErrNone;
	}

static TInt ToReal64Bits(const TRealX& aX, TUint64& aBits)
	{
	const TUint64 sign = TUint64(TUint32(aX.iSign) & 1)<<63;
	const TUint64 KInf = 0x7FF0000000000000ULL;
	TUint64 mant = (TUint64(aX.iMantHi)<<32) | aX.iMantLo;
	if (aX.iExp == KExpSpecial)
		{
		const TUint64 frac = (mant>>11) & 0x000FFFFFFFFFFFFFULL;
		aBits = sign | KInf | frac;
		return frac ? KErrArgument : KErrOverflow;
		}
	TInt exp = TInt(aX.iExp) - 0x7C00;
	if (exp >= 0x7FF)
		{
		aBits = sign | KInf;
		return KErrOverflow;
		}
	TUint64 ext = 0;
	if (exp <= 0)
		{
		if (exp <= -53)
			{
			aBits = sign;
			return aX.iExp ? KErrUnderflow : KErrNone;
			}
		const TInt n = 1 - exp;						// 1..53
		ext = mant<<(64-n);
		mant = (mant>>n) | KMantMsb;
		exp = 0;
		}
	const TUint32 roundBits = TUint32(mant) & 0x7FF;
	TBool up = EFalse;
	if (roundBits > 0x400)
		up = ETrue;
	else if (roundBits == 0x400)
		{
		if (ext || (aX.iFlag & 1))
			up = ETrue;
		else if (aX.iFlag & 2)
			up = EFalse;
		else
			up = TBool(TUint32(mant) & 0x800);
		}
	if (up)
		{
		mant += 0x800;
		if (mant < 0x800)
			{
			mant = (mant>>1) | KMantMsb;
			if (++exp == 0x7FF)
				{
				aBits = sign | KInf;
				return KErrOverflow;
				}
			}
		}
	const TUint64 res = (TUint64(exp)<<52) | ((mant & 0x7FFFFFFFFFFFF800ULL)>>11);
	if (res == 0)
		{
		aBits = sign;
		return KErrUnderflow;
		}
	aBits = sign | res;
	return KErrNone;
	}

/******************************************************************************
 * The exported class. Binary members: reg = aVal, src = *this (see the header comment).
 ******************************************************************************/
typedef TInt (*TRealXBinOp)(SRealX& aReg, const SRealX& aSrc);

static inline TInt BinOp(TRealXBinOp aOp, TRealX& aResult, const TRealX& aThis, const TRealX& aVal)
	{
	SRealX reg;
	SRealX src;
	Load(reg, aVal);
	Load(src, aThis);
	const TInt r = aOp(reg, src);
	Store(aResult, reg);
	return r;
	}

// The operator forms: the result is stored first, THEN any error panics MATHX <error>.
static inline void BinOpPanic(TRealXBinOp aOp, TRealX& aResult, const TRealX& aThis, const TRealX& aVal)
	{
	const TInt r = BinOp(aOp, aResult, aThis, aVal);
	if (r != KErrNone)
		TRealXPanic(r);
	}

static inline void AddUnit(TRealX& aThis, TUint32 aSign)
	{
	SRealX reg;
	SRealX src;
	SetVal(reg, 0x7FFF, aSign, KMantMsb);			// +/- 1.0
	Load(src, aThis);
	const TInt r = XAdd(reg, src);
	Store(aThis, reg);
	if (r != KErrNone)
		TRealXPanic(r);
	}




EXPORT_C TRealX::TRealX()
/**
Constructs a default extended precision object.

This sets the value to zero.
*/
	{
	iMantLo = 0;
	iMantHi = 0;
	iSign = 0;
	iFlag = 0;
	iExp = 0;
	}




EXPORT_C TRealX::TRealX(TUint aExp, TUint aMantHi, TUint aMantLo)
/**
Constructs an extended precision object from an explicit exponent and
a 64 bit mantissa.

@param aExp    The exponent
@param aMantHi The high order 32 bits of the 64 bit mantissa
@param aMantLo The low order 32 bits of the 64 bit mantissa
*/
	{
	// aExp is the raw third word of the object: exponent:flags:sign
	iMantLo = aMantLo;
	iMantHi = aMantHi;
	iSign = TInt8(aExp);
	iFlag = TUint8(aExp>>8);
	iExp = TUint16(aExp>>16);
	}




EXPORT_C TInt TRealX::Set(TInt aInt)
/**
Gives this extended precision object a new value taken
from a signed integer.

@param aInt The signed integer value.

@return KErrNone, always.
*/
	{
	FromInt(*this, aInt);
	return KErrNone;
	}




EXPORT_C TInt TRealX::Set(TUint aInt)
/**
Gives this extended precision object a new value taken from
an unsigned integer.

@param aInt The unsigned integer value.

@return KErrNone, always.
*/
	{
	FromUint(*this, aInt);
	return KErrNone;
	}




EXPORT_C TInt TRealX::Set(const TInt64& aInt)
/**
Gives this extended precision object a new value taken from
a 64 bit integer.

@param aInt The 64 bit integer value.

@return KErrNone, always.
*/
	{
	FromInt64(*this, aInt);
	return KErrNone;
	}




EXPORT_C TRealX::TRealX(TInt aInt)
/**
Constructs an extended precision object from a signed integer value.

@param aInt The signed integer value.
*/
	{
	FromInt(*this, aInt);
	}




EXPORT_C TRealX& TRealX::operator=(TInt aInt)
/**
Assigns the specified signed integer value to this extended precision object.

@param aInt The signed integer value.

@return A reference to this extended precision object.
*/
	{
	FromInt(*this, aInt);
	return *this;
	}




EXPORT_C TRealX::TRealX(TUint aInt)
/**
Constructs an extended precision object from an unsigned integer value.

@param aInt The unsigned integer value.
*/
	{
	FromUint(*this, aInt);
	}




EXPORT_C TRealX& TRealX::operator=(TUint aInt)
/**
Assigns the specified unsigned integer value to this extended precision object.

@param aInt The unsigned integer value.

@return A reference to this extended precision object.
*/
	{
	FromUint(*this, aInt);
	return *this;
	}




EXPORT_C TRealX::TRealX(const TInt64& aInt)
/**
Constructs an extended precision object from a 64 bit integer.

@param aInt A reference to a 64 bit integer.
*/
	{
	FromInt64(*this, aInt);
	}




EXPORT_C TRealX& TRealX::operator=(const TInt64& aInt)
/**
Assigns the specified 64 bit integer value to this extended precision object.

@param aInt A reference to a 64 bit integer.

@return A reference to this extended precision object.
*/
	{
	FromInt64(*this, aInt);
	return *this;
	}




EXPORT_C TInt TRealX::Set(TReal32 aReal)
/**
Gives this extended precision object a new value taken from
a single precision floating point number.

@param aReal The single precision floating point value.

@return KErrNone, if a valid number;
KErrOverflow, if the number is infinite;
KErrArgument, if not a number.
*/
	{
	UReal32 u;
	u.iReal = aReal;
	return FromReal32Bits(*this, u.iBits);
	}




EXPORT_C TInt TRealX::Set(TReal64 aReal)
/**
Gives this extended precision object a new value taken from
a double precision floating point number.

@param aReal The double precision floating point value.

@return KErrNone, if a valid number;
KErrOverflow, if the number is infinite;
KErrArgument, if not a number.
*/
	{
	UReal64 u;
	u.iReal = aReal;
	return FromReal64Bits(*this, u.iBits);
	}




EXPORT_C TRealX::TRealX(TReal32 aReal)
/**
Constructs an extended precision object from
a single precision floating point number.

@param aReal The single precision floating point value.
*/
	{
	UReal32 u;
	u.iReal = aReal;
	FromReal32Bits(*this, u.iBits);
	}




EXPORT_C TRealX& TRealX::operator=(TReal32 aReal)
/**
Assigns the specified single precision floating point number to
this extended precision object.

@param aReal The single precision floating point value.

@return A reference to this extended precision object.
*/
	{
	UReal32 u;
	u.iReal = aReal;
	FromReal32Bits(*this, u.iBits);
	return *this;
	}




EXPORT_C TRealX::TRealX(TReal64 aReal)
/**
Constructs an extended precision object from
a double precision floating point number.

@param aReal The double precision floating point value.
*/
	{
	UReal64 u;
	u.iReal = aReal;
	FromReal64Bits(*this, u.iBits);
	}




EXPORT_C TRealX& TRealX::operator=(TReal64 aReal)
/**
Assigns the specified double precision floating point number to
this extended precision object.

@param aReal The double precision floating point value.

@return A reference to this extended precision object.
*/
	{
	UReal64 u;
	u.iReal = aReal;
	FromReal64Bits(*this, u.iBits);
	return *this;
	}




EXPORT_C TRealX::operator TInt() const
/**
Gets the extended precision value as a signed integer value.

The operator returns:

1. zero , if the extended precision value is not a number

2. 0x7FFFFFFF, if the value is positive and too big to fit into a TInt.

3. 0x80000000, if the value is negative and too big to fit into a TInt.
*/
	{
	const TBool negative = TBool(iSign & 1);
	if (iExp == KExpSpecial)
		{
		if (iMantHi != 0x80000000u || iMantLo != 0)
			return 0;								// NaN
		}
	else
		{
		const TInt shift = 0x801E - TInt(iExp);		// truncation toward zero
		if (shift > 31)
			return 0;
		if (shift > 0)
			{
			const TUint32 abs = iMantHi>>shift;
			return TInt(negative ? 0u-abs : abs);
			}
		}
	return TInt(negative ? 0x80000000u : 0x7FFFFFFFu);
	}




EXPORT_C TRealX::operator TUint() const
/**
Returns the extended precision value as an unsigned signed integer value.

The operator returns:

1. zero, if the extended precision value is not a number

2. 0xFFFFFFFF, if the value is positive and too big to fit into a TUint.

3. zero, if the value is negative and too big to fit into a TUint.
*/
	{
	const TBool negative = TBool(iSign & 1);
	if (iExp == KExpSpecial)
		{
		if (iMantHi != 0x80000000u || iMantLo != 0)
			return 0;								// NaN
		}
	else
		{
		const TInt shift = 0x801E - TInt(iExp);
		if (shift > 31 || (shift >= 0 && negative))
			return 0;
		if (shift >= 0)
			return TUint(iMantHi>>shift);
		}
	return negative ? 0u : 0xFFFFFFFFu;
	}




EXPORT_C TRealX::operator TInt64() const
/**
Returns the extended precision value as a 64 bit integer value.

The operator returns:

1. zero, if the extended precision value is not a number

2. 0x7FFFFFFF FFFFFFFF, if the value is positive and too big to fit
into a TInt64

3. 0x80000000 00000000, if the value is negative and too big to fit
into a TInt.
*/
	{
	const TBool negative = TBool(iSign & 1);
	const TUint64 mant = (TUint64(iMantHi)<<32) | iMantLo;
	if (iExp == KExpSpecial)
		{
		if (mant != KMantMsb)
			return 0;								// NaN
		}
	else
		{
		const TInt shift = 0x803E - TInt(iExp);
		if (shift > 63)
			return 0;
		if (shift > 0)
			{
			const TUint64 abs = mant>>shift;
			return TInt64(negative ? 0ULL-abs : abs);
			}
		}
	return TInt64(negative ? KMantMsb : 0x7FFFFFFFFFFFFFFFULL);
	}




EXPORT_C TRealX::operator TReal32() const
/**
Returns the extended precision value as
a single precision floating point value.
*/
	{
	UReal32 u;
	ToReal32Bits(*this, u.iBits);
	return u.iReal;
	}




EXPORT_C TRealX::operator TReal64() const
/**
Returns the extended precision value as
a double precision floating point value.
*/
	{
	UReal64 u;
	ToReal64Bits(*this, u.iBits);
	return u.iReal;
	}




EXPORT_C TInt TRealX::GetTReal(TReal32& aVal) const
/**
Extracts the extended precision value as
a single precision floating point value.

@param aVal A reference to a single precision object which contains
the result of the operation.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	UReal32 u;
	const TInt r = ToReal32Bits(*this, u.iBits);
	aVal = u.iReal;
	return r;
	}




EXPORT_C TInt TRealX::GetTReal(TReal64& aVal) const
/**
Extracts the extended precision value as
a double precision floating point value.

@param aVal A reference to a double precision object which
contains the result of the operation.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	UReal64 u;
	const TInt r = ToReal64Bits(*this, u.iBits);
	aVal = u.iReal;
	return r;
	}




EXPORT_C void TRealX::SetZero(TBool aNegative)
/**
Sets the value of this extended precision object to zero.

@param aNegative ETrue, the value is a negative zero;
EFalse, the value is a positive zero, this is the default.
*/
	{
	iMantLo = 0;
	iMantHi = 0;
	iSign = TInt8(aNegative ? 1 : 0);
	iFlag = 0;
	iExp = 0;
	}




EXPORT_C void TRealX::SetNaN()
/**
Sets the value of this extended precision object to 'not a number'.
*/
	{
	iMantLo = 0;									// 'real indefinite'
	iMantHi = 0xC0000000u;
	iSign = 1;
	iFlag = 0;
	iExp = 0xFFFF;
	}




EXPORT_C void TRealX::SetInfinite(TBool aNegative)
/**
Sets the value of this extended precision object to infinity.

@param aNegative ETrue, the value is a negative zero;
EFalse, the value is a positive zero.
*/
	{
	iMantLo = 0;
	iMantHi = 0x80000000u;
	iSign = TInt8(aNegative ? 1 : 0);
	iFlag = 0;
	iExp = 0xFFFF;
	}




EXPORT_C TBool TRealX::IsZero() const
/**
Determines whether the extended precision value is zero.

@return True, if the extended precision value is zero, false, otherwise.
*/
	{
	return iExp == 0;
	}




EXPORT_C TBool TRealX::IsNaN() const
/**
Determines whether the extended precision value is 'not a number'.

@return True, if the extended precision value is 'not a number',
false, otherwise.
*/
	{
	return iExp == 0xFFFF && (iMantHi != 0x80000000u || iMantLo != 0);
	}




EXPORT_C TBool TRealX::IsInfinite() const
/**
Determines whether the extended precision value has a finite value.

@return True, if the extended precision value is finite,
false, if the value is 'not a number' or is infinite,
*/
	{
	return iExp == 0xFFFF && iMantHi == 0x80000000u && iMantLo == 0;
	}




EXPORT_C TBool TRealX::IsFinite() const
/**
Determines whether the extended precision value has a finite value.

@return True, if the extended precision value is finite,
false, if the value is 'not a number' or is infinite,
*/
	{
	return iExp != 0xFFFF;
	}




EXPORT_C const TRealX& TRealX::operator+=(const TRealX& aVal)
/**
Adds an extended precision value to this extended precision number.

@param aVal The extended precision value to be added.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	BinOpPanic(&XAdd, *this, *this, aVal);
	return *this;
	}




EXPORT_C const TRealX& TRealX::operator-=(const TRealX& aVal)
/**
Subtracts an extended precision value from this extended precision number.

@param aVal The extended precision value to be subtracted.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	BinOpPanic(&XSub, *this, *this, aVal);
	return *this;
	}




EXPORT_C const TRealX& TRealX::operator*=(const TRealX& aVal)
/**
Multiplies this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the multiplier.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	BinOpPanic(&XMul, *this, *this, aVal);
	return *this;
	}




EXPORT_C const TRealX& TRealX::operator/=(const TRealX& aVal)
/**
Divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
@panic MATHX KErrDivideByZero if the divisor is zero.
*/
	{
	BinOpPanic(&XDiv, *this, *this, aVal);
	return *this;
	}




EXPORT_C const TRealX& TRealX::operator%=(const TRealX& aVal)
/**
Modulo-divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return A reference to this object.

@panic MATHX KErrTotalLossOfPrecision panic if precision is lost.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	BinOpPanic(&XMod, *this, *this, aVal);
	return *this;
	}




EXPORT_C TInt TRealX::AddEq(const TRealX& aVal)
/**
Adds an extended precision value to this extended precision number.

@param aVal The extended precision value to be added.

@return KErrNone, if the operation is successful;
KErrOverflow,if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XAdd, *this, *this, aVal);
	}




EXPORT_C TInt TRealX::SubEq(const TRealX& aVal)
/**
Subtracts an extended precision value from this extended precision number.

@param aVal The extended precision value to be subtracted.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XSub, *this, *this, aVal);
	}




EXPORT_C TInt TRealX::MultEq(const TRealX& aVal)
/**
Multiplies this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the multiplier.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow
*/
	{
	return BinOp(&XMul, *this, *this, aVal);
	}




EXPORT_C TInt TRealX::DivEq(const TRealX& aVal)
/**
Divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow;
KErrDivideByZero, if the divisor is zero.
*/
	{
	return BinOp(&XDiv, *this, *this, aVal);
	}




EXPORT_C TInt TRealX::ModEq(const TRealX& aVal)
/**
Modulo-divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return KErrNone, if the operation is successful;
KErrTotalLossOfPrecision, if precision is lost;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XMod, *this, *this, aVal);
	}




EXPORT_C TRealX TRealX::operator+() const
/**
Returns this extended precision number unchanged.

Note that this may also be referred to as a unary plus operator.

@return The extended precision number.
*/
	{
	return *this;
	}




EXPORT_C TRealX TRealX::operator-() const
/**
Negates this extended precision number.

This may also be referred to as a unary minus operator.

@return The negative of the extended precision number.
*/
	{
	TRealX r(*this);
	r.iSign = TInt8(r.iSign ^ 1);					// NaNs and zeros included
	return r;
	}




EXPORT_C TRealX& TRealX::operator++()
/**
Increments this extended precision number by one,
and then returns a reference to it.

This is also referred to as a prefix operator.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	AddUnit(*this, 0);
	return *this;
	}




EXPORT_C TRealX TRealX::operator++(TInt)
/**
Returns this extended precision number before incrementing it by one.

This is also referred to as a postfix operator.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r(*this);
	AddUnit(*this, 0);
	return r;
	}




EXPORT_C TRealX& TRealX::operator--()
/**
Decrements this extended precision number by one,
and then returns a reference to it.

This is also referred to as a prefix operator.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	AddUnit(*this, 1);
	return *this;
	}




EXPORT_C TRealX TRealX::operator--(TInt)
/**
Returns this extended precision number before decrementing it by one.

This is also referred to as a postfix operator.

@return A reference to this object.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r(*this);
	AddUnit(*this, 1);
	return r;
	}




EXPORT_C TRealX TRealX::operator+(const TRealX& aVal) const
/**
Adds an extended precision value to this extended precision number.

@param aVal The extended precision value to be added.

@return An extended precision object containing the result.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r;
	BinOpPanic(&XAdd, r, *this, aVal);
	return r;
	}




EXPORT_C TRealX TRealX::operator-(const TRealX& aVal) const
/**
Subtracts an extended precision value from this extended precision number.

@param aVal The extended precision value to be subtracted.

@return An extended precision object containing the result.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r;
	BinOpPanic(&XSub, r, *this, aVal);
	return r;
	}




EXPORT_C TRealX TRealX::operator*(const TRealX& aVal) const
/**
Multiplies this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the multiplier.

@return An extended precision object containing the result.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r;
	BinOpPanic(&XMul, r, *this, aVal);
	return r;
	}




EXPORT_C TRealX TRealX::operator/(const TRealX& aVal) const
/**
Divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return An extended precision object containing the result.

@panic MATHX KErrOverflow if the operation results in overflow.
@panic MATHX KErrUnderflow if  the operation results in underflow.
@panic MATHX KErrDivideByZero if the divisor is zero.
*/
	{
	TRealX r;
	BinOpPanic(&XDiv, r, *this, aVal);
	return r;
	}




EXPORT_C TRealX TRealX::operator%(const TRealX& aVal) const
/**
Modulo-divides this extended precision number by an extended precision value.

@param aVal The extended precision value to be used as the divisor.

@return An extended precision object containing the result.

@panic MATHX KErrTotalLossOfPrecision if precision is lost.
@panic MATHX KErrUnderflow if  the operation results in underflow.
*/
	{
	TRealX r;
	BinOpPanic(&XMod, r, *this, aVal);
	return r;
	}




EXPORT_C TInt TRealX::Add(TRealX& aResult, const TRealX& aVal) const
/**
Adds an extended precision value to this extended precision number.

@param aResult On return, a reference to an extended precision object
containing the result of the operation.
@param aVal    The extended precision value to be added.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XAdd, aResult, *this, aVal);
	}




EXPORT_C TInt TRealX::Sub(TRealX& aResult, const TRealX& aVal) const
/**
Subtracts an extended precision value from this extended precision number.

@param aResult On return, a reference to an extended precision object
containing the result of the operation.
@param aVal    The extended precision value to be subtracted.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XSub, aResult, *this, aVal);
	}




EXPORT_C TInt TRealX::Mult(TRealX& aResult, const TRealX& aVal) const
/**
Multiplies this extended precision number by an extended precision value.

@param aResult On return, a reference to an extended precision object
containing the result of the operation.
@param aVal    The extended precision value to be used as the multiplier.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XMul, aResult, *this, aVal);
	}




EXPORT_C TInt TRealX::Div(TRealX& aResult, const TRealX& aVal) const
/**
Divides this extended precision number by an extended precision value.

@param aResult On return, a reference to an extended precision object
containing the result of the operation.
@param aVal    The extended precision value to be used as the divisor.

@return KErrNone, if the operation is successful;
KErrOverflow, if the operation results in overflow;
KErrUnderflow, if the operation results in underflow;
KErrDivideByZero, if the divisor is zero.
*/
	{
	return BinOp(&XDiv, aResult, *this, aVal);
	}




EXPORT_C TInt TRealX::Mod(TRealX& aResult, const TRealX& aVal) const
/**
Modulo-divides this extended precision number by an extended precision value.

@param aResult On return, a reference to an extended precision object
containing the result of the operation.
@param aVal    The extended precision value to be used as the divisor.

@return KErrNone, if the operation is successful;
KErrTotalLossOfPrecision, if precision is lost;
KErrUnderflow, if the operation results in underflow.
*/
	{
	return BinOp(&XMod, aResult, *this, aVal);
	}




EXPORT_C TRealX::TRealXOrder TRealX::Compare(const TRealX& aVal) const
/**
Compares this extended precision number with the specified
extended precision value.

@param aVal A reference to the extended precision value to be compared.

@return The result of the comparison: ELessThan, EEqual, EGreaterThan
or EUnordered (if either operand is a NaN).
*/
	{
	SRealX op1;
	SRealX op2;
	Load(op1, *this);
	Load(op2, aVal);
	return XCompare(op1, op2);
	}
