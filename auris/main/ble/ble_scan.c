#include "ble_scan.h"

#include <string.h>
#include <stdio.h>

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

static ble_scan_results_t s_results;

static int find_device(const ble_addr_t *addr)
{
    for (int i = 0; i < s_results.count; i++) {
        if (memcmp(s_results.devices[i].addr, addr->val, 6) == 0)
            return i;
    }
    return -1;
}

static void add_or_update(const struct ble_gap_disc_desc *disc)
{
    int idx = find_device(&disc->addr);
    if (idx < 0) {
        if (s_results.count >= BLE_SCAN_MAX_DEVICES)
            return;
        idx = s_results.count++;
        memcpy(s_results.devices[idx].addr, disc->addr.val, 6);
        s_results.devices[idx].addr_type = disc->addr.type;
        s_results.devices[idx].name[0] = '\0';
    }

    s_results.devices[idx].rssi = disc->rssi;

    s_results.devices[idx].connectable =
        (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
         disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0) {
        if (fields.name != NULL && fields.name_len > 0) {
            int len = fields.name_len;
            if (len >= BLE_SCAN_NAME_MAX)
                len = BLE_SCAN_NAME_MAX - 1;
            memcpy(s_results.devices[idx].name, fields.name, len);
            s_results.devices[idx].name[len] = '\0';
        }
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        add_or_update(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        break;
    }
    return 0;
}

int ble_scan_start(uint16_t duration_sec)
{
    if (duration_sec == 0)
        duration_sec = 10;

    struct ble_gap_disc_params params = {
        .itvl            = BLE_GAP_SCAN_FAST_INTERVAL_MIN,
        .window          = BLE_GAP_SCAN_FAST_WINDOW,
        .filter_policy   = BLE_HCI_SCAN_FILT_NO_WL,
        .limited         = 0,
        .passive         = 0,  // active scan — get names via SCAN_RSP
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT, duration_sec * 1000,
                          &params, gap_event_cb, NULL);
    if (rc != 0)
        printf("[scan] start failed: %d\n", rc);

    return rc;
}

void ble_scan_stop(void)
{
    ble_gap_disc_cancel();
}

const ble_scan_results_t *ble_scan_get_results(void)
{
    return &s_results;
}

void ble_scan_clear(void)
{
    memset(&s_results, 0, sizeof(s_results));
}

void ble_scan_print_results(void)
{
    printf("\n--- BLE Scan: %d device(s) ---\n", s_results.count);
    for (int i = 0; i < s_results.count; i++) {
        ble_scan_device_t *d = &s_results.devices[i];
        printf("  [%2d] %02X:%02X:%02X:%02X:%02X:%02X  rssi:%d  %s  %s\n",
               i,
               d->addr[5], d->addr[4], d->addr[3],
               d->addr[2], d->addr[1], d->addr[0],
               d->rssi,
               d->connectable ? "conn" : "    ",
               d->name[0] ? d->name : "(unknown)");
    }
    printf("-----------------------------\n");
}
