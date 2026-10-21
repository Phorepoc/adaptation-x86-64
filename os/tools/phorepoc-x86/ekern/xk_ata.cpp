// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_ata.cpp -- the Phorepoc amd64 board's hard disk: an ATA (IDE) PIO media driver behind the REAL
// ELOCD (drivers/locmedia). X4.22.
//
// Ours: the 2010 drop's only ATA driver is drivers/medata/pccd_ata.cpp, which is welded to the PC Card
// socket layer (CIS parsing, config registers, DPcCardSocket). This is the same device model in the
// shape of drivers/medint/iram.cpp -- a FIXED medium (DPrimaryMediaBase with no DFC queue), so ELOCD
// calls Request() straight from the F32 drive thread and everything here is synchronous -- with
// pccd_ata's MBR rules (default boot partition first, then the other FAT/FAT32 primaries).
//
// Hardware: the legacy primary IDE channel (0x1F0-0x1F7 + control 0x3F6), master drive, LBA28 PIO
// (READ/WRITE SECTORS, CACHE FLUSH after every write request). That is what QEMU's PIIX IDE and
// VirtualBox's default PIIX4 controller present, with no PCI work. The drive's interrupt is masked
// (nIEN): each sector's DRQ is polled -- for a PIO transfer the data is ready almost at once, and a
// command that stays busy is waited out with NKern::Sleep. X64_TODO: IRQ 14 + a semaphore, bus-master
// DMA, the secondary channel, LBA48, AHCI.
//
// Local drives: the RAM drive stays local drive 0 (C:, the loader's writable system drive, present
// with or without a disk); this medium's partitions take local drives 1-4, which ESTART's
// auto-detect maps to D:, E:, ... and mounts through elocal (DetectELocal: EMediaHardDisk + a FAT
// partition type). A disk with no MBR (a "superfloppy" FAT volume, or blank) is one partition covering
// the whole disk -- blank means F32 sees it corrupt and `format d:` in ESHELL makes it a FAT volume.
#include <drivers/locmedia.h>
#include <partitions.h>
#include "variantmediadef.h"

_LIT(KPddName, "Media.Ata");
_LIT(KAtaDriveName, "Ata");
_LIT(KAtaMutexName, "AtaPio");

static const TUint16 KAtaData = 0x1F0, KAtaError = 0x1F1, KAtaCount = 0x1F2, KAtaLba0 = 0x1F3,
					 KAtaLba1 = 0x1F4, KAtaLba2 = 0x1F5, KAtaDrive = 0x1F6, KAtaStatus = 0x1F7,
					 KAtaCmd = 0x1F7, KAtaCtrl = 0x3F6;
static const TUint8 KStBsy = 0x80, KStDrdy = 0x40, KStDf = 0x20, KStDrq = 0x08, KStErr = 0x01;
static const TUint8 KCtrlNIen = 0x02, KCtrlSrst = 0x04;
static const TUint8 KCmdReadSectors = 0x20, KCmdWriteSectors = 0x30, KCmdFlushCache = 0xE7, KCmdIdentify = 0xEC;
static const TInt KSectorSize = 512, KSectorShift = 9;
static const TInt KBufSectors = 32;						// 16 KB bounce buffer per command
static const TInt KTimeoutMs = 5000;

