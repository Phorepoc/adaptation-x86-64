/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// xk_sound.h -- the board's sound PDDs and what they share. xk_ac97.cpp (Intel AC'97) and xk_hda.cpp (Intel High
// Definition Audio) each offer Start (find + bring up the controller), NewFactory (the PDD factory the ROM's
// Sys\Bin\soundsc.pdd loads) and SelfTest; xk_sound.cpp picks one at boot.
#ifndef XK_SOUND_H
#define XK_SOUND_H

TInt X64Ac97Start();					// KErrNone, or KErrNotFound when there is no AC'97 controller
DPhysicalDevice* X64Ac97NewFactory();
void X64Ac97SelfTest();

TInt X64HdaStart();						// KErrNone, or KErrNotFound / another error when no HD Audio output works
DPhysicalDevice* X64HdaNewFactory();
void X64HdaSelfTest();

// aFrames stereo frames of a 440 Hz tone at 48 kHz, 16-bit (the boot self-tests; no FPU needed)
void X64FillSine440(TInt16* aBuf, TInt aFrames);

#endif
