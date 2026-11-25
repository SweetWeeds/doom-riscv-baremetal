/*
 * Terminal renderer for the bare-metal DOOM port.
 *
 * Colour modes print the "upper half block" character U+2580 in every cell:
 * its foreground colour paints the top half of the cell and its background
 * colour the bottom half. A terminal cell is about twice as tall as it is
 * wide, so each half is roughly square and one cell carries two pixels.
 *
 * The image is box-filtered down from DOOM's 320x200: every output pixel is
 * the average of the source pixels it covers.
 */
#include "terminal_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_hardware.h"

/* ---- Geometry ----------------------------------------------------------- */

/*
 * DOOM's 320x200 picture was shown on 4:3 monitors, so its pixels are not
 * square. The output keeps the 4:3 picture shape.
 */
#define DISPLAY_ASPECT_WIDTH 4
#define DISPLAY_ASPECT_HEIGHT 3

/* A terminal cell is about twice as tall as it is wide. */
#define TERMINAL_CELL_HEIGHT_IN_CELL_WIDTHS 2

/* Pixels stacked in one cell: 2 with half blocks, 1 with ASCII characters. */
#define PIXELS_PER_CELL_IN_HALF_BLOCK_MODE 2
#define PIXELS_PER_CELL_IN_ASCII_MODE 1

/* One terminal row is kept free for the status line above the picture. */
#define STATUS_LINE_ROW_COUNT 1

#define FALLBACK_COLUMN_COUNT 80
#define MINIMUM_COLUMN_COUNT 16

/* ---- Terminal size query ------------------------------------------------- */

/*
 * Save the cursor, move it as far right/down as the terminal allows, ask for
 * its position (Device Status Report 6), and restore it. The reply
 * "ESC [ rows ; columns R" is therefore the terminal size. Every VT100-style
 * terminal answers this, unlike the xterm-only window size report.
 */
#define TERMINAL_SIZE_QUERY "\x1b" "7" "\x1b[999;999H" "\x1b[6n" "\x1b" "8"
#define TERMINAL_SIZE_REPLY_TIMEOUT_MILLISECONDS 500
#define TERMINAL_REPLY_BUFFER_SIZE 32
#define ESCAPE_CHARACTER 0x1b
#define DECIMAL_BASE 10

/* ---- Escape sequences --------------------------------------------------- */

#define ANSI_CLEAR_SCREEN "\x1b[2J"
#define ANSI_CURSOR_HOME "\x1b[H"
#define ANSI_HIDE_CURSOR "\x1b[?25l"
#define ANSI_SHOW_CURSOR "\x1b[?25h"
#define ANSI_RESET_ATTRIBUTES "\x1b[0m"
#define ANSI_ERASE_TO_END_OF_LINE "\x1b[K"

/* SGR parameters that start an extended colour ("38;2;r;g;b" or "38;5;n"). */
#define ANSI_SET_FOREGROUND_COLOUR 38
#define ANSI_SET_BACKGROUND_COLOUR 48

/* UTF-8 encoding of U+2580 UPPER HALF BLOCK. */
#define UPPER_HALF_BLOCK_UTF8 "\xe2\x96\x80"

/* ---- Colour ------------------------------------------------------------- */

#define PIXEL_RED_SHIFT 16
#define PIXEL_GREEN_SHIFT 8
#define PIXEL_BLUE_SHIFT 0
#define PIXEL_CHANNEL_MASK 0xFF
#define PIXEL_CHANNEL_LEVEL_COUNT 256

/* ITU-R BT.601 luma weights, scaled to integers that sum to 1000. */
#define LUMA_RED_WEIGHT 299
#define LUMA_GREEN_WEIGHT 587
#define LUMA_BLUE_WEIGHT 114
#define LUMA_WEIGHT_TOTAL 1000

/* xterm 256-colour palette: entries 16..231 are a 6x6x6 RGB cube. */
#define PALETTE256_CUBE_FIRST_INDEX 16
#define PALETTE256_CUBE_LEVELS_PER_CHANNEL 6

/* Entries 232..255 are a grey ramp: level = 8 + 10 * step. */
#define PALETTE256_GREY_FIRST_INDEX 232
#define PALETTE256_GREY_STEP_COUNT 24
#define PALETTE256_GREY_FIRST_LEVEL 8
#define PALETTE256_GREY_LEVEL_SPACING 10

/* Sentinel meaning "no colour has been sent on this row yet". */
#define NO_COLOUR_SENT 0xFFFFFFFFu

/* Characters ordered from darkest to brightest. */
static const char brightnessRamp[] = " .:-=+*#%@";
#define BRIGHTNESS_RAMP_LENGTH ((int)(sizeof(brightnessRamp) - 1))

