// xk_e1000.cpp -- the Phorepoc amd64 board's network card: Intel 8254x "e1000". X4.47.
//
// One driver for both hypervisors we run on: QEMU's default NIC is the e1000 (82540EM, 8086:100E) and
// VirtualBox's default "Intel PRO/1000 MT Desktop" is the same chip. The classic descriptor-ring
// interface (Intel 8254x SDM): a ring of 16-byte receive descriptors the card fills with frames
// (RDH advances as it does, we hand slots back through RDT), a ring of transmit descriptors we fill
// (TDT) and it sends (DD in the status byte when done). Registers are MMIO in BAR0; the rings and the
// 2 KB packet buffers are one physically contiguous block (x86 DMA is cache-coherent).
//
// Symbian side: the REAL Ethernet LDD (kernel/eka/drivers/ethernet/d_ethernet.cpp, "Ethernet",
// RBusDevEthernet) is linked in unmodified, and this is its PDD ("Ethernet.E1000", the DEthernet
// interface Syborg's virtio-net and NaviEngine's SMSC 9118 implement): Send() = one frame out,
// and for each frame in, ReceiveIsr() -> the LDD calls ReceiveFrame() to copy it (no FCS).
// Interrupt: PCI INTA# through the PIRQ router (the interrupt-line register, EX64IrqPciRouted, X4.43;
// shared, level, polarity from the MADT). The ISR reads ICR -- which clears the cause, so the line
// drops -- and queues the receive DFC on DfcQue0, where the LDD's own DFCs run too.
//
// Boot self-test: before anything opens the device, one ARP request for 10.0.2.2 from 10.0.2.15 (the
// NAT gateway and first guest address of both QEMU's user network and VirtualBox's NAT) and the first
// few received frames are logged: a reply proves transmit, receive, DMA and the interrupt in one go.
#include <kernel/kern_priv.h>
#include <assp.h>
#include <ethernet.h>
#include "xk_interrupt.h"

