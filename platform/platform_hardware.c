#include "platform_hardware.h"
#include "platform_config.h"

/* ======================================================================== */
/* Console UART                                                             */
/* ======================================================================== */

/*
 * Each UART driver provides tryReadUartByte() (one received byte, or -1) and
 * calls platformConsoleCollectInput() while it waits to transmit; see the
 * receive buffer section below for why.
 */
static int tryReadUartByte(void);

#define NO_BYTE_WAITING (-1)

#if PLATFORM_UART_TYPE == PLATFORM_UART_TYPE_NS16550

/* NS16550A register indexes (multiplied by the register stride). */
#define UART_TRANSMIT_HOLDING_REGISTER_INDEX 0
#define UART_RECEIVE_BUFFER_REGISTER_INDEX 0
#define UART_LINE_STATUS_REGISTER_INDEX 5

#define UART_LINE_STATUS_DATA_READY_BIT 0x01
#define UART_LINE_STATUS_TRANSMIT_HOLDING_EMPTY_BIT 0x20

static volatile uint8_t *uartRegister(int registerIndex)
{
    uintptr_t address = PLATFORM_UART_BASE_ADDRESS
                      + (uintptr_t)registerIndex * PLATFORM_UART_REGISTER_STRIDE_BYTES;
    return (volatile uint8_t *)address;
}

void platformInitializeConsole(void)
{
    /* QEMU's 16550 works from reset; a real one would need its divisor set here. */
}

void platformConsoleWriteCharacter(char character)
{
    /*
     * Poll the "transmit holding register empty" bit. QEMU never reports the
     * UART as busy, but real 16550s and RTL models drop bytes if we don't wait.
     */
    while ((*uartRegister(UART_LINE_STATUS_REGISTER_INDEX) & UART_LINE_STATUS_TRANSMIT_HOLDING_EMPTY_BIT) == 0)
    {
        platformConsoleCollectInput();
    }
    *uartRegister(UART_TRANSMIT_HOLDING_REGISTER_INDEX) = (uint8_t)character;
}

static int tryReadUartByte(void)
{
    if ((*uartRegister(UART_LINE_STATUS_REGISTER_INDEX) & UART_LINE_STATUS_DATA_READY_BIT) == 0)
    {
        return NO_BYTE_WAITING;
    }
    return *uartRegister(UART_RECEIVE_BUFFER_REGISTER_INDEX);
}

#elif PLATFORM_UART_TYPE == PLATFORM_UART_TYPE_SIFIVE

/*
 * SiFive UART ("sifive,uart0"). All registers are 32 bits wide and must be
 * accessed as whole words. Register map from the FU540-C000 manual, chapter
 * "Universal Asynchronous Receiver/Transmitter", and QEMU's model
 * (hw/char/sifive_uart.c).
 */
#define SIFIVE_UART_TRANSMIT_DATA_OFFSET 0x00
#define SIFIVE_UART_RECEIVE_DATA_OFFSET 0x04
#define SIFIVE_UART_TRANSMIT_CONTROL_OFFSET 0x08
#define SIFIVE_UART_RECEIVE_CONTROL_OFFSET 0x0C
#define SIFIVE_UART_INTERRUPT_ENABLE_OFFSET 0x10
#define SIFIVE_UART_BAUD_DIVISOR_OFFSET 0x18

/*
 * Bit 31 of txdata reads 1 while the 8-entry transmit FIFO is full (a write
 * then is dropped). Bit 31 of rxdata reads 1 when the receive FIFO is empty.
 */
#define SIFIVE_UART_FIFO_STATUS_BIT 0x80000000u
#define SIFIVE_UART_DATA_MASK 0xFFu

/* txctrl/rxctrl bit 0 enables the transmitter/receiver; off after reset. */
#define SIFIVE_UART_ENABLE_BIT 0x1u
/* txctrl bit 1 selects two stop bits; left at 0 for the usual 8N1. */

#define SIFIVE_UART_ALL_INTERRUPTS_DISABLED 0x0u

