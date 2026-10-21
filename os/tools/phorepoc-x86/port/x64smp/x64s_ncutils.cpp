// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

/* x64s_ncutils.cpp -- Phorepoc amd64 SMP arch backend: the low-level, per-CPU
 * NKern CPU primitives the arch-neutral nkernSMP core references. The SMP
 * analogue of nkernsmp/x86/ncutils.cpp + ncutilf.cpp. Pure CPU ops + the
 * per-CPU lookup (APIC ID -> TSubScheduler); the scheduler-entangled entry
 * points (Lock/Unlock/Reschedule) live in x64s_ncsched.cpp.
 */
#include <e32def.h>
#include <nkern.h>
#include <nk_priv.h>

#define X64_RFLAGS_IF  0x200

/* LAPIC MMIO register accessor. The base 0xFEE00000 is ABOVE the 2 GiB small-code-
 * model limit, so a plain absolute deref of the constant compiles to a broken
 * addressing form that reads the wrong location (returns 0). Force the address
 * through a register with an opaque asm so GCC materialises the full 64-bit value
 * (movabs) instead of folding it into a small-model absolute memory operand. */
static volatile TUint8* gLapic = (volatile TUint8*)0xFEE00000ULL;   // byte base

/* LAPIC registers MUST be accessed as 32-bit MMIO. A plain C deref of a TUint32*
 * can compile to a 64-bit load here (which QEMU's LAPIC returns 0 for), so force an
 * explicit movl. */
static inline TUint32 lapic_rd(TUint32 aOff)
    {
    volatile TUint8* p = gLapic + aOff;
    TUint32 v;
    __asm__ __volatile__("movl (%1), %k0" : "=r"(v) : "r"(p) : "memory");
    return v;
    }

/* Read the local APIC ID (LAPIC register 0x20, bits 31:24). */
static inline TUint32 lapic_id()
    {
    return lapic_rd(0x20) >> 24;
    }

/* rdtsc as the timestamp source. */
static inline TUint64 rdtsc()
    {
    TUint32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((TUint64)hi << 32) | lo;
    }

extern "C" {

/* mem ops normally supplied by common/x86 .cia */
// weak: a nanokernel-only image uses this; the full kernel (ekern) supplies the real one
__attribute__((weak)) EXPORT_C TAny* memclr(TAny* aTrg, unsigned int aLength)
    {
    void* d = aTrg;
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(aLength) : "a"(0) : "memory");
    return aTrg;
    }

// weak: a nanokernel-only image uses this; the full kernel (ekern) supplies the real one
__attribute__((weak)) EXPORT_C TAny* memset(TAny* aTrg, TInt aValue, unsigned int aLength)
    {
    void* d = aTrg;
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(aLength) : "a"(aValue) : "memory");
    return aTrg;
    }

} // extern "C"

/* --- NKern interrupt control (token = saved IF) ----------------------------- */
EXPORT_C TInt NKern::DisableAllInterrupts()
    {
    TUint64 flags;
    __asm__ __volatile__("pushfq\n\t pop %0\n\t cli" : "=r"(flags) : : "memory", "cc");
    return (TInt)(flags & X64_RFLAGS_IF);
    }

EXPORT_C void NKern::EnableAllInterrupts()
    {
    __asm__ __volatile__("sti" : : : "memory", "cc");
    }

EXPORT_C void NKern::RestoreInterrupts(TInt aRestoreData)
    {
    if (aRestoreData & X64_RFLAGS_IF)
        __asm__ __volatile__("sti" : : : "memory", "cc");
    else
        __asm__ __volatile__("cli" : : : "memory", "cc");
    }

/* --- Per-CPU identity ------------------------------------------------------- */
/* Which CPU is this? The IA-32 port read it from the TSS selector (STR); this one read the local
 * APIC ID register -- an MMIO read that a hypervisor without a virtualised APIC must EMULATE: under
 * VirtualBox on Hyper-V (NEM) one read measured ~27700 cycles against 173 for STR (X4.23), and the
 * nanokernel asks on nearly every call. So: once the kernel image gives each CPU its own TSS
 * (xk_entry.cpp: selector X64_TssSelBase + 16*cpu), STR answers; before that (early boot, an AP
 * before its LTR: TR = 0), and in the nanokernel demo, the local APIC does. */
TUint16 X64_TssSelBase;

static inline TSubScheduler* SubSchedulerFromTr()
    {
    TUint16 sel;
    __asm__ __volatile__("str %0" : "=r"(sel));
    TUint16 base = X64_TssSelBase;
    if (!base || sel < base)
        return 0;
    return &TheSubSchedulers[(sel - base) >> 4];
    }

/* The current CPU number. */
EXPORT_C TInt NKern::CurrentCpu()
    {
    TSubScheduler* ss = SubSchedulerFromTr();
    if (!ss)
        ss = SubSchedulerLookupTable[lapic_id() & 0xff];
    return ss ? (TInt)ss->iCpuNum : 0;
    }

/* Current subscheduler for this CPU (used by CurrentThread etc.). */
TSubScheduler& SubScheduler()
    {
    TSubScheduler* ss = SubSchedulerFromTr();
    return ss ? *ss : *SubSchedulerLookupTable[lapic_id() & 0xff];
    }

/* --- Timestamp / frequencies ------------------------------------------------ */
EXPORT_C TUint64 NKern::Timestamp()
    {
    return rdtsc();
    }

extern "C" TUint64 ktsc_hz();   /* kmain_smp.c -- PIT-calibrated TSC frequency (0 until set) */

EXPORT_C TUint32 NKern::TimestampFrequency()
    {
    /* The real, PIT-calibrated TSC frequency (NKern::Timestamp() is rdtsc()), so the
     * scheduler's per-thread CPU-time accounting reads in true wall-clock units. Falls
     * back to a nominal 1 GHz if queried before the BSP calibration has run. */
    TUint64 hz = ktsc_hz();
    return hz ? (TUint32)hz : 1000000000u;
    }

EXPORT_C TUint32 NKern::CpuTimeMeasFreq()
    {
    return NKern::TimestampFrequency();
    }

/* --- Idle ------------------------------------------------------------------- */
EXPORT_C void NKern::DoIdle()
    {
    __asm__ __volatile__("sti; hlt" : : : "memory");
    }
