#pragma once

#include <stdbool.h>
#include <stdint.h>

void fingerprint_init(void);
bool fingerprint_present_hint(void);
void fingerprint_led_idle(void);
// Show the yellow prompt effect. It reverts to idle on its own after a fixed
// hold, or earlier if a fingerprint result arrives. LED only: this grants no
// authorization and never affects PIV key use.
bool fingerprint_led_prompt(void);
// Expire an elapsed prompt hold. Must be called periodically.
void fingerprint_led_tick(void);
bool fingerprint_authorize_poll_once(void);
bool fingerprint_authorize_once(void);
int fingerprint_count(void);
bool fingerprint_enroll(uint16_t slot, void (*prompt)(const char *message));
bool fingerprint_delete(uint16_t slot);
bool fingerprint_delete_all(void);
