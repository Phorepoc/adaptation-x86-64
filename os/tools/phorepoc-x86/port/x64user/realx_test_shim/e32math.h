/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// e32math.h -- HOSTED TEST SHIM (realx_test.cpp only; never on the euser include path).
// Reproduces exactly the TRealX class of baseline .../eka/include/e32math.h (member order, types,
// signatures) plus the handful of e32def.h/e32err.h names xu_realx.cpp uses, so the real
// xu_realx.cpp compiles unmodified next to libc/libm. The target build uses the real header; the
// sizeof==12 assert in xu_realx.cpp guards the layout on both sides.
#ifndef __REALX_TEST_SHIM_E32MATH_H__
#define __REALX_TEST_SHIM_E32MATH_H__

typedef signed int TInt;
typedef unsigned int TUint;
typedef int TBool;
typedef signed char TInt8;
typedef unsigned char TUint8;
typedef unsigned short TUint16;
typedef unsigned int TUint32;
typedef long long TInt64;
typedef unsigned long long TUint64;
typedef float TReal32;
typedef double TReal64;
enum { EFalse = 0, ETrue = 1 };

#define EXPORT_C
#define IMPORT_C
#define __SOFTFP

const TInt KErrNone = 0;
const TInt KErrArgument = -6;
const TInt KErrTotalLossOfPrecision = -7;
const TInt KErrOverflow = -9;
const TInt KErrUnderflow = -10;
const TInt KErrDivideByZero = -41;

class TRealX
	{
public:
	enum TRealXOrder {ELessThan=1,EEqual=2,EGreaterThan=4,EUnordered=8};
public:
	IMPORT_C TRealX();
	IMPORT_C TRealX(TInt aInt);
	IMPORT_C TRealX(TUint aInt);
	IMPORT_C TRealX(TUint aExp, TUint aMantHi, TUint aMantLo);
	IMPORT_C TRealX(const TInt64 &aInt);
	IMPORT_C TRealX(TReal32 aReal) __SOFTFP;
	IMPORT_C TRealX(TReal64 aReal) __SOFTFP;
	IMPORT_C TRealX &operator=(TInt aInt);
	IMPORT_C TRealX &operator=(TUint aInt);
	IMPORT_C TRealX &operator=(const TInt64& aInt);
	IMPORT_C TRealX &operator=(TReal32 aReal) __SOFTFP;
	IMPORT_C TRealX &operator=(TReal64 aReal) __SOFTFP;
	IMPORT_C TInt Set(TInt aInt);
	IMPORT_C TInt Set(TUint aInt);
	IMPORT_C TInt Set(const TInt64& aInt);
	IMPORT_C TInt Set(TReal32 aReal) __SOFTFP;
	IMPORT_C TInt Set(TReal64 aReal) __SOFTFP;
	IMPORT_C operator TInt() const;
	IMPORT_C operator TUint() const;
	IMPORT_C operator TInt64() const;
	IMPORT_C operator TReal32() const __SOFTFP;
	IMPORT_C operator TReal64() const __SOFTFP;
	IMPORT_C TInt GetTReal(TReal32 &aVal) const;
	IMPORT_C TInt GetTReal(TReal64 &aVal) const;
	IMPORT_C void SetZero(TBool aNegative=EFalse);
	IMPORT_C void SetNaN();
	IMPORT_C void SetInfinite(TBool aNegative);
	IMPORT_C TBool IsZero() const;
	IMPORT_C TBool IsNaN() const;
	IMPORT_C TBool IsInfinite() const;
	IMPORT_C TBool IsFinite() const;
	IMPORT_C const TRealX &operator+=(const TRealX &aVal);
	IMPORT_C const TRealX &operator-=(const TRealX &aVal);
	IMPORT_C const TRealX &operator*=(const TRealX &aVal);
	IMPORT_C const TRealX &operator/=(const TRealX &aVal);
	IMPORT_C const TRealX &operator%=(const TRealX &aVal);
	IMPORT_C TInt AddEq(const TRealX &aVal);
	IMPORT_C TInt SubEq(const TRealX &aVal);
	IMPORT_C TInt MultEq(const TRealX &aVal);
	IMPORT_C TInt DivEq(const TRealX &aVal);
	IMPORT_C TInt ModEq(const TRealX &aVal);
	IMPORT_C TRealX operator+() const;
	IMPORT_C TRealX operator-() const;
	IMPORT_C TRealX &operator++();
	IMPORT_C TRealX operator++(TInt);
	IMPORT_C TRealX &operator--();
	IMPORT_C TRealX operator--(TInt);
	IMPORT_C TRealX operator+(const TRealX &aVal) const;
	IMPORT_C TRealX operator-(const TRealX &aVal) const;
	IMPORT_C TRealX operator*(const TRealX &aVal) const;
	IMPORT_C TRealX operator/(const TRealX &aVal) const;
	IMPORT_C TRealX operator%(const TRealX &aVal) const;
	IMPORT_C TInt Add(TRealX& aResult,const TRealX &aVal) const;
	IMPORT_C TInt Sub(TRealX& aResult,const TRealX &aVal) const;
	IMPORT_C TInt Mult(TRealX& aResult,const TRealX &aVal) const;
	IMPORT_C TInt Div(TRealX& aResult,const TRealX &aVal) const;
	IMPORT_C TInt Mod(TRealX& aResult,const TRealX &aVal) const;
	IMPORT_C TRealXOrder Compare(const TRealX& aVal) const;
	inline TBool operator==(const TRealX &aVal) const;
	inline TBool operator!=(const TRealX &aVal) const;
	inline TBool operator>=(const TRealX &aVal) const;
	inline TBool operator<=(const TRealX &aVal) const;
	inline TBool operator>(const TRealX &aVal) const;
	inline TBool operator<(const TRealX &aVal) const;
public:
	TUint32 iMantLo;
	TUint32 iMantHi;
	TInt8 iSign;
	TUint8 iFlag;
	TUint16 iExp;
	};

// e32math.inl
inline TBool TRealX::operator==(const TRealX &aVal) const
	{return(Compare(aVal)&EEqual);}
inline TBool TRealX::operator!=(const TRealX &aVal) const
	{return!(Compare(aVal)&EEqual);}
inline TBool TRealX::operator>=(const TRealX &aVal) const
	{return(Compare(aVal)&(EEqual|EGreaterThan));}
inline TBool TRealX::operator<=(const TRealX &aVal) const
	{return(Compare(aVal)&(EEqual|ELessThan));}
inline TBool TRealX::operator>(const TRealX &aVal) const
	{return(Compare(aVal)&EGreaterThan);}
inline TBool TRealX::operator<(const TRealX &aVal) const
	{return(Compare(aVal)&ELessThan);}

#endif