static inline TUint32 Inl(TUint16 aPort)
	{ TUint32 v; __asm__ __volatile__("in eax, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void Outl(TUint16 aPort, TUint32 aVal)
	{ __asm__ __volatile__("out dx, eax" : : "d"(aPort), "a"(aVal) : "memory"); }
static TUint32 PciRead(TInt aDev, TInt aReg)
	{ Outl(0xCF8, 0x80000000u | aDev << 11 | (aReg & 0xFC)); return Inl(0xCFC); }
static void PciWrite(TInt aDev, TInt aReg, TUint32 aVal)
	{ Outl(0xCF8, 0x80000000u | aDev << 11 | (aReg & 0xFC)); Outl(0xCFC, aVal); }

// ---- the chip (Intel 8254x SDM, section 13) -----------------------------------------------------------
enum
	{
	ECtrl = 0x0000, EStatus = 0x0008, EIcr = 0x00C0, EIms = 0x00D0, EImc = 0x00D8, ERctl = 0x0100,
	ETctl = 0x0400, ETipg = 0x0410, ERdbal = 0x2800, ERdbah = 0x2804, ERdlen = 0x2808, ERdh = 0x2810,
	ERdt = 0x2818, ETdbal = 0x3800, ETdbah = 0x3804, ETdlen = 0x3808, ETdh = 0x3810, ETdt = 0x3818,
	EMta = 0x5200, ERal = 0x5400, ERah = 0x5404, EMpc = 0x4010
	};
static const TUint32 KCtrlSlu = 1u << 6, KCtrlAsde = 1u << 5, KCtrlRst = 1u << 26;
static const TUint32 KStatusLu = 1u << 1;
static const TUint32 KRctlEn = 1u << 1, KRctlBam = 1u << 15, KRctlSecrc = 1u << 26;	// BSIZE 00 = 2048
static const TUint32 KTctlEn = 1u << 1, KTctlPsp = 1u << 3;
static const TUint32 KIntTxdw = 1u << 0, KIntLsc = 1u << 2, KIntRxdmt0 = 1u << 4, KIntRxo = 1u << 6,
					 KIntRxt0 = 1u << 7;
static const TUint8 KTxEop = 0x01, KTxIfcs = 0x02, KTxRs = 0x08, KDescDd = 0x01, KRxEop = 0x02;

struct SRxDesc { TUint64 iAddr; TUint16 iLength, iChecksum; TUint8 iStatus, iErrors; TUint16 iSpecial; };
struct STxDesc { TUint64 iAddr; TUint16 iLength; TUint8 iCso, iCmd, iStatus, iCss; TUint16 iSpecial; };

// 256 receive slots, as Linux's e1000 (was 16: a burst of replies -- 20 parallel HTTP loads -- overflowed the ring, the card
// dropped SYN-ACKs and DNS answers, and ~1.5% of connects/lookups hit curl's 30 s timeout, 2026-10-14)
static const TInt KRxN = 256, KTxN = 64, KBuf = 2048;
static const TInt KRxRingOff = 0, KTxRingOff = KRxN*16, KRxBufOff = 0x2000, KTxBufOff = KRxBufOff + KRxN*KBuf;
static const TInt KDmaSize = KTxBufOff + KTxN*KBuf;			// 648 KB
static const TInt KDmaAlloc = 0x100000;							// physically contiguous, 1 MB

class DE1000Pdd;

class TE1000
	{
public:
	TInt Init();
	TInt Send(const TUint8* aFrame, TInt aLength);
	void ArpSelfTest();
	inline TUint32 Reg(TInt aOff) const				{ return *(volatile TUint32*)(iMmio + aOff); }
	inline void SetReg(TInt aOff, TUint32 aVal)		{ *(volatile TUint32*)(iMmio + aOff) = aVal; }
	static void Isr(TAny* aPtr);
	static void RxDfcFn(TAny* aPtr);
	static void PollFn(TAny* aPtr);
	void LogFrame(const TUint8* aF, TInt aLen);
public:
	TLinAddr iMmio;
	TUint8* iDma;
	TInt iMissedLogs;
	TPhysAddr iDmaPhys;
	TUint8 iMac[6];
	TInt iRxNext;							// the next receive slot the card will fill
	TInt iTxNext;							// the next transmit slot we fill
	TInt iIrqId;
	TUint32 iIrqs, iRxFrames, iTxFrames, iLogged;
	TBool iLinkUp;
	DE1000Pdd* iPdd;						// the open channel's PDD, or NULL
	TBool iStarted;							// the LDD has started it (frames go up)
	TInt iCur;								// the slot ReceiveFrame copies from
	TDfc iRxDfc;
	NTimer iPoll;
	TE1000() : iMissedLogs(0), iRxDfc(RxDfcFn, this, 1), iPoll(PollFn, this) {}
	};
static TE1000* TheNic;

TInt TE1000::Init()
	{
	TInt dev = -1;
	for (TInt d=0; d<32 && dev<0; ++d)
		{
		const TUint32 id = PciRead(d, 0);
		const TUint32 did = id >> 16;						// 82540EM (QEMU, VBox MT Desktop), 82545EM, 82543GC
		if ((id & 0xFFFF) == 0x8086 && (did == 0x100E || did == 0x100F || did == 0x1004))
			dev = d;
		}
	if (dev < 0)
		return KErrNotFound;
	const TUint32 bar0 = PciRead(dev, 0x10) & ~0xFu;
	PciWrite(dev, 0x04, PciRead(dev, 0x04) | 0x6);			// memory decode + bus master
	DPlatChunkHw* mmio;
	TInt r = DPlatChunkHw::New(mmio, bar0, 0x20000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iMmio = mmio->LinearAddress();
	r = Epoc::AllocPhysicalRam(KDmaAlloc, iDmaPhys);
	DPlatChunkHw* dma = NULL;
	if (r == KErrNone)
		r = DPlatChunkHw::New(dma, iDmaPhys, KDmaAlloc, EMapAttrSupRw | EMapAttrCachedMax);
	if (r != KErrNone)
		return r;
	iDma = (TUint8*)dma->LinearAddress();
	memclr(iDma, KDmaSize);

	SetReg(EImc, 0xFFFFFFFF);								// quiet while we set up
	SetReg(ECtrl, Reg(ECtrl) | KCtrlRst);
	for (TInt i=0; i<100000 && (Reg(ECtrl) & KCtrlRst); ++i) {}
	SetReg(EImc, 0xFFFFFFFF);
	(void)Reg(EIcr);
	SetReg(ECtrl, (Reg(ECtrl) | KCtrlSlu | KCtrlAsde));
	const TUint32 ral = Reg(ERal), rah = Reg(ERah);			// the EEPROM's MAC, loaded by the reset
	for (TInt i=0; i<4; ++i) iMac[i] = (TUint8)(ral >> (8*i));
	iMac[4] = (TUint8)rah;  iMac[5] = (TUint8)(rah >> 8);
	SetReg(ERah, rah | 0x80000000u);						// address valid
	for (TInt i=0; i<128; ++i)
		SetReg(EMta + 4*i, 0);

	SRxDesc* rx = (SRxDesc*)(iDma + KRxRingOff);
	for (TInt i=0; i<KRxN; ++i)
		rx[i].iAddr = iDmaPhys + KRxBufOff + i*KBuf;
	SetReg(ERdbal, iDmaPhys + KRxRingOff);  SetReg(ERdbah, 0);
	SetReg(ERdlen, KRxN * sizeof(SRxDesc));
	SetReg(ERdh, 0);  SetReg(ERdt, KRxN - 1);				// every slot but one is the card's
	iRxNext = 0;
	SetReg(ETdbal, iDmaPhys + KTxRingOff);  SetReg(ETdbah, 0);
	SetReg(ETdlen, KTxN * sizeof(STxDesc));
	SetReg(ETdh, 0);  SetReg(ETdt, 0);
	iTxNext = 0;
	SetReg(ETipg, 0x0060200A);								// the SDM's IEEE 802.3 values
	SetReg(ETctl, KTctlEn | KTctlPsp | (0x10 << 4) | (0x40 << 12));
	SetReg(ERctl, KRctlEn | KRctlBam | KRctlSecrc);

	iRxDfc.SetDfcQ(Kern::DfcQue0());
	const TInt line = PciRead(dev, 0x3C) & 0xFF;
	iIrqId = (line > 0 && line < 16) ? (EX64IrqPciRouted | line) : -1;
	TInt h = iIrqId >= 0 ? Interrupt::Bind(iIrqId, Isr, this) : KErrNotSupported;
	if (h >= 0)
		{
		SetReg(EIms, KIntRxt0 | KIntRxo | KIntRxdmt0 | KIntLsc);
		Interrupt::Enable(h);								// by HANDLE: the pin is shared (VMMDev on VBox)
		}
	iLinkUp = (Reg(EStatus) & KStatusLu) != 0;
	iPoll.OneShot(NKern::TimerTicks(100));					// safety net (and the only path without an IRQ)
	Kern::Printf("[e1000] %08x at 00:%02x.0, MMIO %08x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s, "
					"irq: PIRQ-routed %d (bind %d)", PciRead(dev, 0), dev, bar0, iMac[0], iMac[1], iMac[2],
					iMac[3], iMac[4], iMac[5], iLinkUp ? "up" : "down", line, h);
	return KErrNone;
	}

TInt TE1000::Send(const TUint8* aFrame, TInt aLength)
	{
	if (aLength <= 0 || aLength > KBuf)
		return KErrArgument;
	STxDesc* tx = (STxDesc*)(iDma + KTxRingOff);
	STxDesc& d = tx[iTxNext];
	if (d.iCmd && !(d.iStatus & KDescDd))					// the card has not sent this slot yet
		return KErrOverflow;
	memcpy(iDma + KTxBufOff + iTxNext*KBuf, aFrame, aLength);
	d.iAddr = iDmaPhys + KTxBufOff + iTxNext*KBuf;
	d.iLength = (TUint16)aLength;
	d.iCso = d.iCss = 0;  d.iSpecial = 0;
	d.iStatus = 0;
	d.iCmd = KTxEop | KTxIfcs | KTxRs;
	iTxNext = (iTxNext + 1) % KTxN;
	SetReg(ETdt, iTxNext);
	++iTxFrames;
	return KErrNone;
	}

void TE1000::Isr(TAny* aPtr)
	{
	TE1000& n = *(TE1000*)aPtr;
	const TUint32 icr = n.Reg(EIcr);						// read = clear: the (level) line drops
	if (!icr)
		return;												// someone else's interrupt on a shared line
	++n.iIrqs;
	if (icr & KIntLsc)
		n.iLinkUp = (n.Reg(EStatus) & KStatusLu) != 0;
	if (icr & (KIntRxt0 | KIntRxo | KIntRxdmt0))
		n.iRxDfc.Add();
	}

void TE1000::PollFn(TAny* aPtr)
	{
	TE1000& n = *(TE1000*)aPtr;
	n.iRxDfc.Add();
	const TUint32 missed = n.Reg(EMpc);						// frames the card dropped for want of a receive slot (read = clear)
	if (missed && n.iMissedLogs < 20)
		{
		++n.iMissedLogs;
		Kern::Printf("[e1000] %d frames missed (receive ring full)", missed);
		}
	n.iPoll.Again(NKern::TimerTicks(100));
	}

// ---- the PDD -------------------------------------------------------------------------------------------
_LIT(KE1000PddName, "Ethernet.E1000");

class DE1000Pdd : public DEthernet
	{
public:
	~DE1000Pdd()											{ if (TheNic && TheNic->iPdd == this) TheNic->iPdd = NULL, TheNic->iStarted = EFalse; }
	virtual TInt Start()									{ TheNic->iStarted = ETrue; return KErrNone; }
	virtual void Stop(TStopMode)							{ TheNic->iStarted = EFalse; }
	virtual TInt ValidateConfig(const TEthernetConfigV01& aConfig) const
		{ return (aConfig.iEthSpeed == KEthSpeedUnknown || aConfig.iEthDuplex == KEthDuplexUnknown) ? KErrNotSupported : KErrNone; }
	virtual TInt Configure(TEthernetConfigV01&)				{ return KErrNone; }
	virtual void MacConfigure(TEthernetConfigV01& aConfig)
		{
		memcpy(TheNic->iMac, aConfig.iEthAddress, 6);		// program the new station address
		TheNic->SetReg(ERal, TheNic->iMac[0] | TheNic->iMac[1] << 8 | TheNic->iMac[2] << 16 | (TUint32)TheNic->iMac[3] << 24);
		TheNic->SetReg(ERah, TheNic->iMac[4] | TheNic->iMac[5] << 8 | 0x80000000u);
		}
	virtual void GetConfig(TEthernetConfigV01& aConfig) const
		{
		aConfig.iEthSpeed = KEthSpeed100BaseTX;			// the 1000 Mb/s of a VM's e1000 has no constant here
		aConfig.iEthDuplex = KEthDuplexFull;
		memcpy(aConfig.iEthAddress, TheNic->iMac, 6);
		}
	virtual void CheckConfig(TEthernetConfigV01&)			{}
	virtual void Caps(TDes8& aCaps) const
		{
		TEthernetCaps caps;
		aCaps.FillZ(aCaps.MaxLength());
		aCaps = caps.Left(Min(caps.Length(), aCaps.MaxLength()));
		}
	virtual TInt Send(TBuf8<KMaxEthernetPacket+32>& aBuffer)	{ return TheNic->Send(aBuffer.Ptr(), aBuffer.Length()); }
	virtual TInt ReceiveFrame(TBuf8<KMaxEthernetPacket+32>& aBuffer, TBool aOkToUse)
		{
		if (!aOkToUse)
			return KErrGeneral;								// the LDD is out of buffers: frame dropped
		const SRxDesc& d = ((SRxDesc*)(TheNic->iDma + KRxRingOff))[TheNic->iCur];
		const TInt len = Min((TInt)d.iLength, aBuffer.MaxLength());	// SECRC: no FCS in it
		aBuffer.Copy(TheNic->iDma + KRxBufOff + TheNic->iCur*KBuf, len);
		return KErrNone;
		}
	virtual TInt DisableIrqs()								{ return NKern::DisableAllInterrupts(); }	// x86: one level
	virtual void RestoreIrqs(TInt aLevel)					{ NKern::RestoreInterrupts(aLevel); }
	virtual TDfcQue* DfcQ(TInt)								{ return Kern::DfcQue0(); }
	};

void TE1000::RxDfcFn(TAny* aPtr)
	{
	TE1000& n = *(TE1000*)aPtr;
	SRxDesc* rx = (SRxDesc*)(n.iDma + KRxRingOff);
	for (TInt k=0; k<KRxN; ++k)
		{
		SRxDesc& d = rx[n.iRxNext];
		if (!(d.iStatus & KDescDd))
			break;
		++n.iRxFrames;
		if ((d.iStatus & KRxEop) && !d.iErrors)
			{
			if (n.iLogged < 6)								// bounded: the self-test's view
				n.LogFrame(n.iDma + KRxBufOff + n.iRxNext*KBuf, d.iLength);
			if (n.iPdd && n.iPdd->iLdd && n.iStarted)
				{
				n.iCur = n.iRxNext;
				n.iPdd->ReceiveIsr();						// the LDD pulls it with ReceiveFrame()
				}
			}
		d.iStatus = 0;										// the slot goes back to the card
		n.SetReg(ERdt, n.iRxNext);
		n.iRxNext = (n.iRxNext + 1) % KRxN;
		}
	}

void TE1000::LogFrame(const TUint8* aF, TInt aLen)
	{
	++iLogged;
	const TUint type = aF[12] << 8 | aF[13];
	if (type == 0x0806 && aLen >= 42 && aF[21] == 2)		// ARP reply
		Kern::Printf("[e1000] rx ARP reply: %d.%d.%d.%d is at %02x:%02x:%02x:%02x:%02x:%02x (irqs so far %d)",
						aF[28], aF[29], aF[30], aF[31], aF[22], aF[23], aF[24], aF[25], aF[26], aF[27], iIrqs);
	else
		Kern::Printf("[e1000] rx frame: %d bytes, type %04x, from %02x:%02x:%02x:%02x:%02x:%02x",
						aLen, type, aF[6], aF[7], aF[8], aF[9], aF[10], aF[11]);
	}

void TE1000::ArpSelfTest()
	{
	TUint8 f[42];
	memset(f, 0xFF, 6);										// broadcast
	memcpy(f + 6, iMac, 6);
	f[12] = 0x08;  f[13] = 0x06;							// ARP
	static const TUint8 KArp[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };	// Ethernet/IPv4, request
	memcpy(f + 14, KArp, 8);
	memcpy(f + 22, iMac, 6);
	f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;			// sender 10.0.2.15
	memclr(f + 32, 6);
	f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;			// who has 10.0.2.2?
	TInt r = Send(f, sizeof(f));
	Kern::Printf("[e1000] self-test: ARP who-has 10.0.2.2 tell 10.0.2.15 -> %d", r);
	}

class DE1000Factory : public DPhysicalDevice
	{
public:
	DE1000Factory()			{ iUnitsMask = 1; iVersion = TVersion(KEthernetMajorVersionNumber, KEthernetMinorVersionNumber, KEthernetBuildVersionNumber); }
	virtual TInt Install()	{ return SetName(&KE1000PddName); }
	virtual void GetCaps(TDes8& aDes) const
		{
		TEthernetCaps caps;
		aDes.FillZ(aDes.MaxLength());
		aDes = caps.Left(Min(caps.Length(), aDes.MaxLength()));
		}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&)
		{
		DE1000Pdd* p = new DE1000Pdd;
		aChannel = p;
		if (!p)
			return KErrNoMemory;
		TheNic->iPdd = p;
		return KErrNone;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(iVersion, aVer) || aUnit != 0 || !TheNic || (TheNic->iPdd && TheNic->iPdd->iLdd))
			return KErrNotSupported;
		return KErrNone;
		}
	};

DLogicalDevice* CreateLogicalDevice_enet();					// d_ethernet.cpp (DECLARE_STANDARD_LDD, renamed)

void X64StartNetwork()
	{
	NKern::ThreadEnterCS();
	TheNic = new TE1000;
	TInt r = TheNic ? TheNic->Init() : KErrNoMemory;
	if (r != KErrNone)
		{
		Kern::Printf("[e1000] no e1000 NIC (%d): no network", r);
		delete TheNic;
		TheNic = NULL;
		NKern::ThreadLeaveCS();
		return;
		}
	NKern::ThreadLeaveCS();
	TheNic->ArpSelfTest();
	}

// X4.48: the drivers are no longer installed here. As on a phone, whoever needs them loads them --
// the Ethernet NIF's packet driver does User::LoadLogicalDevice("enet") + LoadPhysicalDevice
// ("ethernet") with the file names CommsDat gives it -- and F32's loader finds the ROM records
// tools/rombuild.py --builtin-device writes (Sys\Bin\enet.ldd, ethernet.pdd). xboot.cpp points
// their export ordinal 1 here, so ExecHandler::DeviceLoad's factory call lands on these.
extern "C" DLogicalDevice* X64CreateLdd_enet()
	{
	return CreateLogicalDevice_enet();
	}

extern "C" DPhysicalDevice* X64CreatePdd_e1000()
	{
	return TheNic ? new DE1000Factory : NULL;				// no NIC: DeviceLoad reports KErrNoMemory
	}
