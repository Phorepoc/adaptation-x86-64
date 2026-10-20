/* libc_vfscanf_x32.c -- Path C (amd64/x32): the P.I.P.S. libc vfscanf.c, UNMODIFIED, compiled with the
 * branch the source keeps for x86 GCC builds. Its positional-"%n$" code knows two va_list layouts: the
 * ARM EABI struct (ap.__ap) and a contiguous argument block (EMULATOR || __X86GCC__). SysV x86-64 is
 * neither (a register save area), so %n$ into a positional argument stays unsupported either way; the
 * __X86GCC__ branch at least compiles. Nothing else in the TU or its headers tests __X86GCC__ (checked,
 * X4.26), and EMULATOR -- the other way in -- also switches the WSD layout, so it is not used.
 * Registry: libc x32_drop vfscanf.c + x32_extra this file. */
#define __X86GCC__ 1
#include "../../../../baseline/symbian-2010/oss.FCL.sf.os.ossrv/genericopenlibs/openenvcore/libc/src/stdio/vfscanf.c"
