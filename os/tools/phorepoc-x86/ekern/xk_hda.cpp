// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_hda.cpp -- Intel High Definition Audio as a SOUND PDD under the REAL shared-chunk sound LDD
// (kernel/eka/drivers/soundsc/soundldd.cpp, linked in unmodified; the contract is the 2010 DSoundScPdd).
// The sibling of xk_ac97.cpp for what AC'97 cannot reach: VMware Workstation ("hdaudio"), real PCs, and the modern
// QEMU / VirtualBox configurations. The controller is found by its PCI CLASS (0x0403), not by id.
//
// Written from the Intel High Definition Audio Specification, revision 1.0a (register map, verbs, widget model,
// buffer descriptor list) and checked against the BSD/MIT-licensed drivers named in
// docs/phorepoc/research/hda-linux-modern.md (FreeBSD snd_hda, OpenBSD azalia, Haiku hda, SerenityOS IntelHDA).
// No GPL source (Linux, QEMU, VirtualBox) was copied: those were read for behaviour only. Provenance of the design
// decisions is in docs/phorepoc/hda.md of the private repository.
//
// Shape:
//  - MMIO BAR0 (16 KB), mapped as xk_e1000.cpp does. Controller reset, then CORB/RIRB for the codec commands, with
//    the immediate-command registers (ICOI/ICII/ICIS) as the fallback when the ring path does not answer. The
//    CORBRP reset is NOT waited for on VMware (PCI vendor 0x15AD): that controller never echoes the bit (Haiku's
//    HDA_QUIRK_NO_CORBRP_RESET_ACK).
//  - Codec bring-up is generic: the audio function group's widgets are read (capabilities, connection lists, pin
//    configuration defaults, amplifier parameters), every usable output pin (line-out / speaker / headphone, not
//    "no connection", not digital) is traced back through mixers and selectors to a DAC, and the whole path is
//    powered, un-muted, and routed. No codec ids are hardcoded. The input side does the same from an ADC to a
//    line-in / microphone pin.
//  - The 2010 LDD hands the PDD physically contiguous pieces of any alignment. An HDA stream is a fixed cyclic
//    buffer descriptor list that never stops by itself, so the PDD owns a bounce ring (32 periods of 1920 bytes =
//    10 ms each at 48 kHz stereo 16-bit = 61 440 bytes, more than the 12 288-byte buffers of the Nokia DevSound
//    adaptation plus the periods in flight). TransferData copies the piece into the ring and returns KErrNotReady
//    when there is no room; a completion DFC (the stream's IOC interrupt, with a 10 ms timer as the safety net)
//    reads the link position, reports every piece the DMA has passed with PlayCallback, in order, and zero-fills
//    the periods already played, so an underrun plays silence while the stream keeps running.
//  - Record: the same machinery mirrored (an input stream into a ring, copied out into the LDD's record pieces).
//  - Power: the audio function group goes to D3 after an idle hold-off, and is brought back (with the whole path
//    re-programmed) by the next PowerUp / StartTransfer.
//
// SOUND_TEST=1 (make_ekern): a boot self-test plays a 1 s 440 Hz sine through the same ring, before any LDD.
// HDADUMP=1 (make_ekern, -DXK_HDA_DUMP): the full widget graph of every codec is logged at boot, for verifying a
// new host (VMware); the controller / codec / chosen-path summary lines are always logged, and the graph is dumped
// anyway when no output path was found.
#include <kernel/kern_priv.h>
#include <drivers/soundsc.h>
#include <assp.h>
#include "xk_interrupt.h"
#include "xk_pci.h"
#include "xk_sound.h"

#ifndef XK_HDA_DUMP
#define XK_HDA_DUMP 0
#endif

// ---- controller registers (spec 1.0a, section 3.3) ---------------------------------------------------
enum
	{
	EGcap = 0x00, EVmin = 0x02, EVmaj = 0x03, EGctl = 0x08, EStatests = 0x0E, EIntctl = 0x20, EIntsts = 0x24,
	ECorbLbase = 0x40, ECorbUbase = 0x44, ECorbWp = 0x48, ECorbRp = 0x4A, ECorbCtl = 0x4C, ECorbSts = 0x4D,
	ECorbSize = 0x4E, ERirbLbase = 0x50, ERirbUbase = 0x54, ERirbWp = 0x58, ERintcnt = 0x5A, ERirbCtl = 0x5C,
	ERirbSts = 0x5D, ERirbSize = 0x5E, EIcoi = 0x60, EIcii = 0x64, EIcis = 0x68,
	ESdBase = 0x80, ESdStride = 0x20,
	// stream descriptor registers
	ESdCtl = 0x00, ESdSts = 0x03, ESdLpib = 0x04, ESdCbl = 0x08, ESdLvi = 0x0C, ESdFmt = 0x12, ESdBdpl = 0x18,
	ESdBdpu = 0x1C
	};
static const TUint32 KGctlCrst = 1u << 0;
static const TUint32 KSdSrst = 1u << 0, KSdRun = 1u << 1, KSdIoce = 1u << 2, KSdFeie = 1u << 3, KSdDeie = 1u << 4;
static const TUint8 KSdStsBcis = 0x04, KSdStsFifoe = 0x08, KSdStsDese = 0x10;
static const TUint32 KIntGie = 1u << 31;

// ---- verbs (section 7.3) ----------------------------------------------------------------------------
enum
	{
	VGetParam = 0xF00, VGetConnSel = 0xF01, VGetConnList = 0xF02, VGetPower = 0xF05, VGetPinCfg = 0xF1C,
	VSetConnSel = 0x701, VSetPower = 0x705, VSetStream = 0x706, VSetPinCtl = 0x707, VSetEapd = 0x70C,
	VSetFormat = 0x2, VSetAmp = 0x3
	};
enum	// parameter ids
	{
	PVendor = 0x00, PSubNodes = 0x04, PFgType = 0x05, PWidgetCaps = 0x09, PPcmRates = 0x0A, PStreamFmts = 0x0B,
	PPinCaps = 0x0C, PInAmpCaps = 0x0D, PConnLen = 0x0E, POutAmpCaps = 0x12
	};
enum { WDac = 0, WAdc = 1, WMixer = 2, WSelector = 3, WPin = 4 };	// widget types (caps bits 23..20)
enum { DevLineOut = 0, DevSpeaker = 1, DevHpOut = 2, DevLineIn = 8, DevMicIn = 0xA };	// pin default devices

static const TInt KMaxConn = 32, KMaxNodes = 40;
static const TInt KPeriod = 1920;						// 10 ms at 48 kHz stereo 16-bit; a multiple of 128 (HDA wants it)
static const TInt KTxPeriods = 32, KRxPeriods = 16;
static const TInt KQ = 32;								// pieces outstanding per stream (the LDD allows 8 requests, each split)
static const TInt KLead = KPeriod;						// after an underrun the next data starts this far ahead of the DMA
static const TInt KTxTag = 1, KRxTag = 2;
static const TInt KIdleHoldOffMs = 3000;				// the function group stays in D0 this long after the last use

_LIT(KHdaPddName, "SoundSc.HDA");

struct SBdle { TUint32 iAddrLo, iAddrHi, iLen, iFlags; };	// IOC = bit 0

struct SWidget
	{
	TUint8 iType;
	TUint8 iNConn;
	TUint16 iConn[KMaxConn];
	TUint32 iCaps, iPinCfg, iPinCaps, iAmpOut, iAmpIn, iPcm;
	};

struct SPiece
	{
	TUint iId;
	TUint64 iEnd;										// play: the absolute ring position after this piece
	TInt iBytes;
	TUint8* iLin;										// record: where the data goes
	TInt iFilled;
	};

struct SStream
	{
	TInt iSd;											// stream descriptor index (inputs first, then outputs)
	TInt iTag;
	TBool iIn;
	TInt iPeriods, iBytes;
	TUint8* iRing;
	TPhysAddr iRingPhys;
	volatile SBdle* iBdl;
	TPhysAddr iBdlPhys;
	TUint16 iFmt;
	TBool iPrepared;									// programmed, not necessarily running
	TBool iRunning;										// RUN set (and not paused)
	TBool iPlaying;										// the DMA has been started since Prepare
	TUint64 iDma;										// bytes the DMA has moved (running count)
	TUint64 iPos;										// play: bytes written into the ring; record: bytes handed out
	TUint64 iZero;										// play: bytes already zero-filled behind the DMA
	TUint32 iLastLpib;
	SPiece iQ[KQ];
	TInt iQHead, iQCount;
	volatile TUint8 iErr;
	};

class DHdaPdd;
void Ldd_PlayCallback(DHdaPdd* aPdd, TUint aId, TInt aResult, TInt aBytes);			// defined after the PDD class
void Ldd_RecordCallback(DHdaPdd* aPdd, TUint aId, TInt aResult, TInt aBytes);

