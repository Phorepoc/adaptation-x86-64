// Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xboot.cpp -- Phorepoc amd64/x32 BOOTSTRAP. The C++ sibling of the IA-32 bootstrap
// (kernel/x86/bootmain.asm + bootutil.asm, steps 3-5) for the FLEXIBLE memory model.
//
// Runs once, in long mode, at LOW identity addresses (boot.S built a throw-away 4 GiB identity
// map and called kmain). It must leave the machine exactly as EKA2's kernel entry expects:
//   - a ROM header + primary/variant image records at KRomLinearBase,
//   - the kernel address space built per include/memmodel/epoc/flexible/x86/mmboot.h, in
//     long-mode format (port/x64mm/mmboot.h): 16 KiB page directory = the 4 PD pages, page
//     tables mapped contiguously at KPageTableBase through PT0, per-CPU PML4 + PDPT above,
//   - super page + CPU pages, RAM/ROM bank lists, page-table-info page, initial page-array
//     segment, KPageInfoMap + the SPageInfo array for every RAM bank,
//   - kernel .data copied / .bss cleared, initial stack painted 0xFF, initial heap mapped,
// then call  entry(ROM header linear, super page linear)  on the initial thread stack.
// The kernel learns which RAM pages are in use by WALKING these tables (Mmu::Init2Common), so
// every page handed out here is either mapped or outside the RAM bank.
//
// Built -masm=intel like the rest. No kernel function may be called from here (wrong addresses
// until the switch); no libc.

#include <e32rom.h>
#include <kernel/kernboot.h>
#include <mmboot.h>
#include "xk_board.h"

typedef TUint64 TEntry;				// a long-mode page-table entry, any level

/******************************************************************************
 * Linker symbols (ekern.lds)
 ******************************************************************************/
extern "C" {
extern char __rom_lin[], __rom_phys[], __rom_size[];
extern char __code_base[], __code_end[];
extern char __data_run[], __data_load[], __data_size[], __svdata_size[];
void _E32Startup(TLinAddr aRomHeader, TLinAddr aSuperPage);		// high half
TAny* X64VariantInitialise(TInt aMode);							// high half (static variant)
TInt X64VariantEntry(TInt aReason);								// high half
TInt X64ExtensionEntry(TInt aReason);							// high half (static no-op extension)
void X64MinisecEntry();										// ROM, runs in ring 3 (xk_minisec.cpp)
TAny* X64CreateLdd_enet();										// high half (xk_e1000.cpp): built-in device factories
TAny* X64CreatePdd_e1000();
TAny* X64CreateLdd_soundsc();									// xk_ac97.cpp (X4.88)
TAny* X64CreatePdd_ac97();
TAny* X64CreateLdd_camerasc();									// xk_camerasc.cpp (camera C1)
TAny* X64CreatePdd_camerasc();
extern char __utext_base[], __utext_end[], __uimage_base[];
void kmain(TUint32 aMultibootInfo);
}
TInt X64ExstartShim(TInt aReason);								// xk_entry.cpp -> baseline kernel/exstart.cpp
#define SYMVAL(s)	((TUint32)(TLinAddr)(s))

/******************************************************************************
 * Serial trace (COM1) -- bounded, boot-time only
 ******************************************************************************/
static inline void outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }
static inline TUint8 inb(TUint16 aPort)
	{ TUint8 v; __asm__ __volatile__("in al, dx" : "=a"(v) : "d"(aPort)); return v; }
static void PutC(char c)
	{
	for (TInt i=0; i<100000 && !(inb(0x3FD)&0x20); ++i) {}
	outb(0x3F8, (TUint8)c);
	}
static void Put(const char* s)
	{ while (*s) { if (*s=='\n') PutC('\r'); PutC(*s++); } }
static void PutHex(TUint32 v)
	{ for (TInt i=28; i>=0; i-=4) PutC("0123456789abcdef"[(v>>i)&15]); }
static void Trace(const char* aWhat, TUint32 aVal)
	{ Put("  [xboot] "); Put(aWhat); Put(" = "); PutHex(aVal); Put("\n"); }
static void Die(const char* aWhy)
	{ Put("  [xboot] FATAL: "); Put(aWhy); Put("\n"); for(;;) { __asm__ __volatile__("cli; hlt"); } }

static void Zero(TUint32 aPhys, TUint32 aSize)
	{ volatile TUint8* p=(volatile TUint8*)aPhys; while (aSize--) *p++=0; }
static void Copy(TUint32 aDst, TUint32 aSrc, TUint32 aSize)
	{ volatile TUint8* d=(volatile TUint8*)aDst; const TUint8* s=(const TUint8*)aSrc; while (aSize--) *d++=*s++; }
static void Fill32(TUint32 aPhys, TUint32 aSize, TUint32 aVal)
	{ volatile TUint32* p=(volatile TUint32*)aPhys; for (aSize>>=2; aSize; --aSize) *p++=aVal; }

/******************************************************************************
 * Physical page allocator: bump, upward from the base of the RAM bank
 ******************************************************************************/
static TUint32 BankBase, BankEnd, AllocNext;

static TUint32 AllocPages(TUint aCount)
	{
	TUint32 p = AllocNext;
	AllocNext += aCount<<KPageShift;
	if (AllocNext > BankEnd)
		Die("out of RAM");
	Zero(p, aCount<<KPageShift);
	return p;
	}

