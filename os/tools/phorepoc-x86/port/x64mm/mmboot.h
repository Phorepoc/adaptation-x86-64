// Copyright (c) 1998-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the License "Eclipse Public License v1.0"
// which accompanies this distribution, and is available
// at the URL "http://www.eclipse.org/legal/epl-v10.html".
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
//
// Description:
//

/**
 @file
 @publishedPartner
 @prototype
*/

// PHOREPOC amd64 (x32 ABI) sibling of include/memmodel/epoc/flexible/x86/mmboot.h -- selected by
// include order (-I port/x64mm first). SAME 32-bit linear map as IA-32 (x32 = 4 GiB of VA); only the
// page-table GEOMETRY changes: long mode walks 8-byte entries, 512 per table. Entries stay
// "32-bit physical address | IA-32 flag bits" in the low half (the flag layout is identical in long
// mode); the high half (NX, physical > 4 GiB) is kept zero for now.
#ifndef __MMBOOT_H__
#define __MMBOOT_H__

// Linear address map
// 00000000-5FFFFFFF	Unmapped
// 60000000-7FFFFFFF	Local data		(PHOREPOC: 0x00400000 on IA-32 -- see below)
// 80000000-			ROM
//         -8FFFFFFF	Global user area
// A0000000-EFFFFFFF	Kernel memory  (PHOREPOC: was 90000000; 80000000-9FFFFFFF = global memory, the ROM first: 512 MB)
// F0000000-FEBFFFFF	Fixed kernel mappings
// FEC00000-FEFFFFFF	IO APIC + Local APIC
// FF000000-FFFFFFFF	???

// Linear addresses

// PHOREPOC amd64: on x86-64 the distance between an XIP image's CODE and its own writable statics
// has to be ENCODABLE. g++ reaches a static with a %rip-relative displacement -- a signed 32-bit
// DISTANCE, not an address -- and no flag stops it (-fno-pic does not; -mcmodel=medium/large are
// rejected in x32 mode). User XIP code lives in the ROM at 0x80000000+, so a process's static data
// at IA-32's KUserLocalDataBase (0x00400000) would be 2 GiB away EXACTLY at the boundary. ARM never
// had this: it reaches a static through an absolute word in a literal pool, and the real x86 port
// was IA-32, where %rip does not exist.
// X4.14-X4.33 moved the WHOLE local range up to 0x60000000 for it, which capped every process at
// 0.5 GiB of heaps, stacks and chunks (X4.34: with 512 MB of RAM the RAM drive's chunk alone left
// efile.exe no room for a thread heap -> Fserv fault 19). Only the statics need to be near the
// ROM, so since X4.34 they are PINNED there instead: an EXE's .data/.bss is mapped at exactly
// KX64ExeDataBase (port/x64mm/mprocess.cpp, EMappingCreateExactVirtual), ROM DLLs' data stays at
// the top of the range (maddressspace.cpp), and KUserLocalDataBase is the stock value again --
// ~2 GiB of local address space per process. tools/rombuild.py (--exe-data-base) must agree, and
// it still REFUSES a displacement that does not fit, so a future map change cannot go quiet.
// KUserLocalDataEnd is unchanged -- maddressspace.cpp asserts it equals iUserDataAddress.
const TLinAddr KUserLocalDataBase		=0x00400000u;
const TLinAddr KX64ExeDataBase			=0x70000000u;	// PHOREPOC x64: every EXE's statics (X4.34)
const TUint KX64ExeDataMax				=0x08000000u;	// PHOREPOC x64: that window, kept out of the common allocator (X4.36)

const TLinAddr KUserLocalDataEnd		=0x80000000u;

/** Everything above here is global (visible to all processes).*/
const TLinAddr KGlobalMemoryBase		=0x80000000u;

const TLinAddr KRomLinearBase			=0x80000000u;

/** Everything below here has user access permissions, everything above is supervisor only. */
const TLinAddr KUserMemoryLimit			=0xA0000000u;	// PHOREPOC x64: was 0x90000000 -- the ROM window (0x80000000..) is now 512 MB, so a ROM with both shells + WebKit (270 MB) fits

const TLinAddr KKernelSectionBase		=0xA0000000u;	// PHOREPOC x64: was 0x90000000 (kernel heaps/stacks/chunks: 0xA0000000..0xC6000000 and 0xC8000000..0xEFFFFFFF)

const TLinAddr KPrimaryIOBase			=0xC6000000u; // XXX This magic constant is hard coded into baseports!
const TLinAddr KPrimaryIOEnd			=0xC8000000u;

const TLinAddr KKernelDataBase			=0xC8000000u;

const TLinAddr KKernelSectionEnd		=0xF0000000u;

