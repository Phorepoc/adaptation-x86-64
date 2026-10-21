// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_entry.cpp -- Phorepoc amd64/x32 kernel image: entry point, early CPU tables, and the
// minimal STATIC variant. amd64 sibling of kernel/x86/k_entry.{cia,cpp} (the entry that calls
// PP::InitSuperPageFromRom -- Track B lost time on the nk_entry path that does not) plus the
// smallest possible "PC" variant so K::InitialiseMicrokernel / A::Init1 have an Asic to talk to.
// Runs in the HIGH half, on the initial thread stack, under the tables xboot.cpp built.

#include <x86_mem.h>
#include <assp.h>
#include <ncern.h>
#include "../port/x64ekern/x64_root.h"

extern "C" void KernelMain();
extern "C" void X64_StartSystemTick();					// xk_irq.cpp
extern "C" void X64_StartBootCpuTick();					// xk_irq.cpp: the boot CPU's local-APIC timer
extern "C" TUint32 X64_NanoWaitCalibration();				// xk_irq.cpp: ps per K::DoNanoWait pass
#include "xk_interrupt.h"										// X64InterruptInit1/2AP (X4.21)
extern "C" void X64_StartApTick();						// xk_irq.cpp: an AP's own local-APIC timer
extern volatile TUint32 X64_TickCount;
extern "C" TSubScheduler* SubSchedulerLookupTable[256];
extern "C" const TUint32 xk_vector_table[256];			// xk_vectors.S
extern "C" void (*const __init_array_start[])();		// ekern.lds
extern "C" void (*const __init_array_end[])();

/******************************************************************************
 * Serial (COM1). This IS the kernel's debug port for now: cutils.cpp is built with
 * __EARLY_DEBUG__, so Kern::Printf reaches DebugOutputChar before any variant exists.
 ******************************************************************************/
static inline void outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }
static inline TUint8 inb(TUint16 aPort)
	{ TUint8 v; __asm__ __volatile__("in al, dx" : "=a"(v) : "d"(aPort)); return v; }

void DebugOutputChar(TUint aChar)
	{
#if XK_UART_BATCH
	// 16550 with FIFOs (FCR=0xC7): THRE = the transmit FIFO is empty, so after one poll 16 bytes may follow with no status read.
	// Under a hypervisor every port access is a VM exit: ~1 exit per byte instead of 2 (UARTBATCH=1 in make_ekern.sh).
	static volatile TUint32 sCount;
	if ((sCount++ & 15) == 0)
		for (TInt i=0; i<100000 && !(inb(0x3FD)&0x20); ++i) {}
#else
	for (TInt i=0; i<100000 && !(inb(0x3FD)&0x20); ++i) {}
#endif
	outb(0x3F8, (TUint8)aChar);
	}
// The nanokernel's debug printf (IA-32: cutils.cia KPrintf, a jump into Kern::Printf). Debug
// kernels reference it from the nkern trace macros.
void KPrintf(const char* aFmt, ...)
	{
	TBuf8<256> b;
	VA_LIST list;
	VA_START(list, aFmt);
	Kern::AppendFormat(b, aFmt, list);
	VA_END(list);
	for (TInt i=0; i<b.Length(); ++i)
		DebugOutputChar(b[i]);
	DebugOutputChar(13); DebugOutputChar(10);
	}
static void Put(const char* s)
	{ while (*s) { if (*s=='\n') DebugOutputChar('\r'); DebugOutputChar((TUint8)*s++); } }
static void PutHex64(TUint64 v)
	{ for (TInt i=60; i>=0; i-=4) DebugOutputChar("0123456789abcdef"[(v>>i)&15]); }
static void Line(const char* aWhat, TUint64 aVal)
	{ Put("  [ekern] "); Put(aWhat); Put(" = "); PutHex64(aVal); Put("\n"); }

/******************************************************************************
 * GDT / TSS / IDT in kernel memory (the boot stub's live in low memory that stops
 * being mapped at the first address-space switch).
 ******************************************************************************/
struct SX64Tss
	{
	TUint32 iRsvd0;  TUint64 iRsp[3];  TUint64 iRsvd1;  TUint64 iIst[7];
	TUint64 iRsvd2;  TUint16 iRsvd3;   TUint16 iIoMapBase;
	} __attribute__((packed));
struct SX64Gate
	{
	TUint16 iOffLow;  TUint16 iSel;  TUint8 iIst;  TUint8 iType;  TUint16 iOffMid;  TUint32 iOffHigh;  TUint32 iZero;
	} __attribute__((packed));
struct SX64TablePtr
	{
	TUint16 iLimit;  TUint64 iBase;
	} __attribute__((packed));

const TUint16 KSelKernCode = 0x08, KSelKernData = 0x10, KSelTss = 0x28;	// 0x18/0x20 = ring-3 code/data

// Which CPU is this? Local APIC id -> kernel CPU number, for the few per-CPU things that live in
// this file (TSS, FPU bounce buffer). Filled by _E32Startup (boot CPU = 0) and NKern::BootAP,
// i.e. before the CPU in question can ask.
TUint8 X64_CpuOfApic[256];
extern TUint16 X64_TssSelBase;			// x64s_ncutils.cpp: STR -> this CPU (X4.23)
static inline TInt ThisCpu()
	{
	TUint16 sel;
	__asm__ __volatile__("str %0" : "=r"(sel));
	if (X64_TssSelBase && sel >= X64_TssSelBase)
		return (sel - X64_TssSelBase) >> 4;
	return X64_CpuOfApic[*(volatile TUint32*)0xFEE00020u >> 24];
	}

static TUint64	Gdt[5+2*KMaxCpus];		// 5 fixed descriptors, then one 16-byte TSS descriptor per CPU
static SX64Tss	TssAll[KMaxCpus];
static SX64Gate	Idt[256];

// Called once per CPU (the boot CPU from _E32Startup, each AP from X64_ApEntry): GDT and IDT are
// shared, the TSS (RSP0 = "this CPU's current thread's supervisor stack") is per CPU.
void X64_InstallDescriptorTables(TInt aCpu, TLinAddr aKernelStackTop)
	{
	SX64Tss& Tss = TssAll[aCpu];
	Gdt[1] = 0x00209A0000000000ULL;			// ring-0 code, L=1
	Gdt[2] = 0x0000920000000000ULL;			// ring-0 data
	Gdt[3] = 0x0020FA0000000000ULL;			// ring-3 code, L=1
	Gdt[4] = 0x0000F20000000000ULL;			// ring-3 data
	TUint64 tb = (TUint64)(TLinAddr)&Tss;
	TUint64 tl = sizeof(SX64Tss)-1;
	Gdt[5+2*aCpu] = (tl & 0xFFFF) | ((tb & 0xFFFFFF) << 16) | (0x89ULL << 40) | ((tb >> 24 & 0xFF) << 56);
	Gdt[6+2*aCpu] = tb >> 32;
	Tss.iRsp[0] = aKernelStackTop;
	Tss.iIoMapBase = sizeof(SX64Tss);

	for (TInt i=0; i<256; ++i)
		{
		TUint32 h = xk_vector_table[i];
		Idt[i].iOffLow = (TUint16)h;  Idt[i].iOffMid = (TUint16)(h>>16);  Idt[i].iOffHigh = 0;
		Idt[i].iSel = KSelKernCode;
		Idt[i].iType = (i==0x20 || i==0x21) ? 0xEE : 0x8E;			// interrupt gate; the two exec gates are DPL3
		}

	SX64TablePtr g = { (TUint16)(sizeof(Gdt)-1), (TUint64)(TLinAddr)Gdt };
	SX64TablePtr d = { (TUint16)(sizeof(Idt)-1), (TUint64)(TLinAddr)Idt };
	__asm__ __volatile__(
		"lgdt [%0]\n"
		"lidt [%1]\n"
		"push 0x08\n" "lea rax, [rip+1f]\n" "push rax\n" "retfq\n" "1:\n"	// reload CS
		"mov eax, 0x10\n" "mov ds, eax\n" "mov es, eax\n" "mov ss, eax\n"
		"xor eax, eax\n" "mov fs, eax\n" "mov gs, eax\n"
		"ltr %w2\n"
		: : "r"(&g), "r"(&d), "r"((TUint32)(KSelTss + 16*aCpu)) : "rax", "memory");

	// X4.27b: FS base = the XIP ROM image (global, user-readable in every process). The prebuilt x32
	// libsupc++ (Ubuntu, -fstack-protector) reads its stack canary at %fs:0x18 in __dynamic_cast /
	// __do_upcast / __do_dyncast; with FS base 0 the first dynamic_cast of a non-null pointer faulted
	// at linear 0x18 (eiksrvs, the S60 boot). Only the canary lives there -- a constant word is all it
	// needs. Nothing reloads FS after this (iret/sysret keep the MSR), so one write per CPU.
	extern char __uimage_base[];
	const TUint64 fsBase = (TUint64)(TLinAddr)__uimage_base;
	__asm__ __volatile__("wrmsr" : : "c"(0xC0000100u), "a"((TUint32)fsBase), "d"((TUint32)(fsBase >> 32)));
	}

