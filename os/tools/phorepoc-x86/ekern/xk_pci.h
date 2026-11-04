/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// xk_pci.h -- PCI configuration mechanism #1 (ports 0xCF8/0xCFC), shared by the board's PCI drivers (the e1000 NIC,
// the VMMDev absolute mouse, the AC'97 and HD Audio sound PDDs): config read/write by bus/device/function, and a scan
// for a device by vendor:device id or by class code.
#ifndef XK_PCI_H
#define XK_PCI_H

static inline TUint32 XkPortInl(TUint16 aPort)
	{ TUint32 v; __asm__ __volatile__("in eax, dx" : "=a"(v) : "d"(aPort)); return v; }
static inline void XkPortOutl(TUint16 aPort, TUint32 aVal)
	{ __asm__ __volatile__("out dx, eax" : : "d"(aPort), "a"(aVal) : "memory"); }
static inline void XkPortOutw(TUint16 aPort, TUint16 aVal)
	{ __asm__ __volatile__("out dx, ax" : : "d"(aPort), "a"(aVal) : "memory"); }

static inline TUint32 XkPciAddr(TInt aBus, TInt aDev, TInt aFn, TInt aReg)
	{ return 0x80000000u | (TUint32)aBus << 16 | (TUint32)aDev << 11 | (TUint32)aFn << 8 | (TUint32)(aReg & 0xFC); }

static inline TUint32 XkPciRead(TInt aBus, TInt aDev, TInt aFn, TInt aReg)
	{ XkPortOutl(0xCF8, XkPciAddr(aBus, aDev, aFn, aReg)); return XkPortInl(0xCFC); }
static inline void XkPciWrite(TInt aBus, TInt aDev, TInt aFn, TInt aReg, TUint32 aVal)
	{ XkPortOutl(0xCF8, XkPciAddr(aBus, aDev, aFn, aReg)); XkPortOutl(0xCFC, aVal); }
static inline void XkPciWrite16(TInt aBus, TInt aDev, TInt aFn, TInt aReg, TUint16 aVal)
	{ XkPortOutl(0xCF8, XkPciAddr(aBus, aDev, aFn, aReg)); XkPortOutw(0xCFC + (aReg & 2), aVal); }

struct SPciLoc { TInt iBus, iDev, iFn; };

// The first function whose (device << 16 | vendor) is aVendorDevice (aClass < 0), or whose class and subclass
// (config dword 8, bits 31..16: 0x0403 = multimedia audio device) are aClass. Scans buses 0..aMaxBus, every device,
// every function of a multi-function device.
static inline TBool XkPciFind(TUint32 aVendorDevice, TInt aClass, SPciLoc& aLoc, TInt aMaxBus = 0)
	{
	for (TInt bus = 0; bus <= aMaxBus; ++bus)
		for (TInt dev = 0; dev < 32; ++dev)
			{
			const TUint32 id0 = XkPciRead(bus, dev, 0, 0);
			if ((id0 & 0xFFFF) == 0xFFFF)
				continue;
			const TInt fns = (XkPciRead(bus, dev, 0, 0x0C) & 0x800000) ? 8 : 1;
			for (TInt fn = 0; fn < fns; ++fn)
				{
				const TUint32 id = fn ? XkPciRead(bus, dev, fn, 0) : id0;
				if ((id & 0xFFFF) == 0xFFFF)
					continue;
				const TBool hit = aClass >= 0 ? (TInt)(XkPciRead(bus, dev, fn, 8) >> 16) == aClass : id == aVendorDevice;
				if (hit)
					{
					aLoc.iBus = bus; aLoc.iDev = dev; aLoc.iFn = fn;
					return ETrue;
					}
				}
			}
	return EFalse;
	}

#endif
