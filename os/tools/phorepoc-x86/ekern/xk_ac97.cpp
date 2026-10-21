// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_ac97.cpp -- Path C (X4.88): Intel AC'97 audio (ICH 82801AA, PCI 8086:2415 -- what QEMU `-device AC97` and
// VirtualBox `--audio-controller ac97` emulate) as a SOUND PDD under the REAL shared-chunk sound LDD
// (kernel/eka/drivers/soundsc/soundldd.cpp, linked in unmodified with its entry renamed, as the Ethernet LDD).
//
// The hardware: two I/O BARs -- NAM (the codec mixer registers) and NABM (the bus-master engine). The PCM-out engine
// walks a 32-entry Buffer Descriptor List of {physical address, sample count, IOC}; CIV = the entry it is playing,
// LVI = the last valid one; when it finishes LVI it halts (DCH) until LVI moves. That maps 1:1 onto
// DSoundScPdd::TransferData: the LDD hands us physically contiguous pieces (up to MaxTransferLen), each becomes a BDL
// entry with IOC, and a completion DFC -- the PCI interrupt, with a 10 ms timer as the safety net -- reports every
// entry the engine has moved past with PlayCallback.
// Format: 16-bit little-endian stereo only (AC'97's native PCM out); the rate through Variable Rate Audio (VRA).
// mdasoundadapter (RMdaDevSound) converts mono and rates itself. The record unit (4) exists because
// mdasoundadapter opens both units and fails if either is missing; it accepts buffers and returns them silent-free
// (no capture path yet).
//
// SOUND_TEST=1 (make_ekern): a boot self-test plays a 1 s 440 Hz sine straight through the engine, before any LDD
// -- with QEMU's `-audiodev wav` the result lands in a .wav file the host can check.
#include <kernel/kern_priv.h>
#include <drivers/soundsc.h>
#include <assp.h>
#include "xk_interrupt.h"

static inline TUint8 Inb(TUint16 p)			{ TUint8 v; asm volatile("in al, dx" : "=a"(v) : "d"(p)); return v; }
static inline TUint16 Inw(TUint16 p)		{ TUint16 v; asm volatile("in ax, dx" : "=a"(v) : "d"(p)); return v; }
static inline TUint32 Inl(TUint16 p)		{ TUint32 v; asm volatile("in eax, dx" : "=a"(v) : "d"(p)); return v; }
static inline void Outb(TUint16 p, TUint8 v)	{ asm volatile("out dx, al" : : "a"(v), "d"(p)); }
static inline void Outw(TUint16 p, TUint16 v)	{ asm volatile("out dx, ax" : : "a"(v), "d"(p)); }
static inline void Outl(TUint16 p, TUint32 v)	{ asm volatile("out dx, eax" : : "a"(v), "d"(p)); }
static TUint32 PciRead(TInt aDev, TInt aReg)	{ Outl(0xCF8, 0x80000000u | (aDev << 11) | (aReg & 0xFC)); return Inl(0xCFC); }
static void PciWrite(TInt aDev, TInt aReg, TUint32 aVal)	{ Outl(0xCF8, 0x80000000u | (aDev << 11) | (aReg & 0xFC)); Outl(0xCFC, aVal); }

// NAM (mixer) registers
static const TUint16 KNamReset = 0x00, KNamMaster = 0x02, KNamPcmOut = 0x18, KNamExtId = 0x28, KNamExtCtrl = 0x2A,
					 KNamFrontRate = 0x2C;
// NABM: the PCM-out box at 0x10 + the global registers
static const TUint16 KPoBdbar = 0x10, KPoCiv = 0x14, KPoLvi = 0x15, KPoSr = 0x16, KPoCr = 0x1B, KGlobCnt = 0x2C,
					 KGlobSta = 0x30;
static const TUint8 KCrRpbm = 0x01, KCrRr = 0x02, KCrLvbie = 0x04, KCrIoce = 0x10;
static const TUint16 KSrDch = 0x01, KSrLvbci = 0x04, KSrBcis = 0x08, KSrFifoe = 0x10;
static const TUint16 KBdIoc = 0x8000;
static const TInt KBdN = 32;
static const TInt KMaxTransfer = 0xFFFE * 2;				// one BDL entry: 0xFFFE 16-bit samples

