// xk_media.cpp -- brings up the REAL local-media stack as built-in kernel extensions (X4.17).
//
// The amd64 sibling of Track B's tools/kernel/ekern_media_glue.cpp. In a real ROM these are
// separate modules: ELOCD.LDD (drivers/locmedia -- an extension that is ALSO an LDD) and
// MEDINT.PDD (drivers/medint/iram.cpp -- an extension that is also a PDD). Here they are linked
// into the kernel image, unmodified, with their per-module entry symbols renamed at compile time
// (make_ekern.sh, the exstart pattern), and this file calls the entries in ROM order from the
// ExtensionInit1 of the board's first extension (xk_entry.cpp):
//
//   1. locmedia InitExtension   -- Kern::AddHalEntry(EHalGroupMedia): what makes
//                                  UserHal::DriveInfo() work (ESTART_7 without it)
//   2. install the 'LocDrv' LDD factory. A real ROM installs it when F32 calls
//      User::LoadLogicalDevice("ELOCD"); there is no ELOCD.LDD file, so F32's load returns
//      KErrNotFound, which its StartupThread tolerates.
//   3. install the 'Media.IRam' PDD factory (same reasoning, no MEDINT.PDD file).
//   4. medint InitExtension     -- LocDrv::RegisterMediaDevice(EFixedMedia0, local drive 0).
//   5. the ATA disk (xk_ata.cpp, X4.22) -- probe the primary IDE master; if there is one, install
//      'Media.Ata' and register EFixedMedia1 on local drives 0-3 = C: onwards, and steps 3 + 4
//      are skipped (X4.30); with make_ekern.sh DISK_AS_D=1 on local drives 1-4 = D: behind the RAM drive.
#include <kernel/kern_priv.h>

#ifndef XK_DISK_AS_D
#define XK_DISK_AS_D 0
#endif

TInt InitExtension_locmedia();
DLogicalDevice* CreateLogicalDevice_locmedia();
TInt InitExtension_medint();
void X64StartAtaDisk(TInt aFirstDrive);	// xk_ata.cpp
void X64StartAtaCard();					// xk_ata.cpp: the memory card (primary slave, X4.111)
TUint32 X64ProbeAtaDisk();
DPhysicalDevice* CreatePhysicalDevice_medint();

void X64StartMediaExtensions()
	{
	TInt r = InitExtension_locmedia();
	Kern::Printf("[media] locmedia (ELOCD) InitExtension -> %d", r);

	DLogicalDevice* ld = CreateLogicalDevice_locmedia();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	Kern::Printf("[media] InstallLogicalDevice(LocDrv) -> %d", r);

	// X4.30: a hard disk, when there is one, is C: -- persistent, like a phone's internal drive --
	// and the RAM drive is not brought up (its local drive 0 is compiled into the unmodified
	// iram.cpp). No disk, or XK_DISK_AS_D=1 (the X4.22 layout): C: = RAM, the disk from D:.
	if (!XK_DISK_AS_D && X64ProbeAtaDisk())
		{
		Kern::Printf("[media] hard disk present: it is C: (no RAM drive)");
		X64StartAtaDisk(0);
		X64StartAtaCard();
		return;
		}

	DPhysicalDevice* pd = CreatePhysicalDevice_medint();
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	Kern::Printf("[media] InstallPhysicalDevice(Media.IRam) -> %d", r);

	r = InitExtension_medint();
	Kern::Printf("[media] medint InitExtension (IRAM = local drive 0) -> %d", r);

	X64StartAtaDisk(1);
	X64StartAtaCard();
	}