// Reschedule hook (x64s_ncsched.cpp): ring 3 -> ring 0 transitions land on the incoming thread's
// own supervisor stack.
extern "C" void board_set_rsp0(void* aTop)
	{
	TssAll[ThisCpu()].iRsp[0] = (TUint64)(TLinAddr)aTop;
	}

/******************************************************************************
 * FPU / SSE. The kernel itself is built -mgeneral-regs-only, so only thread context matters:
 * user code (euser is ordinary SSE2 amd64 code) and nothing in an ISR. EAGER switching --
 * fxsave the outgoing thread, fxrstor the incoming one (hooks in x64s_ncsched.cpp Reschedule) --
 * keeps no "owner" pointer that could dangle when a thread dies. fxsave needs a 16-byte aligned
 * area and NThread::iCoprocessorState is only 8-aligned (heap cell), so bounce through a per-CPU
 * aligned buffer. X64_TODO: lazy (#NM) switching as IA-32 EKA2 does.
 ******************************************************************************/
static TUint64 FpuBounceAll[KMaxCpus][64] __attribute__((aligned(64)));
#define FpuBounce	(FpuBounceAll[ThisCpu()])

// Once per CPU, after SubSchedulerLookupTable knows it.
void X64_EnableFpu()
	{
	__asm__ __volatile__(
		"mov rax, cr0\n" "and rax, -13\n" "or rax, 0x22\n" "mov cr0, rax\n"		// clear EM|TS, set MP|NE
		"mov rax, cr4\n" "or rax, 0x600\n" "mov cr4, rax\n"						// OSFXSR | OSXMMEXCPT
		"fninit\n"
		: : : "rax");
	__asm__ __volatile__("fxsave64 [%0]" : : "r"(FpuBounce) : "memory");
	((TUint32*)FpuBounce)[6] = 0x1F80;											// MXCSR: all exceptions masked
	memcpy(DefaultCoprocessorState, FpuBounce, sizeof(FpuBounce));
	}
extern "C" void board_fpu_save(NThreadBase* aThread)
	{
	__asm__ __volatile__("fxsave64 [%0]" : : "r"(FpuBounce) : "memory");
	memcpy(((NThread*)aThread)->iCoprocessorState, FpuBounce, sizeof(FpuBounce));
	}
extern "C" void board_fpu_restore(NThreadBase* aThread)
	{
	memcpy(FpuBounce, aThread ? ((NThread*)aThread)->iCoprocessorState : DefaultCoprocessorState, sizeof(FpuBounce));
	__asm__ __volatile__("fxrstor64 [%0]" : : "r"(FpuBounce) : "memory");
	}

// ... and in its own address space (x64ekern/xe_mmu.cpp: PDPT[0],[1] -> the process's local PD, CR3).
extern "C" void X64_ProcessSwitch(NThreadBase* aNew);
extern "C" void board_switch_as(NThreadBase* aNew)
	{
	X64_ProcessSwitch(aNew);
	}

struct SX64TrapFrame
	{
	TUint64 iR15, iR14, iR13, iR12, iR11, iR10, iR9, iR8, iRdi, iRsi, iRbp, iRbx, iRdx, iRcx, iRax;
	TUint64 iVector, iError, iRip, iCs, iRflags, iRsp, iSs;
	};

