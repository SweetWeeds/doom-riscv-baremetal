/*
 * The only hardware services the port needs: a byte-wide console, a
 * monotonic clock, and a way to stop the machine.
 */
#ifndef PLATFORM_HARDWARE_H
#define PLATFORM_HARDWARE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Enables the console UART. Called from start.S before .bss is cleared, so
 * implementations must not rely on global variables.
 */
void platformInitializeConsole(void);

void platformConsoleWriteCharacter(char character);
void platformConsoleWriteString(const char *text);

/*
 * Bulk output path used by stdout/stderr. Converts "\n" to "\r\n" (the host
 * terminal is in raw mode) and uses the semihosting console when enabled.
 */
void platformConsoleWriteBytes(const char *bytes, size_t byteCount);

/* Returns -1 when no byte is waiting. */
int platformConsoleReadCharacter(void);

/*
 * Moves bytes waiting in the UART into a larger software buffer, so they are
 * not lost while nobody reads them (the UART itself holds only 8-16 bytes).
 * The transmit loops already do this; call it from other long waits.
 */
void platformConsoleCollectInput(void);

uint64_t platformReadTimerTicks(void);
uint64_t platformReadTimeMilliseconds(void);

void platformPowerOff(int exitCode) __attribute__((noreturn));

#endif /* PLATFORM_HARDWARE_H */
