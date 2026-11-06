#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BLE_SCAN_MAX_DEVICES 32
#define BLE_SCAN_NAME_MAX   32

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    char     name[BLE_SCAN_NAME_MAX];
    int8_t   rssi;
    bool     connectable;
} ble_scan_device_t;

typedef struct {
    ble_scan_device_t devices[BLE_SCAN_MAX_DEVICES];
    int               count;
} ble_scan_results_t;

// Start a BLE scan for duration_sec seconds. 0 = 10s default.
int  ble_scan_start(uint16_t duration_sec);
void ble_scan_stop(void);

const ble_scan_results_t *ble_scan_get_results(void);
void ble_scan_clear(void);
void ble_scan_print_results(void);
