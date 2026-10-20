// xu_leavetrace.cpp -- opt-in bring-up diagnostic: log every User::Leave that crosses a module
// boundary (the x32 sibling of toolchain/euser_dll/leave_trace_dev.cpp). X4.19.
//
// `LEAVE_TRACE=1 py emitter/build_euser_x32.py` compiles this into euser and points the EXPORTED
// ordinals of User::Leave and User::LeaveIfError at these two functions. Every other image reaches
// them through an import veneer, which is a `jmp` (no return address pushed), so
// __builtin_return_address(0) is the call site in the CALLER -- feed it to
// tools/phorepoc-x86/romsym_x32.py. Servers TRAP their failures silently; this names them.
// Leaves raised inside euser itself call the real functions directly and are not logged.
// Off by default: a normal build does not contain this file.
#include <e32std.h>
#include <e32svr.h>

// which thread left (X4.51: a system-wide trace is mostly routine KErrNotFound probes in servers;
// the thread name picks out the application's own)
static void ThreadName8(TDes8& aOut)
	{
	TName n = RThread().Name();
	aOut.Copy(n.Right(Min(n.Length(), aOut.MaxLength())));
	}

static void StackWords(TInt aReason, const TDesC8& aThread, const char* aTag);

extern "C" void X32LeaveTrace_Leave(TInt aReason)
	{
	TBuf8<40> t;
	ThreadName8(t);
	RDebug::Printf("[LEAVE] %d lr=%08x %S", aReason, (TUint)(TUintPtr)__builtin_return_address(0), &t);
	// X4.86: a plain leave with KErrEof / KErrCorrupt / a component-specific code (<= -10000: SWI's SIS parser,
	// SWInst...) carries its callers too -- the "file is corrupted" hunt; the routine codes stay one line
	if (aReason == KErrEof || aReason == KErrCorrupt || aReason <= -10000)
		StackWords(KErrNotFound, t, "[LEAVE]");
	User::Leave(aReason);
	}

// a caller failing to find something (a repository, a file, a plug-in) usually holds the id it looked
// for in a local or a spilled argument: dump the words above a LeaveIfError(KErrNotFound) -- X4.51 (server
// threads "!name": which repository File Manager asks for); X4.53 any thread (a client's
// CRepository::NewL fails in CClientRepository::ConstructL, whose TUid sits in the caller's frame).
// Plain User::Leave(KErrNotFound) -- the file server's routine probes -- is never dumped.
// ROM code addresses: the XIP ROM starts at 0x80000000 and passed 32 MB at X4.63 -- the old
// 0x82000000 bound silently dropped every image linked past it from these backtraces
static const TUint32 KCodeLo = 0x80000000u;
static const TUint32 KCodeHi = 0x88000000u;

static void StackWords(TInt aReason, const TDesC8& /*aThread*/, const char* aTag = "[LEAVE]")
	{
	if (aReason != KErrNotFound && aTag[1] == 'L')
		return;
	const TUint32* sp = (const TUint32*)__builtin_frame_address(0);
	RDebug::Printf("%s   stack %08x %08x %08x %08x %08x %08x %08x %08x", aTag, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
	RDebug::Printf("%s   stack %08x %08x %08x %08x %08x %08x %08x %08x", aTag, sp[8], sp[9], sp[10], sp[11], sp[12], sp[13], sp[14], sp[15]);
	RDebug::Printf("%s   stack %08x %08x %08x %08x %08x %08x %08x %08x", aTag, sp[16], sp[17], sp[18], sp[19], sp[20], sp[21], sp[22], sp[23]);
	// + a poor man's backtrace: the words in the next 128 that point into the XIP ROM (code) -- the
	// callers above the leave (romsym_x32.py names them), as the panic handler's "user stack -> code:"
	// X4.68: up to 4 lines of 13 -- a CommsDat leave is 13 words deep before it leaves commsdat/euser
	TBuf8<120> code;
	TThreadStackInfo si;
	TInt top = 512;                     // words; never past the stack's base (its highest address)
	if (RThread().StackInfo(si) == KErrNone)
		top = Min(top, (TInt)((si.iBase - (TLinAddr)sp) / 4));
	else
		top = 24;
	TInt lines = 0;
	for (TInt i = 24; i < top && lines < 4; ++i)
		{
		if (sp[i] < KCodeLo || sp[i] >= KCodeHi)
			continue;
		code.AppendFormat(_L8(" %08x"), sp[i]);
		if (code.Length() + 9 > code.MaxLength())
			{
			RDebug::Printf("%s   code:%S", aTag, &code);
			code.Zero();
			++lines;
			}
		}
	if (code.Length() && lines < 4)
		RDebug::Printf("%s   code:%S", aTag, &code);
	}

// X4.64: panics too. `ld --wrap` sends every call of User::Panic that crosses an object boundary --
// inside euser too (an RArray bounds check panics USER 130 from euser's own code) -- and, through
// the export table, every other image's, here: the category, reason, the caller and its callers.
extern "C" void __real__ZN4User5PanicERK7TDesC16i(const TDesC& aCategory, TInt aReason);
extern "C" void __wrap__ZN4User5PanicERK7TDesC16i(const TDesC& aCategory, TInt aReason)
	{
	TBuf8<40> t;
	ThreadName8(t);
	TBuf8<KMaxExitCategoryName> c;
	c.Copy(aCategory.Left(KMaxExitCategoryName));
	RDebug::Printf("[PANIC] %S %d lr=%08x %S", &c, aReason, (TUint)(TUintPtr)__builtin_return_address(0), &t);
	StackWords(aReason, t, "[PANIC]");
	__real__ZN4User5PanicERK7TDesC16i(aCategory, aReason);
	}

extern "C" TInt X32LeaveTrace_LeaveIfError(TInt aReason)
	{
	if (aReason < 0)
		{
		TBuf8<40> t;
		ThreadName8(t);
		RDebug::Printf("[LEAVE] %d lr=%08x (LeaveIfError) %S", aReason,
					   (TUint)(TUintPtr)__builtin_return_address(0), &t);
		StackWords(aReason, t);
		User::Leave(aReason);
		}
	return aReason;
	}
