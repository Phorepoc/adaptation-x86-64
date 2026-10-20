/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// u32exec.h -- Phorepoc amd64/x32: SHADOWS include/u32exec.h (this directory is first on the user
// include path). Same contract as the IA-32 branch of the original -- the executive gate is EKA2's
// own: int 0x20 fast / int 0x21 slow, call number in EAX, arguments in ECX, EDX, EBX, ESI, result in
// EAX -- but the IA-32 stubs fetched their arguments from the cdecl STACK ([esp+4]...). Under the
// SysV x32 ABI they arrive in EDI, ESI, EDX, ECX, so each stub is a register shuffle. The kernel gate
// preserves every register except EAX; RBX is callee-saved for our caller, so 3/4-argument stubs
// save it. All Exec:: functions are static and every argument is 32-bit (pointers included).
// Kernel side: tools/phorepoc-x86/ekern/xk_irq.cpp.
#ifndef __U32EXEC_H__
#define __U32EXEC_H__
#include <u32std.h>
#include <cpudefs.h>

#ifdef __KERNEL_MODE__
#define	EXEC_INT64		Int64
#define	EXEC_TIME		TTimeK
#else
#define	EXEC_INT64		TInt64
#define	EXEC_TIME		TInt64
#endif

#define __EXECDECL__	__attribute__((naked))

#define X64_EXEC_NUM(n)	__asm__ __volatile__("mov eax, %0" : : "i"(n))

#define	FAST_EXEC0(n)	X64_EXEC_NUM(n); __asm__("int 0x20 \n ret")
#define	FAST_EXEC1(n)	X64_EXEC_NUM(n); __asm__("mov ecx, edi \n int 0x20 \n ret")
#define	SLOW_EXEC0(n)	X64_EXEC_NUM(n); __asm__("int 0x21 \n ret")
#define	SLOW_EXEC1(n)	X64_EXEC_NUM(n); __asm__("mov ecx, edi \n int 0x21 \n ret")
#define	SLOW_EXEC2(n)	X64_EXEC_NUM(n); __asm__("mov ecx, edi \n mov edx, esi \n int 0x21 \n ret")
#define SLOW_EXEC3(n)	X64_EXEC_NUM(n); \
						__asm__("push rbx      \n" \
								"mov ebx, edx  \n" \
								"mov ecx, edi  \n" \
								"mov edx, esi  \n" \
								"int 0x21      \n" \
								"pop rbx       \n" \
								"ret")
#define SLOW_EXEC4(n)	X64_EXEC_NUM(n); \
						__asm__("push rbx      \n" \
								"mov ebx, edx  \n" \
								"mov edx, esi  \n" \
								"mov esi, ecx  \n" \
								"mov ecx, edi  \n" \
								"int 0x21      \n" \
								"pop rbx       \n" \
								"ret")

#define	KFAST_EXEC0(n)	FAST_EXEC0(n)
#define	KFAST_EXEC1(n)	FAST_EXEC1(n)
#define	KSLOW_EXEC0(n)	SLOW_EXEC0(n)
#define	KSLOW_EXEC1(n)	SLOW_EXEC1(n)
#define	KSLOW_EXEC2(n)	SLOW_EXEC2(n)
#define KSLOW_EXEC3(n)	SLOW_EXEC3(n)
#define KSLOW_EXEC4(n)	SLOW_EXEC4(n)

#ifdef __LEAVE_EQUALS_THROW__
class TTrap;
#endif

#include <exec_enum.h>
#include <e32btrace.h>
#include <exec_user.h>

#endif