/* ---- Output buffering --------------------------------------------------- */

/*
 * A colour frame can be several hundred kilobytes of escape sequences, so it
 * is assembled in a fixed chunk and handed to stdio whenever the chunk fills,
 * instead of reserving memory for a whole frame.
 */
#define OUTPUT_CHUNK_SIZE 65536
#define LONGEST_SINGLE_APPEND 64

static char outputChunk[OUTPUT_CHUNK_SIZE];
static size_t outputChunkLength = 0;

static void flushOutputChunk(void)
{
    if (outputChunkLength > 0)
    {
        fwrite(outputChunk, 1, outputChunkLength, stdout);
        outputChunkLength = 0;
    }
}

static void appendText(const char *text)
{
    size_t textLength = strlen(text);
    if (outputChunkLength + textLength > OUTPUT_CHUNK_SIZE)
    {
        flushOutputChunk();
    }
    memcpy(outputChunk + outputChunkLength, text, textLength);
    outputChunkLength += textLength;
}

static void appendCharacter(char character)
{
    if (outputChunkLength + 1 > OUTPUT_CHUNK_SIZE)
    {
        flushOutputChunk();
    }
    outputChunk[outputChunkLength++] = character;
}

/* ---- Renderer state ----------------------------------------------------- */

static int activeRenderMode = FRAME_RENDER_MODE_ASCII;
static int outputColumnCount = FALLBACK_COLUMN_COUNT;
static int outputRowCount = 0;
static int isRedrawingInPlace = 0;
static int hasClearedScreen = 0;

static int isHalfBlockMode(void)
{
    return activeRenderMode == FRAME_RENDER_MODE_TRUECOLOR
        || activeRenderMode == FRAME_RENDER_MODE_PALETTE256;
}

static int pixelsPerCell(void)
{
    if (isHalfBlockMode())
    {
        return PIXELS_PER_CELL_IN_HALF_BLOCK_MODE;
    }
    return PIXELS_PER_CELL_IN_ASCII_MODE;
}

/*
 * Reads the reply to TERMINAL_SIZE_QUERY. Returns 1 and fills the size when a
 * well-formed reply arrives before the timeout, 0 otherwise.
 */
static int queryTerminalSize(int *columnCount, int *rowCount)
{
    fflush(stdout);
    platformConsoleWriteString(TERMINAL_SIZE_QUERY);

    char reply[TERMINAL_REPLY_BUFFER_SIZE];
    int replyLength = 0;
    uint64_t deadline = platformReadTimeMilliseconds() + TERMINAL_SIZE_REPLY_TIMEOUT_MILLISECONDS;
    int hasSeenReplyEnd = 0;

    while (!hasSeenReplyEnd && platformReadTimeMilliseconds() < deadline)
    {
        int receivedCharacter = platformConsoleReadCharacter();
        if (receivedCharacter < 0)
        {
            continue;
        }
        if (receivedCharacter == ESCAPE_CHARACTER)
        {
            /* Start of the reply; drop anything typed before it. */
            replyLength = 0;
        }
        if (replyLength < TERMINAL_REPLY_BUFFER_SIZE - 1)
        {
            reply[replyLength++] = (char)receivedCharacter;
        }
        if (receivedCharacter == 'R')
        {
            hasSeenReplyEnd = 1;
        }
    }
    reply[replyLength] = '\0';

    if (!hasSeenReplyEnd || replyLength < 2 || reply[0] != ESCAPE_CHARACTER || reply[1] != '[')
    {
        return 0;
    }

    char *parsePosition = reply + 2;
    char *numberEnd = NULL;
    long parsedRowCount = strtol(parsePosition, &numberEnd, DECIMAL_BASE);
    if (numberEnd == parsePosition || *numberEnd != ';')
    {
        return 0;
    }
    parsePosition = numberEnd + 1;
    long parsedColumnCount = strtol(parsePosition, &numberEnd, DECIMAL_BASE);
    if (numberEnd == parsePosition || *numberEnd != 'R')
    {
        return 0;
    }

    *rowCount = (int)parsedRowCount;
    *columnCount = (int)parsedColumnCount;
    return 1;
}

static void restoreTerminalAtExit(void)
{
    flushOutputChunk();
    fputs(ANSI_RESET_ATTRIBUTES ANSI_SHOW_CURSOR "\n", stdout);
    fflush(stdout);
}

/*
 * Chooses output columns and rows so that the picture keeps its 4:3 shape,
 * fits inside the terminal, and never asks for more pixels than DOOM has.
 */
