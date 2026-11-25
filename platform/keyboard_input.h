/*
 * Turns the bytes a serial terminal sends into DOOM key press and release
 * events. A terminal only sends characters, never "key released", so a
 * release is assumed once a key's characters stop arriving (see
 * keyboard_input.c).
 */
#ifndef KEYBOARD_INPUT_H
#define KEYBOARD_INPUT_H

#include <stdint.h>

/*
 * Reads every byte waiting on the console and queues the resulting events;
 * also queues releases for keys that went quiet. currentTimeMilliseconds is
 * DOOM's clock.
 */
void keyboardInputPoll(uint32_t currentTimeMilliseconds);

/* Returns 1 and fills the outputs if an event was queued, 0 otherwise. */
int keyboardInputTakeEvent(int *isPressed, unsigned char *doomKey);

#endif /* KEYBOARD_INPUT_H */
