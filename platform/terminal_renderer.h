/*
 * Draws DOOM's framebuffer as text on a terminal attached to the UART.
 */
#ifndef TERMINAL_RENDERER_H
#define TERMINAL_RENDERER_H

#include <stdint.h>

/* Values for FRAME_RENDER_MODE (chosen at build time from the Makefile). */
#define FRAME_RENDER_MODE_ASCII 0       /* brightness as " .:-=+*#%@", no colour */
#define FRAME_RENDER_MODE_TRUECOLOR 1   /* "▀" half blocks, 24-bit RGB escapes */
#define FRAME_RENDER_MODE_PALETTE256 2  /* "▀" half blocks, xterm 256-colour escapes */

/*
 * Picks the output size. With requestedColumnCount == 0 it asks the terminal
 * for its size; if the terminal does not answer (output is a file, or the
 * terminal does not support the query) it falls back to a fixed width.
 */
void terminalRendererInitialize(int renderMode, int requestedColumnCount, int shouldRedrawInPlace,
                                int sourceWidth, int sourceHeight);

/* pixels: width x height, 0x00RRGGBB per pixel. */
void terminalRendererDrawFrame(const uint32_t *pixels, int width, int height, unsigned long frameNumber);

#endif /* TERMINAL_RENDERER_H */
