/*
 * activation.h
 *
 * Device activation gate.
 *
 *   PIN  = first 8 digits (cyclically) found in base64(md5(device_id_hex))
 *   CODE = ((PIN * 3) - 82) * 2 + 1524
 *        = PIN * 6 + 1360
 *
 * PIN is stable for the life of the chip (derived from FICR->DEVICEID).
 * Activated flag is persisted in VFS at /activation.bin, which survives
 * normal firmware (DFU) updates.
 */
#ifndef ACTIVATION_H
#define ACTIVATION_H

#include <stdbool.h>
#include <stdint.h>

#define ACTIVATION_PIN_LEN 8

/* Load activation state from VFS. Call after settings_init(). Returns
 * 0 on success, negative on storage error. Failure does NOT mean the
 * device is activated; callers must check activation_is_activated(). */
int32_t activation_init(void);

/* True iff a successful activation has been persisted on this device. */
bool activation_is_activated(void);

/* 8-digit PIN as a null-terminated C string (pointer to internal
 * static buffer; caller must not free or modify). */
const char *activation_get_pin_string(void);

/* PIN parsed back to an integer (leading zeros in the string become
 * a smaller integer). */
uint32_t activation_get_pin_value(void);

/* Expected activation code for this device. */
int32_t activation_get_expected_code(void);

/* Try to activate with the user-entered code. Returns true if the
 * code matched (and state was persisted to flash). */
bool activation_try_activate(int32_t entered_code);

#endif /* ACTIVATION_H */