static inline TUint8 Inb(TUint16 aPort)
	{ TUint8 v; __asm__ __volatile__("in al, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void Outb(TUint16 aPort, TUint8 aVal)
	{ __asm__ __volatile__("out dx, al" : : "d"(aPort), "a"(aVal)); }
static inline void InsW(TUint16 aPort, TAny* aBuf, TInt aWords)
	{ __asm__ __volatile__("rep insw" : "+D"(aBuf), "+c"(aWords) : "d"(aPort) : "memory"); }
static inline void OutsW(TUint16 aPort, const TAny* aBuf, TInt aWords)
	{ __asm__ __volatile__("rep outsw" : "+S"(aBuf), "+c"(aWords) : "d"(aPort) : "memory"); }

// the ATA "400 ns" settle after a drive select / command: four reads of the alternate status
static inline void Delay400ns()
	{ for (TInt i=0; i<4; ++i) (void)Inb(KAtaCtrl); }

static TInt CardMediaId = -1;		// the ELOCD media id (not the TMediaDevice) of the card, set when it registers

class DPhysicalDeviceMediaAta : public DPhysicalDevice
	{
public:
	DPhysicalDeviceMediaAta();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DBase*& aChannel, TInt aMediaId, const TDesC8* anInfo, const TVersion& aVer);
	virtual TInt Validate(TInt aDeviceType, const TDesC8* anInfo, const TVersion& aVer);
	virtual TInt Info(TInt aFunction, TAny* a1);
	};

class DMediaDriverAta : public DMediaDriver
	{
public:
	DMediaDriverAta(TInt aMediaId) : DMediaDriver(aMediaId), iUnit(aMediaId == CardMediaId ? 1 : 0) {}
	virtual TInt Request(TLocDrvRequest& aRequest);
	virtual void Disconnect(DLocalDrive* aLocalDrive, TThreadMessage* aMsg) {}
	virtual TInt PartitionInfo(TPartitionInfo& anInfo);
	virtual void NotifyPowerDown() {}
	virtual void NotifyEmergencyPowerDown() {}
	TInt DoCreate();
private:
	TInt Caps(TLocDrvRequest& m);
	TInt Read(TLocDrvRequest& m);
	TInt Write(TLocDrvRequest& m);
	TInt Transfer(TLocDrvRequest& m, TBool aWrite);
	TInt Command(TUint8 aCmd, TUint32 aLba, TInt aCount, TUint8* aBuf);
	TInt Flush();
private:
	TInt iUnit;							// 0 = primary master (the hard disk), 1 = primary slave (the memory card, X4.111)
	TUint8 iBuf[KBufSectors*KSectorSize];
	};

// ---- the controller -------------------------------------------------------------------------------

static TUint32 AtaSectorsU[2];		// LBA28 capacity of the primary master / slave; 0 = no ATA disk there
#define AtaSectors AtaSectorsU[iUnit]
static DMutex* AtaLock;				// one channel, several F32 drive threads (one per partition)

// Wait until BSY clears (and, for aDrq, DRQ sets). Spins briefly, then sleeps a tick at a time.
static TInt WaitReady(TBool aDrq)
	{
	TUint32 start = NKern::TickCount();
	for (TInt spin=0;; ++spin)
		{
		TUint8 st = Inb(KAtaStatus);
		if (!(st & KStBsy))
			{
			if (st & (KStErr|KStDf))
				return KErrCorrupt;
			if (!aDrq || (st & KStDrq))
				return KErrNone;
			}
		if (spin > 2000)
			{
			if ((TInt)(NKern::TickCount() - start) > KTimeoutMs)
				return KErrTimedOut;
			NKern::Sleep(1);
			}
		}
	}

static void SelectLba(TInt aUnit, TUint32 aLba, TInt aCount)
	{
	Outb(KAtaDrive, (TUint8)(0xE0 | (aUnit ? 0x10 : 0) | ((aLba >> 24) & 0x0F)));		// master / slave, LBA
	Delay400ns();
	Outb(KAtaCount, (TUint8)aCount);								// 0 = 256
	Outb(KAtaLba0, (TUint8)aLba);
	Outb(KAtaLba1, (TUint8)(aLba >> 8));
	Outb(KAtaLba2, (TUint8)(aLba >> 16));
	}

// Probe the primary master: floating bus (0xFF) = no controller; IDENTIFY answering with the ATAPI /
// SATA signature in LBA1/LBA2 = not an ATA disk (the CD-ROM lives on the secondary channel anyway).
static TUint32 Identify(TInt aUnit)
	{
	if (Inb(KAtaStatus) == 0xFF)
		return 0;
	if (aUnit == 0)
		{
		Outb(KAtaCtrl, KCtrlNIen | KCtrlSrst);						// software reset (both drives), interrupts off
		Kern::NanoWait(10000);
		Outb(KAtaCtrl, KCtrlNIen);
		Kern::NanoWait(2000000);									// 2 ms
		}
	Outb(KAtaDrive, aUnit ? 0xB0 : 0xA0);
	Delay400ns();
	Outb(KAtaCount, 0); Outb(KAtaLba0, 0); Outb(KAtaLba1, 0); Outb(KAtaLba2, 0);
	Outb(KAtaCmd, KCmdIdentify);
	Delay400ns();
	if (Inb(KAtaStatus) == 0)
		return 0;													// no drive
	for (TInt i=0; i<1000000 && (Inb(KAtaStatus) & KStBsy); ++i) {}
	if (Inb(KAtaLba1) || Inb(KAtaLba2))
		return 0;													// ATAPI / SATA signature
	for (TInt i=0; i<1000000; ++i)
		{
		TUint8 st = Inb(KAtaStatus);
		if (st & KStErr)
			return 0;
		if (st & KStDrq)
			{
			TUint16 id[256];
			InsW(KAtaData, id, 256);
			return (TUint32)id[60] | ((TUint32)id[61] << 16);		// LBA28 user-addressable sectors
			}
		}
	return 0;
	}

// ---- the media driver -----------------------------------------------------------------------------

TInt DMediaDriverAta::DoCreate()
	{
	if (!AtaSectors)
		return KErrNotReady;
	SetTotalSizeInBytes((Int64)AtaSectors << KSectorShift);
	return KErrNone;
	}

// aCount sectors (1..KBufSectors) at aLba, into/out of aBuf. Called with AtaLock held.
TInt DMediaDriverAta::Command(TUint8 aCmd, TUint32 aLba, TInt aCount, TUint8* aBuf)
	{
	TInt r = WaitReady(EFalse);
	if (r != KErrNone)
		return r;
	SelectLba(iUnit, aLba, aCount);
	Outb(KAtaCmd, aCmd);
	Delay400ns();
	for (TInt i=0; i<aCount; ++i, aBuf+=KSectorSize)
		{
		if ((r = WaitReady(ETrue)) != KErrNone)
			return r;
		if (aCmd == KCmdWriteSectors)
			OutsW(KAtaData, aBuf, KSectorSize/2);
		else
			InsW(KAtaData, aBuf, KSectorSize/2);
		}
	return WaitReady(EFalse);
	}

TInt DMediaDriverAta::Flush()
	{
	Outb(KAtaDrive, iUnit ? 0xF0 : 0xE0);
	Delay400ns();
	Outb(KAtaCmd, KCmdFlushCache);
	Delay400ns();
	return WaitReady(EFalse);
	}

// Any byte range: whole sectors go through the bounce buffer; a partial first/last sector of a WRITE
// is read first (read-modify-write).
TInt DMediaDriverAta::Transfer(TLocDrvRequest& m, TBool aWrite)
	{
	const Int64 pos = m.Pos();
	const Int64 len = m.Length();
	if (pos < 0 || len < 0 || pos + len > ((Int64)AtaSectors << KSectorShift))
		return KErrArgument;
	TInt done = 0;
	while (done < len)
		{
		const Int64 p = pos + done;
		const TUint32 lba = (TUint32)(p >> KSectorShift);
		const TInt skip = (TInt)(p & (KSectorSize-1));
		TInt chunk = (TInt)(len - done);
		if (chunk > KBufSectors*KSectorSize - skip)
			chunk = KBufSectors*KSectorSize - skip;
		const TInt n = (skip + chunk + KSectorSize-1) >> KSectorShift;
		TInt r;
		if (!aWrite)
			{
			if ((r = Command(KCmdReadSectors, lba, n, iBuf)) != KErrNone)
				return r;
			TPtrC8 des(iBuf + skip, chunk);
			if ((r = m.WriteRemote(&des, done)) != KErrNone)
				return r;
			}
		else
			{
			if (skip && (r = Command(KCmdReadSectors, lba, 1, iBuf)) != KErrNone)
				return r;
			const TInt tail = (skip + chunk) & (KSectorSize-1);
			if (tail && (n > 1 || !skip) && (r = Command(KCmdReadSectors, lba+n-1, 1, iBuf+(n-1)*KSectorSize)) != KErrNone)
				return r;
			TPtr8 des(iBuf + skip, chunk, chunk);
			if ((r = m.ReadRemote(&des, done)) != KErrNone)
				return r;
			if ((r = Command(KCmdWriteSectors, lba, n, iBuf)) != KErrNone)
				return r;
			}
		done += chunk;
		}
	return aWrite ? Flush() : KErrNone;
	}

TInt DMediaDriverAta::Caps(TLocDrvRequest& m)
	{
	TLocalDriveCapsV6& caps = *(TLocalDriveCapsV6*)m.RemoteDes();
	TLocDrv& d = *m.Drive();
	caps.iType = EMediaHardDisk;
	caps.iConnectionBusType = EConnectionBusInternal;
	if (iUnit)
		{
		// the memory card: what medmmc reports for an SD card -- a removable, formattable hard-disk-type medium
		// (the card is static, always inserted: it sits on a fixed-media registration, no media-change path)
		caps.iDriveAtt = KDriveAttLocal | KDriveAttRemovable;
		caps.iMediaAtt = KMediaAttFormattable;
		}
	else
		{
		caps.iDriveAtt = KDriveAttLocal | KDriveAttInternal;
		caps.iMediaAtt = 0;
		}
	caps.iFileSystemId = KDriveFileSysFAT;
	caps.iPartitionType = d.iPartitionType;
	caps.iSize = d.iPartitionLen;
	caps.iHiddenSectors = (TUint)(d.iPartitionBaseAddr >> KSectorShift);
	caps.iBlockSize = KSectorSize;
	SetTotalSizeInBytes(caps);
	return KErrNone;
	}

TInt DMediaDriverAta::Request(TLocDrvRequest& m)
	{
	TInt r;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*AtaLock);
	switch (m.Id())
		{
	case DLocalDrive::ECaps:	r = Caps(m);				break;
	case DLocalDrive::ERead:	r = Transfer(m, EFalse);	break;
	case DLocalDrive::EWrite:	r = Transfer(m, ETrue);		break;
	default:					r = KErrNotSupported;		break;
		}
	Kern::MutexSignal(*AtaLock);
	NKern::ThreadLeaveCS();
	return r;
	}

