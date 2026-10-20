// tools/phorepoc-x86/port/x64smp/ncern.h
//
// PHOREPOC amd64 SMP nanokernel arch header -- the 64-bit sibling of EKA2's
// e32/include/nkernsmp/x86/ncern.h. Authored, not RE'd. Included by the neutral
// nkernsmp <nkern.h> tail. Selected via -I tools/phorepoc-x86/port/x64smp so the
// baseline tree stays unmodified.
//
// Content is arch-neutral in shape (boot-info + variant-interface blocks); the
// real 64-bit divergence lives in nk_plat.h. SX86APBootInfo is renamed to the
// amd64 spelling but kept structurally identical (an empty extension of the
// neutral SAPBootInfo).

/**
 @file
 @internalComponent
*/

#ifndef __NCERN_X64_H__
#define __NCERN_X64_H__

/** Information needed to boot an AP (amd64 specific) */
struct SX64APBootInfo : public SAPBootInfo
	{
	};

// kernel/x86/cinit.cpp (unmodified baseline) names the IA-32 spelling.
typedef SX64APBootInfo SX86APBootInfo;

/** Variant interface block */
struct SVariantInterfaceBlock : public SInterfaceBlockBase
	{
	TUint64		iMaxCpuClock;				// maximum possible CPU clock frequency on this system
	TUint32		iTimestampFreq;				// rate at which timestamp increments
	TUint32		iMaxTimerClock;				// maximum possible local timer clock frequency
	SRatio*		iTimerFreqR[KMaxCpus];		// timer[i] frequency as a fraction of iMaxTimerClock
	SRatio*		iCpuFreqR[KMaxCpus];		// CPU[i] frequency as a fraction of iMaxCpuClock
	SRatio*		iTimestampFreqR;			// timestamp counter frequency as a fraction of iMaxCpuClock
	};

#endif