/******************************************************************************
 * Mapping. Entry permissions = the IA-32 bits (identical in long mode).
 ******************************************************************************/
const TEntry KPermPde		= KPdePtePresent|KPdePteWrite|KPdePteUser;			// enforce at the PTE
// They must equal what the kernel itself would compose (flexible/mmu/x86/xmmu.cpp Mmu::BlankPte):
// when a kernel memory object later CLAIMS boot mappings (DCoarseMemory::ClaimInitialPages) a debug
// kernel asserts the entries match. Everything mapped at boot is GLOBAL; every amd64 CPU has PGE.
const TEntry KPermKernel	= KPdePtePresent|KPdePteWrite|KPdePteGlobal;
const TEntry KPermRom		= KPdePtePresent|KPdePteUser|KPdePteGlobal;			// read-only, user-visible
const TEntry KPermHw		= KPdePtePresent|KPdePteWrite|KPdePteUncached|KPdePteGlobal;

static TEntry*	KernPd;			// physical = identity pointer: the 4 contiguous PD pages
static TEntry*	Pt0;			// the page table that maps the page-table window
static TUint	PtCount;

static TEntry* PageTableFor(TLinAddr aLin)
	{
	TEntry& pde = KernPd[aLin>>KChunkShift];
	if (pde & KPdePtePresent)
		return (TEntry*)(TUint32)(pde & KPdePtePhysAddrMask);
	if (PtCount >= (TUint)(KChunkSize>>KPageShift))
		Die("page-table window full");
	TUint32 pt = AllocPages(1);
	Pt0[PtCount++] = (TEntry)pt | KPermKernel;		// appears at KPageTableBase + n*4K: contiguous
	pde = (TEntry)pt | KPermPde;
	return (TEntry*)pt;
	}

static void MapContiguous(TLinAddr aLin, TUint32 aPhys, TUint32 aSize, TEntry aPerm)
	{
	for (TUint32 off=0; off<aSize; off+=KPageSize)
		{
		TEntry& pte = PageTableFor(aLin+off)[((aLin+off)>>KPageShift) & (KChunkMask>>KPageShift)];
		if (pte)
			Die("double mapping");
		pte = (TEntry)(aPhys+off) | aPerm;
		}
	}

static TUint32 AllocAndMap(TLinAddr aLin, TUint32 aSize, TEntry aPerm)
	{
	aSize = (aSize+KPageMask)&~KPageMask;
	TUint32 p = AllocPages(aSize>>KPageShift);
	MapContiguous(aLin, p, aSize, aPerm);
	return p;
	}

/******************************************************************************
 * Fixed boot-time addresses above the flexible map's last region (KPageTableEnd =
 * 0xFEC00000 was chosen by the IA-32 port so the APICs can be identity-mapped).
 ******************************************************************************/
const TLinAddr KIoApicLin	= 0xFEC00000u;
const TLinAddr KLocalApicLin= 0xFEE00000u;
const TLinAddr KX64RootLin	= 0xFEF00000u;	// per-CPU PML4+PDPT pages: cpu n at +n*0x2000 (PML4), +0x1000 (PDPT)
const TUint    KX64MaxCpus	= 8;

// Offsets inside super page + CPU pages (include/kernel/x86/bootdefs.h)
const TUint32 KSuperCpuPages	= 6;		// SMP: super page + 5 CPU pages
const TUint32 KRomTableOffset	= 0x1980;
const TUint32 KRamTableOffset	= 0x1A00;

const TUint32 KInitStackSize	= 0x4000;	// 64-bit frames: 4x the IA-32 0x1000
const TUint32 KHeapSizeMin		= 0x00100000;
const TUint32 KHeapSizeMax		= 0x01000000;

static TUint32 RamTopFromMultiboot(TUint32 aInfo)
	{
	// multiboot2 info: u32 total_size, u32 reserved, then tags { u32 type, u32 size, ... } 8-aligned.
	// tag 4 = basic meminfo { mem_lower KiB, mem_upper KiB (above 1 MiB) }.
	TUint32 total = *(TUint32*)aInfo;
	for (TUint32 off=8; off<total; )
		{
		TUint32* tag = (TUint32*)(aInfo+off);
		if (tag[0]==0)
			break;
		if (tag[0]==4)
			return 0x100000u + (tag[3]<<10);
		off += (tag[1]+7)&~7u;
		}
	Die("no multiboot2 meminfo tag");
	return 0;
	}

// The linear framebuffer GRUB set up for boot.S's multiboot2 framebuffer request (tag 8:
// u64 addr, u32 pitch, u32 width, u32 height, u8 bpp, u8 type), for the board's display HAL
// (xk_display.cpp) -- xk_board.h's super-page words. Absent = VGA text only. X4.18.
static void FramebufferFromMultiboot(TUint32 aInfo, TInt* aOut)
	{
	TUint32 total = *(TUint32*)aInfo;
	for (TUint32 off=8; off<total; )
		{
		TUint32* tag = (TUint32*)(aInfo+off);
		if (tag[0]==0)
			break;
		if (tag[0]==8 && tag[3]==0)			// addr high word 0: below 4 GiB
			{
			const TUint8* b = (const TUint8*)(tag+7);
			aOut[KX64SuperFbPhys] = (TInt)tag[2];
			aOut[KX64SuperFbPitch] = (TInt)tag[4];
			aOut[KX64SuperFbWidth] = (TInt)tag[5];
			aOut[KX64SuperFbHeight] = (TInt)tag[6];
			aOut[KX64SuperFbFormat] = b[0] | ((b[1]==1) << 8);
			return;
			}
		off += (tag[1]+7)&~7u;
		}
	}