class THda
	{
public:
	TInt Init();
	// registers
	volatile TUint8* Mm() const				{ return (volatile TUint8*)iMmio; }
	TUint8 R8(TInt o)						{ return *(volatile TUint8*)(iMmio + o); }
	TUint16 R16(TInt o)						{ return *(volatile TUint16*)(iMmio + o); }
	TUint32 R32(TInt o)						{ return *(volatile TUint32*)(iMmio + o); }
	void W8(TInt o, TUint8 v)				{ *(volatile TUint8*)(iMmio + o) = v; }
	void W16(TInt o, TUint16 v)				{ *(volatile TUint16*)(iMmio + o) = v; }
	void W32(TInt o, TUint32 v)				{ *(volatile TUint32*)(iMmio + o) = v; }
	TInt Sd(TInt aSd) const					{ return ESdBase + aSd * ESdStride; }
	static void Delay(TUint aUs)			{ Kern::NanoWait(aUs * 1000); }
	TBool WaitBits(TInt aReg, TUint32 aMask, TUint32 aVal, TUint aUsMax, TInt aWidth = 32);
	// codec commands
	TBool Cmd(TInt aNid, TUint32 aVerb, TUint32& aResp);
	TBool CorbCmd(TUint32 aWord, TUint32& aResp);
	TBool ImmCmd(TUint32 aWord, TUint32& aResp);
	TUint32 Param(TInt aNid, TInt aParam)	{ TUint32 r; return Cmd(aNid, VGetParam << 8 | aParam, r) ? r : 0; }
	TUint32 Get(TInt aNid, TUint32 aVerb12, TUint32 aPayload)	{ TUint32 r; return Cmd(aNid, aVerb12 << 8 | aPayload, r) ? r : 0; }
	void Set12(TInt aNid, TUint32 aVerb12, TUint32 aPayload8)	{ TUint32 r; Cmd(aNid, aVerb12 << 8 | (aPayload8 & 0xFF), r); }
	void Set4(TInt aNid, TUint32 aVerb4, TUint32 aPayload16)	{ TUint32 r; Cmd(aNid, aVerb4 << 16 | (aPayload16 & 0xFFFF), r); }
	void SetAmp(TInt aNid, TBool aOut, TInt aIdx, TBool aMute, TInt aGain);
	// controller + codec bring-up
	TInt ResetController();
	TInt SetupRings();
	TInt ProbeCodec(TInt aCad);
	void ReadConn(TInt aNid, SWidget& aW);
	TBool FindToDac(TInt aNid, TInt aDepth, TUint8* aPath, TInt& aLen, TUint8* aSel, TUint8* aSeen);
	TBool FindToPin(TInt aNid, TInt aDepth, TUint8* aPath, TInt& aLen, TUint8* aSel, TUint8* aSeen);
	TBool AddPath(TUint8* aNodes, TUint8* aSel, TUint32* aMask, TInt& aN, const TUint8* aPath, TInt aLen, const TUint8* aSelByNid);
	void SelectOutputs();
	void SelectInput();
	void SetupOutput();
	void SetupInput();
	void ApplyVolume();
	void ApplyRecLevel();
	void ApplyFormats();
	void DumpGraph();
	TInt AmpSteps(TInt aNid, TBool aOut, TInt& aOffset, TInt& aNum, TInt& aStep, TBool& aMuteCap);
	// power
	void EnsurePowered();
	void IdleDown();
	void ArmIdle();
	// streams
	void SdReset(TInt aSd);
	TInt PrepareStream(SStream& s);
	void RunStream(SStream& s);
	void HaltStream(SStream& s, TBool aReset);
	void UpdatePos(SStream& s);
	void ZeroPlayed(SStream& s);
	TInt TxWrite(TUint aId, const TUint8* aSrc, TInt aBytes);
	void TxStop();
	void TxPause();
	void TxResume();
	void TxDfc();
	TInt RxQueue(TUint aId, TUint8* aDst, TInt aBytes);
	void RxStart();
	void RxStop();
	void RxPause();
	void RxDfc();
	void CopyIn(TUint8* aDst, const TUint8* aSrc, TInt aBytes);
	TInt FormatFor(TInt aHz) const;
	void SelfTest();
	void SelfFeed();
	// interrupts, timers
	static void Isr(TAny* aPtr);
	static void DoneDfcFn(TAny* aPtr);
	static void PollFn(TAny* aPtr);
	static void IdleTimerFn(TAny* aPtr);
	static void IdleDfcFn(TAny* aPtr);
	static void SelfDfcFn(TAny* aPtr);
public:
	SPciLoc iLoc;
	TUint32 iPciId;
	TLinAddr iMmio;
	TInt iIss, iOss, iBss;
	TInt iIrqId;
	TBool iNoCorbAck;									// VMware: the CORBRP reset bit is never echoed
	TBool iImmediate;									// commands go through ICOI/ICII/ICIS
	TBool iCorbOk;										// the CORB/RIRB path has answered at least once
	TInt iCorbN, iRirbN, iRirbRp;
	volatile TUint32* iCorb;
	volatile TUint32* iRirb;									// pairs: response, response-ex
	TPhysAddr iCtlPhys;
	TUint8* iCtl;
	// the codec
	TInt iCad, iAfg;
	TUint32 iVendor;
	SWidget* iW;										// 256 entries, by node id
	TUint32 iAfgAmpOut, iAfgAmpIn, iAfgPcm;
	TUint8 iOutNodes[KMaxNodes], iOutSel[KMaxNodes];
	TUint32 iOutMask[KMaxNodes];
	TInt iNOut;
	TUint8 iInNodes[KMaxNodes], iInSel[KMaxNodes];
	TUint32 iInMask[KMaxNodes];
	TInt iNIn;
	TInt iVolNode;										// the widget whose output amp carries the play volume (-1: software)
	TInt iRecNode;										// the ADC whose input amp carries the record level (-1: none)
	TInt iVolume, iRecLevel;
	TInt iSwGain;										// 256 = unity; below = software attenuation in CopyIn
	TUint32 iTxRates, iRxRates;							// KSoundRate*Hz masks the converters support
	TBool iHasIn;
	TBool iPowered;
	TInt iRxHz, iTxHz;
	// streams
	SStream iTx, iRx;
	TUint8* iScratch;									// TxResume's rotation buffer
	DHdaPdd* iTxPdd;
	DHdaPdd* iRxPdd;
	// completion
	TDynamicDfcQue* iDfcQ;
	TDfc iDoneDfc, iIdleDfc, iSelfDfc;
	NTimer iPoll, iIdleTimer;
	// self-test
	TBool iSelfActive;
	TUint8* iSelfBuf;
	TInt iSelfTotal, iSelfOff, iSelfId, iSelfDone;
	THda() : iDoneDfc(DoneDfcFn, this, 1), iIdleDfc(IdleDfcFn, this, 1), iSelfDfc(SelfDfcFn, this, 1),
			 iPoll(PollFn, this), iIdleTimer(IdleTimerFn, this) {}
	};

static THda* TheHda;

// ---- low-level waits and the command path -------------------------------------------------------------

TBool THda::WaitBits(TInt aReg, TUint32 aMask, TUint32 aVal, TUint aUsMax, TInt aWidth)
	{
	for (TUint t = 0; ; t += 10)
		{
		const TUint32 v = aWidth == 8 ? R8(aReg) : aWidth == 16 ? R16(aReg) : R32(aReg);
		if ((v & aMask) == aVal)
			return ETrue;
		if (t >= aUsMax)
			return EFalse;
		Delay(10);
		}
	}

TBool THda::CorbCmd(TUint32 aWord, TUint32& aResp)
	{
	const TInt wp = (R16(ECorbWp) + 1) % iCorbN;
	iCorb[wp] = aWord;
	W16(ECorbWp, (TUint16)wp);
	for (TInt t = 0; t < 5000; ++t)						// 50 ms
		{
		while ((R16(ERirbWp) & 0xFF) != (TUint)iRirbRp)
			{
			iRirbRp = (iRirbRp + 1) % iRirbN;
			const TUint32 resp = iRirb[2 * iRirbRp], ex = iRirb[2 * iRirbRp + 1];
			if (ex & 0x10)								// an unsolicited response: not ours
				continue;
			aResp = resp;
			W8(ERirbSts, 0x5);							// RINTFL / RIRBOIS write-1-to-clear: QEMU's CORB engine stops after RINTCNT responses until the guest does
			iCorbOk = ETrue;
			return ETrue;
			}
		Delay(10);
		}
#if XK_HDA_DUMP
	Kern::Printf("[hda] command %08x timed out: CORBWP %d CORBRP %04x RIRBWP %04x (ours %d) RIRBSTS %02x", aWord, wp, R16(ECorbRp), R16(ERirbWp), iRirbRp, R8(ERirbSts));
#endif
	return EFalse;
	}

TBool THda::ImmCmd(TUint32 aWord, TUint32& aResp)
	{
	if (!WaitBits(EIcis, 1, 0, 5000, 16))					// ICB: a previous command still running
		return EFalse;
	W16(EIcis, 2);										// IRV: write 1 to clear
	W32(EIcoi, aWord);
	W16(EIcis, 1);										// ICB: go
	if (!WaitBits(EIcis, 3, 2, 50000, 16))				// ICB clear and IRV set
		return EFalse;
	aResp = R32(EIcii);
	W16(EIcis, 2);
	return ETrue;
	}

TBool THda::Cmd(TInt aNid, TUint32 aVerb, TUint32& aResp)
	{
	const TUint32 w = (TUint32)iCad << 28 | (TUint32)aNid << 20 | (aVerb & 0xFFFFF);
	if (!iImmediate)
		{
		if (CorbCmd(w, aResp))
			return ETrue;
		if (iCorbOk)
			return EFalse;								// the ring works: this widget just does not answer
		iImmediate = ETrue;								// the ring never answered: the immediate registers it is
		Kern::Printf("[hda] CORB/RIRB did not answer: immediate commands");
		}
	return ImmCmd(w, aResp);
	}

