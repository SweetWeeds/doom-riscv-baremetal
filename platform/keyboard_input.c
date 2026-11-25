/*
 * Keyboard control over a serial terminal.
 *
 * A terminal (MobaXterm, PuTTY, QEMU -nographic, ...) sends characters, not
 * key presses and releases:
 *
 * - Holding a key sends its character once, then after the operating
 *   system's repeat delay (about 250-660 ms, 500 ms by default on Windows)
 *   again at the repeat rate (about 30 per second).
 * - Nothing is sent when a key is released, and Ctrl, Shift and Alt alone
 *   send nothing at all.
 * - Only the key pressed last repeats, so two keys cannot be held together.
 * - Arrow and function keys arrive as escape sequences such as ESC [ A.
 *
 * So every character becomes a "key down" event (repeats included, the same
 * way SDL reports keyboard auto-repeat, which also lets menus scroll), and a
 * "key up" event is made up when the character stops arriving. After the
 * first character the wait has to cover the repeat delay, otherwise a held
 * key would stop briefly before the repeats start; once repeats are seen a
 * short wait is enough. A different key releases the previous one right away,
 * because the terminal has stopped repeating it anyway.
 *
 * Ctrl (fire), Shift (run) and Alt (strafe) are replaced by character keys;
 * see translateCharacter().
 */
#include "keyboard_input.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "doomkeys.h"
#include "platform_hardware.h"

/* ---- Timing (overridable with -D, all in DOOM clock milliseconds) ------- */

/* Must exceed the terminal's key repeat delay, or a held key stutters once. */
#ifndef KEY_RELEASE_WAIT_AFTER_FIRST_CHARACTER_MILLISECONDS
#define KEY_RELEASE_WAIT_AFTER_FIRST_CHARACTER_MILLISECONDS 550
#endif

/* Must exceed the gap between repeated characters (about 33 ms at 30/s). */
#ifndef KEY_RELEASE_WAIT_AFTER_REPEAT_MILLISECONDS
#define KEY_RELEASE_WAIT_AFTER_REPEAT_MILLISECONDS 150
#endif

/*
 * An escape sequence's bytes follow the ESC within a fraction of a
 * millisecond even at 115200 bps; an ESC with nothing after it for this long
 * is the Escape key itself.
 */
#ifndef LONE_ESCAPE_WAIT_MILLISECONDS
#define LONE_ESCAPE_WAIT_MILLISECONDS 100
#endif

/*
 * 1 = print every key event DOOM receives, for checking what a terminal
 * sends (the lines overwrite the picture, so only for diagnosis).
 */
#ifndef SHOULD_LOG_KEY_EVENTS
#define SHOULD_LOG_KEY_EVENTS 0
#endif

/* ---- Characters --------------------------------------------------------- */

#define ESCAPE_CHARACTER 0x1B
#define CONTROL_C_CHARACTER 0x03
#define CARRIAGE_RETURN_CHARACTER '\r'
#define LINE_FEED_CHARACTER '\n'
#define TAB_CHARACTER '\t'
#define DELETE_CHARACTER 0x7F
#define BACKSPACE_CHARACTER 0x08
#define CONTROL_SEQUENCE_INTRODUCER '['   /* ESC [ ... (arrows, F5-F12, Home, ...) */
#define SINGLE_SHIFT_THREE_INTRODUCER 'O' /* ESC O ... (arrows in "application mode", F1-F4) */

/* ECMA-48: a control sequence ends with a byte in this range. */
#define CONTROL_SEQUENCE_FINAL_BYTE_FIRST 0x40
#define CONTROL_SEQUENCE_FINAL_BYTE_LAST 0x7E
#define CONTROL_SEQUENCE_PARAMETER_SEPARATOR ';'
#define CONTROL_SEQUENCE_MAXIMUM_PARAMETER 9999

/* Final bytes of ESC [ <number> ~ sequences (xterm/VT220 numbering). */
#define TILDE_SEQUENCE_FINAL_BYTE '~'

#define QUIT_EXIT_CODE 0

/* ---- Fixed sizes -------------------------------------------------------- */

#define EVENT_QUEUE_SIZE 64 /* must be a power of two */
#define EVENT_QUEUE_INDEX_MASK (EVENT_QUEUE_SIZE - 1)
#define NO_DOOM_KEY 0
#define DECIMAL_BASE 10

/* ---- State -------------------------------------------------------------- */

typedef struct
{
    int isPressed;
    unsigned char doomKey;
} KeyEvent;