static void SetEntry(TPartitionEntry& aDest, const TMBRPartitionEntry& aSrc)
	{
	aDest.iPartitionBaseAddr = (Int64)aSrc.iFirstSector << KSectorShift;
	aDest.iPartitionLen = (Int64)aSrc.iNumSectors << KSectorShift;
	aDest.iBootIndicator = aSrc.iX86BootIndicator;
	aDest.iPartitionType = aSrc.iPartitionType;
	}

// A FAT boot sector (a superfloppy volume) also ends in 55 AA: tell it from an MBR by its jump.
static TBool IsFatBootSector(const TUint8* aSector)
	{
	return (aSector[0] == 0xEB || aSector[0] == 0xE9) && aSector[11] == 0 && aSector[12] == 2;	// bytes/sector 512
	}

// Fixed media: ELOCD's OpenMediaDriver wants KErrNone for "done, synchronously" (locmedia.cpp).
TInt DMediaDriverAta::PartitionInfo(TPartitionInfo& anInfo)
	{
	anInfo.iPartitionCount = 0;
	anInfo.iMediaSizeInBytes = TotalSizeInBytes();
	NKern::ThreadEnterCS();
	Kern::MutexWait(*AtaLock);
	TInt r = Command(KCmdReadSectors, 0, 1, iBuf);
	Kern::MutexSignal(*AtaLock);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	TInt n = 0;
	if (*(TUint16*)(iBuf + KMBRSignatureOffset) == KMBRSignature && !IsFatBootSector(iBuf))
		{
		TMBRPartitionEntry pe[KMBRMaxPrimaryPartitions];
		memcpy(pe, iBuf + KMBRFirstPartitionOffset, sizeof(pe));	// (the table is not 4-byte aligned)
		TInt boot = -1;
		for (TInt i=0; i<KMBRMaxPrimaryPartitions; ++i)
			if (pe[i].IsDefaultBootPartition())
				{ SetEntry(anInfo.iEntry[n++], pe[i]); boot = i; break; }
		for (TInt i=0; i<KMBRMaxPrimaryPartitions; ++i)
			if (i != boot && (pe[i].IsValidDosPartition() || pe[i].IsValidFAT32Partition()))
				SetEntry(anInfo.iEntry[n++], pe[i]);
		}
	if (n == 0)
		{
		// no MBR: the whole disk is one volume (FAT32 past 2 GB, as a formatter would choose)
		anInfo.iEntry[0].iPartitionBaseAddr = 0;
		anInfo.iEntry[0].iPartitionLen = TotalSizeInBytes();
		anInfo.iEntry[0].iBootIndicator = 0;
		anInfo.iEntry[0].iPartitionType = TotalSizeInBytes() > (Int64(2047) << 20) ? KPartitionTypeWin95FAT32 : KPartitionTypeFAT16;
		n = 1;
		}
	anInfo.iPartitionCount = n;
	Kern::Printf("[ata] %d partition(s); first at sector %d, %d MB, type %02x", n,
					(TInt)(anInfo.iEntry[0].iPartitionBaseAddr >> KSectorShift),
					(TInt)(anInfo.iEntry[0].iPartitionLen >> 20), anInfo.iEntry[0].iPartitionType);
	return KErrNone;
	}

