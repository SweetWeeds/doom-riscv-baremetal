/*
 * Board description for the bare-metal DOOM build.
 *
 * Every address and clock rate the port depends on lives in this one file so
 * that moving to another simulator or SoC (Spike, NEMU, XiangShan, an FPGA
 * board) only means overriding these values, for example from the Makefile
 * with -DPLATFORM_UART_BASE_ADDRESS=0x...
 *
 * The defaults describe the QEMU "virt" machine. They were read from the
 * device tree that QEMU itself emits (qemu-system-riscv64 -machine
 * virt,dumpdtb=virt.dtb), not from memory.
 *
 * This header is included both from C and from the linker script (through the
 * C preprocessor), so it must contain only #define lines.
 */
#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

/* Main memory: QEMU virt places DRAM at 0x8000_0000. */
#ifndef PLATFORM_RAM_BASE_ADDRESS
#define PLATFORM_RAM_BASE_ADDRESS 0x80000000
#endif

/* Default QEMU virt memory is 128 MiB. DOOM itself needs roughly 12 MiB. */
#ifndef PLATFORM_RAM_SIZE_BYTES
#define PLATFORM_RAM_SIZE_BYTES 0x08000000
#endif

/* Stack sits at the very top of RAM. */
#ifndef PLATFORM_STACK_SIZE_BYTES
#define PLATFORM_STACK_SIZE_BYTES 0x00100000
#endif

/* Hart that runs DOOM; all others park in wfi. */
#ifndef PLATFORM_BOOT_HART_ID
#define PLATFORM_BOOT_HART_ID 0
#endif

/* ---- Console UART ---- */

#define PLATFORM_UART_TYPE_NS16550 0  /* 16550A compatible (QEMU virt) */
#define PLATFORM_UART_TYPE_SIFIVE 1   /* "sifive,uart0" (QEMU sifive_u, FU540/FU740, Chipyard) */

#ifndef PLATFORM_UART_TYPE
#define PLATFORM_UART_TYPE PLATFORM_UART_TYPE_NS16550
#endif

/* QEMU virt: /soc/serial@10000000. */
#ifndef PLATFORM_UART_BASE_ADDRESS
#define PLATFORM_UART_BASE_ADDRESS 0x10000000
#endif

/* NS16550 only: byte stride between registers (1 on QEMU; some SoCs use 4). */
#ifndef PLATFORM_UART_REGISTER_STRIDE_BYTES
#define PLATFORM_UART_REGISTER_STRIDE_BYTES 1
#endif

/*
 * SiFive UART only: clock feeding the UART (on SiFive SoCs, the peripheral
 * bus clock "tlclk"). When non-zero, the baud divisor is programmed as
 * clock / baud - 1. When 0, the divisor is left as the boot loader set it.
 */
#ifndef PLATFORM_UART_INPUT_CLOCK_HZ
#define PLATFORM_UART_INPUT_CLOCK_HZ 0
#endif

#ifndef PLATFORM_UART_BAUD_RATE
#define PLATFORM_UART_BAUD_RATE 115200
#endif

/*
 * Free-running machine timer (CLINT "mtime" register).
 * QEMU virt: CLINT at 0x0200_0000, mtime at offset 0xBFF8.
 */
#ifndef PLATFORM_MACHINE_TIMER_ADDRESS
#define PLATFORM_MACHINE_TIMER_ADDRESS 0x0200BFF8
#endif

/* Device tree "timebase-frequency": 0x989680 = 10 MHz on QEMU virt, 1 MHz on sifive_u. */
#ifndef PLATFORM_MACHINE_TIMER_FREQUENCY_HZ
#define PLATFORM_MACHINE_TIMER_FREQUENCY_HZ 10000000
#endif

/* ---- How the program stops ---- */

/* Write to a "sifive,test" device (QEMU virt). */
#define PLATFORM_EXIT_METHOD_POWER_OFF_DEVICE 0
/* Semihosting SYS_EXIT: QEMU with -semihosting-config, or a debugger. */
#define PLATFORM_EXIT_METHOD_SEMIHOSTING 1
/* Real hardware without either: stop in a wfi loop. */
#define PLATFORM_EXIT_METHOD_HALT 2

#ifndef PLATFORM_EXIT_METHOD
#define PLATFORM_EXIT_METHOD PLATFORM_EXIT_METHOD_POWER_OFF_DEVICE
#endif

/*
 * "sifive,test" power-off device. Writing PASS powers the machine off with a
 * zero host exit status; writing (code << 16) | FAIL exits with that code.
 */

#ifndef PLATFORM_POWER_OFF_ADDRESS
#define PLATFORM_POWER_OFF_ADDRESS 0x00100000
#endif

#define PLATFORM_POWER_OFF_PASS_VALUE 0x5555
#define PLATFORM_POWER_OFF_FAIL_VALUE 0x3333
#define PLATFORM_POWER_OFF_EXIT_CODE_SHIFT 16

/*
 * RISC-V semihosting console: stdout is handed to the host a whole buffer at
 * a time instead of one UART register write per byte. QEMU's 16550 model
 * costs one host system call per byte (about 0.5 MB/s), which limits colour
 * frame output; semihosting removes that limit.
 *
 * Only for QEMU started with "-semihosting-config enable=on,target=native"
 * (or a debugger that services semihosting). Elsewhere the semihosting
 * "ebreak" sequence is an ordinary breakpoint and traps.
 */
#ifndef PLATFORM_HAS_SEMIHOSTING_CONSOLE
#define PLATFORM_HAS_SEMIHOSTING_CONSOLE 0
#endif

#endif /* PLATFORM_CONFIG_H */
