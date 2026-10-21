/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// e32def.h -- HOSTED TEST SHIM (atomics_test.cpp only; never on the kernel/euser include path).
// x64s_atomics.cpp uses a handful of names from the real .../eka/include/e32def.h; this supplies
// exactly those so the REAL file compiles unmodified next to libc. The target build uses the real
// header. Widths are fixed-size here for the same reason they are there: the atomics are a
// width-exact contract (8/16/32/64), and a wrong width would silently change the RMW size.
#ifndef __ATOMICS_TEST_SHIM_E32DEF_H__
#define __ATOMICS_TEST_SHIM_E32DEF_H__

typedef signed int TInt;
typedef unsigned int TUint;
typedef int TBool;
typedef void TAny;
typedef signed char TInt8;
typedef unsigned char TUint8;
typedef signed short TInt16;
typedef unsigned short TUint16;
typedef signed int TInt32;
typedef unsigned int TUint32;
typedef long long TInt64;
typedef unsigned long long TUint64;

#define EXPORT_C
#define IMPORT_C

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#endif
