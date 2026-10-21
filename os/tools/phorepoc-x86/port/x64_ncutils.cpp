// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

/* x64_ncutils.cpp -- Phorepoc amd64 arch backend: the low-level NKern CPU
 * primitives the arch-neutral nkern core references but that are inherently
 * machine-specific. The amd64 analogue of nkern/x86/ncutils.cpp + the ARM
 * ncutils.cia. Pure, self-contained CPU ops only; the scheduler-entangled
 * NKern::Lock/Unlock/PreemptionPoint + the context switch (Reschedule) live with
 * ncsched (authored next), since they manipulate TheScheduler + reschedule.
 */
#include <e32def.h>
#include <nkern.h>

/* IF = interrupt-enable flag, RFLAGS bit 9. */
#define X64_RFLAGS_IF  0x200

extern "C" {

/* memclr(dst, len): zero len bytes at dst, return dst. Kernel's own bzero;
 * normally supplied by common/x86 as a .cia. rep stosb is correct and adequate. */
EXPORT_C TAny* memclr(TAny* aTrg, unsigned int aLength)
    {
    void* d = aTrg;
    __asm__ __volatile__("rep stosb"
        : "+D"(d), "+c"(aLength)
        : "a"(0)
        : "memory");
    return aTrg;
    }

} // extern "C"

/* --- NKern interrupt control ------------------------------------------------
 * DisableAllInterrupts() returns an opaque token capturing the prior interrupt
 * state; RestoreInterrupts() puts it back. On amd64 the token is the saved
 * RFLAGS (only IF matters here). These are C++-linkage statics (NKern::) to
 * match the neutral core's references. */

EXPORT_C TInt NKern::DisableAllInterrupts()
    {
    TUint64 flags;
    __asm__ __volatile__("pushfq\n\t"
                         "pop %0\n\t"
                         "cli"
        : "=r"(flags) : : "memory", "cc");
    return (TInt)(flags & X64_RFLAGS_IF);   // low bits carry IF; enough to restore
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
