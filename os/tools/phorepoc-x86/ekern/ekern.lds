/* ekern.lds -- Phorepoc amd64/x32: ONE multiboot2 ELF holding
 *   - the throw-away LOW boot stub (boot.S + xboot.cpp), identity-addressed at 1 MiB, and
 *   - the EKA2 kernel image at the flexible memory model's NATIVE linear addresses
 *     (include/memmodel/epoc/flexible/x86/mmboot.h): "ROM" at KRomLinearBase 0x80000000,
 *     kernel .data/.bss at KKernelDataBase 0xC8000000 (load image kept inside the ROM, as a
 *     real XIP ROM does -- the bootstrap copies it out).
 * GRUB loads by physical (LMA) address; xboot.cpp builds the page tables that make the VMAs
 * true. Non-PIC x32 code is fine above 2 GiB (32-bit addressing, R_X86_64_32) -- see
 * docs/phorepoc/x86-64-port.md X4.3.
 */
ENTRY(_start)

MEMORY
	{
	LOW   (rwx) : ORIGIN = 0x00100000, LENGTH = 1M			/* boot stub: VMA = LMA           */
	ROMP  (rwx) : ORIGIN = 0x00200000, LENGTH = 30M			/* where the ROM sits physically  */
	ROMV  (rx)  : ORIGIN = 0x80000000, LENGTH = 30M			/* ... and linearly               */
	DATV  (rw)  : ORIGIN = 0xC8000000, LENGTH = 64M			/* kernel .data/.bss, run address */
	}

SECTIONS
	{
	.boot :
		{
		KEEP(*(.multiboot))
		*boot.o(.text .text.* .rodata .rodata.* .data .data.*)
		*xboot.o(.text .text.* .rodata .rodata.* .data .data.*)
		} >LOW
	.boot.bss (NOLOAD) :
		{
		*boot.o(.bss .bss.* COMMON)
		*xboot.o(.bss .bss.* COMMON)
		} >LOW

	/* ROM header area: TRomHeader + the primary/variant TRomEntry + TRomImageHeader records.
	 * Reserved here, FILLED IN AT BOOT by xboot.cpp from the symbols below (the "ROM" is RAM). */
	.romhdr : ALIGN(0x1000)
		{
		__rom_lin = .;
		BYTE(0)
		. = __rom_lin + 0x1000;
		} >ROMV AT>ROMP
	__rom_phys = LOADADDR(.romhdr);
	/* Every ROM section's load address is pinned to  __rom_phys + (its linear address - __rom_lin):
	 * with plain ">ROMV AT>ROMP" ld aligns VMA and LMA independently, and one 16-byte slip breaks the
	 * ROM's linear<->physical correspondence (seen: .rodata strings read 16 bytes off). */

	.text ALIGN(0x1000) : AT(__rom_phys + (ADDR(.text) - __rom_lin))
		{
		__code_base = .;
		*(.text.entry)				/* _E32Startup first */
		*(.text .text.*)
		. = ALIGN(0x1000);
		__utext_base = .;			/* ring-3 XIP code: the ROM secondary (xk_minisec.cpp) */
		*(.utext)
		__utext_end = .;
		} >ROMV
	.rodata ALIGN(32) : AT(__rom_phys + (ADDR(.rodata) - __rom_lin))
		{
		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)
		. = ALIGN(8);
		__init_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		KEEP(*(.ctors))
		__init_array_end = .;
		. = ALIGN(0x1000);
		} >ROMV
	__code_end = .;
	__data_load = .;													/* ROM-linear address of the .data image */

	.data 0xC8000000 : AT(__rom_phys + (__data_load - __rom_lin))
		{
		__data_run = .;
		*(.data .data.*)
		*(.ustacks)
		. = ALIGN(4);
		__data_run_end = .;
		} >DATV
	__data_size = __data_run_end - __data_run;

	.bss (NOLOAD) : ALIGN(4)
		{
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(0x1000);
		__bss_end = .;
		} >DATV
	__svdata_size = __bss_end - __data_run;							/* TRomHeader::iTotalSvDataSize */

	/* The statically linked user EXE (uexe/uexe.lds is linked for exactly this ROM address). */
	.uimage 0x80400000 : AT(__rom_phys + 0x400000)
		{
		__uimage_base = .;
		KEEP(*(.uimage))
		. = ALIGN(0x1000);
		}
	ASSERT(__data_load + SIZEOF(.data) <= 0x80400000, "kernel image overlaps the user image window")
	__rom_size = (LOADADDR(.uimage) + SIZEOF(.uimage)) - __rom_phys;	/* bytes of ROM actually loaded */

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
	}
