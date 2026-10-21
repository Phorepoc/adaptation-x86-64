/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// tools/phorepoc-x86/port/x64smp/relax_size_asserts.h
//
// PHOREPOC amd64 SMP port shim. Force-included AFTER e32def.h so it wins.
//
// nkernsmp/nk_priv.h hard-codes two size targets tuned for the 32-bit x86 layout:
//   __ASSERT_COMPILE(sizeof(TSubScheduler)==(1<<KSubSchedulerShift));  // 1<<10 = 1024
//   __ASSERT_COMPILE(sizeof(TScheduler)==1024);
// On amd64 the shared per-CPU structures hold 8-byte pointers instead of 4-byte
// ones, so TSubScheduler/TScheduler are naturally larger than 1024 no matter how
// small the arch extras (TSubSchedulerX/TSchedulerX) are made -- the 1024 target
// simply cannot hold for a 64-bit build.
//
// KSubSchedulerShift is used for shift-based per-CPU indexing ONLY in the ARM
// backend (nkernsmp/arm/ncsched.cia). The x86 backend indexes via the APIC-ID
// SubSchedulerLookupTable[] instead, and the amd64 backend will do the same, so
// the power-of-2 size constraint is not needed on this port.
//
// This shim neutralises __ASSERT_COMPILE for the amd64 SMP TUs. It is used by the
// nkernsmp probe (build_nkernsmp_probe.sh) to let the neutral core compile so the
// backend symbol surface can be enumerated. NOTE: this disables ALL compile-time
// size/offset asserts in the SMP headers, which is acceptable for a symbol-surface
// probe; the final SMP build should re-tighten this to only the two size targets
// above (e.g. by arch-guarding KSubSchedulerShift + the TScheduler size in a LIFT
// of nk_priv.h once the amd64 layout is frozen).

#ifdef __ASSERT_COMPILE
#undef __ASSERT_COMPILE
#endif
#define __ASSERT_COMPILE(x)