_LIT(KAc97PddName, "SoundSc.AC97");

struct SBd { TUint32 iAddr; TUint16 iSamples; TUint16 iFlags; };

class DAc97Pdd;

class TAc97
	{
public:
	TInt Init();
	void Reset();
	TInt SetRate(TInt aHz);
	void SetVolume(TInt aVol);								// 0..255 (the LDD's scale)
	TInt Queue(TPhysAddr aPhys, TInt aBytes, TUint aId);	// -> KErrOverflow when the list is full
	void Start();
	void Stop();
	void Pause(TBool aPause);
	static void Isr(TAny* aPtr);
	static void DoneDfcFn(TAny* aPtr);
	static void PollFn(TAny* aPtr);
	void SelfTest();
public:
	TUint16 iNam, iNabm;
	TInt iDev;
	TInt iIrqId;
	TPhysAddr iBdlPhys;
	volatile SBd* iBdl;
	TUint iIds[KBdN];										// transfer id per entry
	TInt iBytes[KBdN];
	TInt iHead, iCount;										// the oldest pending entry, how many are pending
	TBool iRunPending;										// Start() came before the first buffer (audio U5)
	TInt iRate;
	DAc97Pdd* iTxPdd;										// the play channel bound to the engine (or NULL)
	TDynamicDfcQue* iDfcQ;
	TDfc iDoneDfc;
	NTimer iPoll;
	TAc97() : iDoneDfc(DoneDfcFn, this, 1), iPoll(PollFn, this) {}
	};

static TAc97* TheAc97;

void TAc97::Reset()
	{
	Outb(iNabm + KPoCr, 0);
	Outb(iNabm + KPoCr, KCrRr);								// reset the PCM-out box registers
	for (TInt i = 0; i < 1000 && (Inb(iNabm + KPoCr) & KCrRr); ++i) {}
	Outw(iNabm + KPoSr, KSrLvbci | KSrBcis | KSrFifoe);
	Outl(iNabm + KPoBdbar, iBdlPhys);
	iHead = iCount = 0;
	iRunPending = EFalse;
	}