typedef enum
{
    PARSER_STATE_TEXT,
    PARSER_STATE_AFTER_ESCAPE,
    PARSER_STATE_CONTROL_SEQUENCE,
    PARSER_STATE_SINGLE_SHIFT_THREE,
} ParserState;

static KeyEvent eventQueue[EVENT_QUEUE_SIZE];
static uint32_t eventQueueWriteCount;
static uint32_t eventQueueReadCount;

/* At most one key is down: the terminal only ever repeats one. */
static int isKeyHeld;
static unsigned char heldDoomKey;
static uint32_t heldKeyLastCharacterTimeMilliseconds;
static int hasHeldKeyRepeated;

/* Run is a toggle, since holding Shift sends nothing. */
static int isRunToggledOn;

static ParserState parserState = PARSER_STATE_TEXT;
static uint32_t escapeTimeMilliseconds;
static int firstSequenceParameter;
static int isReadingFirstSequenceParameter;

/* ---- Event queue -------------------------------------------------------- */

static uint32_t lastPollTimeMilliseconds;

static void queueEvent(int isPressed, unsigned char doomKey)
{
#if SHOULD_LOG_KEY_EVENTS
    printf("[key] %lu ms %s 0x%02x\n", (unsigned long)lastPollTimeMilliseconds,
           isPressed ? "down" : "up", doomKey);
#endif
    uint32_t queuedEventCount = eventQueueWriteCount - eventQueueReadCount;
    if (queuedEventCount >= EVENT_QUEUE_SIZE)
    {
        /* DOOM takes events every tick; a full queue means input is flooding. */
        return;
    }
    KeyEvent *event = &eventQueue[eventQueueWriteCount & EVENT_QUEUE_INDEX_MASK];
    event->isPressed = isPressed;
    event->doomKey = doomKey;
    eventQueueWriteCount++;
}

int keyboardInputTakeEvent(int *isPressed, unsigned char *doomKey)
{
    if (eventQueueReadCount == eventQueueWriteCount)
    {
        return 0;
    }
    KeyEvent *event = &eventQueue[eventQueueReadCount & EVENT_QUEUE_INDEX_MASK];
    *isPressed = event->isPressed;
    *doomKey = event->doomKey;
    eventQueueReadCount++;
    return 1;
}

/* ---- Held key and made-up release -------------------------------------- */

static void releaseHeldKey(void)
{
    queueEvent(0, heldDoomKey);
    isKeyHeld = 0;
}

static void pressKey(unsigned char doomKey, uint32_t currentTimeMilliseconds)
{
    if (isKeyHeld && heldDoomKey != doomKey)
    {
        /* The terminal repeats only the newest key, so the previous one is up. */
        releaseHeldKey();
    }

    queueEvent(1, doomKey);

    if (isKeyHeld)
    {
        hasHeldKeyRepeated = 1;
    }
    else
    {
        isKeyHeld = 1;
        heldDoomKey = doomKey;
        hasHeldKeyRepeated = 0;
    }
    heldKeyLastCharacterTimeMilliseconds = currentTimeMilliseconds;
}

static void releaseKeyIfQuiet(uint32_t currentTimeMilliseconds)
{
    if (!isKeyHeld)
    {
        return;
    }
    uint32_t releaseWaitMilliseconds = KEY_RELEASE_WAIT_AFTER_FIRST_CHARACTER_MILLISECONDS;
    if (hasHeldKeyRepeated)
    {
        releaseWaitMilliseconds = KEY_RELEASE_WAIT_AFTER_REPEAT_MILLISECONDS;
    }
    uint32_t quietMilliseconds = currentTimeMilliseconds - heldKeyLastCharacterTimeMilliseconds;
    if (quietMilliseconds >= releaseWaitMilliseconds)
    {
        releaseHeldKey();
    }
}

static void toggleRun(void)
{
    isRunToggledOn = !isRunToggledOn;
    queueEvent(isRunToggledOn, KEY_RSHIFT);
}

/* ---- Character translation ---------------------------------------------- */

#define FIRST_UPPERCASE_LETTER 'A'
#define LAST_UPPERCASE_LETTER 'Z'
#define LOWERCASE_OFFSET ('a' - 'A')
#define FIRST_PRINTABLE_CHARACTER ' '
#define LAST_PRINTABLE_CHARACTER '~'

#define RUN_TOGGLE_CHARACTER 'r'