static volatile uint32_t *sifiveUartRegister(uintptr_t offset)
{
    return (volatile uint32_t *)(PLATFORM_UART_BASE_ADDRESS + offset);
}

/*
 * Called from start.S before .bss is cleared, so it must not keep state in
 * global variables. Safe to call again.
 */
void platformInitializeConsole(void)
{
#if PLATFORM_UART_INPUT_CLOCK_HZ > 0
    /* baud = clock / (divisor + 1); round to the nearest divisor. */
    uint32_t baudDivisor = (uint32_t)((PLATFORM_UART_INPUT_CLOCK_HZ + PLATFORM_UART_BAUD_RATE / 2)
                                      / PLATFORM_UART_BAUD_RATE) - 1;
    *sifiveUartRegister(SIFIVE_UART_BAUD_DIVISOR_OFFSET) = baudDivisor;
#endif
    *sifiveUartRegister(SIFIVE_UART_INTERRUPT_ENABLE_OFFSET) = SIFIVE_UART_ALL_INTERRUPTS_DISABLED;
    *sifiveUartRegister(SIFIVE_UART_TRANSMIT_CONTROL_OFFSET) = SIFIVE_UART_ENABLE_BIT;
    *sifiveUartRegister(SIFIVE_UART_RECEIVE_CONTROL_OFFSET) = SIFIVE_UART_ENABLE_BIT;
}

void platformConsoleWriteCharacter(char character)
{
    volatile uint32_t *transmitData = sifiveUartRegister(SIFIVE_UART_TRANSMIT_DATA_OFFSET);
    while ((*transmitData & SIFIVE_UART_FIFO_STATUS_BIT) != 0)
    {
        platformConsoleCollectInput();
    }
    *transmitData = (uint8_t)character;
}

static int tryReadUartByte(void)
{
    /*
     * Reading rxdata removes the byte from the FIFO, so the "empty" flag and
     * the data must come from this single read.
     */
    uint32_t receiveData = *sifiveUartRegister(SIFIVE_UART_RECEIVE_DATA_OFFSET);
    if ((receiveData & SIFIVE_UART_FIFO_STATUS_BIT) != 0)
    {
        return NO_BYTE_WAITING;
    }
    return (int)(receiveData & SIFIVE_UART_DATA_MASK);
}

#else
#error "Unknown PLATFORM_UART_TYPE"
#endif

/* ======================================================================== */
/* Receive buffer                                                           */
/* ======================================================================== */

/*
 * The UART's own receive FIFO is tiny (8 bytes on the SiFive UART, 16 on a
 * 16550). While a large frame goes out at 115200 bps the CPU spends most of
 * its time waiting for transmit space, and key presses typed meanwhile would
 * overflow that FIFO and be lost. So the transmit wait loops move received
 * bytes into this larger buffer, and reads take bytes from here first.
 *
 * The indexes only ever grow and are masked on use, so the buffer stays in
 * bounds even if a fatal trap prints something before .bss is cleared.
 */
#define RECEIVE_BUFFER_SIZE_BYTES 256 /* must be a power of two */
#define RECEIVE_BUFFER_INDEX_MASK (RECEIVE_BUFFER_SIZE_BYTES - 1)
static uint8_t receiveBuffer[RECEIVE_BUFFER_SIZE_BYTES];
static uint32_t receiveBufferWriteCount;
static uint32_t receiveBufferReadCount;

void platformConsoleCollectInput(void)
{
    for (;;)
    {
        uint32_t storedByteCount = receiveBufferWriteCount - receiveBufferReadCount;
        if (storedByteCount >= RECEIVE_BUFFER_SIZE_BYTES)
        {
            /* Full: leave further bytes in the UART rather than overwrite older input. */
            return;
        }
        int receivedByte = tryReadUartByte();
        if (receivedByte == NO_BYTE_WAITING)
        {
            return;
        }
        receiveBuffer[receiveBufferWriteCount & RECEIVE_BUFFER_INDEX_MASK] = (uint8_t)receivedByte;
        receiveBufferWriteCount++;
    }
}

