/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// variantmediadef.h -- the Phorepoc amd64 board's local-media map (VariantMediaDefIncludePath).
// Read by the unmodified drivers/medint/iram.cpp (the internal RAM drive).
//
// The internal RAM drive is LOCAL DRIVE 0. ESTART's auto-detect path (no ESTART.TXT) maps local
// drive 0 -> C: (DefaultLocalDrives[]) and, because DetectIRam recognises it, mounts elocal/FAT with
// FS_FORMAT_COLD: C: is volatile, and always there (X4.17). The ATA disk (xk_ata.cpp, X4.22) is a
// second fixed medium whose partitions take local drives 1-4 -> D:, E:, ... (DetectELocal).
// docs/phorepoc/x86-64-port.md.
#ifndef __VARIANTMEDIADEF_H__
#define __VARIANTMEDIADEF_H__

#define IRAM_DRIVECOUNT 1
#define IRAM_DRIVELIST  0       // C:
#define IRAM_NUMMEDIA   1
#define IRAM_DRIVENAME "IRam"

#define MEDIA_DEVICE_ATA EFixedMedia1
#define ATA_DRIVECOUNT  4
#define ATA_DRIVELIST   1,2,3,4 // D: E: F: G:
#define ATA_NUMMEDIA    1

// X4.111: the memory card = the ATA primary SLAVE (QEMU/VirtualBox second disk). A removable drive on local drive 4
// (estart.txt: F:, where the 701 has its card; E: = mass memory there).
#define MEDIA_DEVICE_CARD EFixedMedia2
#define CARD_DRIVECOUNT 1
#define CARD_DRIVELIST  4

#endif  // __VARIANTMEDIADEF_H__
