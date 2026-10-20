// xu_exec.cpp -- Phorepoc amd64/x32 euser: sibling of euser/epoc/x86/uc_exec.cia.
// Emits the whole generated user-side executive layer (exec_user.h: Exec::, E32Loader::, UserSvr::
// stubs) through the SysV register-shuffle macros of our shadowed u32exec.h, plus the handful of
// hand-written stubs the .cia carries.

#define __GEN_USER_EXEC_CODE__
#include <e32svr.h>
#include <e32atomics.h>
#include <u32exec.h>

#define NAKED	__attribute__((naked))

/******************************************************************************
 * Slow executive calls with preprocessing or extra arguments
 ******************************************************************************/
NAKED TInt Exec::SessionSend(TInt /*aHandle*/, TInt /*aFunction*/, TAny* /*aPtr*/, TRequestStatus* /*aStatus*/)
	{
	SLOW_EXEC4(EExecSessionSend);
	}
NAKED TInt Exec::SessionSendSync(TInt /*aHandle*/, TInt /*aFunction*/, TAny* /*aPtr*/, TRequestStatus* /*aStatus*/)
	{
	SLOW_EXEC4(EExecSessionSendSync);
	}
NAKED TInt Exec::MessageIpcCopy(TInt /*aHandle*/, TInt /*aParam*/, SIpcCopyInfo& /*aInfo*/, TInt /*anOffset*/)
	{
	SLOW_EXEC4(EExecMessageIpcCopy);
	}
NAKED TInt Exec::BTraceOut(TUint32 /*a0*/, TUint32 /*a1*/, const BTrace::SExecExtension& /*aExtension*/, TInt /*aDataSize*/)
	{
	SLOW_EXEC4(EExecBTraceOut);
	}
NAKED TInt Exec::BTraceOutBig(TUint32 /*a0*/, TUint32 /*a1*/, const BTrace::SExecExtension& /*aExtension*/, TInt /*aDataSize*/)
	{
	SLOW_EXEC4(EExecBTraceOutBig);
	}
NAKED TInt Exec::UTraceOut(TUint32 /*a0*/, TUint32 /*a1*/, const BTrace::SExecExtension& /*aExtension*/, TInt /*aDataSize*/)
	{
	SLOW_EXEC4(EExecUTraceOut);
	}

// Exec::ThreadKill(KCurrentThreadHandle, aType, aReason, aCategory); never returns.
// In: edi = aType, esi = aReason, edx = aCategory.  Out: ecx, edx, ebx, esi.
NAKED void ExitCurrentThread(TExitType /*aType*/, TInt /*aReason*/, const TDesC8* /*aCategory*/)
	{
	X64_EXEC_NUM(EExecThreadKill);
	__asm__("mov ebx, esi\n"
			"mov esi, edx\n"
			"mov edx, edi\n"
			"mov ecx, 0xffff8001\n"			// KCurrentThreadHandle
			"int 0x21\n"
			"ud2\n");
	}

// Atomically take the request-status pointer (so a request completes once), store the completion
// code in the first word of the TRequestStatus (iStatus), then signal the thread.
EXPORT_C void RThread::RequestComplete(TRequestStatus*& aStatus, TInt aReason) const
	{
	TRequestStatus* s = (TRequestStatus*)__e32_atomic_swp_ord_ptr(&aStatus, 0);
	if (s)
		{
		*(TInt*)s = aReason;
		Exec::ThreadRequestSignal(iHandle);
		}
	}
