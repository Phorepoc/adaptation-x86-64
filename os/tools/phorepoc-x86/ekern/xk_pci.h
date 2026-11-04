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

// A function is a PCI-PCI bridge when its header type (config dword 3, bits 22..16) is 1 and its class is 0x0604; its
// secondary bus number is config dword 6, bits 15..8 (the firmware numbers the buses). VMware puts devices behind PCIe
// root ports (pciBridge4..7: the xHCI controller, NVMe, vmxnet3, e1000e), so a scan of bus 0 alone misses them.
static inline TBool XkPciIsBridge(TInt aBus, TInt aDev, TInt aFn)
	{ return ((XkPciRead(aBus, aDev, aFn, 0x0C) >> 16) & 0x7F) == 1 && (XkPciRead(aBus, aDev, aFn, 8) >> 16) == 0x0604; }

// The first function whose (device << 16 | vendor) is aVendorDevice (aClass < 0), or whose class and subclass
// (config dword 8, bits 31..16: 0x0403 = multimedia audio device) are aClass. Scans bus 0 and then, depth first, the
// secondary bus of every bridge it meets (buses up to aMaxBus), every device, every function of a multi-function
// device. aDepth bounds the recursion (a misconfigured bridge that points back at its own bus).
static inline TBool XkPciFindOn(TInt aBus, TUint32 aVendorDevice, TInt aClass, SPciLoc& aLoc, TInt aMaxBus, TInt aDepth)
	{
	for (TInt dev = 0; dev < 32; ++dev)
		{
		const TUint32 id0 = XkPciRead(aBus, dev, 0, 0);
		if ((id0 & 0xFFFF) == 0xFFFF)
			continue;
		const TInt fns = (XkPciRead(aBus, dev, 0, 0x0C) & 0x800000) ? 8 : 1;
		for (TInt fn = 0; fn < fns; ++fn)
			{
			const TUint32 id = fn ? XkPciRead(aBus, dev, fn, 0) : id0;
			if ((id & 0xFFFF) == 0xFFFF)
				continue;
			const TBool hit = aClass >= 0 ? (TInt)(XkPciRead(aBus, dev, fn, 8) >> 16) == aClass : id == aVendorDevice;
			if (hit)
				{
				aLoc.iBus = aBus; aLoc.iDev = dev; aLoc.iFn = fn;
				return ETrue;
				}
			if (aDepth > 0 && XkPciIsBridge(aBus, dev, fn))
				{
				const TInt sec = (TInt)((XkPciRead(aBus, dev, fn, 0x18) >> 8) & 0xFF);
				if (sec > aBus && sec <= aMaxBus && XkPciFindOn(sec, aVendorDevice, aClass, aLoc, aMaxBus, aDepth - 1))
					return ETrue;
				}
			}
		}
	return EFalse;
	}

static inline TBool XkPciFind(TUint32 aVendorDevice, TInt aClass, SPciLoc& aLoc, TInt aMaxBus = 255)
	{ return XkPciFindOn(0, aVendorDevice, aClass, aLoc, aMaxBus, 8); }

#endif
