// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// atomics_test.cpp -- HOSTED test for the REAL x64smp/x64s_atomics.cpp (compiled unmodified
// against atomics_test_shim/e32def.h). Written for X4.9b, when the xor and tas families were
// added for euser's export table, but it covers the whole e32atomics surface the file provides.
//
// What it checks, per width (8/16/32/64) and per ordering suffix (rlx/acq/rel/ord):
//   * the RETURN VALUE is the ORIGINAL contents, not the new one (the classic way to get these
//     wrong; every caller of add/and/ior/xor/axo/tau/tas depends on it),
//   * the stored result matches a plain non-atomic reference model,
//   * tas uses a SIGNED threshold compare and tau an UNSIGNED one over the SAME bit patterns --
//     the only difference between the two families, and invisible unless the test feeds them
//     values that straddle the sign bit,
//   * the RMW is width-exact: a neighbouring guard word on each side must be untouched, and
//     results must wrap within the width,
//   * cas succeeds/fails and writes back the seen value on failure,
//   * swp/load/store round trips, the bit scans (including the empty-word -1) and popcount.
// Semantics only -- this is a single-threaded oracle test; the concurrency proof is the kernel
// itself running SMP (make_ekern.sh CPUS=4).
//
// Bounded output: one line per section, ending in `ATOMICS OK` or `ATOMICS FAIL n`.

#include <e32def.h>
#include <stdio.h>

extern "C" {
// The real spelling is <op>_<order><bits>, e.g. __e32_atomic_add_rlx8 -- so the ordering suffix
// goes BETWEEN the op and the width.
#define DECL4(ret, op, bits, args) \
    ret __e32_atomic_##op##_rlx##bits args; ret __e32_atomic_##op##_acq##bits args; \
    ret __e32_atomic_##op##_rel##bits args; ret __e32_atomic_##op##_ord##bits args;
#define DECLW(bits, T, S) \
    T __e32_atomic_load_acq##bits(const volatile TAny*); \
    T __e32_atomic_store_rel##bits(volatile TAny*, T); \
    T __e32_atomic_store_ord##bits(volatile TAny*, T); \
    DECL4(T, swp, bits, (volatile TAny*, T)) \
    DECL4(TBool, cas, bits, (volatile TAny*, T*, T)) \
    DECL4(T, add, bits, (volatile TAny*, T)) \
    DECL4(T, and, bits, (volatile TAny*, T)) \
    DECL4(T, ior, bits, (volatile TAny*, T)) \
    DECL4(T, xor, bits, (volatile TAny*, T)) \
    DECL4(T, axo, bits, (volatile TAny*, T, T)) \
    DECL4(T, tau, bits, (volatile TAny*, T, T, T)) \
    DECL4(S, tas, bits, (volatile TAny*, S, S, S))
DECLW(8,  TUint8,  TInt8)
DECLW(16, TUint16, TInt16)
DECLW(32, TUint32, TInt32)
DECLW(64, TUint64, TInt64)
TInt __e32_find_ms1_32(TUint32); TInt __e32_find_ls1_32(TUint32);
TInt __e32_find_ms1_64(TUint64); TInt __e32_find_ls1_64(TUint64);
TInt __e32_bit_count_32(TUint32); TInt __e32_bit_count_64(TUint64);
void __e32_memory_barrier(void); void __e32_io_completion_barrier(void);
}

static int gFail = 0;
static const char* gSection = "";

static void Check(bool ok, const char* what, long long got, long long want)
    {
    if (ok)
        return;
    if (++gFail <= 20)                      // bounded output: first 20 failures only
        printf("  FAIL %s/%s: got %lld want %lld\n", gSection, what, got, want);
    }

// A cell with guard words either side, so a too-wide RMW is caught rather than tolerated.
template <class T> struct TGuarded
    {
    TUint64 iLo; volatile T iV; TUint64 iHi;
    TGuarded(T v) : iLo(0x1122334455667788ULL), iV(v), iHi(0x99aabbccddeeff00ULL) {}
    bool GuardsIntact() const
        { return iLo == 0x1122334455667788ULL && iHi == 0x99aabbccddeeff00ULL; }
    };

