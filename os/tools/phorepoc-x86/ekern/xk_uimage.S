/* xk_uimage.S -- Phorepoc amd64/x32 kernel image: carries the statically linked user EXE
 * (tools/phorepoc-x86/uexe/, flat binary linked for ROM linear 0x80400000) inside the ROM.
 * ekern.lds pins this section to that address; xboot.cpp reads the 32-byte header at its start
 * and describes it to the kernel as the ROM's secondary. UIMAGE_BIN comes from make_ekern.sh;
 * without it the window stays empty and the asm minisec is the secondary. */
    .section .uimage, "a"
#ifdef UIMAGE_BIN
    .incbin UIMAGE_BIN
#else
    .long 0
#endif
    .section .note.GNU-stack, "", @progbits