// ---- the PDD factory + registration ---------------------------------------------------------------

DPhysicalDeviceMediaAta::DPhysicalDeviceMediaAta()
	{
	iUnitsMask = 0x1;
	iVersion = TVersion(KMediaDriverInterfaceMajorVersion, KMediaDriverInterfaceMinorVersion, KMediaDriverInterfaceBuildVersion);
	}

TInt DPhysicalDeviceMediaAta::Install()					{ return SetName(&KPddName); }
void DPhysicalDeviceMediaAta::GetCaps(TDes8&) const		{}

TInt DPhysicalDeviceMediaAta::Create(DBase*& aChannel, TInt aMediaId, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(iVersion, aVer))
		return KErrNotSupported;
	DMediaDriverAta* d = new DMediaDriverAta(aMediaId);
	aChannel = d;
	return d ? d->DoCreate() : KErrNoMemory;
	}

TInt DPhysicalDeviceMediaAta::Validate(TInt aDeviceType, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(iVersion, aVer))
		return KErrNotSupported;
	return (aDeviceType == MEDIA_DEVICE_ATA || aDeviceType == MEDIA_DEVICE_CARD) ? KErrNone : KErrNotSupported;
	}

TInt DPhysicalDeviceMediaAta::Info(TInt aFunction, TAny*)
	{
	return aFunction == EPriority ? KMediaDriverPriorityNormal : KErrNotSupported;
	}