/*
 * Plain characters. DOOM's own bindings are arrows to move and turn, Ctrl to
 * fire, Space to use, Alt to strafe, Shift to run, Esc for the menu, Tab for
 * the map, 1-7 for weapons. The modifier keys are moved to letters here, and
 * WASD is added because most players expect it. Any letter mapped here can no
 * longer be typed, which breaks cheat codes that contain it (idkfa, iddqd).
 */
static unsigned char translateCharacter(unsigned char character)
{
    if (character >= FIRST_UPPERCASE_LETTER && character <= LAST_UPPERCASE_LETTER)
    {
        /* Shift + letter: same key (DOOM bindings and menus use lowercase). */
        character = (unsigned char)(character + LOWERCASE_OFFSET);
    }

    switch (character)
    {
    case CARRIAGE_RETURN_CHARACTER:
    case LINE_FEED_CHARACTER:
        return KEY_ENTER;
    case DELETE_CHARACTER:
    case BACKSPACE_CHARACTER:
        return KEY_BACKSPACE;
    case TAB_CHARACTER:
        return KEY_TAB;
    case 'w':
        return KEY_UPARROW;
    case 's':
        return KEY_DOWNARROW;
    case 'a':
    case ',':
        return KEY_STRAFE_L;
    case 'd':
    case '.':
        return KEY_STRAFE_R;
    case 'f':
        return KEY_FIRE;
    case ' ':
    case 'e':
        return KEY_USE;
    default:
        break;
    }

    if (character >= FIRST_PRINTABLE_CHARACTER && character <= LAST_PRINTABLE_CHARACTER)
    {
        /* Menus ('y'/'n'), weapons '1'-'7', screen size '-'/'=', ... */
        return character;
    }
    return NO_DOOM_KEY;
}

/* ESC [ <final> and ESC O <final>: arrows, Home and End. */
static unsigned char translateCursorKeyFinalByte(unsigned char finalByte)
{
    switch (finalByte)
    {
    case 'A':
        return KEY_UPARROW;
    case 'B':
        return KEY_DOWNARROW;
    case 'C':
        return KEY_RIGHTARROW;
    case 'D':
        return KEY_LEFTARROW;
    case 'H':
        return KEY_HOME;
    case 'F':
        return KEY_END;
    default:
        return NO_DOOM_KEY;
    }
}

/*
 * ESC O P/Q/R/S = F1-F4. Not accepted after ESC [: there "ESC [ rows ; columns R"
 * is the terminal's cursor position report, and a late or repeated report
 * would otherwise press F3 (DOOM's load menu).
 */
static unsigned char translateSingleShiftThreeFinalByte(unsigned char finalByte)
{
    switch (finalByte)
    {
    case 'P':
        return KEY_F1;
    case 'Q':
        return KEY_F2;
    case 'R':
        return KEY_F3;
    case 'S':
        return KEY_F4;
    default:
        return translateCursorKeyFinalByte(finalByte);
    }
}

/*
 * ESC [ <number> ~ : the number identifies the key (VT220 numbering, used by
 * xterm, PuTTY and MobaXterm; the gaps at 9, 10, 16 and 22 are historical).
 */
typedef struct
{
    int sequenceNumber;
    unsigned char doomKey;
} TildeSequenceKey;

static const TildeSequenceKey tildeSequenceKeys[] = {
    {1, KEY_HOME},  {2, KEY_INS},   {3, KEY_DEL},   {4, KEY_END},
    {5, KEY_PGUP},  {6, KEY_PGDN},  {7, KEY_HOME},  {8, KEY_END},
    {11, KEY_F1},   {12, KEY_F2},   {13, KEY_F3},   {14, KEY_F4},
    {15, KEY_F5},   {17, KEY_F6},   {18, KEY_F7},   {19, KEY_F8},
    {20, KEY_F9},   {21, KEY_F10},  {23, KEY_F11},  {24, KEY_F12},
};

static unsigned char translateTildeSequence(int sequenceNumber)
{
    size_t keyCount = sizeof(tildeSequenceKeys) / sizeof(tildeSequenceKeys[0]);
    for (size_t keyIndex = 0; keyIndex < keyCount; keyIndex++)
    {
        if (tildeSequenceKeys[keyIndex].sequenceNumber == sequenceNumber)
        {
            return tildeSequenceKeys[keyIndex].doomKey;
        }
    }
    return NO_DOOM_KEY;
}

/* ---- Byte parser -------------------------------------------------------- */

static void pressIfMapped(unsigned char doomKey, uint32_t currentTimeMilliseconds)
{
    if (doomKey != NO_DOOM_KEY)
    {
        pressKey(doomKey, currentTimeMilliseconds);
    }
}