// Processors: GRUB hands over a copy of the ACPI RSDP (multiboot2 tag 14 = v1, 15 = v2); RSDT ->
// "APIC" table (MADT) -> one type-0 record per local APIC. Done HERE because the tables live in
// firmware memory that only the bootstrap's identity map reaches. Returns the enabled CPUs' APIC
// ids, boot CPU first; 0 = nothing found (the kernel then runs uniprocessor).
static TUint FindCpus(TUint32 aInfo, TUint8* aApicIds, TUint aMax, TUint8* aIsa)
	{
	const TUint8* rsdp = 0;
	TUint32 total = *(TUint32*)aInfo;
	for (TUint32 off=8; off<total; )
		{
		TUint32* tag = (TUint32*)(aInfo+off);
		if (tag[0]==0)
			break;
		if (tag[0]==14 || tag[0]==15)
			rsdp = (const TUint8*)(tag+2);
		off += (tag[1]+7)&~7u;
		}
	if (!rsdp)
		return 0;
	const TUint32* rsdt = (const TUint32*)*(const TUint32*)(rsdp+16);
	if (!rsdt || rsdt[0] != 0x54445352u)							// "RSDT"
		return 0;
	TUint32 bsp;
	{ TUint32 a=1,b,c,d; __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d)); bsp = b>>24; }
	TUint n = 0;
	for (TUint i=0; i<(rsdt[1]-36)/4; ++i)
		{
		const TUint8* t = (const TUint8*)rsdt[9+i];
		if (*(const TUint32*)t != 0x43495041u)						// "APIC"
			continue;
		TUint32 len = *(const TUint32*)(t+4);
		aApicIds[n++] = (TUint8)bsp;
		for (TUint32 o=44; o+2<=len && n<aMax; o+=t[o+1])
			{
			if (t[o+1]<2)
				break;
			if (t[o]==0 && (*(const TUint32*)(t+o+4) & 1) && t[o+3]!=bsp)
				aApicIds[n++] = t[o+3];
			}
		// type 2 = interrupt source override: ISA irq t[o+3] arrives on GSI [o+4], MPS INTI flags [o+8]
		// (bits 0-1 polarity 1 high / 3 low, bits 2-3 trigger 1 edge / 3 level). xk_board.h encoding.
		for (TUint32 o=44; o+2<=len; o+=t[o+1])
			{
			if (t[o+1]<2)
				break;
			if (t[o]!=2 || t[o+2]!=0 || t[o+3]>=16)
				continue;
			TUint32 gsi = *(const TUint32*)(t+o+4);
			TUint16 fl = *(const TUint16*)(t+o+8);
			if (gsi >= 32)
				continue;
			TUint8 e = (TUint8)(KX64IsaOverride | gsi);
			if ((fl & 3) == 3)
				e |= KX64IsaActiveLow;
			if (((fl>>2) & 3) == 3)
				e |= KX64IsaLevel;
			aIsa[t[o+3]] = e;
			}
		break;
		}
	return n;
	}