void THda::SetAmp(TInt aNid, TBool aOut, TInt aIdx, TBool aMute, TInt aGain)
	{
	Set4(aNid, VSetAmp, (aOut ? 0x8000 : 0x4000) | 0x3000 | (aIdx & 0xF) << 8 | (aMute ? 0x80 : 0) | (aGain & 0x7F));
	}

// ---- controller bring-up -----------------------------------------------------------------------------

TInt THda::ResetController()
	{
	// out of reset: stop everything the firmware may have left running
	W8(ECorbCtl, 0);
	W8(ERirbCtl, 0);
	W32(EIntctl, 0);
	W32(EGctl, 0);										// CRST=0 -> the controller goes into reset
	WaitBits(EGctl, KGctlCrst, 0, 100000);
	Delay(200);											// >= 100 us in reset (spec 4.2.2)
	W32(EGctl, KGctlCrst);
	if (!WaitBits(EGctl, KGctlCrst, KGctlCrst, 100000))
		return KErrTimedOut;
	Delay(1000);										// codecs need >= 521 us to announce themselves
	for (TInt t = 0; t < 100 && !(R16(EStatests) & 0x7FFF); ++t)
		Delay(1000);
	return (R16(EStatests) & 0x7FFF) ? KErrNone : KErrNotFound;
	}

TInt THda::SetupRings()
	{
	// CORB + RIRB: the size the hardware offers, at most 256 entries
	const TUint8 cs = (R8(ECorbSize) >> 4) & 0xF, rs = (R8(ERirbSize) >> 4) & 0xF;
	iCorbN = (cs & 4) ? 256 : (cs & 2) ? 16 : 2;
	iRirbN = (rs & 4) ? 256 : (rs & 2) ? 16 : 2;
	W8(ECorbSize, (TUint8)((iCorbN == 256) ? 2 : (iCorbN == 16) ? 1 : 0));
	W8(ERirbSize, (TUint8)((iRirbN == 256) ? 2 : (iRirbN == 16) ? 1 : 0));
	W32(ECorbLbase, iCtlPhys);  W32(ECorbUbase, 0);
	W32(ERirbLbase, iCtlPhys + 0x400);  W32(ERirbUbase, 0);
	// the CORB read pointer: set the reset bit, wait for the hardware to echo it, clear it, wait again -- except on
	// VMware, which never echoes (Haiku HDA_QUIRK_NO_CORBRP_RESET_ACK): there the bit is set and cleared blind
	W16(ECorbRp, 0x8000);
	if (iNoCorbAck)
		Delay(100);
	else
		WaitBits(ECorbRp, 0x8000, 0x8000, 1000, 16);
	W16(ECorbRp, 0);
	if (!iNoCorbAck)
		WaitBits(ECorbRp, 0x8000, 0, 1000, 16);
	W16(ECorbWp, 0);
	W16(ERirbWp, 0x8000);								// RIRB write pointer reset (self-clearing)
	iRirbRp = 0;
	W16(ERintcnt, 1);
	W8(ERirbSts, 0x5);
#ifdef XK_HDA_IMM
	iImmediate = ETrue;									// test knob (HDAIMM=1): the immediate-command path only
#endif
	W8(ECorbCtl, 0x2);									// CORB DMA run
	W8(ERirbCtl, 0x3);									// RIRB DMA run + response flag (RINTFL) enabled; INTCTL.CIE stays off, so the responses are polled, not interrupts
	return KErrNone;
	}

// ---- the codec -----------------------------------------------------------------------------------------

void THda::ReadConn(TInt aNid, SWidget& aW)
	{
	aW.iNConn = 0;
	const TUint32 lp = Param(aNid, PConnLen);
	const TInt n = lp & 0x7F;
	const TBool longForm = (lp & 0x80) != 0;
	const TInt per = longForm ? 2 : 4, bits = longForm ? 16 : 8;
	const TUint32 range = longForm ? 0x8000u : 0x80u, mask = longForm ? 0xFFFFu : 0xFFu;
	TInt prev = -1;
	for (TInt i = 0; i < n; i += per)
		{
		const TUint32 r = Get(aNid, VGetConnList, i);
		for (TInt j = 0; j < per && i + j < n; ++j)
			{
			const TUint32 e = (r >> (j * bits)) & mask;
			const TInt v = (TInt)(e & ~range);
			if ((e & range) && prev >= 0)				// a range: prev+1 .. v, each its own connection index
				{
				for (TInt x = prev + 1; x <= v && aW.iNConn < KMaxConn; ++x)
					aW.iConn[aW.iNConn++] = (TUint16)x;
				}
			else if (aW.iNConn < KMaxConn)
				aW.iConn[aW.iNConn++] = (TUint16)v;
			prev = v;
			}
		}
	}

// A depth-first search from an output pin through mixers and selectors to a (non-digital) DAC. aPath receives the
// nodes found, DAC first; aSel[nid] the connection index taken at each node.
TBool THda::FindToDac(TInt aNid, TInt aDepth, TUint8* aPath, TInt& aLen, TUint8* aSel, TUint8* aSeen)
	{
	const SWidget& w = iW[aNid];
	if (w.iType == WDac)
		{
		if (w.iCaps & (1 << 9))
			return EFalse;								// digital
		aPath[aLen++] = (TUint8)aNid;
		return ETrue;
		}
	if (aDepth > 10 || (w.iType != WMixer && w.iType != WSelector && !(w.iType == WPin && aDepth == 0)))
		return EFalse;
	for (TInt i = 0; i < w.iNConn; ++i)
		{
		const TInt c = w.iConn[i];
		if (c > 255 || aSeen[c])
			continue;
		aSeen[c] = 1;
		if (FindToDac(c, aDepth + 1, aPath, aLen, aSel, aSeen))
			{
			aSel[aNid] = (TUint8)i;
			aPath[aLen++] = (TUint8)aNid;
			return ETrue;
			}
		}
	return EFalse;
	}

// The same from an ADC down through selectors / mixers to an input pin (aPath: the pin first, the ADC last).
TBool THda::FindToPin(TInt aNid, TInt aDepth, TUint8* aPath, TInt& aLen, TUint8* aSel, TUint8* aSeen)
	{
	const SWidget& w = iW[aNid];
	if (w.iType == WPin)
		{
		if (aDepth == 0 || !(w.iPinCaps & (1 << 5)) || (w.iCaps & (1 << 9)))
			return EFalse;
		const TUint32 cfg = w.iPinCfg;
		const TInt dev = (cfg >> 20) & 0xF;
		if (((cfg >> 30) & 3) == 1 || (dev != DevMicIn && dev != DevLineIn))
			return EFalse;
		aPath[aLen++] = (TUint8)aNid;
		return ETrue;
		}
	if (aDepth > 10 || (w.iType != WMixer && w.iType != WSelector && !(w.iType == WAdc && aDepth == 0)))
		return EFalse;
	for (TInt i = 0; i < w.iNConn; ++i)
		{
		const TInt c = w.iConn[i];
		if (c > 255 || aSeen[c])
			continue;
		aSeen[c] = 1;
		if (FindToPin(c, aDepth + 1, aPath, aLen, aSel, aSeen))
			{
			aSel[aNid] = (TUint8)i;
			aPath[aLen++] = (TUint8)aNid;
			return ETrue;
			}
		}
	return EFalse;
	}

// Merge a found path into the node list. A selector (or pin) takes one input: a conflicting choice rejects the
// path; a mixer may carry several.
TBool THda::AddPath(TUint8* aNodes, TUint8* aSel, TUint32* aMask, TInt& aN, const TUint8* aPath, TInt aLen,
					const TUint8* aSelByNid)
	{
	for (TInt k = 0; k < aLen; ++k)
		{
		const TInt nid = aPath[k];
		const TInt t = iW[nid].iType;
		if (t != WSelector && t != WPin && t != WAdc)
			continue;
		for (TInt i = 0; i < aN; ++i)
			if (aNodes[i] == nid && aSel[i] != aSelByNid[nid])
				return EFalse;
		}
	for (TInt k = 0; k < aLen; ++k)
		{
		const TInt nid = aPath[k];
		TInt at = -1;
		for (TInt i = 0; i < aN; ++i)
			if (aNodes[i] == nid)
				at = i;
		if (at < 0)
			{
			if (aN >= KMaxNodes)
				return EFalse;
			at = aN++;
			aNodes[at] = (TUint8)nid;
			aSel[at] = aSelByNid[nid];
			aMask[at] = 0;
			}
		const SWidget& w = iW[nid];
		if (w.iType == WMixer || w.iType == WSelector || w.iType == WPin || w.iType == WAdc)
			aMask[at] |= 1u << (aSelByNid[nid] & 31);
		}
	return ETrue;
	}

static inline TInt PinDevice(TUint32 aCfg)	{ return (aCfg >> 20) & 0xF; }
static inline TInt PinConnectivity(TUint32 aCfg)	{ return (aCfg >> 30) & 3; }	// 0 jack, 1 none, 2 fixed, 3 both

