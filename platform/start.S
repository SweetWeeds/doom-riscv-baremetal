/*
 * Reset entry point. Runs in machine mode with no firmware underneath
 * (QEMU is started with -bios none).
 */

#include "platform_config.h"

/* mstatus.FS = Initial. Without this the first floating-point instruction traps. */
#define MSTATUS_FLOATING_POINT_STATE_INITIAL 0x2000

    .section .text.boot, "ax", @progbits
    .globl _start
_start:
    /*
     * Only PLATFORM_BOOT_HART_ID runs DOOM; every other hart parks forever.
     * On SiFive FU540-style SoCs (and QEMU sifive_u) hart 0 is a management
     * core without an FPU, so DOOM must run on hart 1 there.
     */
    csrr    t0, mhartid
    li      t1, PLATFORM_BOOT_HART_ID
    bne     t0, t1, park_secondary_hart

    /*
     * gp must be loaded without linker relaxation, otherwise the linker would
     * rewrite this very load as a gp-relative access before gp is valid.
     */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, __stack_top

    la      t0, trap_entry
    csrw    mtvec, t0

    /*
     * Bring up the console before anything else can trap, so the trap
     * handler can always report. (Runs before .bss is cleared on purpose:
     * the UART set-up keeps no state in memory.)
     */
    call    platformInitializeConsole

    li      t0, MSTATUS_FLOATING_POINT_STATE_INITIAL
    csrs    mstatus, t0
    csrwi   fcsr, 0

    /* Zero .bss (start and end are 16-byte aligned by the linker script). */
    la      t0, __bss_start
    la      t1, __bss_end
clear_bss_loop:
    bgeu    t0, t1, clear_bss_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       clear_bss_loop
clear_bss_done:

    /* Run C constructors, then main(), then exit() with main's result. */
    call    __libc_init_array
    li      a0, 0
    li      a1, 0
    call    main
    call    exit

park_secondary_hart:
    wfi
    j       park_secondary_hart

/*
 * Any trap is fatal in this port (no interrupts are enabled), so the entry
 * just hands over to C, which prints the cause and powers off.
 */
    .align 4
trap_entry:
    call    platformHandleFatalTrap
1:
    j       1b

/* newlib's __libc_init_array / __libc_fini_array call these. Nothing to do. */
    .section .text, "ax", @progbits
    .globl _init
    .globl _fini
_init:
    ret
_fini:
    ret