// Soft-off: RSDT -> "FACP" (FADT): PM1a_CNT_BLK [64], PM1b_CNT_BLK [68], DSDT [40]; the DSDT's AML
// names \_S5 = Package {SLP_TYPa, SLP_TYPb, ...} (NameOp 08 "_S5_" PackageOp 12 PkgLength NumElements,
// each element a BytePrefix 0A value or a bare Zero/One). Same identity-map reason as FindCpus. X4.74.
static void FindPowerOff(TUint32 aInfo, TInt* aSp)
	{
	aSp[KX64SuperPm1a] = aSp[KX64SuperPm1b] = 0;
	aSp[KX64SuperPm1Evt] = aSp[KX64SuperSci] = aSp[KX64SuperSmiCmd] = 0;
	const TUint8* rsdp = 0;
	TUint32 total = *(TUint32*)aInfo;
	for (TUint32 off=8; off<total; )
		{
		TUint32* tag = (TUint32*)(aInfo+off);
		if (tag[0]==0)
			break;
		if (tag[0]==14 || tag[0]==15)
			rsdp = (const TUint8*)(tag+2);
		off += (tag[1]+7)&~7u;
		}
	if (!rsdp)
		return;
	const TUint32* rsdt = (const TUint32*)*(const TUint32*)(rsdp+16);
	if (!rsdt || rsdt[0] != 0x54445352u)							// "RSDT"
		return;
	for (TUint i=0; i<(rsdt[1]-36)/4; ++i)
		{
		const TUint8* t = (const TUint8*)rsdt[9+i];
		if (*(const TUint32*)t != 0x50434146u)						// "FACP"
			continue;
		// the power button's event block + SCI + the ACPI-mode switch (X4.75): PM1a_EVT_BLK [56], PM1_EVT_LEN
		// [88], SCI_INT [46], SMI_CMD [48], ACPI_ENABLE [52]
		aSp[KX64SuperPm1Evt] = (TInt)((*(const TUint32*)(t+56) & 0xFFFF) | (TUint32)t[88] << 16);
		aSp[KX64SuperSci] = (TInt)(*(const TUint16*)(t+46) | (TUint32)t[52] << 16);
		aSp[KX64SuperSmiCmd] = (TInt)*(const TUint32*)(t+48);
		TUint32 pm1a = *(const TUint32*)(t+64), pm1b = *(const TUint32*)(t+68);
		const TUint8* dsdt = (const TUint8*)*(const TUint32*)(t+40);
		if (!pm1a || !dsdt || *(const TUint32*)dsdt != 0x54445344u)	// "DSDT"
			return;
		TUint32 len = *(const TUint32*)(dsdt+4);
		for (TUint32 o=36; o+12<len; ++o)
			{
			const TUint8* p = dsdt+o;
			if (p[0]!='_' || p[1]!='S' || p[2]!='5' || p[3]!='_' || !(p[-1]==0x08 || (p[-2]==0x08 && p[-1]=='\\')) || p[4]!=0x12)
				continue;
			p += 5;
			p += ((*p & 0xC0) >> 6) + 1;							// PkgLength
			++p;													// NumElements
			TUint32 typ[2];
			for (TInt k=0; k<2; ++k)
				{
				if (*p==0x0A)
					++p;
				typ[k] = *p++ & 7;
				}
			aSp[KX64SuperPm1a] = (TInt)(KX64AcpiValid | (typ[0]<<16) | (pm1a & 0xFFFF));
			if (pm1b)
				aSp[KX64SuperPm1b] = (TInt)(KX64AcpiValid | (typ[1]<<16) | (pm1b & 0xFFFF));
			return;
			}
		return;
		}
	}

