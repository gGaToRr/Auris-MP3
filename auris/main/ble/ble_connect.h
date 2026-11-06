#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "host/ble_gap.h"

// Connect to a device by address. Enforces:
//   - LE Secure Connections Only (no Legacy — anti-downgrade)
//   - Bonding with persistent keys
//   - Reject re-pairing if already bonded (anti-BLERP repairing attack)
int  ble_connect_to(const uint8_t addr[6], uint8_t addr_type);
void ble_disconnect(void);

bool     ble_connect_is_connected(void);
uint16_t ble_connect_get_handle(void);

// GAP event callback — also used by advertising to handle incoming connections
int ble_connect_gap_event_cb(struct ble_gap_event *event, void *arg);

// Called to restart advertising after disconnect (set by app_main)
typedef void (*ble_connect_on_disconnect_fn)(void);
void ble_connect_set_on_disconnect(ble_connect_on_disconnect_fn fn);