int platformConsoleReadCharacter(void)
{
    platformConsoleCollectInput();
    if (receiveBufferReadCount == receiveBufferWriteCount)
    {
        return NO_BYTE_WAITING;
    }
    uint8_t receivedByte = receiveBuffer[receiveBufferReadCount & RECEIVE_BUFFER_INDEX_MASK];
    receiveBufferReadCount++;
    return receivedByte;
}

void platformConsoleWriteString(const char *text)
{
    while (*text != '\0')
    {
        if (*text == '\n')
        {
            /* The host terminal is in raw mode under QEMU -nographic. */
            platformConsoleWriteCharacter('\r');
        }
        platformConsoleWriteCharacter(*text);
        text++;
    }
}

static void writeBytesToUart(const char *bytes, size_t byteCount)
{
    for (size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        if (bytes[byteIndex] == '\n')
        {
            /* The host terminal is in raw mode under QEMU -nographic. */
            platformConsoleWriteCharacter('\r');
        }
        platformConsoleWriteCharacter(bytes[byteIndex]);
    }
}

/* ======================================================================== */
/* Machine timer                                                            */
/* ======================================================================== */

#define MILLISECONDS_PER_SECOND 1000

uint64_t platformReadTimerTicks(void)
{
    /*
     * Read the CLINT register over MMIO rather than with the rdtime
     * instruction: in machine mode without firmware, rdtime is not guaranteed
     * to be implemented (QEMU traps it unless the time CSR is emulated).
     */
    volatile uint64_t *machineTimer = (volatile uint64_t *)PLATFORM_MACHINE_TIMER_ADDRESS;
    return *machineTimer;
}

uint64_t platformReadTimeMilliseconds(void)
{
    /*
     * Split into whole seconds and the remainder instead of dividing by
     * "ticks per millisecond": with a 32.768 kHz timer that would be 32, not
     * 32.768, and time would run 2.4 % fast.
     */
    uint64_t timerTicks = platformReadTimerTicks();
    uint64_t wholeSeconds = timerTicks / PLATFORM_MACHINE_TIMER_FREQUENCY_HZ;
    uint64_t remainingTicks = timerTicks % PLATFORM_MACHINE_TIMER_FREQUENCY_HZ;
    return wholeSeconds * MILLISECONDS_PER_SECOND
         + remainingTicks * MILLISECONDS_PER_SECOND / PLATFORM_MACHINE_TIMER_FREQUENCY_HZ;
}

/* ======================================================================== */
/* Semihosting (QEMU or a debugger acting as the host)                      */
/* ======================================================================== */

#define IS_SEMIHOSTING_NEEDED \
    (PLATFORM_HAS_SEMIHOSTING_CONSOLE || PLATFORM_EXIT_METHOD == PLATFORM_EXIT_METHOD_SEMIHOSTING)

#if IS_SEMIHOSTING_NEEDED

/* RISC-V semihosting operation numbers (shared with the Arm specification). */
#define SEMIHOSTING_OPERATION_OPEN 0x01
#define SEMIHOSTING_OPERATION_WRITE 0x05
#define SEMIHOSTING_OPERATION_EXIT 0x18

/* SYS_EXIT reason meaning "the application finished"; the exit code follows. */
#define SEMIHOSTING_EXIT_REASON_APPLICATION_EXIT 0x20026

/*
 * The semihosting trap is this exact three-instruction sequence. The
 * instructions must be uncompressed and must not straddle a page boundary,
 * hence norvc and the 16-byte alignment.
 */
static long semihostingCall(long operation, void *parameterBlock)
{
    register long operationAndResult __asm__("a0") = operation;
    register void *parameterRegister __asm__("a1") = parameterBlock;
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        ".balign 16\n"
        "slli x0, x0, 0x1f\n"
        "ebreak\n"
        "srai x0, x0, 7\n"
        ".option pop\n"
        : "+r"(operationAndResult)
        : "r"(parameterRegister)
        : "memory");
    return operationAndResult;
}

#endif /* IS_SEMIHOSTING_NEEDED */

