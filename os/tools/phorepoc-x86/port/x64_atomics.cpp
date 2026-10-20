/* x64_atomics.cpp -- Phorepoc amd64 arch backend: the e32atomics primitives
 * the arch-neutral nkern core actually references, implemented in x86_64.
 *
 * This is the NEW per-arch backend behind EKA2's existing nkern arch boundary
 * (the amd64 analogue of common/{arm,x86}/atomics.cia). Grown on demand: we add
 * a primitive here the first time a neutral TU needs it. Starting with the one
 * nklib.cpp uses.
 */
#include <e32def.h>

extern "C" {

/* Return the bit number (0..31) of the most significant 1 bit, or -1 if zero.
 * x86_64: BSR sets ZF when the source is 0 (result undefined), so branch on it. */
EXPORT_C TInt __e32_find_ms1_32(TUint32 v)
    {
    TInt r;
    __asm__ ("bsr %1, %0\n\t"
             "jnz 1f\n\t"
             "mov $-1, %0\n\t"
             "1:"
             : "=r"(r)
             : "rm"(v)
             : "cc");
    return r;
    }

/* Least significant 1 bit (0..31), -1 if zero. BSF is the LS analogue of BSR. */
EXPORT_C TInt __e32_find_ls1_32(TUint32 v)
    {
    TInt r;
    __asm__ ("bsf %1, %0\n\t"
             "jnz 1f\n\t"
             "mov $-1, %0\n\t"
             "1:"
             : "=r"(r)
             : "rm"(v)
             : "cc");
    return r;
    }

/* Most significant 1 bit of a 64-bit value (0..63), -1 if zero. On amd64 a
 * single 64-bit BSR does it directly (no hi/lo split like a 32-bit port needs). */
EXPORT_C TInt __e32_find_ms1_64(TUint64 v)
    {
    TInt64 r;
    __asm__ ("bsr %1, %0\n\t"
             "jnz 1f\n\t"
             "mov $-1, %0\n\t"
             "1:"
             : "=r"(r)
             : "rm"(v)
             : "cc");
    return (TInt)r;
    }

/* Atomic exchange, 8-bit, full fence. XCHG has an implicit LOCK on memory. */
EXPORT_C TUint8 __e32_atomic_swp_ord8(volatile TAny* a, TUint8 v)
    {
    __asm__ __volatile__("xchg %0, %1"
             : "+q"(v), "+m"(*(volatile TUint8*)a)
             : : "memory");
    return v;
    }

/* Atomic exchange, 32-bit, full fence. */
EXPORT_C TUint32 __e32_atomic_swp_ord32(volatile TAny* a, TUint32 v)
    {
    __asm__ __volatile__("xchg %0, %1"
             : "+r"(v), "+m"(*(volatile TUint32*)a)
             : : "memory");
    return v;
    }

/* Atomic add, 32-bit, returns the ORIGINAL value, full fence.
 * LOCK XADD writes (dst+src) to memory and returns the old dst in the source reg. */
EXPORT_C TUint32 __e32_atomic_add_ord32(volatile TAny* a, TUint32 v)
    {
    __asm__ __volatile__("lock xadd %0, %1"
             : "+r"(v), "+m"(*(volatile TUint32*)a)
             : : "memory", "cc");
    return v;
    }

} // extern "C"