extern "C" void X64_EarlyTrap(SX64TrapFrame* aFrame)
	{
	TUint64 cr2;
	__asm__ __volatile__("mov %0, cr2" : "=r"(cr2));
	Put("\n[FAULT] ekern early trap -- HALTED\n");
	Line("vector", aFrame->iVector);  Line("error ", aFrame->iError);
	Line("rip   ", aFrame->iRip);     Line("cr2   ", cr2);
	Line("rsp   ", aFrame->iRsp);     Line("cs    ", aFrame->iCs);
	Line("rax   ", aFrame->iRax);     Line("rbx   ", aFrame->iRbx);
	Line("rcx   ", aFrame->iRcx);     Line("rdx   ", aFrame->iRdx);
	Line("rdi   ", aFrame->iRdi);     Line("rsi   ", aFrame->iRsi);
	Line("rbp   ", aFrame->iRbp);
	// bounded stack peek: return addresses in the image help place the fault
	Put("  [ekern] stack:");
	TUint64* s = (TUint64*)(TLinAddr)aFrame->iRsp;
	// (supervisor stacks live from KKernelSectionBase up; only words that look like ROM code are shown)
	for (TInt i=0; i<96 && (TLinAddr)s >= 0xA0000000u; ++i, ++s)
		if ((*s>>32)==0 && (TUint32)*s >= 0x80000000u && (TUint32)*s < 0x80400000u)
			{ Put(" "); PutHex64(*s); }
	Put("\n");
	for (;;) { __asm__ __volatile__("cli; hlt"); }
	}

/******************************************************************************
 * The PC real-time clock (MC146818 CMOS, ports 0x70/0x71) -- X4.31. EKA2 keeps system time as seconds
 * from 2000-01-01 00:00 (UTC); the kernel reads this once at boot and writes it back on
 * User::SetUTCTime. BCD or binary and 12/24-hour per status register B, as the firmware left it;
 * reads repeat until two agree (never straddle an update). The CMOS century byte is not standard
 * (ACPI FADT names it): years are 2000-2099, which is all EKA2's TInt seconds-from-2000 can say anyway.
 ******************************************************************************/
static TSpinLock RtcLock(TSpinLock::EOrderGenericIrqLow0);

static TUint8 Cmos(TUint8 aReg)				{ outb(0x70, aReg); return inb(0x71); }
static void CmosSet(TUint8 aReg, TUint8 aVal)	{ outb(0x70, aReg); outb(0x71, aVal); }
static TInt FromBcd(TInt aV)				{ return (aV & 0x0F) + (aV >> 4) * 10; }
static TInt ToBcd(TInt aV)					{ return ((aV / 10) << 4) | (aV % 10); }
static TBool IsLeap(TInt aY)				{ return (aY % 4 == 0 && aY % 100 != 0) || aY % 400 == 0; }
static const TInt KDaysBeforeMonth[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

struct SRtc { TUint8 iSec, iMin, iHour, iDay, iMonth, iYear; };

static void RtcSnapshot(SRtc& aT)
	{
	for (TInt i=0; i<100000 && (Cmos(0x0A) & 0x80); ++i) {}		// update in progress (< 2 ms)
	aT.iSec = Cmos(0x00); aT.iMin = Cmos(0x02); aT.iHour = Cmos(0x04);
	aT.iDay = Cmos(0x07); aT.iMonth = Cmos(0x08); aT.iYear = Cmos(0x09);
	}

static TInt X64RtcRead(TInt& aTime)
	{
	SRtc a, b;
	TInt irq = __SPIN_LOCK_IRQSAVE(RtcLock);
	RtcSnapshot(b);
	TInt tries = 0;
	do { a = b; RtcSnapshot(b); } while (memcompare((const TUint8*)&a, sizeof(a), (const TUint8*)&b, sizeof(b)) && ++tries < 5);
	const TUint8 regB = Cmos(0x0B);
	__SPIN_UNLOCK_IRQRESTORE(RtcLock, irq);

	const TBool bcd = !(regB & 0x04), h12 = !(regB & 0x02);
	const TBool pm = h12 && (a.iHour & 0x80);
	TInt hour = a.iHour & 0x7F;
	TInt sec = a.iSec, min = a.iMin, day = a.iDay, month = a.iMonth, year = a.iYear;
	if (bcd)
		{ sec = FromBcd(sec); min = FromBcd(min); hour = FromBcd(hour); day = FromBcd(day);
		  month = FromBcd(month); year = FromBcd(year); }
	if (h12)
		hour = (hour % 12) + (pm ? 12 : 0);
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59)
		{ aTime = 0; return KErrCorrupt; }
	year += 2000;
	TInt days = 0;
	for (TInt y=2000; y<year; ++y)
		days += IsLeap(y) ? 366 : 365;
	days += KDaysBeforeMonth[month-1] + (month > 2 && IsLeap(year) ? 1 : 0) + day - 1;
	aTime = ((days * 24 + hour) * 60 + min) * 60 + sec;
	return KErrNone;
	}