#if PLATFORM_HAS_SEMIHOSTING_CONSOLE

/* ":tt" opened with mode 4 ("w") is the host's standard output. */
#define SEMIHOSTING_CONSOLE_NAME ":tt"
#define SEMIHOSTING_OPEN_MODE_WRITE 4

#define SEMIHOSTING_HANDLE_NOT_OPENED (-2)
#define SEMIHOSTING_HANDLE_UNAVAILABLE (-1)

/*
 * Console bytes are staged here so that "\n" can be expanded to "\r\n"
 * before one semihosting write hands the whole block to the host.
 */
#define SEMIHOSTING_STAGING_BUFFER_SIZE 16384

static long semihostingConsoleHandle = SEMIHOSTING_HANDLE_NOT_OPENED;

static long openSemihostingConsole(void)
{
    if (semihostingConsoleHandle == SEMIHOSTING_HANDLE_NOT_OPENED)
    {
        long openParameters[3];
        openParameters[0] = (long)SEMIHOSTING_CONSOLE_NAME;
        openParameters[1] = SEMIHOSTING_OPEN_MODE_WRITE;
        openParameters[2] = (long)(sizeof(SEMIHOSTING_CONSOLE_NAME) - 1);
        long handle = semihostingCall(SEMIHOSTING_OPERATION_OPEN, openParameters);
        if (handle < 0)
        {
            handle = SEMIHOSTING_HANDLE_UNAVAILABLE;
        }
        semihostingConsoleHandle = handle;
    }
    return semihostingConsoleHandle;
}

/*
 * QEMU puts its stdio in non-blocking mode, so when the terminal cannot keep
 * up the host write fails with EAGAIN and SYS_WRITE reports bytes as not
 * written. Those bytes must be sent again or frames arrive truncated. If the
 * host makes no progress at all for this long, the output is dropped rather
 * than hanging the game.
 */
#define SEMIHOSTING_WRITE_STALL_LIMIT_MILLISECONDS 2000

static void writeSemihostingBlock(long handle, const char *bytes, size_t byteCount)
{
    size_t writtenByteCount = 0;
    uint64_t lastProgressTime = platformReadTimeMilliseconds();

    while (writtenByteCount < byteCount)
    {
        long writeParameters[3];
        writeParameters[0] = handle;
        writeParameters[1] = (long)(bytes + writtenByteCount);
        writeParameters[2] = (long)(byteCount - writtenByteCount);

        /* SYS_WRITE returns how many bytes were NOT written. */
        long unwrittenByteCount = semihostingCall(SEMIHOSTING_OPERATION_WRITE, writeParameters);
        size_t justWrittenByteCount = (byteCount - writtenByteCount) - (size_t)unwrittenByteCount;

        if (justWrittenByteCount > 0)
        {
            writtenByteCount += justWrittenByteCount;
            lastProgressTime = platformReadTimeMilliseconds();
        }
        else if (platformReadTimeMilliseconds() - lastProgressTime > SEMIHOSTING_WRITE_STALL_LIMIT_MILLISECONDS)
        {
            return;
        }
    }
}

void platformConsoleWriteBytes(const char *bytes, size_t byteCount)
{
    long handle = openSemihostingConsole();
    if (handle == SEMIHOSTING_HANDLE_UNAVAILABLE)
    {
        writeBytesToUart(bytes, byteCount);
        return;
    }

    static char stagingBuffer[SEMIHOSTING_STAGING_BUFFER_SIZE];
    size_t stagedLength = 0;
    for (size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        /* Leave room for a two-byte "\r\n". */
        if (stagedLength + 2 > SEMIHOSTING_STAGING_BUFFER_SIZE)
        {
            writeSemihostingBlock(handle, stagingBuffer, stagedLength);
            stagedLength = 0;
        }
        if (bytes[byteIndex] == '\n')
        {
            stagingBuffer[stagedLength++] = '\r';
        }
        stagingBuffer[stagedLength++] = bytes[byteIndex];
    }
    if (stagedLength > 0)
    {
        writeSemihostingBlock(handle, stagingBuffer, stagedLength);
    }
}

