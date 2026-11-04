/****************************************************************************
 * Physical key handling
 *
 *   Key1 (PA34) -> open the pet page
 *   Key2 (PA11) -> ask the on-device model a random question and record the
 *                  exchange in the conversation history
 *
 * Both keys come from the board's /dev/buttons driver
 * (vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/sf32lb52_buttons.c).
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef __KEY_INPUT_H
#define __KEY_INPUT_H

/**
 * Start the key monitor thread.
 *
 * Idempotent; safe to call when /dev/buttons does not exist (logs and gives
 * up, the rest of the UI is unaffected).
 *
 * @return 0 when the thread is running, negative errno otherwise.
 */
int key_input_start(void);

#endif /* __KEY_INPUT_H */