void THda::SelectOutputs()
	{
	iNOut = 0;
	// candidates: output-capable, connected, analog pins that are line-out / speaker / headphone, in that order
	// (then by default association and sequence)
	TInt cand[64], key[64], nc = 0;
	for (TInt n = 0; n < 256 && nc < 64; ++n)
		{
		const SWidget& w = iW[n];
		if (w.iType != WPin || !(w.iCaps) || !(w.iPinCaps & (1 << 4)) || (w.iCaps & (1 << 9)))
			continue;
		const TInt dev = PinDevice(w.iPinCfg);
		if (PinConnectivity(w.iPinCfg) == 1 || (dev != DevLineOut && dev != DevSpeaker && dev != DevHpOut))
			continue;
		cand[nc] = n;
		key[nc++] = dev << 16 | ((w.iPinCfg >> 4) & 0xF) << 8 | (w.iPinCfg & 0xF);
		}
	for (TInt i = 1; i < nc; ++i)						// insertion sort by key
		{
		const TInt c = cand[i], k = key[i];
		TInt j = i - 1;
		for (; j >= 0 && key[j] > k; --j)
			{
			cand[j + 1] = cand[j];
			key[j + 1] = key[j];
			}
		cand[j + 1] = c;
		key[j + 1] = k;
		}
	for (TInt i = 0; i < nc; ++i)
		{
		TUint8 path[16], sel[256], seen[256];
		TInt len = 0;
		memclr(sel, sizeof(sel));
		memclr(seen, sizeof(seen));
		seen[cand[i]] = 1;
		if (FindToDac(cand[i], 0, path, len, sel, seen))
			{
			if (AddPath(iOutNodes, iOutSel, iOutMask, iNOut, path, len, sel))
				Kern::Printf("[hda] output: pin %02x (%s, cfg %08x) <- DAC %02x", cand[i],
							 PinDevice(iW[cand[i]].iPinCfg) == DevSpeaker ? "speaker" :
							 PinDevice(iW[cand[i]].iPinCfg) == DevHpOut ? "headphone" : "line-out",
							 iW[cand[i]].iPinCfg, path[0]);
			}
		}
	}

void THda::SelectInput()
	{
	iNIn = 0;
	TInt best = -1, bestKey = 0x7FFFFFFF;
	TUint8 bpath[16], bsel[256];
	TInt blen = 0;
	for (TInt n = 0; n < 256; ++n)
		{
		if (iW[n].iType != WAdc || !iW[n].iCaps || (iW[n].iCaps & (1 << 9)))
			continue;
		TUint8 path[16], sel[256], seen[256];
		TInt len = 0;
		memclr(sel, sizeof(sel));
		memclr(seen, sizeof(seen));
		seen[n] = 1;
		if (!FindToPin(n, 0, path, len, sel, seen))
			continue;
		const TInt dev = PinDevice(iW[path[0]].iPinCfg);
		const TInt k = (dev == DevMicIn ? 0 : 1) << 8 | (iW[path[0]].iPinCfg & 0xFF);
		if (k < bestKey)
			{
			bestKey = k;
			best = n;
			blen = len;
			for (TInt i = 0; i < len; ++i)
				bpath[i] = path[i];
			memcpy(bsel, sel, sizeof(sel));
			}
		}
	if (best >= 0 && AddPath(iInNodes, iInSel, iInMask, iNIn, bpath, blen, bsel))
		{
		iRecNode = best;
		iHasIn = ETrue;
		Kern::Printf("[hda] input: ADC %02x <- pin %02x (%s, cfg %08x)", best, bpath[0],
					 PinDevice(iW[bpath[0]].iPinCfg) == DevMicIn ? "microphone" : "line-in", iW[bpath[0]].iPinCfg);
		}
	}

// Amplifier parameters of a widget (its own when it overrides, else the function group's). Returns the caps word.
TInt THda::AmpSteps(TInt aNid, TBool aOut, TInt& aOffset, TInt& aNum, TInt& aStep, TBool& aMuteCap)
	{
	const SWidget& w = iW[aNid];
	const TBool present = (w.iCaps & (aOut ? 1 << 2 : 1 << 1)) != 0;
	if (!present)
		return 0;
	TUint32 c = aOut ? w.iAmpOut : w.iAmpIn;
	if (!(w.iCaps & (1 << 3)))
		c = aOut ? iAfgAmpOut : iAfgAmpIn;
	aOffset = c & 0x7F;
	aNum = (c >> 8) & 0x7F;
	aStep = ((c >> 16) & 0x7F) + 1;						// in 0.25 dB
	aMuteCap = (c >> 31) & 1;
	return 1;
	}

void THda::SetupOutput()
	{
	for (TInt i = 0; i < iNOut; ++i)
		{
		const TInt nid = iOutNodes[i];
		const SWidget& w = iW[nid];
		if (w.iCaps & (1 << 10))
			Set12(nid, VSetPower, 0);					// D0
		TInt off = 0, num = 0, step = 1;
		TBool mc = EFalse;
		if (AmpSteps(nid, ETrue, off, num, step, mc) && nid != iVolNode)
			SetAmp(nid, ETrue, 0, EFalse, off);			// 0 dB, un-muted (the volume node is set by ApplyVolume)
		if (AmpSteps(nid, EFalse, off, num, step, mc))
			for (TInt c = 0; c < w.iNConn; ++c)			// input amplifiers: the connection on the path open, the others closed
				SetAmp(nid, EFalse, c, !(iOutMask[i] & (1u << c)), off);
		switch (w.iType)
			{
		case WDac:
			Set4(nid, VSetFormat, iTx.iFmt);
			Set12(nid, VSetStream, KTxTag << 4);		// stream tag, channel 0
			break;
		case WPin:
			Set12(nid, VSetPinCtl, PinDevice(w.iPinCfg) == DevHpOut ? 0xC0 : 0x40);	// output enable (+ headphone amp)
			if (w.iPinCaps & (1 << 16))
				Set12(nid, VSetEapd, 0x2);				// external amplifier on
			if (w.iNConn > 1)
				Set12(nid, VSetConnSel, iOutSel[i]);
			break;
		case WSelector:
			Set12(nid, VSetConnSel, iOutSel[i]);
			break;
		default:
			break;
			}
		}
	ApplyVolume();
	}

void THda::SetupInput()
	{
	if (!iHasIn)
		return;
	for (TInt i = 0; i < iNIn; ++i)
		{
		const TInt nid = iInNodes[i];
		const SWidget& w = iW[nid];
		if (w.iCaps & (1 << 10))
			Set12(nid, VSetPower, 0);
		TInt off = 0, num = 0, step = 1;
		TBool mc = EFalse;
		if (AmpSteps(nid, ETrue, off, num, step, mc))
			SetAmp(nid, ETrue, 0, EFalse, off);
		if (AmpSteps(nid, EFalse, off, num, step, mc) && nid != iRecNode)
			for (TInt c = 0; c < w.iNConn; ++c)
				SetAmp(nid, EFalse, c, !(iInMask[i] & (1u << c)), off);
		switch (w.iType)
			{
		case WAdc:
			Set4(nid, VSetFormat, iRx.iFmt);
			Set12(nid, VSetStream, KRxTag << 4);
			if (w.iNConn > 1)
				Set12(nid, VSetConnSel, iInSel[i]);
			break;
		case WPin:
			{
			TUint32 ctl = 0x20;							// input enable
			if (PinDevice(w.iPinCfg) == DevMicIn)		// microphone bias: 80% else 50% of Vref when the pin offers it
				ctl |= (w.iPinCaps & (1 << 12)) ? 4 : (w.iPinCaps & (1 << 9)) ? 1 : 0;
			Set12(nid, VSetPinCtl, ctl);
			break;
			}
		case WSelector:
			Set12(nid, VSetConnSel, iInSel[i]);
			break;
		default:
			break;
			}
		}
	ApplyRecLevel();
	}

// 0..255, 255 = full, each step below 0.5 dB. The play volume goes to the first output amplifier on the path that has
// gain steps; a path with none is attenuated in software as the data is copied into the ring.
void THda::ApplyVolume()
	{
	iSwGain = 256;
	if (iVolNode < 0)
		{
		iSwGain = iVolume <= 0 ? 0 : (iVolume * 256) / 255;
		return;
		}
	TInt off = 0, num = 0, step = 1;
	TBool mc = EFalse;
	AmpSteps(iVolNode, ETrue, off, num, step, mc);
	if (iVolume <= 0)
		{
		if (mc)
			SetAmp(iVolNode, ETrue, 0, ETrue, 0);
		else
			{
			SetAmp(iVolNode, ETrue, 0, EFalse, 0);		// the lowest step, and silence in software
			iSwGain = 0;
			}
		return;
		}
	const TInt down = ((255 - iVolume) * 2 + step - 1) / step;	// steps below 0 dB
	TInt idx = off - down;
	if (idx < 0)
		idx = 0;
	SetAmp(iVolNode, ETrue, 0, EFalse, idx);
	}

// the converters take the stream format (SetConfig); the streams themselves take it when prepared
void THda::ApplyFormats()
	{
	for (TInt i = 0; i < iNOut; ++i)
		if (iW[iOutNodes[i]].iType == WDac)
			Set4(iOutNodes[i], VSetFormat, iTx.iFmt);
	for (TInt i = 0; i < iNIn; ++i)
		if (iW[iInNodes[i]].iType == WAdc)
			Set4(iInNodes[i], VSetFormat, iRx.iFmt);
	}