static TInt X64RtcWrite(TInt aTime)
	{
	if (aTime < 0)
		return KErrArgument;
	TInt days = aTime / 86400, rem = aTime % 86400;
	TInt year = 2000;
	while (days >= (IsLeap(year) ? 366 : 365) && year < 2099)
		days -= IsLeap(year++) ? 366 : 365;
	TInt month = 12;
	while (month > 1 && days < KDaysBeforeMonth[month-1] + (month > 2 && IsLeap(year) ? 1 : 0))
		--month;
	const TInt day = days - KDaysBeforeMonth[month-1] - (month > 2 && IsLeap(year) ? 1 : 0) + 1;
	TInt v[6] = { rem % 60, (rem / 60) % 60, rem / 3600, day, month, year - 2000 };
	static const TUint8 KReg[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
	TInt irq = __SPIN_LOCK_IRQSAVE(RtcLock);
	const TUint8 regB = Cmos(0x0B);
	const TBool bcd = !(regB & 0x04), h12 = !(regB & 0x02);
	if (h12)
		v[2] = (v[2] % 12 == 0 ? 12 : v[2] % 12) | (v[2] >= 12 ? 0x80 : 0);		// BCD below keeps bit 7
	CmosSet(0x0B, regB | 0x80);										// SET: freeze updates
	for (TInt i=0; i<6; ++i)
		CmosSet(KReg[i], (TUint8)(bcd ? (ToBcd(v[i] & 0x7F) | (v[i] & 0x80)) : v[i]));
	CmosSet(0x0B, regB);
	__SPIN_UNLOCK_IRQRESTORE(RtcLock, irq);
	return KErrNone;
	}

TInt X64RtcReadSeconds(TInt& aSecondsFrom2000)	{ return X64RtcRead(aSecondsFrom2000); }	// xk_timesync.cpp

// The kernel's default initial-time hook (putils.cpp) ignores the RTC on a COLD start -- a phone's RTC
// is not trusted after power loss -- and falls back to the ROM timestamp (the "12:00 am" every boot).
// A PC's CMOS clock is battery-backed: trust it, unless it cannot be read (then the ROM time, as before).
static TInt X64InitialTime()
	{
	TInt seconds;
	return X64RtcRead(seconds) == KErrNone ? seconds : KErrCorrupt;
	}

/******************************************************************************
 * The minimal static variant (a "PC" with nothing but a serial port so far)
 ******************************************************************************/
class DX64Variant : public Asic
	{
public:
	virtual TMachineStartupType StartupReason()				{ return EStartupColdReset; }
	virtual void Init1()									{ X64InterruptInit1(); Kern::SetInitialTimeHandler(X64InitialTime); }
	virtual void GetCpuBootData(SCpuBootData* aInfo);
	virtual void Init2AP()									{ X64InterruptInit2AP(); X64_StartApTick(); }
	virtual void Init3()									{ X64_StartSystemTick(); }
	virtual void DebugOutput(TUint aChar)					{ DebugOutputChar(aChar); }
	virtual void Idle();
	virtual TInt MsTickPeriod()								{ return 1000; }
	virtual TInt SystemTimeInSecondsFrom2000(TInt& aTime)	{ return X64RtcRead(aTime); }
	virtual TInt SetSystemTimeInSecondsFrom2000(TInt aTime)	{ return X64RtcWrite(aTime); }
	// A::Init1 asks right after Init1(), on the boot CPU, interrupts off: measure, THEN start the tick
	// (the measurement borrows the local-APIC counter).
	virtual TUint32 NanoWaitCalibration()					{ TUint32 c = X64_NanoWaitCalibration(); X64_StartBootCpuTick(); return c; }
	virtual TInt VariantHal(TInt aFunction, TAny* a1, TAny* a2);
	virtual TPtr8 MachineConfiguration()
		{ return TPtr8((TUint8*)KMachineConfigLinAddr, 0x400, 0x400); }
	};

// EHalGroupVariant. EVariantHalVariantInfo is the one UserHal::MachineInfo() requires ("must
// always be implemented!", us_exec.cpp) -- fntstore's CFontStore::ConstructL, i.e. FBSERV, dies
// without it. Shape and machine UID from the Syborg ASSP (baseport/syborg/specific/assp.cpp); our
// HAL values.hda carries the same EMachineUid. X4.19.
TInt DX64Variant::VariantHal(TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	switch (aFunction)
		{
	case EVariantHalVariantInfo:
		{
		TVariantInfoV01Buf infoBuf;
		TVariantInfoV01& info = infoBuf();
		const TUint clockKHz = 2000000;				// X64_TODO: calibrate (TheVib.iMaxCpuClock)
		info.iRomVersion = Epoc::RomHeader().iVersion;
		info.iMachineUniqueId = TInt64(0x10005152) << 32;
		info.iLedCapabilities = 0;
		info.iProcessorClockInKHz = clockKHz;
		info.iSpeedFactor = clockKHz / 25;
		Kern::InfoCopy(*(TDes8*)a1, infoBuf);
		return KErrNone;
		}
	case EVariantHalDebugPortSet:
		{
		TUint32 port = (TUint32)a1;					// COM1 only (xk_entry DebugOutputChar)
		if (port != 0 && port != (TUint32)KNullDebugPort)
			return KErrNotSupported;
		Kern::SuperPage().iDebugPort = port;
		return KErrNone;
		}
	case EVariantHalDebugPortGet:
		{
		TUint32 port = Kern::SuperPage().iDebugPort;
		kumemput32(a1, &port, sizeof(port));
		return KErrNone;
		}
	default:
		return KErrNotSupported;
		}
	}

void DX64Variant::Idle()
	{
	__asm__ __volatile__("sti; hlt");
	}

// The boot processor + whatever the bootstrap found in the ACPI MADT; A::InitAPs boots them one by one.
void DX64Variant::GetCpuBootData(SCpuBootData* aInfo)
	{
	memclr(aInfo, KMaxCpus*sizeof(SCpuBootData));
	TUint32 id = *(volatile TUint32*)0xFEE00020u >> 24;
	aInfo[0].iPresent = 1;
	aInfo[0].iAPICID = (TUint8)id;
	// the application processors the bootstrap found in the ACPI MADT (x64_root.h); SMP=0 builds ignore them
	const SSuperPageBase& s = Kern::SuperPage();
	TInt n = s.iReserved[KX64SuperCpuCount];
	for (TInt i=1; i<n && i<KMaxCpus && i<KX64MaxBootCpus; ++i)
		{
		aInfo[i].iPresent = 1;
		aInfo[i].iAPICID = (TUint8)s.iReserved[KX64SuperCpuApicId+i];
		}
	}

// The variant interface block NKern::Init0 consumes (K::VariantData[0]). Null ratio pointers =
// "every CPU and timer runs at the maximum frequency".
static SVariantInterfaceBlock TheVib;

static DX64Variant TheVariant;		// constructed by the static-ctor walk below, before KernelMain

// The one kernel extension the ROM must carry -- see xboot.cpp. Called with
// KModuleEntryReasonVariantInit0 / ExtensionInit0 (return = start-up priority) / ExtensionInit1.
// Until this slot becomes exstart it hosts a bounded SELF-TEST of the scheduler + timer path: a
// supervisor thread that sleeps on the nanokernel timer queue four times and exits. It proves, on
// the real kernel: DThread creation, a second context, blocking (NKern::Sleep -> NTimer ->
// NTimerQ::Tick in the ISR -> the timer DFC thread -> wake), preemption of the null thread from
// the tick, and thread exit.
_LIT(KProbeName, "X64Probe");
static TInt ProbeThread(TAny*)
	{
	Kern::Printf("[probe] thread running, tick %d", NKern::TickCount());

	// A deliberate kernel-mode fault under an exception trap: #PF -> X64_Trap -> Exc::Dispatch ->
	// the thread's TExcTrap -> TExcTrap::Exception longjmps back here with KErrBadDescriptor.
	TInt r = 12345;
	XTRAP(r, XT_DEFAULT, { volatile TInt v = *(volatile TInt*)0x0BAD0000u; (void)v; });
	Kern::Printf("[probe] XTRAP around a read of 0x0BAD0000 -> %d (KErrBadDescriptor = %d)", r, KErrBadDescriptor);
	for (TInt i=1; i<=4; ++i)
		{
		NKern::Sleep(500);
		Kern::Printf("[probe] woke #%d at tick %d (slept 500)", i, NKern::TickCount());
		}
	Kern::Printf("[probe] exiting -- kernel threads + NTimer sleep + timer DFC all work");
	return KErrNone;
	}

void X64StartMediaExtensions();		// xk_media.cpp: ELOCD + the IRAM drive (X4.17)
void X64StartDisplay();				// xk_display.cpp: EHalGroupDisplay, VGA text (X4.18)
void X64StartKeyboard();			// xk_keyboard.cpp: i8042 -> Kern::AddEvent (X4.18)
void X64StartNetwork();				// xk_e1000.cpp: the e1000 NIC + Ethernet LDD (X4.47)
void X64StartSound();				// xk_ac97.cpp: AC'97 audio (X4.88)
void X64StartVmwSvga();				// xk_vmwsvga.cpp: read-only VMware SVGA II / SVGA3D capability probe (vm-gpu-options.md S0)
void X64ArmThreadDump();			// xk_threaddump.cpp: make_ekern.sh DUMP=<s> (X4.27)
void X64StartPower();				// xk_power.cpp: power controller + handler, ACPI off / reset (X4.74)

extern "C" TInt X64ExtensionEntry(TInt aReason)
	{
	if (aReason != KModuleEntryReasonExtensionInit1)
		return KErrNone;
	X64StartPower();					// X4.74: before the drivers -- SSM off/restart need it
	X64StartMediaExtensions();
	X64StartDisplay();
	X64StartKeyboard();
	X64StartNetwork();					// X4.47: the e1000 NIC + the Ethernet LDD
	X64StartSound();					// X4.88: AC'97 audio (its LDD/PDD load by file name, like the NIC's)
	X64StartVmwSvga();						// S0: logs [svga] lines (a no-op line when the device is absent)
	X64ArmThreadDump();
	SThreadCreateInfo info;
	info.iType = EThreadSupervisor;
	info.iFunction = ProbeThread;
	info.iPtr = NULL;
	info.iSupervisorStack = NULL;
	info.iSupervisorStackSize = 0;			// default
	info.iInitialThreadPriority = 24;
	info.iName.Set(KProbeName);
	info.iTotalSize = sizeof(info);
	TInt r = Kern::ThreadCreate(info);
	Kern::Printf("[probe] Kern::ThreadCreate -> %d", r);
	if (r == KErrNone)
		Kern::ThreadResume(*(DThread*)info.iHandle);
	return KErrNone;
	}

// The REAL exstart (baseline kernel/exstart.cpp, KernelModuleEntry renamed by make_ekern.sh).
// A ROM extension's static data is constructed when the kernel ATTACHES it (P::StartExtensions);
// linked statically, exstart's global `TDfc StartSecondaryDfc(.., Kern::SvMsgQue(), ..)` was
// constructed by _E32Startup's ctor walk, long before the supervisor message queue existed, so it
// captured a NULL queue. Re-bind it here, at the moment a real attach would have run the ctor.
TInt X64ExstartEntry(TInt aReason);
extern TDfc StartSecondaryDfc;
TInt X64ExstartShim(TInt aReason)
	{
	if (aReason == KModuleEntryReasonExtensionInit1)
		StartSecondaryDfc.SetDfcQ(Kern::SvMsgQue());
	return X64ExstartEntry(aReason);
	}

extern "C" TInt X64VariantEntry(TInt /*aReason*/)
	{
	return 1;						// bit 0: also call VariantInitialise(1) -> the interface block
	}
extern "C" TAny* X64VariantInitialise(TInt aMode)
	{
	if (aMode==0)
		return (TAny*)(Asic*)&TheVariant;
	if (aMode==1)
		{
		TheVib.iVer = 0;
		TheVib.iSize = sizeof(SVariantInterfaceBlock);
		TheVib.iMaxCpuClock = UI64LIT(2000000000);	// X64_TODO: calibrate (TSC vs PIT), as kmain_smp.c does
		TheVib.iTimestampFreq = 1000000;
		TheVib.iMaxTimerClock = 128*1000;			// NKern::Init0 divides by 128 -> 1000 timer ticks/s
		return &TheVib;
		}
	return NULL;
	}

/******************************************************************************
 * Kernel entry: _E32Startup(ROM header, super page)
 ******************************************************************************/
extern "C" __attribute__((section(".text.entry"))) void _E32Startup(TLinAddr aRomHeader, TLinAddr aSuperPage)
	{
	TLinAddr sp;
	__asm__ __volatile__("mov %0, esp" : "=r"(sp));
	Put("\n[ekern] _E32Startup: in the high half, on the initial thread stack\n");
	Line("ROM header", aRomHeader);  Line("super page", aSuperPage);  Line("rsp", sp);

	// The firmware leaves the legacy 8259 PICs delivering IRQ0-7 on vectors 8-15 -- on top of the CPU
	// exceptions (a PIT tick looked like a #DF with a bogus frame). Remap to 0x30-0x3F (0x20/0x21 are EKA2's exec gates) and mask
	// every line; interrupt sources are opened one by one as their handlers become real.
	outb(0x20,0x11); outb(0xA0,0x11); outb(0x21,0x30); outb(0xA1,0x38);
	outb(0x21,0x04); outb(0xA1,0x02); outb(0x21,0x01); outb(0xA1,0x01);
	outb(0x21,0xFF); outb(0xA1,0xFF);

	X64_CpuOfApic[*(volatile TUint32*)0xFEE00020u >> 24] = 0;
	X64_InstallDescriptorTables(0, (sp+0xFFF)&~0xFFFu);
	X64_TssSelBase = KSelTss;				// from now on STR says which CPU (each loads KSelTss + 16*cpu)
	X64_EnableFpu();
	Put("[ekern] GDT/TSS/IDT now in kernel memory; faults are loud from here\n");

	// Nothing low is needed any more: drop the bootstrap's temporary 4 MiB identity window (two 2 MiB
	// PDEs at the bottom of the kernel's local page directory) before the memory model inventories
	// the address space.
	TPde* pd = (TPde*)KPageDirectoryBase;
	pd[0] = 0;  pd[1] = 0;
	__asm__ __volatile__("mov rax, cr3; mov cr3, rax" : : : "rax");

	// CPU 0's long-mode root (built by xboot.cpp) -> the address-space switch code.
	const SSuperPageBase& s = *(const SSuperPageBase*)aSuperPage;
	X64_CpuRoot[0].iPml4Phys = (TUint32)s.iReserved[KX64SuperRootPhys];
	X64_CpuRoot[0].iPdpt = (TUint64*)((TLinAddr)s.iReserved[KX64SuperRootLin] + 0x1000);

	// SubScheduler() resolves through the local APIC id from the very first NKern call.
	TUint32 apic = *(volatile TUint32*)0xFEE00020u >> 24;
	SubSchedulerLookupTable[apic & 0xFF] = &TheSubSchedulers[0];

	PP::InitSuperPageFromRom(aRomHeader, aSuperPage);

	TInt n = 0;
	for (void (*const* f)() = __init_array_start; f < __init_array_end; ++f, ++n)
		(*f)();
	Line("static constructors run", (TUint64)n);
	TheSubSchedulers[0].iSSX.iAPICID = apic;		// after the TSubScheduler ctor, which resets it

	Put("[ekern] -> KernelMain()\n");
	KernelMain();
	Put("[ekern] KernelMain returned?!\n");
	for (;;) { __asm__ __volatile__("cli; hlt"); }
	}