static TInt AtaDriveNumbers[ATA_DRIVECOUNT] = { ATA_DRIVELIST };
static TInt CardDriveNumbers[CARD_DRIVECOUNT] = { CARD_DRIVELIST };
static TBool AtaPddInstalled;

// X4.30: probe only (xk_media.cpp asks first, to decide which medium becomes C:). 0 = no disk.
TUint32 X64ProbeAtaDisk()
	{
	if (!AtaSectorsU[0])
		AtaSectorsU[0] = Identify(0);
	return AtaSectorsU[0];
	}

// X4.111: the primary slave = the memory card. Probe after the master (which resets the channel).
TUint32 X64ProbeAtaCard()
	{
	X64ProbeAtaDisk();
	if (!AtaSectorsU[1])
		AtaSectorsU[1] = Identify(1);
	return AtaSectorsU[1];
	}

// install the PDD (once) and register one unit's media on its local drives
static void StartUnit(TInt aUnit, TInt aDevice, TInt aCount, TInt* aDrives, const char* aWhat)
	{
	TInt r = KErrNone;
	if (!AtaLock)
		r = Kern::MutexCreate(AtaLock, KAtaMutexName, KMutexOrdGeneral0);
	if (r == KErrNone && !AtaPddInstalled)
		{
		DPhysicalDevice* pd = new DPhysicalDeviceMediaAta;
		r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
		AtaPddInstalled = (r == KErrNone);
		}
	DPrimaryMediaBase* pM = r == KErrNone ? new DPrimaryMediaBase : NULL;
	if (pM)
		r = LocDrv::RegisterMediaDevice((TMediaDevice)aDevice, aCount, aDrives, pM, ATA_NUMMEDIA, KAtaDriveName);
	else if (r == KErrNone)
		r = KErrNoMemory;
	if (aUnit && pM && r == KErrNone)
		CardMediaId = pM->iMediaId;
	Kern::Printf("[ata] %s: %d sectors (%d MB), PIO; local drives %d.. -> %d",
					aWhat, AtaSectorsU[aUnit], AtaSectorsU[aUnit] >> 11, aDrives[0], r);
	}

// From xk_media.cpp after ELOCD: with a disk present install the PDD + register it on local drives
// aFirstDrive.. (0 = the disk is C:, as on a phone -- X4.30; 1 = D: behind the RAM drive, X4.22).
void X64StartAtaDisk(TInt aFirstDrive)
	{
	if (!X64ProbeAtaDisk())
		{
		Kern::Printf("[ata] no ATA disk on the primary IDE channel");
		return;
		}
	for (TInt i=0; i<ATA_DRIVECOUNT; ++i)
		AtaDriveNumbers[i] = aFirstDrive + i;
	StartUnit(0, MEDIA_DEVICE_ATA, ATA_DRIVECOUNT, AtaDriveNumbers, "primary master");
	}

// X4.111: the memory card, local drive CARD_DRIVELIST (estart.txt maps it to F:)
void X64StartAtaCard()
	{
	if (!X64ProbeAtaCard())
		{
		Kern::Printf("[ata] no memory card (primary slave)");
		return;
		}
	StartUnit(1, MEDIA_DEVICE_CARD, CARD_DRIVECOUNT, CardDriveNumbers, "primary slave = memory card");
	}