void THda::ApplyRecLevel()
	{
	if (iRecNode < 0)
		return;
	TInt off = 0, num = 0, step = 1;
	TBool mc = EFalse;
	if (!AmpSteps(iRecNode, EFalse, off, num, step, mc))
		return;
	// the ADC's input amplifier, on the connection on the path
	TInt c = 0;
	for (TInt i = 0; i < iNIn; ++i)
		if (iInNodes[i] == iRecNode)
			c = iInSel[i];
	if (iRecLevel <= 0 && mc)
		{
		SetAmp(iRecNode, EFalse, c, ETrue, 0);
		return;
		}
	const TInt down = ((255 - iRecLevel) * 2 + step - 1) / step;
	TInt idx = off - down;
	if (idx < 0)
		idx = 0;
	SetAmp(iRecNode, EFalse, c, EFalse, idx);
	}

void THda::DumpGraph()
	{
	static const char* const KTypes[] = { "DAC", "ADC", "MIX", "SEL", "PIN", "PWR", "VOL", "BEEP" };
	for (TInt n = 0; n < 256; ++n)
		{
		const SWidget& w = iW[n];
		if (!w.iCaps)
			continue;
		Kern::Printf("[hda]   nid %02x %s caps %08x%s%s ampout %08x ampin %08x", n,
					 w.iType < 8 ? KTypes[w.iType] : "???", w.iCaps, (w.iCaps & (1 << 9)) ? " digital" : "",
					 (w.iCaps & (1 << 10)) ? " pwr" : "", w.iAmpOut, w.iAmpIn);
		if (w.iType == WPin)
			Kern::Printf("[hda]          pincfg %08x pincaps %08x", w.iPinCfg, w.iPinCaps);
		if (w.iType == WDac || w.iType == WAdc)
			Kern::Printf("[hda]          pcm %08x", w.iPcm);
		for (TInt i = 0; i < w.iNConn; ++i)
			Kern::Printf("[hda]          conn[%d] = %02x", i, w.iConn[i]);
		}
	}

TInt THda::ProbeCodec(TInt aCad)
	{
	iCad = aCad;
	TUint32 v;
	if (!Cmd(0, VGetParam << 8 | PVendor, v))
		return KErrTimedOut;
	iVendor = v;
	const TUint32 sub = Param(0, PSubNodes);
	TInt afg = -1;
	for (TInt fg = (sub >> 16) & 0xFF, e = fg + (sub & 0xFF); fg < e && afg < 0; ++fg)
		if ((Param(fg, PFgType) & 0xFF) == 1)
			afg = fg;
	Kern::Printf("[hda] codec %d: vendor/device %08x, rev %08x, audio function group %d", aCad, iVendor, Param(0, 2), afg);
	if (afg < 0)
		return KErrNotFound;
	iAfg = afg;
	Set12(afg, VSetPower, 0);							// D0
	for (TInt t = 0; t < 200; ++t)
		{
		const TUint32 p = Get(afg, VGetPower, 0);
		if (((p >> 4) & 0xF) == (p & 0xF))
			break;
		Delay(200);
		}
	iAfgAmpOut = Param(afg, POutAmpCaps);
	iAfgAmpIn = Param(afg, PInAmpCaps);
	iAfgPcm = Param(afg, PPcmRates);
	memclr(iW, 256 * sizeof(SWidget));
	const TUint32 ws = Param(afg, PSubNodes);
	const TInt first = (ws >> 16) & 0xFF, count = ws & 0xFF;
	for (TInt n = first; n < first + count && n < 256; ++n)
		{
		SWidget& w = iW[n];
		w.iCaps = Param(n, PWidgetCaps);
		if (!w.iCaps)
			continue;
		w.iType = (TUint8)((w.iCaps >> 20) & 0xF);
		if (w.iCaps & (1 << 2))
			w.iAmpOut = Param(n, POutAmpCaps);
		if (w.iCaps & (1 << 1))
			w.iAmpIn = Param(n, PInAmpCaps);
		if (w.iCaps & (1 << 8))
			ReadConn(n, w);
		if (w.iType == WPin)
			{
			w.iPinCaps = Param(n, PPinCaps);
			w.iPinCfg = Get(n, VGetPinCfg, 0);
			}
		if (w.iType == WDac || w.iType == WAdc)
			w.iPcm = (w.iCaps & (1 << 4)) ? Param(n, PPcmRates) : iAfgPcm;
		}
#if XK_HDA_DUMP
	DumpGraph();
#endif
	SelectOutputs();
	if (!iNOut)
		return KErrNotFound;
	SelectInput();
	return KErrNone;
	}

// The rates (KSoundRate*Hz) a converter's PCM size/rate parameter offers: bits 0..6 = 8, 11.025, 16, 22.05, 32,
// 44.1, 48 kHz; 16-bit samples = bit 17.
static TUint32 RatesFromPcm(TUint32 aPcm)
	{
	if (!(aPcm & (1u << 17)))
		return 0;
	TUint32 r = 0;
	if (aPcm & 0x01) r |= KSoundRate8000Hz;
	if (aPcm & 0x02) r |= KSoundRate11025Hz;
	if (aPcm & 0x04) r |= KSoundRate16000Hz;
	if (aPcm & 0x08) r |= KSoundRate22050Hz;
	if (aPcm & 0x10) r |= KSoundRate32000Hz;
	if (aPcm & 0x20) r |= KSoundRate44100Hz;
	if (aPcm & 0x40) r |= KSoundRate48000Hz;
	return r;
	}

// The stream-format word (SDnFMT = the converter's format verb) for 16-bit stereo at aHz: base rate, multiplier,
// divisor (spec 3.7.1). -1 for a rate the format cannot express as one of the supported ones.
TInt THda::FormatFor(TInt aHz) const
	{
	switch (aHz)
		{
	case 48000:	return 0x0011;
	case 44100:	return 0x4011;
	case 32000:	return 0x0A11;							// 48 kHz x 2 / 3
	case 22050:	return 0x4111;							// 44.1 kHz / 2
	case 16000:	return 0x0211;							// 48 kHz / 3
	case 11025:	return 0x4311;							// 44.1 kHz / 4
	case 8000:	return 0x0511;							// 48 kHz / 6
	default:	return -1;
		}
	}

// ---- init ---------------------------------------------------------------------------------------------

