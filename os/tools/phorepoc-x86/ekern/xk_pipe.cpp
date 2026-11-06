// xk_pipe.cpp -- the built-in-device factory of the pipe driver (rombuild --builtin-device 'Sys\Bin\pipelib.ldd=ldd:8'; xboot.cpp points export 1 here),
// as the Ethernet LDD's (xk_e1000.cpp). The LDD is the 2010 kernel/eka/drivers/pipe/dpipe.cpp, compiled unmodified with only its entry points
// renamed (make_ekern.sh). RPipe::Create (euser) does User::LoadLogicalDevice("PIPELIB"); without this file Open C pipe() fails with ENOENT,
// with it pipe(), popen() and everything built on them work.
#include <kernel/kern_priv.h>

DLogicalDevice* CreateLogicalDevice_pipe();		// dpipe.cpp (DECLARE_STANDARD_LDD, renamed)

extern "C" DLogicalDevice* X64CreateLdd_pipe()
	{
	return CreateLogicalDevice_pipe();
	}
