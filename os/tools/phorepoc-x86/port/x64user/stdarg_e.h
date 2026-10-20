/* stdarg_e.h -- Path C x32 override of ESTLIB's epoc32/include/libc/stdarg_e.h (X4.41).
 *
 * The original defines va_list as `char*` and va_arg as a walk up a CONTIGUOUS STACK of int-sized
 * slots (the same fallback port_prelude.h already replaces for e32def.h's VA_LIST). On the SysV
 * x86-64 ABI (x32 included) variadic arguments arrive in registers, so that walk reads whatever
 * is on the stack: sqlite3_config(SQLITE_CONFIG_MALLOC, &methods) copied stack garbage (UTF-16
 * text) into its malloc table and sqlsrv.exe jumped to 0x0069006c. port/x64user is on the include
 * path before the mmp's libc directory, so <stdarg_e.h> (included by libc/stdarg.h) resolves here.
 */
#ifndef _STDARG_E_H
#define _STDARG_E_H

typedef __builtin_va_list __e32_va_list;

#define va_start(ap, pn) __builtin_va_start(ap, pn)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)
/* ... and tell openenvcore's stdio.h, which otherwise redefines it as `(dst = src)` -- an array
   assignment on x86-64, where va_list is an array (libc vfprintf.c, X4.51) */
#define _VA_COPY_DEFINED

#endif