TInt TAc97::Init()
	{
	iDev = -1;
	for (TInt d = 0; d < 32 && iDev < 0; ++d)
		if (PciRead(d, 0) == 0x24158086)
			iDev = d;
	if (iDev < 0)
		return KErrNotFound;
	iNam = (TUint16)(PciRead(iDev, 0x10) & ~3u);
	iNabm = (TUint16)(PciRead(iDev, 0x14) & ~3u);
	PciWrite(iDev, 0x04, PciRead(iDev, 0x04) | 0x5);		// I/O decode + bus master
	// cold reset off (GLOB_CNT bit 1), then the codec's "primary ready" (GLOB_STA bit 8)
	Outl(iNabm + KGlobCnt, 0x2);
	TInt i;
	for (i = 0; i < 100000 && !(Inl(iNabm + KGlobSta) & 0x100); ++i) {}
	Outw(iNam + KNamReset, 0);
	Outw(iNam + KNamMaster, 0x0000);						// 0 dB, unmuted
	Outw(iNam + KNamPcmOut, 0x0808);						// 0 dB, unmuted
	if (Inw(iNam + KNamExtId) & 1)							// VRA
		Outw(iNam + KNamExtCtrl, Inw(iNam + KNamExtCtrl) | 1);
	SetRate(48000);
	// the BDL: one page of physically contiguous RAM
	TInt r = Epoc::AllocPhysicalRam(0x1000, iBdlPhys);
	if (r != KErrNone)
		return r;
	DPlatChunkHw* ch = NULL;
	r = DPlatChunkHw::New(ch, iBdlPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iBdl = (volatile SBd*)ch->LinearAddress();
	memclr((TAny*)iBdl, 0x1000);
	Reset();
	_LIT(KAudioDfc, "AC97 DFC");
	r = Kern::DynamicDfcQCreate(iDfcQ, 27, KAudioDfc);
	if (r != KErrNone)
		return r;
	iDoneDfc.SetDfcQ(iDfcQ);
	const TInt line = PciRead(iDev, 0x3C) & 0xFF;
	iIrqId = (line > 0 && line < 16) ? (EX64IrqPciRouted | line) : -1;
	TInt h = iIrqId >= 0 ? Interrupt::Bind(iIrqId, Isr, this) : KErrNotSupported;
	if (h >= 0)
		Interrupt::Enable(h);
	Kern::Printf("[ac97] %08x at 00:%02x.0, NAM %04x NABM %04x, codec %s, VRA %d, irq %d (bind %d)",
				 PciRead(iDev, 0), iDev, iNam, iNabm, (Inl(iNabm + KGlobSta) & 0x100) ? "ready" : "NOT ready",
				 Inw(iNam + KNamExtId) & 1, line, h);
	return KErrNone;
	}

TInt TAc97::SetRate(TInt aHz)
	{
	if (!(Inw(iNam + KNamExtId) & 1))
		return aHz == 48000 ? KErrNone : KErrNotSupported;	// no VRA: 48 kHz only
	Outw(iNam + KNamFrontRate, (TUint16)aHz);
	iRate = Inw(iNam + KNamFrontRate);
	return iRate == aHz ? KErrNone : KErrNotSupported;
	}

void TAc97::SetVolume(TInt aVol)
	{
	// 0..255 -> AC'97 attenuation 31..0 in 1.5 dB steps (both channels); 0 = mute
	const TInt att = 31 - (aVol * 31) / 255;
	Outw(iNam + KNamPcmOut, aVol <= 0 ? 0x8000 : (TUint16)((att << 8) | att));
	}

TInt TAc97::Queue(TPhysAddr aPhys, TInt aBytes, TUint aId)
	{
	if (iCount >= KBdN - 1)
		return KErrOverflow;
	const TInt idx = (iHead + iCount) & (KBdN - 1);
	iBdl[idx].iAddr = aPhys;
	iBdl[idx].iSamples = (TUint16)(aBytes / 2);
	iBdl[idx].iFlags = KBdIoc;
	iIds[idx] = aId;
	iBytes[idx] = aBytes;
	++iCount;
	Outb(iNabm + KPoLvi, (TUint8)idx);						// a halted engine restarts on a new LVI
	if (iRunPending)
		{
		iRunPending = EFalse;
		Outb(iNabm + KPoCr, KCrRpbm | KCrIoce | KCrLvbie);
		}
	return KErrNone;
	}

void TAc97::Start()
	{
	// (audio U5) run the engine only once entry 0 holds a buffer: started empty, it consumed the stale entry 0 and
	// halted there, and the first real buffer's LVI write then restarted it at the NEXT entry (a restart advances CIV)
	// -- the first buffer of every stream was skipped (a Nokia ringing tone lost its first note, a WAV ~50 ms)
	if (iCount)
		Outb(iNabm + KPoCr, KCrRpbm | KCrIoce | KCrLvbie);
	else
		iRunPending = ETrue;
	iPoll.OneShot(NKern::TimerTicks(10));
	}

void TAc97::Stop()
	{
	iPoll.Cancel();
	Reset();
	}

void TAc97::Pause(TBool aPause)
	{
	const TUint8 cr = Inb(iNabm + KPoCr);
	Outb(iNabm + KPoCr, aPause ? (TUint8)(cr & ~KCrRpbm) : (TUint8)(cr | KCrRpbm));
	}

void TAc97::Isr(TAny* aPtr)
	{
	TAc97& a = *(TAc97*)aPtr;
	const TUint16 sr = Inw(a.iNabm + KPoSr);
	if (sr & (KSrLvbci | KSrBcis | KSrFifoe))
		{
		Outw(a.iNabm + KPoSr, sr & (KSrLvbci | KSrBcis | KSrFifoe));	// write-1-to-clear
		a.iDoneDfc.Add();
		}
	}

void TAc97::PollFn(TAny* aPtr)
	{
	TAc97& a = *(TAc97*)aPtr;
	a.iDoneDfc.Add();
	if (a.iCount)
		a.iPoll.Again(NKern::TimerTicks(10));
	}

class DAc97Pdd : public DSoundScPdd
	{
public:
	DAc97Pdd(TInt aUnit) : iUnit(aUnit) {}
	~DAc97Pdd()								{ if (TheAc97 && TheAc97->iTxPdd == this) { TheAc97->Stop(); TheAc97->iTxPdd = NULL; } }
	virtual TDfcQue* DfcQ(TInt)				{ return TheAc97->iDfcQ; }
	virtual void GetChunkCreateInfo(TChunkCreateInfo& aInfo)
		{
		aInfo.iType = TChunkCreateInfo::ESharedKernelMultiple;
		aInfo.iMapAttr = EMapAttrFullyBlocking;
		aInfo.iOwnsMemory = ETrue;
		aInfo.iDestroyedDfc = NULL;
		}
	virtual void Caps(TDes8& aCapsBuf) const
		{
		TSoundFormatsSupportedV02 caps;
		caps.iDirection = iUnit == KSoundScTxUnit0 ? ESoundDirPlayback : ESoundDirRecord;
		caps.iChannels = KSoundStereoChannel;
		caps.iRates = KSoundRate8000Hz | KSoundRate11025Hz | KSoundRate12000Hz | KSoundRate16000Hz | KSoundRate22050Hz |
					  KSoundRate24000Hz | KSoundRate32000Hz | KSoundRate44100Hz | KSoundRate48000Hz;
		caps.iEncodings = KSoundEncoding16BitPCM;
		caps.iDataFormats = KSoundDataFormatInterleaved;
		caps.iRequestMinSize = 4;
		caps.iRequestAlignment = 2;
		caps.iHwConfigNotificationSupport = EFalse;
		aCapsBuf.FillZ(aCapsBuf.MaxLength());
		TPtrC8 p((const TUint8*)&caps, sizeof(caps));
		aCapsBuf = p.Left(Min(p.Length(), aCapsBuf.MaxLength()));
		}
	virtual TInt MaxTransferLen() const		{ return KMaxTransfer; }
	virtual TInt SetConfig(const TDesC8& aConfigBuf)
		{
		TCurrentSoundFormatV02 c;
		TPtr8 p((TUint8*)&c, sizeof(c));
		Kern::InfoCopy(p, aConfigBuf);
		if (c.iChannels != 2 || c.iEncoding != ESoundEncoding16BitPCM || c.iDataFormat != ESoundDataFormatInterleaved)
			return KErrNotSupported;
		static const TInt KRates[] = { 7350, 8000, 8820, 9600, 11025, 12000, 14700, 16000, 22050, 24000, 29400, 32000,
									   44100, 48000 };
		if ((TUint)c.iRate >= sizeof(KRates) / sizeof(KRates[0]))
			return KErrNotSupported;
		return iUnit == KSoundScTxUnit0 ? TheAc97->SetRate(KRates[c.iRate]) : KErrNone;
		}
	virtual TInt SetVolume(TInt aVolume)	{ if (iUnit == KSoundScTxUnit0) TheAc97->SetVolume(aVolume); return KErrNone; }
	virtual TInt StartTransfer()
		{
		if (iUnit != KSoundScTxUnit0)
			return KErrNone;
		TheAc97->iTxPdd = this;
		TheAc97->Start();
		return KErrNone;
		}
	virtual TInt TransferData(TUint aId, TLinAddr, TPhysAddr aPhys, TInt aBytes)
		{
		if (iUnit != KSoundScTxUnit0)
			{
			Ldd()->RecordCallback(aId, KErrNone, 0);		// no capture path yet: nothing recorded
			return KErrNone;
			}
		return TheAc97->Queue(aPhys, aBytes, aId);
		}
	virtual void StopTransfer()				{ if (iUnit == KSoundScTxUnit0) TheAc97->Stop(); }
	virtual TInt PauseTransfer()			{ if (iUnit == KSoundScTxUnit0) TheAc97->Pause(ETrue); return KErrNone; }
	virtual TInt ResumeTransfer()			{ if (iUnit == KSoundScTxUnit0) TheAc97->Pause(EFalse); return KErrNone; }
	virtual TInt PowerUp()					{ return KErrNone; }
	virtual void PowerDown()				{}
	virtual TInt CustomConfig(TInt, TAny*)	{ return KErrNotSupported; }
	void Done(TUint aId, TInt aBytes)		{ Ldd()->PlayCallback(aId, KErrNone, aBytes); }
private:
	TInt iUnit;
	};

void TAc97::DoneDfcFn(TAny* aPtr)
	{
	TAc97& a = *(TAc97*)aPtr;
	// entries the engine has moved past: those before CIV, or all of them once it halted on LVI
	const TBool halted = Inw(a.iNabm + KPoSr) & KSrDch;
	const TInt civ = Inb(a.iNabm + KPoCiv) & (KBdN - 1);
	while (a.iCount && (halted || a.iHead != civ))
		{
		const TInt i = a.iHead;
		a.iHead = (a.iHead + 1) & (KBdN - 1);
		--a.iCount;
		if (a.iTxPdd)
			a.iTxPdd->Done(a.iIds[i], a.iBytes[i]);
		else if (!a.iCount)
			Kern::Printf("[ac97] self-test: all entries played (last id %d)", a.iIds[i]);
		}
	}

class DAc97Factory : public DPhysicalDevice
	{
public:
	DAc97Factory()			{ iUnitsMask = (1 << KSoundScTxUnit0) | (1 << KSoundScRxUnit0); iVersion = RSoundSc::VersionRequired(); }
	virtual TInt Install()	{ return SetName(&KAc97PddName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
		{
		aChannel = new DAc97Pdd(aUnit);
		return aChannel ? KErrNone : KErrNoMemory;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(RSoundSc::VersionRequired(), aVer) || !TheAc97)
			return KErrNotSupported;
		return (aUnit == KSoundScTxUnit0 || aUnit == KSoundScRxUnit0) ? KErrNone : KErrNotSupported;
		}
	};

// SOUND_TEST=1: 1 s of a 440 Hz sine at 48 kHz (a quarter-wave table, no FPU), queued in 32 KB pieces, played
// straight through the engine; the completion DFC logs how many entries finished
void TAc97::SelfTest()
	{
	const TInt KBytes = 48000 * 4;							// 1 s, 16-bit stereo
	TPhysAddr phys;
	if (Epoc::AllocPhysicalRam(Kern::RoundToPageSize(KBytes), phys) != KErrNone)
		return;
	DPlatChunkHw* ch = NULL;
	if (DPlatChunkHw::New(ch, phys, Kern::RoundToPageSize(KBytes), EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return;
	TInt16* s = (TInt16*)ch->LinearAddress();
	// sin(2*pi*k/n) for a 440 Hz period at 48 kHz (~109 samples): a parabolic approximation, amplitude 8000
	for (TInt i = 0; i < KBytes / 4; ++i)
		{
		const TInt ph = (i * 440 * 4096 / 48000) & 4095;	// phase in 1/4096 of a period
		const TInt x = (ph & 2047) - 1024;					// -1024..1023 over a half period
		TInt v = 8000 - (8000 * x / 1024) * x / 1024;		// 0..8000..0 (a parabola)
		if (ph >= 2048)
			v = -v;
		s[2 * i] = s[2 * i + 1] = (TInt16)v;
		}
	SetRate(48000);
	for (TInt off = 0, id = 0; off < KBytes; off += 32768, ++id)
		Queue(phys + off, Min(32768, KBytes - off), id);
	Kern::Printf("[ac97] self-test: 1 s of 440 Hz queued in %d entries", iCount);
	Start();
	}

DLogicalDevice* CreateLogicalDevice_soundsc();				// soundldd.cpp (DECLARE_STANDARD_LDD, renamed)

void X64StartSound()
	{
	NKern::ThreadEnterCS();
	TheAc97 = new TAc97;
	TInt r = TheAc97 ? TheAc97->Init() : KErrNoMemory;
	if (r != KErrNone)
		{
		Kern::Printf("[ac97] no AC'97 audio (%d): no sound", r);
		delete TheAc97;
		TheAc97 = NULL;
		}
#ifdef SOUND_TEST
	else
		TheAc97->SelfTest();
#endif
	NKern::ThreadLeaveCS();
	}

// Sys\Bin\esoundsc.ldd / soundsc.pdd (rombuild --builtin-device; xboot.cpp points export 1 here) -- the names
// mdasoundadapter loads
extern "C" DLogicalDevice* X64CreateLdd_soundsc()
	{
	return CreateLogicalDevice_soundsc();
	}

extern "C" DPhysicalDevice* X64CreatePdd_ac97()
	{
	return TheAc97 ? new DAc97Factory : NULL;
	}