static void chooseOutputSize(int availableColumnCount, int availableRowCount, int sourceWidth, int sourceHeight)
{
    int columnCount = availableColumnCount;
    if (columnCount > sourceWidth)
    {
        columnCount = sourceWidth;
    }

    /*
     * Picture height in cell widths = columns * 3/4. One cell is two cell
     * widths tall, so rows = columns * 3 / (4 * 2).
     */
    int rowsPerColumnDenominator = DISPLAY_ASPECT_WIDTH * TERMINAL_CELL_HEIGHT_IN_CELL_WIDTHS;
    int rowCount = columnCount * DISPLAY_ASPECT_HEIGHT / rowsPerColumnDenominator;

    /* Never more pixel rows than DOOM has; never taller than the terminal. */
    int maximumRowCount = sourceHeight / pixelsPerCell();
    int isTerminalHeightKnown = availableRowCount > 0;
    if (isTerminalHeightKnown && availableRowCount - STATUS_LINE_ROW_COUNT < maximumRowCount)
    {
        maximumRowCount = availableRowCount - STATUS_LINE_ROW_COUNT;
    }
    if (rowCount > maximumRowCount)
    {
        rowCount = maximumRowCount;
        columnCount = rowCount * rowsPerColumnDenominator / DISPLAY_ASPECT_HEIGHT;
    }
    if (columnCount < MINIMUM_COLUMN_COUNT)
    {
        columnCount = MINIMUM_COLUMN_COUNT;
        rowCount = columnCount * DISPLAY_ASPECT_HEIGHT / rowsPerColumnDenominator;
    }

    outputColumnCount = columnCount;
    outputRowCount = rowCount;
}

void terminalRendererInitialize(int renderMode, int requestedColumnCount, int shouldRedrawInPlace,
                                int sourceWidth, int sourceHeight)
{
    activeRenderMode = renderMode;
    isRedrawingInPlace = shouldRedrawInPlace;

    int availableColumnCount = FALLBACK_COLUMN_COUNT;
    int availableRowCount = 0; /* 0 = unknown, do not limit by height */
    const char *sizeSource = "fallback";

    if (requestedColumnCount > 0)
    {
        availableColumnCount = requestedColumnCount;
        sizeSource = "requested";
    }
    else
    {
        int terminalColumnCount = 0;
        int terminalRowCount = 0;
        if (queryTerminalSize(&terminalColumnCount, &terminalRowCount))
        {
            availableColumnCount = terminalColumnCount;
            availableRowCount = terminalRowCount;
            sizeSource = "terminal";
        }
    }

    chooseOutputSize(availableColumnCount, availableRowCount, sourceWidth, sourceHeight);

    printf("[baremetal] terminal output: %d x %d cells (%s size), %d x %d pixels\n",
           outputColumnCount, outputRowCount, sizeSource,
           outputColumnCount, outputRowCount * pixelsPerCell());

    if (isHalfBlockMode() || isRedrawingInPlace)
    {
        atexit(restoreTerminalAtExit);
    }
}

/* ---- Pixel sampling ----------------------------------------------------- */

static uint32_t averageColourOfRegion(const uint32_t *pixels, int width,
                                      int firstColumn, int endColumn, int firstRow, int endRow)
{
    uint32_t redSum = 0;
    uint32_t greenSum = 0;
    uint32_t blueSum = 0;
    for (int row = firstRow; row < endRow; row++)
    {
        const uint32_t *pixelRow = pixels + row * width;
        for (int column = firstColumn; column < endColumn; column++)
        {
            uint32_t pixel = pixelRow[column];
            redSum += (pixel >> PIXEL_RED_SHIFT) & PIXEL_CHANNEL_MASK;
            greenSum += (pixel >> PIXEL_GREEN_SHIFT) & PIXEL_CHANNEL_MASK;
            blueSum += (pixel >> PIXEL_BLUE_SHIFT) & PIXEL_CHANNEL_MASK;
        }
    }
    uint32_t pixelCount = (uint32_t)((endColumn - firstColumn) * (endRow - firstRow));
    uint32_t red = redSum / pixelCount;
    uint32_t green = greenSum / pixelCount;
    uint32_t blue = blueSum / pixelCount;
    return (red << PIXEL_RED_SHIFT) | (green << PIXEL_GREEN_SHIFT) | (blue << PIXEL_BLUE_SHIFT);
}

/* Source range [first, end) covered by output index "index" of "count". */
static void sourceRangeForOutputIndex(int index, int count, int sourceSize, int *first, int *end)
{
    *first = index * sourceSize / count;
    *end = (index + 1) * sourceSize / count;
    if (*end <= *first)
    {
        *end = *first + 1;
    }
}

