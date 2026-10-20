// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// xu_utl.cpp -- Phorepoc amd64/x32 euser: siblings of euser/epoc/x86/{uc_trp,uc_utl,uc_i64}.cia.
// C++ wherever the IA-32 original was asm only for speed; naked asm only where register state IS
// the semantics (TTrap = setjmp/longjmp for the cleanup stack).

#include <e32std.h>
#include <e32std_private.h>
#include <e32math.h>
#include <e32hashtab.h>
#include <e32atomics.h>
#include <u32exec.h>
#include "../x64ekern/xe_exctrap_asm.h"		// the save/resume sequences, run-tested hosted (exctrap_test.c)

/******************************************************************************
 * TTrap (uc_trp.cia). iState[16 words] = { rip, rsp, rbx, rbp, r12..r15 } as 8 qwords, at
 * offset 0 of the TTrap. Trap() saves the CALLER's non-volatile context and tail-jumps to a C++
 * helper, whose plain return gives Trap()'s caller its 0; DoLeave() resumes there with 1.
 ******************************************************************************/
#ifndef __LEAVE_EQUALS_THROW__
void DoTrap(TTrap* aFrame);						// uc_trp.cpp

extern "C" TInt X64U_TrapEnter(TTrap* aFrame, TInt* aResult)
	{
	*aResult = 0;
	aFrame->iResult = aResult;
	DoTrap(aFrame);
	return 0;
	}

EXPORT_C __attribute__((naked)) TInt TTrap::Trap(TInt& /*aResult*/)
	{
	__asm__(X64_SAVE_NONVOLATILE "jmp X64U_TrapEnter\n");		// rdi = this, rsi = &aResult already
	}

__attribute__((naked)) void DoLeave(TTrap* /*aFrame*/)
	{
	__asm__("mov esi, 1\n" X64_RESUME_NONVOLATILE);
	}
#endif

/******************************************************************************
 * RFastLock (uc_utl.cia): iCount 0 = free, -1 = held, < -1 = held with waiters.
 ******************************************************************************/
EXPORT_C void BootEpoc(TBool)
	{
	}

EXPORT_C void RFastLock::Wait()
	{
	if ((TInt)__e32_atomic_add_ord32(&iCount, TUint32(-1)) < 0)		// returns the OLD value
		Exec::SemaphoreWait(iHandle, 0);
	}

EXPORT_C TInt RFastLock::Poll()
	{
	TUint32 expect = 0;
	return __e32_atomic_cas_ord32(&iCount, &expect, TUint32(-1)) ? KErrNone : KErrTimedOut;
	}

EXPORT_C void RFastLock::Signal()
	{
	if ((TInt)__e32_atomic_add_ord32(&iCount, 1) < -1)				// OLD < -1: somebody is waiting
		Exec::SemaphoreSignal1(iHandle);
	}

/******************************************************************************
 * Default hashes (uc_utl.cia), transcribed: golden-ratio multiply over 32-bit chunks.
 ******************************************************************************/
const TUint32 KGolden = 0x9E3779B9u;

TUint32 DefaultStringHash(const TUint8* aPtr, TInt aLen)
	{
	TUint32 h = 0;
	for (; aLen >= 4; aLen -= 4, aPtr += 4)
		h = (h ^ (aPtr[0] | (aPtr[1]<<8) | (aPtr[2]<<16) | ((TUint32)aPtr[3]<<24))) * KGolden;
	if (aLen)
		{
		TUint32 x = aPtr[0];
		if (aLen >= 2) x |= aPtr[1]<<8;
		if (aLen > 2) x |= aPtr[2]<<16;
		h = (h ^ x) * KGolden;
		}
	return h;
	}

static inline TUint32 Rol8(TUint32 a)	{ return (a<<8) | (a>>24); }

TUint32 DefaultWStringHash(const TUint16* aPtr, TInt aLen)		// aLen in BYTES
	{
	TUint32 h = 0;
	for (; aLen >= 8; aLen -= 8, aPtr += 4)
		{
		TUint32 lo = aPtr[0] | ((TUint32)aPtr[1]<<16), hi = aPtr[2] | ((TUint32)aPtr[3]<<16);
		h = (h ^ lo ^ Rol8(hi)) * KGolden;
		}
	if (aLen)
		{
		if (aLen > 4)
			h ^= Rol8(aPtr[2]);
		TUint32 x = aPtr[0];
		if (aLen >= 4) x |= (TUint32)aPtr[1]<<16;
		h = (h ^ x) * KGolden;
		}
	return h;
	}

EXPORT_C TUint32 DefaultHash::Integer(const TInt& aInt)
	{
	return (TUint32)aInt * KGolden;
	}

/******************************************************************************
 * 64-bit helpers (uc_i64.cia): amd64 has the instruction.
 ******************************************************************************/
EXPORT_C void Math::UMul64(Uint64 aX, Uint64 aY, Uint64& aOutH, Uint64& aOutL)
	{
	unsigned __int128 p = (unsigned __int128)aX * aY;
	aOutH = (Uint64)(p >> 64);
	aOutL = (Uint64)p;
	}

// __e32_atomic_tas_ord32 used to live here: the shared x64s_atomics.cpp now carries the whole
// tas/xor family (X4.9b -- euseru.def exports all 32 of them), so this private copy is gone.