TInt THda::Init()
	{
	if (!XkPciFind(0, 0x0403, iLoc, 15))
		return KErrNotFound;
	const TInt b = iLoc.iBus, d = iLoc.iDev, f = iLoc.iFn;
	iPciId = XkPciRead(b, d, f, 0);
	const TUint32 bar0 = XkPciRead(b, d, f, 0x10);
	if ((bar0 & 1) || ((bar0 & 6) == 4 && XkPciRead(b, d, f, 0x14) != 0))
		return KErrNotSupported;						// an I/O BAR, or a BAR above 4 GB (TPhysAddr is 32 bit here)
	iNoCorbAck = (iPciId & 0xFFFF) == 0x15AD;
	XkPciWrite(b, d, f, 0x04, (XkPciRead(b, d, f, 0x04) | 0x6) & ~(1u << 10));	// memory decode + bus master + INTx enabled
	if ((iPciId & 0xFFFF) == 0x8086)					// Intel: traffic class 0 for all streams (TCSEL, config 0x44 low bits)
		XkPciWrite(b, d, f, 0x44, XkPciRead(b, d, f, 0x44) & ~7u);
	DPlatChunkHw* mmio = NULL;
	TInt r = DPlatChunkHw::New(mmio, bar0 & ~0xFu, 0x4000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iMmio = mmio->LinearAddress();
	const TUint16 gcap = R16(EGcap);
	iOss = (gcap >> 12) & 0xF;
	iIss = (gcap >> 8) & 0xF;
	iBss = (gcap >> 3) & 0x1F;
	if (!iOss)
		return KErrNotSupported;
	// DMA memory: the command rings and the descriptor lists in one page, the two audio rings contiguous and uncached
	iW = (SWidget*)Kern::Alloc(256 * sizeof(SWidget));
	iScratch = (TUint8*)Kern::Alloc(KTxPeriods * KPeriod);
	if (!iW || !iScratch)
		return KErrNoMemory;
	r = Epoc::AllocPhysicalRam(0x1000, iCtlPhys);
	DPlatChunkHw* ch = NULL;
	if (r == KErrNone)
		r = DPlatChunkHw::New(ch, iCtlPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iCtl = (TUint8*)ch->LinearAddress();
	memclr(iCtl, 0x1000);
	iCorb = (volatile TUint32*)iCtl;
	iRirb = (volatile TUint32*)(iCtl + 0x400);
	SStream* ss[2] = { &iTx, &iRx };
	for (TInt k = 0; k < 2; ++k)
		{
		SStream& s = *ss[k];
		memclr(&s, sizeof(s));
		s.iIn = (k == 1);
		s.iTag = s.iIn ? KRxTag : KTxTag;
		s.iPeriods = s.iIn ? KRxPeriods : KTxPeriods;
		s.iBytes = s.iPeriods * KPeriod;
		s.iSd = s.iIn ? 0 : iIss;
		s.iFmt = 0x0011;
		s.iBdlPhys = iCtlPhys + (s.iIn ? 0xE00 : 0xC00);
		s.iBdl = (volatile SBdle*)(iCtl + (s.iIn ? 0xE00 : 0xC00));
		r = Epoc::AllocPhysicalRam(Kern::RoundToPageSize(s.iBytes), s.iRingPhys);
		DPlatChunkHw* rc = NULL;
		if (r == KErrNone)
			r = DPlatChunkHw::New(rc, s.iRingPhys, Kern::RoundToPageSize(s.iBytes), EMapAttrSupRw | EMapAttrFullyBlocking);
		if (r != KErrNone)
			return r;
		s.iRing = (TUint8*)rc->LinearAddress();
		memclr(s.iRing, s.iBytes);
		}
	_LIT(KAudioDfc, "HDA DFC");
	r = Kern::DynamicDfcQCreate(iDfcQ, 27, KAudioDfc);
	if (r != KErrNone)
		return r;
	iDoneDfc.SetDfcQ(iDfcQ);
	iIdleDfc.SetDfcQ(iDfcQ);
	iSelfDfc.SetDfcQ(iDfcQ);
	iVolume = 255;
	iRecLevel = 255;
	iVolNode = iRecNode = -1;
	iTxHz = iRxHz = 48000;

	r = ResetController();
	Kern::Printf("[hda] %08x at %02x:%02x.%x, MMIO %08x, v%d.%d, %d input / %d output / %d bidirectional streams, codecs %04x%s",
				 iPciId, b, d, f, bar0 & ~0xFu, R8(EVmaj), R8(EVmin), iIss, iOss, iBss, R16(EStatests),
				 iNoCorbAck ? " (no CORBRP ack)" : "");
	if (r != KErrNone)
		return r;
	SetupRings();
	TInt found = KErrNotFound;
	const TUint16 present = R16(EStatests);
	for (TInt cad = 0; cad < 15 && found != KErrNone; ++cad)
		if (present & (1 << cad))
			found = ProbeCodec(cad);
	if (found != KErrNone)
		{
		Kern::Printf("[hda] no analog output path (%d); widget graph of the last codec:", found);
#if !XK_HDA_DUMP
		DumpGraph();
#endif
		return found;
		}
	// the play volume rides on the first output amplifier with gain steps on a path: the DAC's first
	for (TInt pass = 0; pass < 2 && iVolNode < 0; ++pass)
		for (TInt i = 0; i < iNOut && iVolNode < 0; ++i)
			{
			TInt off, num, step;
			TBool mc;
			if ((iW[iOutNodes[i]].iType == WDac) == (pass == 0) && AmpSteps(iOutNodes[i], ETrue, off, num, step, mc) && num > 0)
				iVolNode = iOutNodes[i];
			}
	iTxRates = iRxRates = 0;
	for (TInt i = 0; i < iNOut; ++i)
		if (iW[iOutNodes[i]].iType == WDac)
			iTxRates = iTxRates ? (iTxRates & RatesFromPcm(iW[iOutNodes[i]].iPcm)) : RatesFromPcm(iW[iOutNodes[i]].iPcm);
	if (!(iTxRates & KSoundRate48000Hz) && !(iTxRates & KSoundRate44100Hz))
		iTxRates = KSoundRate48000Hz | KSoundRate44100Hz;	// parameter unreadable or odd: assume the mandatory rates
	if (iHasIn && iIss > 0)
		{
		iRxRates = RatesFromPcm(iW[iRecNode].iPcm);
		if (!iRxRates)
			iRxRates = KSoundRate48000Hz | KSoundRate44100Hz;
		}
	else
		iHasIn = EFalse;
	Kern::Printf("[hda] play volume %s, record %s, TX rates %08x", iVolNode >= 0 ? "in the codec amp" : "in software",
				 iHasIn ? "present" : "absent", iTxRates);
	SetupOutput();
	SetupInput();
	iPowered = ETrue;
	// the interrupt: the stream IOC interrupts, with the poll timer as the safety net
	const TInt line = XkPciRead(b, d, f, 0x3C) & 0xFF;
	iIrqId = (line > 0 && line < 16) ? (EX64IrqPciRouted | line) : -1;
	TInt h = iIrqId >= 0 ? Interrupt::Bind(iIrqId, Isr, this) : KErrNotSupported;
	if (h >= 0)
		Interrupt::Enable(h);
	Kern::Printf("[hda] ready: irq %d (bind %d), TX stream %d, RX stream %d", line, h, iTx.iSd, iHasIn ? iRx.iSd : -1);
	return KErrNone;
	}

// ---- power -----------------------------------------------------------------------------------------------

void THda::EnsurePowered()
	{
	iIdleTimer.Cancel();
	if (iPowered)
		return;
	Set12(iAfg, VSetPower, 0);
	for (TInt t = 0; t < 500; ++t)
		{
		const TUint32 p = Get(iAfg, VGetPower, 0);
		if (((p >> 4) & 0xF) == (p & 0xF))
			break;
		Delay(200);
		}
	SetupOutput();										// the D3 state may have dropped amplifier / pin settings
	SetupInput();
	iPowered = ETrue;
	}

void THda::IdleDown()
	{
	if (iTx.iPrepared || iRx.iPrepared || iSelfActive || !iPowered)
		return;
	Set12(iAfg, VSetPower, 3);							// D3
	iPowered = EFalse;
	}

void THda::ArmIdle()
	{
	iIdleTimer.OneShot(NKern::TimerTicks(KIdleHoldOffMs));
	}

void THda::IdleTimerFn(TAny* aPtr)	{ ((THda*)aPtr)->iIdleDfc.Add(); }
void THda::IdleDfcFn(TAny* aPtr)	{ ((THda*)aPtr)->IdleDown(); }
void THda::SelfDfcFn(TAny* aPtr)	{ ((THda*)aPtr)->SelfFeed(); }

// ---- streams ---------------------------------------------------------------------------------------------

void THda::SdReset(TInt aSd)
	{
	const TInt o = Sd(aSd);
	W32(o + ESdCtl, KSdSrst);
	WaitBits(o + ESdCtl, KSdSrst, KSdSrst, 1000);
	W32(o + ESdCtl, 0);
	WaitBits(o + ESdCtl, KSdSrst, 0, 1000);
	}

// Program a stream to start at ring offset 0: reset, the descriptor list (one entry per period, IOC on each), the
// cyclic buffer length, last valid index, format. RUN is NOT set -- a stream started empty plays the previous contents.
TInt THda::PrepareStream(SStream& s)
	{
	const TInt o = Sd(s.iSd);
	SdReset(s.iSd);
	for (TInt i = 0; i < s.iPeriods; ++i)
		{
		s.iBdl[i].iAddrLo = s.iRingPhys + i * KPeriod;
		s.iBdl[i].iAddrHi = 0;
		s.iBdl[i].iLen = KPeriod;
		s.iBdl[i].iFlags = 1;
		}
	W32(o + ESdBdpl, s.iBdlPhys);
	W32(o + ESdBdpu, 0);
	W32(o + ESdCbl, s.iBytes);
	W16(o + ESdLvi, (TUint16)(s.iPeriods - 1));
	W16(o + ESdFmt, s.iFmt);
	W8(o + ESdSts, KSdStsBcis | KSdStsFifoe | KSdStsDese);
	W32(o + ESdCtl, (TUint32)s.iTag << 20 | KSdIoce | KSdFeie | KSdDeie);
	s.iDma = s.iPos = s.iZero = 0;
	s.iLastLpib = 0;
	s.iQHead = s.iQCount = 0;
	s.iErr = 0;
	s.iPrepared = ETrue;
	s.iPlaying = EFalse;
	return KErrNone;
	}

void THda::RunStream(SStream& s)
	{
	const TInt o = Sd(s.iSd);
	W32(EIntctl, R32(EIntctl) | KIntGie | (1u << s.iSd));
	W32(o + ESdCtl, R32(o + ESdCtl) & 0xFFFFFF | KSdRun);
	s.iRunning = ETrue;
	s.iPlaying = ETrue;
	iPoll.OneShot(NKern::TimerTicks(10));
	}

// RUN off (and wait for the hardware to say so); aReset also resets the stream (SRST) and masks its interrupt
void THda::HaltStream(SStream& s, TBool aReset)
	{
	const TInt o = Sd(s.iSd);
	W32(o + ESdCtl, R32(o + ESdCtl) & 0xFFFFFF & ~KSdRun);
	WaitBits(o + ESdCtl, KSdRun, 0, 1000);
	s.iRunning = EFalse;
	if (aReset)
		{
		W32(EIntctl, R32(EIntctl) & ~(1u << s.iSd));
		SdReset(s.iSd);
		s.iPrepared = EFalse;
		s.iPlaying = EFalse;
		}
	}

// The running DMA byte count from the link position (LPIB wraps at the cyclic buffer length)
void THda::UpdatePos(SStream& s)
	{
	TUint32 lp = R32(Sd(s.iSd) + ESdLpib);
	if (lp >= (TUint32)s.iBytes)
		lp -= s.iBytes;
	const TUint32 d = lp >= s.iLastLpib ? lp - s.iLastLpib : lp + s.iBytes - s.iLastLpib;
	s.iLastLpib = lp;
	s.iDma += d;
	}

// Everything behind the DMA position is stale audio: zero it, so an underrun replays silence
void THda::ZeroPlayed(SStream& s)
	{
	if (s.iDma - s.iZero > (TUint64)s.iBytes)
		s.iZero = s.iDma - s.iBytes;
	while (s.iZero < s.iDma)
		{
		const TInt off = (TInt)(s.iZero % s.iBytes);
		const TInt n = (TInt)Min((TUint64)(s.iBytes - off), s.iDma - s.iZero);
		memclr(s.iRing + off, n);
		s.iZero += n;
		}
	}

void THda::CopyIn(TUint8* aDst, const TUint8* aSrc, TInt aBytes)
	{
	if (iSwGain >= 256)
		{
		memcpy(aDst, aSrc, aBytes);
		return;
		}
	const TInt16* s = (const TInt16*)aSrc;
	TInt16* d = (TInt16*)aDst;
	for (TInt i = 0; i < aBytes / 2; ++i)
		d[i] = (TInt16)((s[i] * iSwGain) >> 8);
	}

// ---- playback -----------------------------------------------------------------------------------------

TInt THda::TxWrite(TUint aId, const TUint8* aSrc, TInt aBytes)
	{
	SStream& s = iTx;
	if (aBytes <= 0 || aBytes > s.iBytes - KLead)
		return KErrArgument;
	if (!s.iPrepared)
		{
		EnsurePowered();
		PrepareStream(s);
		}
	if (s.iPlaying)
		{
		UpdatePos(s);
		ZeroPlayed(s);
		}
	if (s.iQCount >= KQ)
		return KErrNotReady;
	if (s.iPlaying && !s.iQCount && s.iPos < s.iDma + KLead)	// an underrun happened: start the new burst a little ahead
		s.iPos = (s.iDma + KLead + 3) & ~(TUint64)3;
	if (s.iPos + aBytes > s.iDma + s.iBytes)
		return KErrNotReady;							// the ring has no room for it yet
	for (TInt done = 0; done < aBytes; )
		{
		const TInt off = (TInt)((s.iPos + done) % s.iBytes);
		const TInt n = Min(aBytes - done, s.iBytes - off);
		CopyIn(s.iRing + off, aSrc + done, n);
		done += n;
		}
	SPiece& p = s.iQ[(s.iQHead + s.iQCount) % KQ];
	p.iId = aId;
	p.iBytes = aBytes;
	s.iPos += aBytes;
	p.iEnd = s.iPos;
	++s.iQCount;
	if (!s.iPlaying)
		{
		RunStream(s);									// the first data is in the ring: now the DMA may start
		}
	return KErrNone;
	}

void THda::TxStop()
	{
	SStream& s = iTx;
	if (s.iPrepared || s.iRunning)
		HaltStream(s, ETrue);
	s.iQHead = s.iQCount = 0;						// the LDD has given up on them: no callbacks
	memclr(s.iRing, s.iBytes);
	iPoll.Cancel();
	ArmIdle();
	}

void THda::TxPause()
	{
	SStream& s = iTx;
	if (!s.iRunning)
		return;
	UpdatePos(s);
	HaltStream(s, EFalse);
	}

// Resume from the next sample not yet played. The hardware may or may not keep its place in the descriptor list
// across RUN=0/1, so the unplayed audio is moved to the start of the ring and the stream restarted from there.
void THda::TxResume()
	{
	SStream& s = iTx;
	if (!s.iPlaying || s.iRunning)
		return;
	ZeroPlayed(s);
	const TInt unplayed = (TInt)(s.iPos - s.iDma);
	for (TInt done = 0; done < unplayed; )
		{
		const TInt off = (TInt)((s.iDma + done) % s.iBytes);
		const TInt n = Min(unplayed - done, s.iBytes - off);
		memcpy(iScratch + done, s.iRing + off, n);
		done += n;
		}
	memclr(s.iRing, s.iBytes);
	const TUint64 base = s.iDma;
	const TInt qh = s.iQHead, qc = s.iQCount;
	SPiece keep[KQ];
	for (TInt i = 0; i < qc; ++i)
		keep[i] = s.iQ[(qh + i) % KQ];
	PrepareStream(s);									// resets the stream and the counters
	memcpy(s.iRing, iScratch, unplayed);
	for (TInt i = 0; i < qc; ++i)
		{
		s.iQ[i] = keep[i];
		s.iQ[i].iEnd -= base;
		}
	s.iQCount = qc;
	s.iPos = unplayed;
	RunStream(s);
	}

void THda::TxDfc()
	{
	SStream& s = iTx;
	if (!s.iPlaying)
		return;
	UpdatePos(s);
	ZeroPlayed(s);
	if (s.iErr & KSdStsDese)
		{
		s.iErr = 0;
		Kern::Printf("[hda] play stream descriptor error");
		if (s.iQCount && iTxPdd)
			{
			SPiece p = s.iQ[s.iQHead];
			s.iQHead = (s.iQHead + 1) % KQ;
			--s.iQCount;
			Ldd_PlayCallback(iTxPdd, p.iId, KErrGeneral, 0);
			}
		}
	s.iErr = 0;
	while (s.iQCount && s.iQ[s.iQHead].iEnd <= s.iDma)
		{
		const SPiece p = s.iQ[s.iQHead];				// pop first: the callback can queue the next piece
		s.iQHead = (s.iQHead + 1) % KQ;
		--s.iQCount;
		if (iTxPdd)
			Ldd_PlayCallback(iTxPdd, p.iId, KErrNone, p.iBytes);
		else if (iSelfActive)
			{
			++iSelfDone;
			if (iSelfDone == iSelfId && iSelfOff >= iSelfTotal)
				{
				Kern::Printf("[hda] self-test: all pieces played (%d)", iSelfDone);
				iSelfActive = EFalse;
				TxStop();
				return;
				}
			}
		}
	if (iSelfActive)
		SelfFeed();
	}

// ---- record -----------------------------------------------------------------------------------------------

void THda::RxStart()
	{
	SStream& s = iRx;
	EnsurePowered();
	PrepareStream(s);
	memclr(s.iRing, s.iBytes);
	RunStream(s);
	}

void THda::RxStop()
	{
	SStream& s = iRx;
	if (s.iPrepared || s.iRunning)
		HaltStream(s, ETrue);
	s.iQHead = s.iQCount = 0;
	if (!iTx.iPrepared)
		iPoll.Cancel();
	ArmIdle();
	}

TInt THda::RxQueue(TUint aId, TUint8* aDst, TInt aBytes)
	{
	SStream& s = iRx;
	if (!s.iPrepared)
		return KErrNotReady;
	if (s.iQCount >= KQ)
		return KErrNotReady;
	SPiece& p = s.iQ[(s.iQHead + s.iQCount) % KQ];
	p.iId = aId;
	p.iBytes = aBytes;
	p.iLin = aDst;
	p.iFilled = 0;
	++s.iQCount;
	return KErrNone;
	}

void THda::RxPause()
	{
	SStream& s = iRx;
	if (!s.iPrepared)
		return;
	const TInt partial = s.iQCount ? s.iQ[s.iQHead].iFilled : 0;
	s.iQHead = s.iQCount = 0;
	HaltStream(s, ETrue);
	if (iRxPdd)
		Ldd_RecordCallback(iRxPdd, 0, KErrNone, partial);	// the single completion the LDD expects for a pause
	}

void THda::RxDfc()
	{
	SStream& s = iRx;
	if (!s.iRunning)
		return;
	UpdatePos(s);
	s.iErr = 0;
	if (!s.iQCount)
		{
		s.iPos = s.iDma;								// nobody is listening: the audio just recorded is dropped
		return;
		}
	if (s.iDma - s.iPos > (TUint64)s.iBytes)			// we fell a whole ring behind: skip to the oldest valid audio
		s.iPos = s.iDma - s.iBytes + KPeriod;
	while (s.iQCount && s.iPos < s.iDma)
		{
		SPiece& p = s.iQ[s.iQHead];
		const TInt off = (TInt)(s.iPos % s.iBytes);
		const TInt n = (TInt)Min((TUint64)(p.iBytes - p.iFilled), Min((TUint64)(s.iBytes - off), s.iDma - s.iPos));
		memcpy(p.iLin + p.iFilled, s.iRing + off, n);
		p.iFilled += n;
		s.iPos += n;
		if (p.iFilled == p.iBytes)
			{
			const SPiece done = p;
			s.iQHead = (s.iQHead + 1) % KQ;
			--s.iQCount;
			if (iRxPdd)
				Ldd_RecordCallback(iRxPdd, done.iId, KErrNone, done.iBytes);
			}
		}
	}

// ---- interrupts and completion ---------------------------------------------------------------------------

void THda::Isr(TAny* aPtr)
	{
	THda& h = *(THda*)aPtr;
	const TUint32 is = h.R32(EIntsts);
	TBool any = EFalse;
	SStream* ss[2] = { &h.iTx, &h.iRx };
	for (TInt k = 0; k < 2; ++k)
		{
		SStream& s = *ss[k];
		if (s.iPrepared && (is & (1u << s.iSd)))
			{
			const TInt o = h.Sd(s.iSd) + ESdSts;
			const TUint8 sts = h.R8(o);
			h.W8(o, sts & (KSdStsBcis | KSdStsFifoe | KSdStsDese));	// write-1-to-clear
			s.iErr |= sts & (KSdStsFifoe | KSdStsDese);
			any = ETrue;
			}
		}
	if (any)
		h.iDoneDfc.Add();
	}

void THda::PollFn(TAny* aPtr)
	{
	THda& h = *(THda*)aPtr;
	h.iDoneDfc.Add();
	if (h.iTx.iRunning || h.iRx.iRunning)
		h.iPoll.Again(NKern::TimerTicks(10));
	}

void THda::DoneDfcFn(TAny* aPtr)
	{
	THda& h = *(THda*)aPtr;
	h.TxDfc();
	h.RxDfc();
	}

// ---- the PDD -------------------------------------------------------------------------------------------

class DHdaPdd : public DSoundScPdd
	{
public:
	DHdaPdd(TInt aUnit) : iUnit(aUnit) {}
	~DHdaPdd()
		{
		if (!TheHda)
			return;
		if (iUnit == KSoundScTxUnit0 && TheHda->iTxPdd == this)
			{
			TheHda->TxStop();
			TheHda->iTxPdd = NULL;
			}
		else if (iUnit == KSoundScRxUnit0 && TheHda->iRxPdd == this)
			{
			TheHda->RxStop();
			TheHda->iRxPdd = NULL;
			}
		}
	virtual TDfcQue* DfcQ(TInt)				{ return TheHda->iDfcQ; }
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
		caps.iRates = (iUnit == KSoundScTxUnit0 || !TheHda->iHasIn) ? TheHda->iTxRates : TheHda->iRxRates;
		caps.iEncodings = KSoundEncoding16BitPCM;
		caps.iDataFormats = KSoundDataFormatInterleaved;
		caps.iRequestMinSize = 4;
		caps.iRequestAlignment = 2;
		caps.iHwConfigNotificationSupport = EFalse;
		aCapsBuf.FillZ(aCapsBuf.MaxLength());
		TPtrC8 p((const TUint8*)&caps, sizeof(caps));
		aCapsBuf = p.Left(Min(p.Length(), aCapsBuf.MaxLength()));
		}
	virtual TInt MaxTransferLen() const		{ return (KTxPeriods / 2) * KPeriod; }	// half the ring: any piece fits an empty ring
	virtual TInt SetConfig(const TDesC8& aConfigBuf)
		{
		TCurrentSoundFormatV02 c;
		TPtr8 p((TUint8*)&c, sizeof(c));
		Kern::InfoCopy(p, aConfigBuf);
		if (c.iChannels != 2 || c.iEncoding != ESoundEncoding16BitPCM || c.iDataFormat != ESoundDataFormatInterleaved)
			return KErrNotSupported;
		static const TInt KRates[] = { 7350, 8000, 8820, 9600, 11025, 12000, 14700, 16000, 22050, 24000, 29400, 32000,
									   44100, 48000 };
		static const TUint32 KMask[] = { 0, KSoundRate8000Hz, 0, 0, KSoundRate11025Hz, 0, 0, KSoundRate16000Hz,
										 KSoundRate22050Hz, 0, 0, KSoundRate32000Hz, KSoundRate44100Hz, KSoundRate48000Hz };
		if ((TUint)c.iRate >= sizeof(KRates) / sizeof(KRates[0]))
			return KErrNotSupported;
		const TInt fmt = TheHda->FormatFor(KRates[c.iRate]);
		const TUint32 have = (iUnit == KSoundScTxUnit0 || !TheHda->iHasIn) ? TheHda->iTxRates : TheHda->iRxRates;
		if (fmt < 0 || !(KMask[c.iRate] & have))
			return KErrNotSupported;
		if (iUnit == KSoundScTxUnit0)
			TheHda->iTx.iFmt = (TUint16)fmt;
		else if (TheHda->iHasIn)
			TheHda->iRx.iFmt = (TUint16)fmt;
		if (TheHda->iPowered)
			TheHda->ApplyFormats();						// (a powered-down codec gets it from SetupOutput / SetupInput on wake-up)
		return KErrNone;
		}
	virtual TInt SetVolume(TInt aVolume)
		{
		const TInt v = Min(Max(aVolume, 0), 255);
		if (iUnit == KSoundScTxUnit0)
			{
			TheHda->iVolume = v;
			if (TheHda->iPowered)
				TheHda->ApplyVolume();					// (a powered-down codec is programmed with it on wake-up)
			}
		else
			{
			TheHda->iRecLevel = v;
			if (TheHda->iPowered)
				TheHda->ApplyRecLevel();
			}
		return KErrNone;
		}
	virtual TInt StartTransfer()
		{
		TheHda->EnsurePowered();
		if (iUnit == KSoundScTxUnit0)
			{
			TheHda->iTxPdd = this;
			TheHda->PrepareStream(TheHda->iTx);			// programmed; RUN waits for the first data (the AC'97 first-buffer lesson)
			}
		else
			{
			TheHda->iRxPdd = this;
			if (!TheHda->iHasIn)
				return KErrNone;
			TheHda->RxStart();
			}
		return KErrNone;
		}
	virtual TInt TransferData(TUint aId, TLinAddr aLin, TPhysAddr, TInt aBytes)
		{
		if (iUnit == KSoundScTxUnit0)
			return TheHda->TxWrite(aId, (const TUint8*)aLin, aBytes);
		if (!TheHda->iHasIn)
			{
			Ldd()->RecordCallback(aId, KErrNone, 0);	// no input path on this codec: nothing recorded
			return KErrNone;
			}
		return TheHda->RxQueue(aId, (TUint8*)aLin, aBytes);
		}
	virtual void StopTransfer()
		{
		if (iUnit == KSoundScTxUnit0)
			TheHda->TxStop();
		else
			TheHda->RxStop();
		}
	virtual TInt PauseTransfer()
		{
		if (iUnit == KSoundScTxUnit0)
			TheHda->TxPause();
		else if (TheHda->iHasIn)
			TheHda->RxPause();
		else
			Ldd()->RecordCallback(0, KErrNone, 0);
		return KErrNone;
		}
	virtual TInt ResumeTransfer()
		{
		if (iUnit == KSoundScTxUnit0)
			TheHda->TxResume();
		else if (TheHda->iHasIn)
			TheHda->RxStart();							// the LDD queues new record pieces itself
		return KErrNone;
		}
	virtual TInt PowerUp()					{ TheHda->EnsurePowered(); return KErrNone; }
	virtual void PowerDown()				{ TheHda->ArmIdle(); }
	virtual TInt CustomConfig(TInt, TAny*)	{ return KErrNotSupported; }
	void Play(TUint aId, TInt aResult, TInt aBytes)		{ Ldd()->PlayCallback(aId, aResult, aBytes); }
	void Rec(TUint aId, TInt aResult, TInt aBytes)		{ Ldd()->RecordCallback(aId, aResult, aBytes); }
private:
	TInt iUnit;
	};

// (the PDD class is defined after THda, whose DFCs call back into it)
void Ldd_PlayCallback(DHdaPdd* aPdd, TUint aId, TInt aResult, TInt aBytes)	{ aPdd->Play(aId, aResult, aBytes); }
void Ldd_RecordCallback(DHdaPdd* aPdd, TUint aId, TInt aResult, TInt aBytes)	{ aPdd->Rec(aId, aResult, aBytes); }

class DHdaFactory : public DPhysicalDevice
	{
public:
	DHdaFactory()			{ iUnitsMask = (1 << KSoundScTxUnit0) | (1 << KSoundScRxUnit0); iVersion = RSoundSc::VersionRequired(); }
	virtual TInt Install()	{ return SetName(&KHdaPddName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
		{
		aChannel = new DHdaPdd(aUnit);
		return aChannel ? KErrNone : KErrNoMemory;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(RSoundSc::VersionRequired(), aVer) || !TheHda)
			return KErrNotSupported;
		return (aUnit == KSoundScTxUnit0 || aUnit == KSoundScRxUnit0) ? KErrNone : KErrNotSupported;
		}
	};

// ---- the boot self-test -----------------------------------------------------------------------------------

void THda::SelfTest()
	{
	iSelfTotal = 48000 * 4;							// 1 s, 16-bit stereo
	TPhysAddr phys;
	if (Epoc::AllocPhysicalRam(Kern::RoundToPageSize(iSelfTotal), phys) != KErrNone)
		return;
	DPlatChunkHw* ch = NULL;
	if (DPlatChunkHw::New(ch, phys, Kern::RoundToPageSize(iSelfTotal), EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return;
	iSelfBuf = (TUint8*)ch->LinearAddress();
	X64FillSine440((TInt16*)iSelfBuf, iSelfTotal / 4);
	iSelfActive = ETrue;
	iSelfOff = iSelfId = iSelfDone = 0;
	Kern::Printf("[hda] self-test: 1 s of 440 Hz queued through the ring");
	iSelfDfc.Add();										// the feeding runs on the audio DFC thread, like the LDD's calls
	}

void THda::SelfFeed()
	{
	while (iSelfOff < iSelfTotal)
		{
		const TInt n = Min(iSelfTotal - iSelfOff, 8192);
		if (TxWrite(iSelfId, iSelfBuf + iSelfOff, n) != KErrNone)
			break;
		iSelfOff += n;
		++iSelfId;
		}
	}

// ---- entry points (xk_sound.cpp) ---------------------------------------------------------------------------

TInt X64HdaStart()
	{
	NKern::ThreadEnterCS();
	TheHda = new THda;
	TInt r = TheHda ? TheHda->Init() : KErrNoMemory;
	if (r != KErrNone)
		{
		if (r != KErrNotFound)
			Kern::Printf("[hda] controller found but no sound output (%d)", r);
		delete TheHda;
		TheHda = NULL;
		}
	NKern::ThreadLeaveCS();
	return r;
	}

void X64HdaSelfTest()
	{
	if (TheHda)
		TheHda->SelfTest();
	}

DPhysicalDevice* X64HdaNewFactory()
	{
	return TheHda ? new DHdaFactory : NULL;
	}
