// xu_mem.cpp -- Phorepoc amd64/x32 euser: builds the UNMODIFIED baseline common/mem.cpp.
// mem.cpp carries an out-of-line Mem::Compare under `__GCC32__ && !__KERNEL_MODE__` that was written
// for GCC/ARM, where e32std.inl suppresses the inline one. On GCC/x86 (a user-side configuration the
// 2010 tree never built: real x86 euser uses cmem.cia) both exist -> redefinition. Hide __GCC32__
// from the TU body only; every header it needs is already included (and guarded) by then.
#include "common.h"
#undef __GCC32__
#include "../../../../baseline/kernelhwsrv/kernel/eka/common/mem.cpp"
