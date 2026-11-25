/*
 * doomgeneric backend for a bare-metal RISC-V machine with only a UART.
 *
 * Default behaviour is a headless benchmark: DOOM replays the demo lump
 * "demo1" from the shareware WAD as fast as the CPU allows (-timedemo), prints
 * the timing line, and powers the machine off. Because the demo is a recorded
 * input stream, the instruction stream is the same on every run, which is
 * what a CPU simulator workload needs.
 *
 * Optionally (FRAME_OUTPUT_INTERVAL > 0) every Nth frame is printed to the
 * UART as ASCII art or colour half blocks (terminal_renderer.c).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "platform_hardware.h"
#include "terminal_renderer.h"

/* ---- Build-time options (overridable from the Makefile) ---------------- */

/* Command line handed to DOOM, split on spaces. */
#ifndef DOOM_COMMAND_LINE
#define DOOM_COMMAND_LINE "doom -iwad doom1.wad -timedemo demo1 -nogui"
#endif

/* 0 = never print frames (pure benchmark). N = print every Nth frame. */
#ifndef FRAME_OUTPUT_INTERVAL
#define FRAME_OUTPUT_INTERVAL 0
#endif

/* FRAME_RENDER_MODE_ASCII, _TRUECOLOR or _PALETTE256 (terminal_renderer.h). */
#ifndef FRAME_RENDER_MODE
#define FRAME_RENDER_MODE FRAME_RENDER_MODE_ASCII
#endif

/* 0 = ask the terminal for its size and fill it. N = use N columns. */
#ifndef FRAME_COLUMN_COUNT
#define FRAME_COLUMN_COUNT 0
#endif

/*
 * 1 = redraw every frame at the top-left of the screen, so frames animate in
 * place. 0 = append frames one after another (log-friendly).
 */
#ifndef SHOULD_REDRAW_IN_PLACE
#define SHOULD_REDRAW_IN_PLACE 0
#endif

/*
 * 0 = DOOM's clock is the hardware timer (realtics/fps are meaningful).
 * 1 = DOOM's clock is a virtual counter, so the executed instruction stream
 *     does not depend on core speed (see DG_GetTicksMs below).
 */
#ifndef SHOULD_USE_DETERMINISTIC_CLOCK
#define SHOULD_USE_DETERMINISTIC_CLOCK 0
#endif

static uint32_t drawnFrameCount = 0;

/* ---- doomgeneric platform callbacks ------------------------------------ */

void DG_Init(void)
{
    printf("[baremetal] doomgeneric on bare-metal RISC-V, %dx%d framebuffer, frame output interval %d, %s clock\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY, FRAME_OUTPUT_INTERVAL,
           SHOULD_USE_DETERMINISTIC_CLOCK ? "virtual (deterministic)" : "hardware timer");

    /*
     * Only set up the terminal when frames will be printed: the size query
     * talks to the terminal and reads the hardware timer, which a headless
     * benchmark must not do.
     */
    if (FRAME_OUTPUT_INTERVAL > 0)
    {
        terminalRendererInitialize(FRAME_RENDER_MODE, FRAME_COLUMN_COUNT, SHOULD_REDRAW_IN_PLACE,
                                   DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    }
}

void DG_DrawFrame(void)
{
    drawnFrameCount++;
    if (FRAME_OUTPUT_INTERVAL > 0 && drawnFrameCount % FRAME_OUTPUT_INTERVAL == 0)
    {
        terminalRendererDrawFrame(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY, drawnFrameCount);
    }
}

#if SHOULD_USE_DETERMINISTIC_CLOCK

/*
 * Virtual clock: time moves only when DOOM looks at it or sleeps, never with
 * the hardware timer. Some DOOM loops (the screen "wipe" transition, for
 * example) spin until the clock changes, so with a real clock the number of
 * executed instructions depends on how fast the core is. With this clock the
 * instruction stream is identical on every core and simulator configuration.
 * The "realtics" figure DOOM prints is then meaningless; measure cycles in
 * the simulator instead.
 */
#define VIRTUAL_MILLISECONDS_PER_CLOCK_QUERY 1

static uint32_t virtualTimeMilliseconds = 0;

void DG_SleepMs(uint32_t milliseconds)
{
    virtualTimeMilliseconds += milliseconds;
}

uint32_t DG_GetTicksMs(void)
{
    virtualTimeMilliseconds += VIRTUAL_MILLISECONDS_PER_CLOCK_QUERY;
    return virtualTimeMilliseconds;
}

#else /* real hardware timer */

void DG_SleepMs(uint32_t milliseconds)
{
    uint64_t wakeUpTime = platformReadTimeMilliseconds() + milliseconds;
    while (platformReadTimeMilliseconds() < wakeUpTime)
    {
    }
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)platformReadTimeMilliseconds();
}

