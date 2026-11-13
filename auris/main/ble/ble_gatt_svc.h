#pragma once

#include <stdint.h>
#include <stdbool.h>

// Custom Auris GATT service — all characteristics require encryption.
//
// Service UUID:  a1b2c3d4-0001-4000-8000-00805f9b34fb
// Characteristics:
//   - Notification RX (write): phone writes commands/data here
//   - Notification TX (notify): ESP sends alerts/status to phone
//
// Connection validation flow (Android):
//   1. Phone connects → pairing + SC encryption
//   2. Phone discovers services, subscribes to TX (CCCD)
//   3. Auris detects subscription on encrypted link → sends "AURIS:OK"
//   4. Phone receives notification → connection validated end-to-end

void ble_gatt_svc_init(void);

// Send a notification to the connected peer (max 20 bytes for default MTU).
int ble_gatt_svc_notify(const uint8_t *data, uint16_t len);

// Events forwarded from the GAP callback in ble_connect.
void ble_gatt_svc_on_connect(uint16_t conn_handle);
void ble_gatt_svc_on_disconnect(void);
void ble_gatt_svc_on_subscribe(uint16_t conn_handle, bool subscribed);
void ble_gatt_svc_on_encrypted(uint16_t conn_handle);

bool ble_gatt_svc_is_subscribed(void);
