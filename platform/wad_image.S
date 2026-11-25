/*
 * Embeds the game data (WAD) into the executable image. There is no disk on
 * a bare-metal target, so system_calls.c serves this blob as a read-only file.
 * WAD_FILE_PATH is supplied by the Makefile.
 */
    .section .rodata.embedded_wad, "a", @progbits
    .balign 16
    .globl embeddedWadStart
    .globl embeddedWadEnd
embeddedWadStart:
    .incbin WAD_FILE_PATH
embeddedWadEnd:
