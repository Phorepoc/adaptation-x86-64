// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// xk_sound.cpp -- which sound PDD this boot gets. The ROM carries one built-in physical device, Sys\Bin\soundsc.pdd
// (built-in device id 4), and the real shared-chunk sound LDD (soundldd.cpp, unmodified, entry renamed) binds to
// whatever "SoundSc.*" PDD that file installs. The probe is the configuration: AC'97 first (QEMU `-device AC97`,
// VirtualBox `--audio-controller ac97`; the tested path of every earlier image stays byte for byte), then Intel
// HD Audio (PCI class 0403: QEMU `-device intel-hda`, VirtualBox `hda`, VMware `hdaudio`, real PCs). No controller:
// no PDD, the load fails and the system runs without sound.
#include <kernel/kern_priv.h>
#include <drivers/soundsc.h>
#include "xk_sound.h"

DLogicalDevice* CreateLogicalDevice_soundsc();				// soundldd.cpp (DECLARE_STANDARD_LDD, renamed)

enum TSoundKind { ENoSound, EAc97, EHda };
static TSoundKind TheSound = ENoSound;

void X64FillSine440(TInt16* aBuf, TInt aFrames)
	{
	// a parabolic approximation of sin, amplitude 8000: phase in 1/4096 of a 440 Hz period
	for (TInt i = 0; i < aFrames; ++i)
		{
		const TInt ph = (i * 440 * 4096 / 48000) & 4095;
		const TInt x = (ph & 2047) - 1024;					// -1024..1023 over a half period
		TInt v = 8000 - (8000 * x / 1024) * x / 1024;		// 0..8000..0 (a parabola)
		if (ph >= 2048)
			v = -v;
		aBuf[2 * i] = aBuf[2 * i + 1] = (TInt16)v;
		}
	}

void X64StartSound()
	{
	TInt r = X64Ac97Start();
	if (r == KErrNone)
		TheSound = EAc97;
	else
		{
		r = X64HdaStart();
		if (r == KErrNone)
			TheSound = EHda;
		}
	if (TheSound == ENoSound)
		{
		Kern::Printf("[sound] no AC'97 or HD Audio output (%d): no sound", r);
		return;
		}
	Kern::Printf("[sound] %s PDD", TheSound == EAc97 ? "AC'97" : "HD Audio");
#ifdef SOUND_TEST
	NKern::ThreadEnterCS();
	if (TheSound == EAc97)
		X64Ac97SelfTest();
	else
		X64HdaSelfTest();
	NKern::ThreadLeaveCS();
#endif
	}

// Sys\Bin\esoundsc.ldd / soundsc.pdd (rombuild --builtin-device; xboot.cpp points exports 3 and 4 here) -- the names
// mdasoundadapter loads
extern "C" DLogicalDevice* X64CreateLdd_soundsc()
	{
	return CreateLogicalDevice_soundsc();
	}

extern "C" DPhysicalDevice* X64CreatePdd_sound()
	{
	switch (TheSound)
		{
	case EAc97:	return X64Ac97NewFactory();
	case EHda:	return X64HdaNewFactory();
	default:	return NULL;
		}
	}