#else /* UART only */

void platformConsoleWriteBytes(const char *bytes, size_t byteCount)
{
    writeBytesToUart(bytes, byteCount);
}

#endif /* PLATFORM_HAS_SEMIHOSTING_CONSOLE */

/* ======================================================================== */
/* Stopping the machine                                                     */
/* ======================================================================== */

/* The power-off register carries the exit code in its upper 16 bits. */
#define POWER_OFF_EXIT_CODE_MASK 0xFFFF

void platformPowerOff(int exitCode)
{
#if PLATFORM_EXIT_METHOD == PLATFORM_EXIT_METHOD_POWER_OFF_DEVICE
    volatile uint32_t *powerOffRegister = (volatile uint32_t *)PLATFORM_POWER_OFF_ADDRESS;
    if (exitCode == 0)
    {
        *powerOffRegister = PLATFORM_POWER_OFF_PASS_VALUE;
    }
    else
    {
        uint32_t encodedExitCode = ((uint32_t)exitCode & POWER_OFF_EXIT_CODE_MASK) << PLATFORM_POWER_OFF_EXIT_CODE_SHIFT;
        *powerOffRegister = encodedExitCode | PLATFORM_POWER_OFF_FAIL_VALUE;
    }
#elif PLATFORM_EXIT_METHOD == PLATFORM_EXIT_METHOD_SEMIHOSTING
    /* On 64-bit targets SYS_EXIT takes a block: { reason, exit code }. */
    long exitParameters[2];
    exitParameters[0] = SEMIHOSTING_EXIT_REASON_APPLICATION_EXIT;
    exitParameters[1] = exitCode;
    semihostingCall(SEMIHOSTING_OPERATION_EXIT, exitParameters);
#elif PLATFORM_EXIT_METHOD == PLATFORM_EXIT_METHOD_HALT
    (void)exitCode;
#else
#error "Unknown PLATFORM_EXIT_METHOD"
#endif
    for (;;)
    {
        __asm__ volatile("wfi");
    }
}

/* ======================================================================== */
/* Fatal trap report                                                        */
/* ======================================================================== */

#define HEXADECIMAL_DIGITS_PER_64_BIT_VALUE 16
#define BITS_PER_HEXADECIMAL_DIGIT 4
#define HEXADECIMAL_DIGIT_MASK 0xF

static void writeHexadecimal(uint64_t value)
{
    static const char hexadecimalDigits[] = "0123456789abcdef";
    platformConsoleWriteString("0x");
    for (int digitIndex = HEXADECIMAL_DIGITS_PER_64_BIT_VALUE - 1; digitIndex >= 0; digitIndex--)
    {
        unsigned int digit = (value >> (digitIndex * BITS_PER_HEXADECIMAL_DIGIT)) & HEXADECIMAL_DIGIT_MASK;
        platformConsoleWriteCharacter(hexadecimalDigits[digit]);
    }
}

#define FATAL_TRAP_EXIT_CODE 3

/*
 * Called from trap_entry in start.S. No interrupts are enabled, so every trap
 * is a bug (illegal instruction, misaligned or faulting access). Report it
 * with raw UART writes only, because libc state may be what is broken.
 */
void platformHandleFatalTrap(void)
{
    uint64_t trapCause;
    uint64_t trapProgramCounter;
    uint64_t trapValue;
    __asm__ volatile("csrr %0, mcause" : "=r"(trapCause));
    __asm__ volatile("csrr %0, mepc" : "=r"(trapProgramCounter));
    __asm__ volatile("csrr %0, mtval" : "=r"(trapValue));

    platformConsoleWriteString("\n*** fatal trap: mcause=");
    writeHexadecimal(trapCause);
    platformConsoleWriteString(" mepc=");
    writeHexadecimal(trapProgramCounter);
    platformConsoleWriteString(" mtval=");
    writeHexadecimal(trapValue);
    platformConsoleWriteString("\n");
    platformPowerOff(FATAL_TRAP_EXIT_CODE);
}
