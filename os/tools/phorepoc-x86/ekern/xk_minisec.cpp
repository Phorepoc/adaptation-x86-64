// xk_minisec.cpp -- Phorepoc amd64/x32: the first USER-MODE program. amd64 sibling of Track B's
// tools/kernel/minisec.s: a stand-in for the ROM "secondary" (efile.exe's slot) that exstart.cpp
// launches as a real DProcess. It runs in ring 3, XIP from the ROM (whole ROM = user-readable),
// on the user stack the kernel made for it, and talks to the kernel ONLY through the executive
// gate (int 0x20 fast / int 0x21 slow; EAX = call, ECX EDX EBX ESI = arguments -- u32exec.h).
//
// No euser yet, so the three exec stubs below are hand-rolled. Everything lives in .utext /
// .rodata; nothing here may touch a kernel symbol. It must never exit (the secondary is
// system-critical): it ends in Exec::WaitForAnyRequest for ever.
// docs/phorepoc/x86-64-port.md X4.7.

typedef unsigned int TUint32;
typedef unsigned char TUint8;

#define USER	__attribute__((section(".utext")))
#define INL		static inline __attribute__((always_inline))

enum { EFastExecWaitForAnyRequest = 0, EFastExecNTickCount = 14, EExecThreadId = 38, EExecDebugPrint = 86 };
const TUint32 KCurrentThreadHandle = 0xFFFF8001u;

INL TUint32 FastExec(TUint32 aNum, TUint32 a0)
	{
	TUint32 r;
	__asm__ __volatile__("int 0x20" : "=a"(r) : "a"(aNum), "c"(a0));
	return r;
	}
INL TUint32 SlowExec(TUint32 aNum, TUint32 a0, TUint32 a1)
	{
	TUint32 r;
	__asm__ __volatile__("int 0x21" : "=a"(r) : "a"(aNum), "c"(a0), "d"(a1) : "memory");
	return r;
	}

// A TPtrC8 by hand: word 0 = length | (EPtrC=1)<<28, word 1 = pointer.
struct SPtrC8 { TUint32 iLengthAndType; const TUint8* iPtr; };

INL void Print(const TUint8* aText, TUint32 aLen)
	{
	volatile SPtrC8 d;
	d.iLengthAndType = aLen | (1u<<28);
	d.iPtr = aText;
	SlowExec(EExecDebugPrint, (TUint32)&d, 0);
	}

INL TUint32 PutStr(TUint8* aBuf, TUint32 aPos, const char* aStr)
	{
	while (*aStr)
		aBuf[aPos++] = (TUint8)*aStr++;
	return aPos;
	}
INL TUint32 PutHex(TUint8* aBuf, TUint32 aPos, TUint32 aVal)
	{
	for (int i=28; i>=0; i-=4)
		{
		TUint32 n = (aVal>>i)&15;
		aBuf[aPos++] = (TUint8)(n<10 ? '0'+n : 'a'+n-10);
		}
	return aPos;
	}

extern "C" USER void X64MinisecMain(TUint32 aInitialSp)
	{
	volatile TUint8 vbuf[128];
	TUint8* buf = (TUint8*)vbuf;
	TUint32 n;

	TUint32 cs = 0;
	__asm__ __volatile__("mov %0, cs" : "=r"(cs));
	n = PutStr(buf, 0, "[USERLAND] hello from ring 3: cs=");
	n = PutHex(buf, n, cs);
	n = PutStr(buf, n, " sp=");
	n = PutHex(buf, n, aInitialSp);
	Print(buf, n);

	TUint32 t0 = FastExec(EFastExecNTickCount, 0);
	TUint32 id = SlowExec(EExecThreadId, KCurrentThreadHandle, 0);		// EF_P: handle -> DThread*
	n = PutStr(buf, 0, "[USERLAND] fast exec NTickCount=");
	n = PutHex(buf, n, t0);
	n = PutStr(buf, n, "  slow exec ThreadId(current)=");
	n = PutHex(buf, n, id);
	Print(buf, n);

	// Burn a second of pure ring-3 CPU: only the tick interrupt (taken FROM user mode) can move the
	// clock, and timeslicing may preempt us on the way.
	volatile TUint32 spins = 0;
	TUint32 t1;
	do	{
		for (TUint32 i=0; i<100000; ++i)
			++spins;
		t1 = FastExec(EFastExecNTickCount, 0);
		} while (t1-t0 < 1000);
	n = PutStr(buf, 0, "[USERLAND] spun in ring 3 for ticks=");
	n = PutHex(buf, n, t1-t0);
	n = PutStr(buf, n, " spins=");
	n = PutHex(buf, n, spins);
	Print(buf, n);

	n = PutStr(buf, 0, "[USERLAND] done -- parking in Exec::WaitForAnyRequest");
	Print(buf, n);
	for (;;)
		FastExec(EFastExecWaitForAnyRequest, 0);
	}

// Process entry point (TRomImageHeader::iEntryPoint). rsp = the thread create-info block the kernel
// copied to the top of the user stack; rbx = 0 for the first thread of the process.
extern "C" USER __attribute__((naked)) void X64MinisecEntry()
	{
	__asm__("mov edi, esp\n"
			"and rsp, -16\n"
			"call X64MinisecMain\n"
			"ud2\n");
	}
