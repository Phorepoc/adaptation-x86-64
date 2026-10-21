// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_timesync.cpp -- the Phorepoc amd64 board keeps its clock on the host's time. X4.42.
//
// EKA2 reads the RTC once at boot (X4.31) and from then on counts ticks. In a VM those ticks stop
// while the VM is paused or the host sleeps (and can lag under load), so the guest's clock falls
// behind the real one. What every hypervisor's guest additions do about it is a periodic resync
// against a host clock. Here, every KSyncMs on DfcQue0 (where the VMMDev mouse polling runs too,
// so the two never share its request buffer at once):
//   * VirtualBox: VMMDev GetHostTime (xk_absmouse.cpp, the Guest Additions' own time source), ms
//     since the Unix epoch, UTC;
//   * otherwise: the CMOS clock itself. QEMU's defaults to host time and keeps running while the
//     guest is paused; on a real PC it is the battery clock (User::SetUTCTime writes it, X4.31).
// If the kernel's UTC is off by KSlackSecs or more, Kern::SetSystemTime sets it (hardware RTC too,
// asynchronous change notification: tzserver and the status bar see an ordinary time change).
// The phone equivalent is NITZ (network time from the cell network), and a network board would
// add NTP. The 2 s slack is the CMOS clock's 1 s resolution plus a margin.
#include <kernel/kern_priv.h>
#include "xk_absmouse.h"

TInt X64RtcReadSeconds(TInt& aSecondsFrom2000);			// xk_entry.cpp

static const TInt KSyncMs = 60000;
static const TInt KSlackSecs = 2;
static const TInt64 KUnixEpochUs = MAKE_TINT64(0x00dcddb3, 0x0f2f8000);	// 1970-01-01 in TTime units
static const TInt64 K2000Us = KUnixEpochUs + TInt64(946684800) * 1000000;	// 2000-01-01 (EKA2's RTC zero)

static TDfc* TheSyncDfc;
static NTimer* TheSyncTimer;

static void SyncTimerFn(TAny*)
	{
	TheSyncDfc->Add();
	}

static void SyncDfcFn(TAny*)
	{
	TInt64 hostUs;
	const char* src = "VMMDev";
	TInt64 ms;
	if (X64VmmHostTimeMs(ms) == KErrNone)
		hostUs = KUnixEpochUs + ms * 1000;
	else
		{
		TInt s;
		src = "CMOS";
		if (X64RtcReadSeconds(s) != KErrNone)
			{
			TheSyncTimer->OneShot(NKern::TimerTicks(KSyncMs));
			return;
			}
		hostUs = K2000Us + TInt64(s) * 1000000;
		}
	const TInt64 diff = hostUs - Kern::SystemTime();
	static TBool first = ETrue;
	if (first)
		Kern::Printf("[timesync] every %d s from %s, first offset %d ms", KSyncMs / 1000, src, (TInt)(diff / 1000));
	first = EFalse;
	if (diff >= TInt64(KSlackSecs) * 1000000 || diff <= -TInt64(KSlackSecs) * 1000000)
		{
		NKern::ThreadEnterCS();
		TInt r = Kern::SetSystemTime(hostUs, Kern::ETimeSet_SetHwRtc | Kern::ETimeSet_AsyncNotify);
		NKern::ThreadLeaveCS();
		Kern::Printf("[timesync] %s: kernel clock was %d s off -> set (%d)", src, (TInt)(diff / 1000000), r);
		}
	TheSyncTimer->OneShot(NKern::TimerTicks(KSyncMs));
	}

void X64TimeSyncStart()
	{
	NKern::ThreadEnterCS();
	TheSyncDfc = new TDfc(SyncDfcFn, NULL, Kern::DfcQue0(), 3);
	TheSyncTimer = new NTimer(SyncTimerFn, NULL);
	NKern::ThreadLeaveCS();
	if (!TheSyncDfc || !TheSyncTimer)
		return;
	TheSyncTimer->OneShot(NKern::TimerTicks(KSyncMs));
	}