// One width's worth of checks. T = unsigned type, S = its signed twin.
#define TESTW(bits, T, S) \
static void Test##bits() \
    { \
    gSection = "u" #bits; \
    static const T kVals[] = { (T)0, (T)1, (T)2, (T)0x7f, (T)0x80, (T)0xff, \
                               (T)(~(T)0 >> 1), (T)((~(T)0 >> 1) + 1), (T)~(T)0 }; \
    const int n = (int)(sizeof(kVals) / sizeof(kVals[0])); \
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) \
        { \
        const T a = kVals[i], b = kVals[j]; \
        /* --- RMW: returns the ORIGINAL, stores the reference result --------------- */ \
        struct { const char* n; T want; T (*fn)(volatile TAny*, T); } rmw[] = { \
            { "add_rlx", (T)(a + b), __e32_atomic_add_rlx##bits }, \
            { "add_ord", (T)(a + b), __e32_atomic_add_ord##bits }, \
            { "and_acq", (T)(a & b), __e32_atomic_and_acq##bits }, \
            { "ior_rel", (T)(a | b), __e32_atomic_ior_rel##bits }, \
            { "xor_rlx", (T)(a ^ b), __e32_atomic_xor_rlx##bits }, \
            { "xor_acq", (T)(a ^ b), __e32_atomic_xor_acq##bits }, \
            { "xor_rel", (T)(a ^ b), __e32_atomic_xor_rel##bits }, \
            { "xor_ord", (T)(a ^ b), __e32_atomic_xor_ord##bits }, \
        }; \
        for (int k = 0; k < (int)(sizeof(rmw) / sizeof(rmw[0])); ++k) \
            { \
            TGuarded<T> c(a); \
            T old = rmw[k].fn((volatile TAny*)&c.iV, b); \
            Check(old == a, rmw[k].n, (long long)old, (long long)a); \
            Check(c.iV == rmw[k].want, rmw[k].n, (long long)c.iV, (long long)rmw[k].want); \
            Check(c.GuardsIntact(), "guard", 0, 0); \
            } \
        /* --- axo: *a = (*a & u) ^ v ---------------------------------------------- */ \
        { TGuarded<T> c(a); T old = __e32_atomic_axo_ord##bits((volatile TAny*)&c.iV, b, (T)~b); \
          Check(old == a, "axo old", (long long)old, (long long)a); \
          Check(c.iV == (T)((a & b) ^ (T)~b), "axo", (long long)c.iV, (long long)(T)((a & b) ^ (T)~b)); \
          Check(c.GuardsIntact(), "guard", 0, 0); } \
        /* --- tau (UNSIGNED threshold) vs tas (SIGNED) over the SAME bits ---------- */ \
        { const T u = (T)3, v = (T)0xfe; \
          TGuarded<T> c(a); T old = __e32_atomic_tau_ord##bits((volatile TAny*)&c.iV, b, u, v); \
          T wantU = (T)(a + (a >= b ? u : v)); \
          Check(old == a, "tau old", (long long)old, (long long)a); \
          Check(c.iV == wantU, "tau", (long long)c.iV, (long long)wantU); \
          TGuarded<T> d(a); \
          S olds = __e32_atomic_tas_ord##bits((volatile TAny*)&d.iV, (S)b, (S)u, (S)v); \
          T wantS = (T)((S)a + ((S)a >= (S)b ? (S)u : (S)v)); \
          Check(olds == (S)a, "tas old", (long long)olds, (long long)(S)a); \
          Check(d.iV == wantS, "tas", (long long)d.iV, (long long)wantS); \
          Check(d.GuardsIntact(), "guard", 0, 0); \
          /* all four tas orderings must agree */ \
          TGuarded<T> e(a); __e32_atomic_tas_rlx##bits((volatile TAny*)&e.iV, (S)b, (S)u, (S)v); \
          TGuarded<T> f(a); __e32_atomic_tas_acq##bits((volatile TAny*)&f.iV, (S)b, (S)u, (S)v); \
          TGuarded<T> g(a); __e32_atomic_tas_rel##bits((volatile TAny*)&g.iV, (S)b, (S)u, (S)v); \
          Check(e.iV == wantS && f.iV == wantS && g.iV == wantS, "tas suffixes", \
                (long long)e.iV, (long long)wantS); } \
        /* --- swp / load / store / cas -------------------------------------------- */ \
        { TGuarded<T> c(a); T old = __e32_atomic_swp_ord##bits((volatile TAny*)&c.iV, b); \
          Check(old == a && c.iV == b, "swp", (long long)c.iV, (long long)b); \
          Check(__e32_atomic_load_acq##bits((const volatile TAny*)&c.iV) == b, "load", 0, 0); \
          __e32_atomic_store_ord##bits((volatile TAny*)&c.iV, a); \
          Check(c.iV == a, "store", (long long)c.iV, (long long)a); \
          Check(c.GuardsIntact(), "guard", 0, 0); } \
        { TGuarded<T> c(a); T exp = a; \
          Check(__e32_atomic_cas_ord##bits((volatile TAny*)&c.iV, &exp, b) != 0, "cas hit", 0, 1); \
          Check(c.iV == b, "cas stored", (long long)c.iV, (long long)b); \
          T wrong = (T)(a ^ (T)1); exp = wrong; TGuarded<T> d(a); \
          TBool r = __e32_atomic_cas_ord##bits((volatile TAny*)&d.iV, &exp, b); \
          if (wrong != a) \
              { Check(r == 0, "cas miss", r, 0); \
                Check(exp == a, "cas writeback", (long long)exp, (long long)a); \
                Check(d.iV == a, "cas untouched", (long long)d.iV, (long long)a); } } \
        } \
    printf("  u%-3d done (%d values squared)\n", bits, n); \
    }

TESTW(8,  TUint8,  TInt8)
TESTW(16, TUint16, TInt16)
TESTW(32, TUint32, TInt32)
TESTW(64, TUint64, TInt64)

static void TestScans()
    {
    gSection = "scans";
    Check(__e32_find_ms1_32(0) == -1, "ms1_32(0)", __e32_find_ms1_32(0), -1);
    Check(__e32_find_ls1_32(0) == -1, "ls1_32(0)", __e32_find_ls1_32(0), -1);
    Check(__e32_find_ms1_64(0) == -1, "ms1_64(0)", __e32_find_ms1_64(0), -1);
    Check(__e32_find_ls1_64(0) == -1, "ls1_64(0)", __e32_find_ls1_64(0), -1);
    for (int b = 0; b < 32; ++b)
        {
        TUint32 v = (TUint32)1 << b;
        Check(__e32_find_ms1_32(v) == b, "ms1_32", __e32_find_ms1_32(v), b);
        Check(__e32_find_ls1_32(v) == b, "ls1_32", __e32_find_ls1_32(v), b);
        Check(__e32_find_ms1_32((TUint32)(v | 1)) == b, "ms1_32 pair",
              __e32_find_ms1_32((TUint32)(v | 1)), b);
        Check(__e32_bit_count_32((TUint32)(v | 1)) == (b ? 2 : 1), "count_32",
              __e32_bit_count_32((TUint32)(v | 1)), b ? 2 : 1);
        }
    for (int b = 0; b < 64; ++b)
        {
        TUint64 v = (TUint64)1 << b;
        Check(__e32_find_ms1_64(v) == b, "ms1_64", __e32_find_ms1_64(v), b);
        Check(__e32_find_ls1_64(v) == b, "ls1_64", __e32_find_ls1_64(v), b);
        }
    Check(__e32_bit_count_32(0xffffffffu) == 32, "count_32 full",
          __e32_bit_count_32(0xffffffffu), 32);
    Check(__e32_bit_count_64(~(TUint64)0) == 64, "count_64 full",
          __e32_bit_count_64(~(TUint64)0), 64);
    __e32_memory_barrier();
    __e32_io_completion_barrier();
    printf("  scans + barriers done\n");
    }

int main()
    {
    Test8(); Test16(); Test32(); Test64(); TestScans();
    if (gFail)
        printf("ATOMICS FAIL %d\n", gFail);
    else
        printf("ATOMICS OK\n");
    return gFail ? 1 : 0;
    }