const TLinAddr KSuperPageLinAddr		=0xF0000000u;
const TLinAddr KMachineConfigLinAddr	=0xF0000800u;

const TLinAddr KPageInfoMap				=0xF0010000u;
const TLinAddr KApTrampolinePageLin		=0xF0040000u;
const TLinAddr KTempAddr				=0xF0080000u;
const TLinAddr KTempAddrEnd				=0xF0100000u;
const TLinAddr KBiosInfoLin				=0xF0100000u;	// RAM info + MP info

const TLinAddr KPageTableInfoBase		=0xF0C00000u;
const TLinAddr KPageTableInfoEnd		=0xF1000000u;

const TLinAddr KPageArraySegmentBase	=0xF1000000u;
const TLinAddr KPageArraySegmentEnd		=0xF2000000u;

const TLinAddr KPageInfoLinearBase		=0xF2000000u;
const TLinAddr KPageInfoLinearEnd		=0xF4000000u;

const TLinAddr KPageDirectoryBase		=0xF4000000u;
const TLinAddr KPageDirectoryEnd		=0xF5000000u;

const TLinAddr KIPCAlias				=0xF6000000u;
const TLinAddr KIPCAliasAreaSize		=0x02000000u;

const TLinAddr KPageTableBase			=0xF8000000u;
const TLinAddr KPageTableEnd			=0xFEC00000u;

										// FEC00000-FEFFFFFF	IO APIC + Local APIC



// Constants for X86 MMU
#ifndef __KPAGESIZE_DEFINED__
const TInt KPageShift=12;
const TInt KPageSize=1<<KPageShift;
const TInt KPageMask=KPageSize-1;
#define __KPAGESIZE_DEFINED__
#endif
const TInt KChunkShift=21;							// PHOREPOC x64: one long-mode page table (512 x 8-byte PTEs) maps 2 MiB
const TInt KChunkSize=1<<KChunkShift;
const TInt KChunkMask=KChunkSize-1;
const TInt KPageTableShift=KChunkShift-KPageShift+3;	// PHOREPOC x64: PTE is 8 bytes -> a page table is exactly one page
const TInt KPageTableSize=1<<KPageTableShift;
const TInt KPageTableMask=KPageTableSize-1;
const TInt KPtClusterShift=KPageShift-KPageTableShift;
const TInt KPtClusterSize=1<<KPtClusterShift;
const TInt KPtClusterMask=KPtClusterSize-1;
const TInt KPtBlockShift=KPageShift-4;					/**< @internalTechnology */	// sizeof(SPageTableInfo)=16
const TInt KPtBlockSize=1<<KPtBlockShift;				/**< @internalTechnology */
const TInt KPtBlockMask=KPtBlockSize-1;					/**< @internalTechnology */
const TInt KPagesInPDEShift=KChunkShift-KPageShift;
const TInt KPagesInPDE=1<<KPagesInPDEShift;
const TInt KPagesInPDEMask=KPagesInPDE-1;

const TInt KPageDirectoryShift=32-KChunkShift+3;		// PHOREPOC x64: PDE is 8 bytes -> 4 GiB = 2048 PDEs = the 4 PD pages (16 KiB, like ARM)
const TInt KPageDirectorySize=1<<KPageDirectoryShift;
const TInt KPageDirectoryMask=KPageDirectorySize-1;

const TInt KNumOsAsids=1024;
const TUint KKernelOsAsid=0;

const TUint32 KPdePtePresent=0x01;
const TUint32 KPdePteWrite=0x02;
const TUint32 KPdePteUser=0x04;
const TUint32 KPdePteWriteThrough=0x08;
const TUint32 KPdePteUncached=0x10;
const TUint32 KPdePteAccessed=0x20;
const TUint32 KPdePteDirty=0x40;
const TUint32 KPdeLargePage=0x80;						// Pentium and above, not 486
const TUint32 KPdePteGlobal=0x100;						// P6 and above, not 486 or Pentium

const TUint32 KPdePtePhysAddrMask=0xfffff000u;
const TUint32 KPdeLargePagePhysAddrMask=0xffe00000u;	// PHOREPOC x64: large page = 2 MiB	// Pentium and above, not 486
const TUint32 KPdeUnallocatedEntry=0;
const TUint32 KPteUnallocatedEntry=0;
const TUint32 KPdeMatchMask=KPdePteAccessed|KPdePteDirty|KPdePteUser|KPdePteWrite; // ignore 'user' and 'write' in PDEs
const TUint32 KPteMatchMask=KPdePteAccessed|KPdePteDirty;

const TInt KPageInfoShift = 5;

const TInt KPageColourShift=0;
const TInt KPageColourCount=(1<<KPageColourShift);
const TInt KPageColourMask=KPageColourCount-1;

#endif	// __MMBOOT_H__