static void handleTextByte(unsigned char receivedByte, uint32_t currentTimeMilliseconds)
{
    if (receivedByte == ESCAPE_CHARACTER)
    {
        parserState = PARSER_STATE_AFTER_ESCAPE;
        escapeTimeMilliseconds = currentTimeMilliseconds;
        return;
    }
    if (receivedByte == CONTROL_C_CHARACTER)
    {
        /*
         * QEMU -nographic and serial terminals pass Ctrl-C to the program
         * instead of stopping anything, so it is the way out of the game.
         */
        printf("\n[baremetal] Ctrl-C: quitting\n");
        exit(QUIT_EXIT_CODE);
    }
    if (receivedByte == RUN_TOGGLE_CHARACTER || receivedByte == RUN_TOGGLE_CHARACTER - LOWERCASE_OFFSET)
    {
        toggleRun();
        return;
    }
    pressIfMapped(translateCharacter(receivedByte), currentTimeMilliseconds);
}

static void handleByte(unsigned char receivedByte, uint32_t currentTimeMilliseconds)
{
    switch (parserState)
    {
    case PARSER_STATE_TEXT:
        handleTextByte(receivedByte, currentTimeMilliseconds);
        return;

    case PARSER_STATE_AFTER_ESCAPE:
        if (receivedByte == CONTROL_SEQUENCE_INTRODUCER)
        {
            parserState = PARSER_STATE_CONTROL_SEQUENCE;
            firstSequenceParameter = 0;
            isReadingFirstSequenceParameter = 1;
            return;
        }
        if (receivedByte == SINGLE_SHIFT_THREE_INTRODUCER)
        {
            parserState = PARSER_STATE_SINGLE_SHIFT_THREE;
            return;
        }
        /* ESC followed by anything else: the Escape key, then that byte (Alt+key). */
        parserState = PARSER_STATE_TEXT;
        pressKey(KEY_ESCAPE, currentTimeMilliseconds);
        handleTextByte(receivedByte, currentTimeMilliseconds);
        return;

    case PARSER_STATE_SINGLE_SHIFT_THREE:
        parserState = PARSER_STATE_TEXT;
        pressIfMapped(translateSingleShiftThreeFinalByte(receivedByte), currentTimeMilliseconds);
        return;

    case PARSER_STATE_CONTROL_SEQUENCE:
        if (receivedByte >= CONTROL_SEQUENCE_FINAL_BYTE_FIRST && receivedByte <= CONTROL_SEQUENCE_FINAL_BYTE_LAST)
        {
            parserState = PARSER_STATE_TEXT;
            if (receivedByte == TILDE_SEQUENCE_FINAL_BYTE)
            {
                pressIfMapped(translateTildeSequence(firstSequenceParameter), currentTimeMilliseconds);
                return;
            }
            /* Modifier parameters (ESC [ 1 ; 5 A = Ctrl+Up) are ignored. */
            pressIfMapped(translateCursorKeyFinalByte(receivedByte), currentTimeMilliseconds);
            return;
        }
        if (receivedByte == CONTROL_SEQUENCE_PARAMETER_SEPARATOR)
        {
            isReadingFirstSequenceParameter = 0;
            return;
        }
        if (isReadingFirstSequenceParameter && receivedByte >= '0' && receivedByte <= '9'
            && firstSequenceParameter <= CONTROL_SEQUENCE_MAXIMUM_PARAMETER)
        {
            firstSequenceParameter = firstSequenceParameter * DECIMAL_BASE + (receivedByte - '0');
        }
        return;
    }
}

void keyboardInputPoll(uint32_t currentTimeMilliseconds)
{
    lastPollTimeMilliseconds = currentTimeMilliseconds;

    /*
     * Take every waiting byte before judging timeouts: after a long frame
     * transmission the rest of an escape sequence, or the next repeat of a
     * held key, is usually already in the receive buffer.
     */
    for (;;)
    {
        int receivedByte = platformConsoleReadCharacter();
        if (receivedByte < 0)
        {
            break;
        }
        handleByte((unsigned char)receivedByte, currentTimeMilliseconds);
    }

    if (parserState == PARSER_STATE_AFTER_ESCAPE
        && currentTimeMilliseconds - escapeTimeMilliseconds >= LONE_ESCAPE_WAIT_MILLISECONDS)
    {
        parserState = PARSER_STATE_TEXT;
        pressKey(KEY_ESCAPE, currentTimeMilliseconds);
    }

    releaseKeyIfQuiet(currentTimeMilliseconds);
}