/*
 * The xterm cube levels are not evenly spaced: the first step jumps from 0
 * to 95. Rounding on an even grid would push DOOM's many dark browns to
 * black or to a too-bright level, so the nearest real level is searched.
 */
static const int palette256CubeLevels[PALETTE256_CUBE_LEVELS_PER_CHANNEL] = { 0, 95, 135, 175, 215, 255 };

static int channelToNearestCubeLevel(int channel)
{
    int nearestLevelIndex = 0;
    int nearestDistance = PIXEL_CHANNEL_LEVEL_COUNT;
    for (int levelIndex = 0; levelIndex < PALETTE256_CUBE_LEVELS_PER_CHANNEL; levelIndex++)
    {
        int distance = abs(channel - palette256CubeLevels[levelIndex]);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestLevelIndex = levelIndex;
        }
    }
    return nearestLevelIndex;
}

static int squaredColourDistance(int red, int green, int blue, int otherRed, int otherGreen, int otherBlue)
{
    int redDifference = red - otherRed;
    int greenDifference = green - otherGreen;
    int blueDifference = blue - otherBlue;
    return redDifference * redDifference + greenDifference * greenDifference + blueDifference * blueDifference;
}

static int colourToPalette256Index(uint32_t colour)
{
    int red = (int)((colour >> PIXEL_RED_SHIFT) & PIXEL_CHANNEL_MASK);
    int green = (int)((colour >> PIXEL_GREEN_SHIFT) & PIXEL_CHANNEL_MASK);
    int blue = (int)((colour >> PIXEL_BLUE_SHIFT) & PIXEL_CHANNEL_MASK);

    /* Candidate 1: nearest point of the 6x6x6 colour cube. */
    int redLevel = channelToNearestCubeLevel(red);
    int greenLevel = channelToNearestCubeLevel(green);
    int blueLevel = channelToNearestCubeLevel(blue);
    int levels = PALETTE256_CUBE_LEVELS_PER_CHANNEL;
    int cubeIndex = PALETTE256_CUBE_FIRST_INDEX + redLevel * levels * levels + greenLevel * levels + blueLevel;
    int cubeDistance = squaredColourDistance(red, green, blue,
                                             palette256CubeLevels[redLevel],
                                             palette256CubeLevels[greenLevel],
                                             palette256CubeLevels[blueLevel]);

    /* Candidate 2: nearest step of the 24-step grey ramp (8, 18, ..., 238). */
    int averageChannel = (red + green + blue) / 3;
    int greyStep = (averageChannel - PALETTE256_GREY_FIRST_LEVEL + PALETTE256_GREY_LEVEL_SPACING / 2)
                 / PALETTE256_GREY_LEVEL_SPACING;
    if (greyStep < 0)
    {
        greyStep = 0;
    }
    if (greyStep >= PALETTE256_GREY_STEP_COUNT)
    {
        greyStep = PALETTE256_GREY_STEP_COUNT - 1;
    }
    int greyLevel = PALETTE256_GREY_FIRST_LEVEL + greyStep * PALETTE256_GREY_LEVEL_SPACING;
    int greyDistance = squaredColourDistance(red, green, blue, greyLevel, greyLevel, greyLevel);

    if (greyDistance < cubeDistance)
    {
        return PALETTE256_GREY_FIRST_INDEX + greyStep;
    }
    return cubeIndex;
}

/*
 * Appends the escape that sets the foreground ("38") or background ("48")
 * colour. In 256-colour mode the colour is first reduced to a palette index
 * so that identical indexes can be skipped by the caller.
 */
static void appendColourEscape(int isForeground, uint32_t colour)
{
    char escape[LONGEST_SINGLE_APPEND];
    int selector = ANSI_SET_BACKGROUND_COLOUR;
    if (isForeground)
    {
        selector = ANSI_SET_FOREGROUND_COLOUR;
    }

    if (activeRenderMode == FRAME_RENDER_MODE_PALETTE256)
    {
        snprintf(escape, sizeof(escape), "\x1b[%d;5;%dm", selector, (int)colour);
    }
    else
    {
        snprintf(escape, sizeof(escape), "\x1b[%d;2;%u;%u;%um", selector,
                 (unsigned)((colour >> PIXEL_RED_SHIFT) & PIXEL_CHANNEL_MASK),
                 (unsigned)((colour >> PIXEL_GREEN_SHIFT) & PIXEL_CHANNEL_MASK),
                 (unsigned)((colour >> PIXEL_BLUE_SHIFT) & PIXEL_CHANNEL_MASK));
    }
    appendText(escape);
}