extern "C" void kmain(TUint32 aMultibootInfo)
	{
	outb(0x3F9,0); outb(0x3FB,0x80); outb(0x3F8,1); outb(0x3F9,0); outb(0x3FB,3); outb(0x3FA,0xC7);
	Put("\n[xboot] Phorepoc amd64/x32 bootstrap -- EKA2 flexible memory model, long-mode tables\n");

	const TUint32 romLin	= SYMVAL(__rom_lin);
	const TUint32 romPhys	= SYMVAL(__rom_phys);
	const TUint32 romSize	= (SYMVAL(__rom_size)+KPageMask)&~KPageMask;
	const TUint32 svData	= SYMVAL(__svdata_size);
	Trace("ROM phys", romPhys); Trace("ROM size", romSize); Trace("SvData size", svData);

	// ---- RAM: one bank, from just above the ROM image + the per-CPU root tables to the top.
	// (bootmain.asm "ROM is actually in RAM": the ROM area is excised from the RAM list.)
	TUint32 ramTop = RamTopFromMultiboot(aMultibootInfo) & ~(TUint32)KChunkMask;
	// iTotalRamSize (and much of the kernel's RAM arithmetic) is a TInt, TPhysAddr 32 bits: below 2 GiB.
	// More needs the LP64 kernel (64-bit TPhysAddr) -- x86-64-port.md X4.34.
	if (ramTop > 0x7FE00000u)
		ramTop = 0x7FE00000u;
	const TUint32 rootPhys = romPhys + romSize;	// 2 pages per CPU, NOT in the RAM bank
	Zero(rootPhys, KX64MaxCpus*2*KPageSize);
	BankBase = (rootPhys + KX64MaxCpus*2*KPageSize + KChunkMask) & ~(TUint32)KChunkMask;
	BankEnd = ramTop;
	AllocNext = BankBase;
	Trace("RAM bank base", BankBase); Trace("RAM bank end", BankEnd);

	// ---- InitMemMapSystem: page directory, PT0 self-mapping the page-table window, PD mapped.
	KernPd = (TEntry*)AllocPages(KPageDirectorySize>>KPageShift);
	Pt0 = (TEntry*)AllocPages(1);
	Pt0[0] = (TEntry)(TUint32)Pt0 | KPermKernel;
	PtCount = 1;
	KernPd[KPageTableBase>>KChunkShift] = (TEntry)(TUint32)Pt0 | KPermPde;
	MapContiguous(KPageDirectoryBase, (TUint32)KernPd, KPageDirectorySize, KPermKernel);

	// ---- ROM, super + CPU pages, page-table info, initial page-array segment, hardware.
	MapContiguous(romLin, romPhys, romSize, KPermRom);
	const TUint32 superPhys = AllocAndMap(KSuperPageLinAddr, KSuperCpuPages<<KPageShift, KPermKernel);
	AllocAndMap(KPageTableInfoBase, KPageSize, KPermKernel);
	AllocAndMap(KPageArraySegmentBase, KPageSize, KPermKernel);
	MapContiguous(KIoApicLin, KIoApicLin, KPageSize, KPermHw);
	MapContiguous(KLocalApicLin, KLocalApicLin, KPageSize, KPermHw);
	MapContiguous(KX64RootLin, rootPhys, KX64MaxCpus*2*KPageSize, KPermKernel);

	// ---- kernel .data/.bss + initial thread stack + initial heap, contiguous at iKernDataAddress.
	TUint32 heap = ((ramTop-BankBase)>>(KPageShift-1)) + 24*1024;		// ~2 bytes per RAM page + 24K
	if (heap < KHeapSizeMin)
		heap = KHeapSizeMin;
	heap = (heap+KPageMask)&~KPageMask;
	const TUint32 svRounded = (svData+KPageMask)&~KPageMask;
	const TLinAddr kernData = SYMVAL(__data_run);
	const TUint32 kdPhys = AllocAndMap(kernData, svRounded+KInitStackSize+heap, KPermKernel);
	Copy(kdPhys, romPhys + (SYMVAL(__data_load)-romLin), SYMVAL(__data_size));	// .data; .bss already zero
	Fill32(kdPhys+svRounded, KInitStackSize, 0xFFFFFFFFu);
	const TLinAddr initSp = kernData + svRounded + KInitStackSize;
	Trace("kernel data phys", kdPhys); Trace("initial heap", heap); Trace("initial SP", initSp);

	// ---- bank lists (CPU page), then the SPageInfo array: one page of 32-byte records per
	// 128 RAM pages; bit n of KPageInfoMap set <=> page n of the array exists.
	SRamBank* ram = (SRamBank*)(superPhys+KRamTableOffset);
	ram[0].iBase = BankBase; ram[0].iSize = BankEnd-BankBase;		// [1] = {0,0} ends RAM; [2] = {0,0} ends reserved
	SRomBank* rom = (SRomBank*)(superPhys+KRomTableOffset);
	rom[0].iBase = romPhys; rom[0].iSize = romSize; rom[0].iLinBase = romLin;	// [1] = 0 terminator
	const TUint32 mapPhys = AllocAndMap(KPageInfoMap, KPageSize, KPermKernel);
	const TUint blockShift = 24-KPageInfoShift;
	TUint first = BankBase>>blockShift, last = (BankEnd-1)>>blockShift;
	for (TUint n=first; n<=last; ++n)
		((TUint8*)mapPhys)[n>>3] |= (TUint8)(1u<<(n&7));
	AllocAndMap(KPageInfoLinearBase+(first<<KPageShift), (last-first+1)<<KPageShift, KPermKernel);

	// ---- the ROM header + image records, written through the physical alias of the ROM.
	TRomHeader& rh = *(TRomHeader*)romPhys;
	struct SRecords												// lives at ROM + sizeof(TRomHeader)
		{
		TRomEntry			iPrimaryEntry;	TUint8 iPad1[32];	// iName[] extends past the struct
		TRomImageHeader		iPrimary;
		TRomEntry			iVariantEntry;	TUint8 iPad2[32];
		TRomImageHeader		iVariant;
		TUint32				iVariantExports[1];
		TRomEntry			iExtensionEntry; TUint8 iPad4[32];
		TRomImageHeader		iExtension;
		TRomEntry			iExstartEntry;	TUint8 iPad5[32];
		TRomImageHeader		iExstart;
		TRomEntry			iSecondaryEntry; TUint8 iPad6[32];
		TRomImageHeader		iSecondary;
		TRomRootDirectoryList iRootList;
		TRomDir				iRootDir;		TUint8 iPad3[32];
		};
	const TUint32 recOff = (sizeof(TRomHeader)+15)&~15u;
	if (recOff+sizeof(SRecords) > (TUint32)KPageSize)
		Die("ROM header area too small");
	SRecords& rec = *(SRecords*)(romPhys+recOff);
	const TLinAddr recLin = romLin+recOff;
	#define REC_LIN(f)	(recLin + (TUint32)((TUint8*)&rec.f - (TUint8*)&rec))

	// ROM build time, microseconds since 0 AD: P::InitSystemTime falls back to it on a cold start and
	// panics (PLAT 0) if it is not a valid date. 730497 days to 2000-01-01 (stimer.cpp) + 9759 to 2026-09-20.
	rh.iTime = (TInt64)(730497+9759) * 86400 * 1000000;
	rh.iRomBase = romLin;
	rh.iRomSize = romSize;
	rh.iUncompressedSize = romSize;
	rh.iRomHeaderSize = sizeof(TRomHeader);
	rh.iKernDataAddress = kernData;
	rh.iKernelLimit = (kernData + svRounded + KInitStackSize + KHeapSizeMax + 0xFFFFFu) & ~0xFFFFFu;	// 1 MiB-rounded
	rh.iTotalSvDataSize = svData;
	rh.iPrimaryFile = REC_LIN(iPrimaryEntry);
	rh.iVariantFile = REC_LIN(iVariantEntry);
	rh.iExtensionFile = REC_LIN(iExtensionEntry);
	rh.iUserDataAddress = KUserLocalDataEnd;					// flexible model: XIP user .data sits BELOW this (maddressspace.cpp)
	rh.iTraceMask[0] = XBOOT_TRACE_MASK;							// make_ekern.sh TRACE= (debug kernels only)
	rh.iKernelConfigFlags = 0;

	TRomImageHeader& k = rec.iPrimary;
	k.iUid1 = 0x1000007Au;  k.iUid2 = 0x1000008Bu;  k.iUid3 = 0x10287037u;	// EXE / kernel / SMP kernel
	k.iEntryPoint = SYMVAL(_E32Startup);
	k.iCodeAddress = SYMVAL(__code_base);
	k.iCodeSize = k.iTextSize = SYMVAL(__code_end)-SYMVAL(__code_base);
	k.iDataAddress = SYMVAL(__data_load);
	k.iDataSize = SYMVAL(__data_size);
	k.iBssSize = svData - SYMVAL(__data_size);
	k.iTotalDataSize = svData;
	k.iDataBssLinearBase = kernData;
	k.iHeapSizeMin = heap;  k.iHeapSizeMax = KHeapSizeMax;  k.iStackSize = KInitStackSize;
	k.iFlags = KRomImageFlagPrimary;
	k.iPriority = EPrioritySupervisor;
	k.iS.iCaps[0] = 0xFFFFFFFFu;  k.iS.iCaps[1] = 0xFFFFFFFFu;
	k.iHardwareVariant = 0x01000000u;							// top byte <= 3: compatible with anything
	rec.iPrimaryEntry.iSize = romSize;
	rec.iPrimaryEntry.iAddressLin = REC_LIN(iPrimary);
	rec.iPrimaryEntry.iNameLength = 5;
	{ const char* n="ekern"; for (TInt i=0;i<5;++i) rec.iPrimaryEntry.iName[i<<1]=(TUint8)n[i]; }

	TRomImageHeader& v = rec.iVariant;							// the variant is linked INTO the kernel image
	v.iUid1 = 0x10000079u;										// DLL
	v.iEntryPoint = SYMVAL(X64VariantEntry);
	v.iExportDirCount = 1;
	v.iExportDir = REC_LIN(iVariantExports);
	rec.iVariantExports[0] = SYMVAL(X64VariantInitialise);
	v.iFlags = KRomImageFlagVariant;
	v.iS.iCaps[0] = 0xFFFFFFFFu;  v.iS.iCaps[1] = 0xFFFFFFFFu;
	v.iHardwareVariant = 0x01000000u;
	rec.iVariantEntry.iAddressLin = REC_LIN(iVariant);
	rec.iVariantEntry.iNameLength = 5;
	{ const char* n="ecust"; for (TInt i=0;i<5;++i) rec.iVariantEntry.iName[i<<1]=(TUint8)n[i]; }

	// At least ONE kernel extension must exist: P::StartExtensions does new RArray(count) and a
	// zero granularity panics KERN-COMMON 127. A static no-op for now; this slot becomes exstart.
	TRomImageHeader& x = rec.iExtension;
	x.iUid1 = 0x10000079u;										// DLL
	x.iEntryPoint = SYMVAL(X64ExtensionEntry);
	x.iFlags = KRomImageFlagExtension;
	x.iS.iCaps[0] = 0xFFFFFFFFu;  x.iS.iCaps[1] = 0xFFFFFFFFu;
	x.iHardwareVariant = 0x01000000u;
	rec.iExtensionEntry.iAddressLin = REC_LIN(iExtension);
	rec.iExtensionEntry.iNameLength = 4;
	{ const char* n="xext"; for (TInt i=0;i<4;++i) rec.iExtensionEntry.iName[i<<1]=(TUint8)n[i]; }
	x.iNextExtension = REC_LIN(iExstartEntry);

	// Second extension = the REAL exstart (baseline kernel/exstart.cpp, linked into the image): it
	// creates + resumes the ROM's secondary process from a DFC on the supervisor message queue.
	TRomImageHeader& xs = rec.iExstart;
	xs.iUid1 = 0x10000079u;
	xs.iEntryPoint = SYMVAL(X64ExstartShim);
	xs.iFlags = KRomImageFlagExtension;
	xs.iS.iCaps[0] = 0xFFFFFFFFu;  xs.iS.iCaps[1] = 0xFFFFFFFFu;
	xs.iHardwareVariant = 0x01000000u;
	rec.iExstartEntry.iAddressLin = REC_LIN(iExstart);
	rec.iExstartEntry.iNameLength = 7;
	{ const char* n="exstart"; for (TInt i=0;i<7;++i) rec.iExstartEntry.iName[i<<1]=(TUint8)n[i]; }

	// ---- the secondary (efile.exe's slot). Three sources, in descending order of realness:
	//
	// 1. A PRELINKED ROM SECTION ('PRSC', tools/rombuild.py --section): real TRomEntry +
	//    TRomImageHeader records for each E32Image, laid out for this linear base with
	//    relocations applied and every import slot already holding its resolved export
	//    address. There is nothing to fabricate -- iSecondaryFile points straight INTO the
	//    section, and the kernel reads records a ROM builder wrote, not ones we invent here.
	//    A v2 section also carries a REAL DIRECTORY TREE (TRomDir blocks + the
	//    TRomRootDirectoryList), so Z: has files in it: the fileserver's CRom mounts from
	//    TRomHeader.iRomRootDirectoryList / SSuperPageBase.iRootDirList and binary-searches
	//    those blocks. Without one the ROM is a valid but EMPTY volume.
	//    { magic, version, base, size, count, rootDirList, secondaryEntry, romDllDataSize }
	//    v3 (X4.27): word 7 = the ROM DLL static data the section writer placed BELOW
	//    KUserLocalDataEnd -> iTotalUserDataSize, which the flexible model reserves in every process.
	// 2. The flat statically-linked euser EXE ('XUEX', uexe/) -- one image, no ordinals.
	// 3. minisec, the hand-rolled ring-3 asm program (xk_minisec.cpp). No euser at all.
	TLinAddr rootDirList = REC_LIN(iRootList);					// fabricated empty root (2. and 3.)
	const TUint32* uh = (const TUint32*)(romPhys + (SYMVAL(__uimage_base) - romLin));
	if (uh[0] == 0x43535250u)									// 'PRSC'
		{
		if (uh[1] != 4)
			Die("ROM section version is not 4 -- rebuild it with tools/make_phorepoc_rom.sh");
		if (uh[2] != SYMVAL(__uimage_base))
			Die("ROM section was laid out for a different linear base");
		rh.iSecondaryFile = uh[6];
		rootDirList = uh[5];
		rh.iTotalUserDataSize = uh[7];
		Trace("ROM DLL static data (below KUserLocalDataEnd)", uh[7]);
		Trace("ROM section images", uh[4]);
		Trace("ROM root directory list", uh[5]);
		Trace("secondary TRomEntry", uh[6]);
		// v4 (X4.48): built-in device drivers. The section carries real TRomImageHeader records for
		// Sys\Bin\<x>.ldd/.pdd (KRomImageFlagDevice, one export) so F32's loader finds them and
		// ExecHandler::DeviceLoad calls export 1; the drivers themselves are linked into this image,
		// so their addresses are filled in here, as for the variant record above.
		if (uh[8])
			{
			const TUint32* dt = (const TUint32*)(romPhys + (uh[8] - romLin));
			for (TUint32 i = 0; i < dt[0]; ++i)
				{
				TRomImageHeader& d = *(TRomImageHeader*)(romPhys + (dt[1 + 2*i] - romLin));
				TUint32 f = 0;
				switch (dt[2 + 2*i])
					{
					case 1: f = SYMVAL(X64CreateLdd_enet); break;		// Sys\Bin\enet.ldd
					case 2: f = SYMVAL(X64CreatePdd_e1000); break;		// Sys\Bin\ethernet.pdd
					case 3: f = SYMVAL(X64CreateLdd_soundsc); break;	// Sys\Bin\esoundsc.ldd (X4.88)
					case 4: f = SYMVAL(X64CreatePdd_ac97); break;		// Sys\Bin\soundsc.pdd
					case 5: f = SYMVAL(X64CreateLdd_camerasc); break;	// Sys\Bin\ecamerasc.ldd (camera C1)
					case 6: f = SYMVAL(X64CreatePdd_camerasc); break;	// Sys\Bin\camerasc.pdd
					default: Die("ROM section: unknown built-in device id");
					}
				d.iEntryPoint = SYMVAL(X64ExtensionEntry);
				*(TUint32*)(romPhys + (d.iExportDir - romLin)) = f;
				}
			Trace("built-in device drivers", dt[0]);
			}
		Put("[xboot] secondary = prelinked ROM section (real TRomImageHeader records + Z: tree)\n");
		}
	else
		{
		TRomImageHeader& u = rec.iSecondary;
		u.iUid1 = 0x1000007Au;									// EXE
		u.iEntryPoint = SYMVAL(X64MinisecEntry);
		u.iCodeAddress = SYMVAL(__utext_base);
		u.iCodeSize = u.iTextSize = SYMVAL(__utext_end)-SYMVAL(__utext_base);
		u.iDataBssLinearBase = KX64ExeDataBase;
		u.iHeapSizeMin = 0x1000;  u.iHeapSizeMax = 0x100000;  u.iStackSize = 0x2000;
		if (uh[0] == 0x58455558u)								// 'XUEX'
			{
			u.iEntryPoint = uh[1];
			u.iCodeAddress = uh[7];
			u.iCodeSize = u.iTextSize = uh[2];
			u.iDataAddress = uh[3];
			u.iDataSize = uh[4];
			u.iBssSize = uh[5];
			u.iTotalDataSize = uh[4]+uh[5];
			u.iDataBssLinearBase = uh[6];
			u.iHeapSizeMin = 0x1000;  u.iHeapSizeMax = 0x100000;  u.iStackSize = 0x4000;
			Trace("secondary = euser EXE, entry", u.iEntryPoint);
			}
		else
			Trace("secondary = minisec, entry", u.iEntryPoint);
		u.iPriority = EPriorityHigh;
		u.iS.iCaps[0] = 0xFFFFFFFFu;  u.iS.iCaps[1] = 0xFFFFFFFFu;
		u.iHardwareVariant = 0x01000000u;
		rec.iSecondaryEntry.iSize = u.iCodeSize;
		rec.iSecondaryEntry.iAddressLin = REC_LIN(iSecondary);
		rec.iSecondaryEntry.iNameLength = 9;
		{ const char* n="efile.exe"; for (TInt i=0;i<9;++i) rec.iSecondaryEntry.iName[i<<1]=(TUint8)n[i]; }
		rh.iSecondaryFile = REC_LIN(iSecondaryEntry);
		}

	rec.iRootList.iNumRootDirs = 1;
	rec.iRootList.iRootDir[0].iHardwareVariant = 0x01000000u;
	rec.iRootList.iRootDir[0].iAddressLin = REC_LIN(iRootDir);
	rec.iRootDir.iSize = 0;			// an empty directory: iSize counts the TRomEntry region only,
									// so 0 entries -> 0, and the zeroed pad reads as a 0/0 sort info
	rh.iRomRootDirectoryList = rootDirList;						// the section's tree, or the above

	// ---- super page
	SSuperPageBase& sp = *(SSuperPageBase*)superPhys;
	sp.iTotalRomSize = romSize;
	sp.iTotalRamSize = BankEnd-BankBase;
	sp.iPageDir = KPageDirectoryBase;
	sp.iRamBootData = KSuperPageLinAddr+KRamTableOffset;
	sp.iRomBootData = KSuperPageLinAddr+KRomTableOffset;
	sp.iBootFlags = KBootFlagRunFromRAM;
	sp.iMachineData = KMachineConfigLinAddr;
	sp.iActiveVariant = 0x01000000u;
	sp.iPrimaryEntry = REC_LIN(iPrimaryEntry);
	sp.iInitialHeapSize = heap;
	sp.iDebugPort = 0;
	sp.iRootDirList = rootDirList;			// FindRomRootDirectory prefers this over the ROM header's
	sp.iKernelLimit = rh.iKernelLimit;
	sp.iRomHeaderPhys = romPhys;
	{ TUint32 a=1,b,c,d; __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d)); sp.iCpuId = d; }
	sp.iReserved[0] = (TInt)rootPhys;							// PHOREPOC: per-CPU PML4/PDPT area, physical
	sp.iReserved[1] = (TInt)KX64RootLin;						//           ... and linear
	FramebufferFromMultiboot(aMultibootInfo, sp.iReserved);	// [16..20], xk_board.h (X4.18)
	Trace("framebuffer phys", (TUint32)sp.iReserved[KX64SuperFbPhys]);
	{
	TUint8 ids[KX64MaxCpus];
	TUint8* isa = (TUint8*)&sp.iReserved[KX64SuperIsaMap];	// 16 bytes = [21..24], 0 = identity
	for (TUint i=0; i<16; ++i)
		isa[i] = 0;
	TUint n = FindCpus(aMultibootInfo, ids, KX64MaxCpus, isa);
	Trace("ISA irq 0 override (0 = none)", isa[0]);
	sp.iReserved[2] = (TInt)n;									// x64_root.h KX64SuperCpuCount / ApicId
	for (TUint i=0; i<n; ++i)
		sp.iReserved[3+i] = ids[i];
	Trace("CPUs reported by the ACPI MADT", n);
	}
	FindPowerOff(aMultibootInfo, sp.iReserved);				// [14..15], xk_board.h (X4.74)
	Trace("ACPI S5 PM1a (port | SLP_TYP<<16)", (TUint32)sp.iReserved[KX64SuperPm1a]);
	Trace("ACPI PM1 event block (port | len<<16)", (TUint32)sp.iReserved[KX64SuperPm1Evt]);
	Trace("ACPI SCI (irq | ACPI_ENABLE<<16)", (TUint32)sp.iReserved[KX64SuperSci]);

	// ---- CPU 0's long-mode root: PML4[0] -> PDPT; PDPT[0..3] -> the four PD pages. A temporary
	// 4 MiB identity window (two 2 MiB pages in the kernel's LOCAL half, below
	// KUserLocalDataBase) keeps this code + the boot GDT/stack alive across the switch; the
	// kernel entry moves off them at once.
	// (Every CPU's root starts out as the KERNEL's address space; an AP loads its own at start-up.)
	for (TUint cpu=0; cpu<KX64MaxCpus; ++cpu)
		{
		TEntry* pml4 = (TEntry*)(rootPhys + cpu*2*KPageSize);
		TEntry* pdpt = (TEntry*)(rootPhys + cpu*2*KPageSize + KPageSize);
		pml4[0] = (TEntry)(TUint32)pdpt | KPermPde;
		for (TUint i=0; i<4; ++i)
			pdpt[i] = ((TEntry)(TUint32)KernPd + i*KPageSize) | KPermPde;
		}
	KernPd[0] = 0x000000u | KPermKernel | KPdeLargePage;
	KernPd[1] = 0x200000u | KPermKernel | KPdeLargePage;

	Trace("page tables used", PtCount); Trace("RAM used by boot", AllocNext-BankBase);
	Trace("kernel entry", k.iEntryPoint); Trace("super page phys", superPhys);
	Put("[xboot] switching to the kernel address space, calling _E32Startup(ROM header, super page)\n");

	// CR4.PGE on (global pages) before the kernel asks for them; then CR3, stack, call.
	__asm__ __volatile__(
		"mov rax, cr4\n" "or rax, 0x80\n" "mov cr4, rax\n"
		"mov cr3, rcx\n"
		"mov esp, edx\n"
		"xor ebp, ebp\n"
		"call rbx\n"
		: : "c"((TUint64)rootPhys), "d"((TUint64)initSp), "b"((TUint64)k.iEntryPoint),
			"D"((TUint64)romLin), "S"((TUint64)KSuperPageLinAddr) : "memory");
	Die("kernel entry returned");
	}