#endif /* SHOULD_USE_DETERMINISTIC_CLOCK */

int DG_GetKey(int *isPressed, unsigned char *doomKey)
{
    /* Demo playback needs no input; report "no key event". */
    (void)isPressed;
    (void)doomKey;
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ---- Clean exit for -timedemo ------------------------------------------ */

/*
 * DOOM ends a timedemo by calling I_Error("timed N gametics in M realtics
 * ..."), which exits with status -1 as if it had failed. The linker is told to
 * route I_Error calls here (-Wl,--wrap=I_Error) so that a finished timedemo
 * exits with status 0 and a real error still exits non-zero.
 */
#define ERROR_MESSAGE_BUFFER_SIZE 512
#define TIMEDEMO_RESULT_PREFIX "timed "
#define DOOM_ERROR_EXIT_CODE 1
#define TIMEDEMO_SUCCESS_EXIT_CODE 0

void __wrap_I_Error(const char *format, ...)
{
    char message[ERROR_MESSAGE_BUFFER_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    int isTimedemoResult = strncmp(message, TIMEDEMO_RESULT_PREFIX, strlen(TIMEDEMO_RESULT_PREFIX)) == 0;
    if (isTimedemoResult)
    {
        printf("\n[baremetal] %s\n", message);
        printf("[baremetal] frames drawn: %lu\n", (unsigned long)drawnFrameCount);
        fflush(stdout);
        exit(TIMEDEMO_SUCCESS_EXIT_CODE);
    }

    printf("\n[baremetal] I_Error: %s\n", message);
    fflush(stdout);
    exit(DOOM_ERROR_EXIT_CODE);
}

/* ---- Keep the demo name alive (upstream doomgeneric bug) --------------- */

/*
 * D_DoomMain() passes the address of its local array "demolumpname" to
 * G_DeferedPlayDemo()/G_TimeDemo(), which keep the pointer in a global and
 * read it again when the demo ends. In Chocolate Doom D_DoomMain() never
 * returns, so that was safe; doomgeneric returns from it after the first
 * tick, the array's stack space is reused, and "-playdemo" then fails at the
 * end with "W_GetNumForName: <garbage> not found!". The linker routes both
 * calls here (-Wl,--wrap=...) to hand DOOM a copy that lives forever.
 */
#define LUMP_NAME_BUFFER_SIZE 9 /* WAD lump names are at most 8 characters */

void __real_G_DeferedPlayDemo(char *demoName);
void __real_G_TimeDemo(char *demoName);

static char persistentDemoName[LUMP_NAME_BUFFER_SIZE];

static char *copyDemoName(const char *demoName)
{
    strncpy(persistentDemoName, demoName, sizeof(persistentDemoName) - 1);
    persistentDemoName[sizeof(persistentDemoName) - 1] = '\0';
    return persistentDemoName;
}

void __wrap_G_DeferedPlayDemo(char *demoName)
{
    __real_G_DeferedPlayDemo(copyDemoName(demoName));
}

void __wrap_G_TimeDemo(char *demoName)
{
    __real_G_TimeDemo(copyDemoName(demoName));
}

/* ---- Entry point ------------------------------------------------------- */

#define MAXIMUM_ARGUMENT_COUNT 32

int main(void)
{
    static char commandLine[] = DOOM_COMMAND_LINE;
    static char *argumentList[MAXIMUM_ARGUMENT_COUNT + 1];
    int argumentCount = 0;

    char *token = strtok(commandLine, " ");
    while (token != NULL && argumentCount < MAXIMUM_ARGUMENT_COUNT)
    {
        argumentList[argumentCount++] = token;
        token = strtok(NULL, " ");
    }
    argumentList[argumentCount] = NULL;

    printf("[baremetal] command line: %s\n", DOOM_COMMAND_LINE);

#if SHOULD_USE_DETERMINISTIC_CLOCK
    /* No hardware-timer reads at all: the printed digits would vary. */
    doomgeneric_Create(argumentCount, argumentList);
#else
    uint64_t startTimeMilliseconds = platformReadTimeMilliseconds();
    doomgeneric_Create(argumentCount, argumentList);
    printf("[baremetal] startup took %lu ms\n",
           (unsigned long)(platformReadTimeMilliseconds() - startTimeMilliseconds));
#endif

    for (;;)
    {
        doomgeneric_Tick();
    }
}