/* What is actually compared and sent: RGB, or the palette index. */
static uint32_t colourAsSent(uint32_t colour)
{
    if (activeRenderMode == FRAME_RENDER_MODE_PALETTE256)
    {
        return (uint32_t)colourToPalette256Index(colour);
    }
    return colour;
}

static void appendHalfBlockRow(const uint32_t *pixels, int width, int height, int cellRow)
{
    int pixelRowCount = outputRowCount * PIXELS_PER_CELL_IN_HALF_BLOCK_MODE;
    int topPixelRow = cellRow * PIXELS_PER_CELL_IN_HALF_BLOCK_MODE;
    int topFirst, topEnd, bottomFirst, bottomEnd;
    sourceRangeForOutputIndex(topPixelRow, pixelRowCount, height, &topFirst, &topEnd);
    sourceRangeForOutputIndex(topPixelRow + 1, pixelRowCount, height, &bottomFirst, &bottomEnd);

    /* Only send a colour escape when the colour differs from the last cell. */
    uint32_t lastForeground = NO_COLOUR_SENT;
    uint32_t lastBackground = NO_COLOUR_SENT;

    for (int column = 0; column < outputColumnCount; column++)
    {
        int columnFirst, columnEnd;
        sourceRangeForOutputIndex(column, outputColumnCount, width, &columnFirst, &columnEnd);

        uint32_t topColour = colourAsSent(averageColourOfRegion(pixels, width, columnFirst, columnEnd, topFirst, topEnd));
        uint32_t bottomColour = colourAsSent(averageColourOfRegion(pixels, width, columnFirst, columnEnd, bottomFirst, bottomEnd));

        if (topColour != lastForeground)
        {
            appendColourEscape(1, topColour);
            lastForeground = topColour;
        }
        if (bottomColour != lastBackground)
        {
            appendColourEscape(0, bottomColour);
            lastBackground = bottomColour;
        }
        appendText(UPPER_HALF_BLOCK_UTF8);
    }
    /* Reset before the newline so the background colour does not bleed. */
    appendText(ANSI_RESET_ATTRIBUTES "\n");
}

static void appendAsciiRow(const uint32_t *pixels, int width, int height, int cellRow)
{
    int rowFirst, rowEnd;
    sourceRangeForOutputIndex(cellRow, outputRowCount, height, &rowFirst, &rowEnd);

    for (int column = 0; column < outputColumnCount; column++)
    {
        int columnFirst, columnEnd;
        sourceRangeForOutputIndex(column, outputColumnCount, width, &columnFirst, &columnEnd);
        uint32_t colour = averageColourOfRegion(pixels, width, columnFirst, columnEnd, rowFirst, rowEnd);

        uint32_t luma = (((colour >> PIXEL_RED_SHIFT) & PIXEL_CHANNEL_MASK) * LUMA_RED_WEIGHT
                       + ((colour >> PIXEL_GREEN_SHIFT) & PIXEL_CHANNEL_MASK) * LUMA_GREEN_WEIGHT
                       + ((colour >> PIXEL_BLUE_SHIFT) & PIXEL_CHANNEL_MASK) * LUMA_BLUE_WEIGHT)
                      / LUMA_WEIGHT_TOTAL;
        int rampIndex = (int)luma * BRIGHTNESS_RAMP_LENGTH / PIXEL_CHANNEL_LEVEL_COUNT;
        appendCharacter(brightnessRamp[rampIndex]);
    }
    appendCharacter('\n');
}

void terminalRendererDrawFrame(const uint32_t *pixels, int width, int height, unsigned long frameNumber)
{
    char statusLine[LONGEST_SINGLE_APPEND];

    if (isRedrawingInPlace)
    {
        if (!hasClearedScreen)
        {
            appendText(ANSI_HIDE_CURSOR ANSI_CLEAR_SCREEN);
            hasClearedScreen = 1;
        }
        appendText(ANSI_CURSOR_HOME);
    }

    snprintf(statusLine, sizeof(statusLine), "---- frame %lu ----", frameNumber);
    appendText(statusLine);
    if (isRedrawingInPlace)
    {
        appendText(ANSI_ERASE_TO_END_OF_LINE);
    }
    appendCharacter('\n');

    for (int cellRow = 0; cellRow < outputRowCount; cellRow++)
    {
        if (isHalfBlockMode())
        {
            appendHalfBlockRow(pixels, width, height, cellRow);
        }
        else
        {
            appendAsciiRow(pixels, width, height, cellRow);
        }
    }

    flushOutputChunk();
    fflush(stdout);
}
