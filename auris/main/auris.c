#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "ble/ble_scan.h"
#include "ble/ble_connect.h"

#define TAG "auris"

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,  // connectable
        .disc_mode = BLE_GAP_DISC_MODE_GEN,  // general discoverable
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    ble_gap_adv_set_fields(&fields);

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT, NULL,
                               BLE_HS_FOREVER, &adv_params,
                               ble_connect_gap_event_cb, NULL);
    if (rc != 0)
        printf("[adv] start failed: %d\n", rc);
    else
        printf("[adv] advertising as \"%s\"\n", name);
}

static void configure_security(void)
{
    // SC Only + bonding + MITM
    ble_hs_cfg.sm_io_cap     = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding    = 1;
    ble_hs_cfg.sm_mitm       = 1;
    ble_hs_cfg.sm_sc         = 1;   // Secure Connections
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

static void on_sync(void)
{
    // Generate RPA from IRK stored in NVS
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    configure_security();
    ble_connect_set_on_disconnect(start_advertising);
    start_advertising();

    printf("\n=== Auris-MP3 — BLE Ready ===\n");
    printf("Advertising as \"Auris-MP3\" — connect from your phone\n");
    printf("Commands:\n");
    printf("  scan [sec]  — scan for BLE devices\n");
    printf("  conn <idx>  — connect to device by index\n");
    printf("  disc        — disconnect\n");
    printf("  list        — show last scan results\n");
    printf("=============================\n\n");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void read_line(char *buf, int max)
{
    int i = 0;
    while (i < max - 1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }
        putchar(c);
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
}

static void cmd_loop(void *param)
{
    (void)param;
    char line[64];

    // Wait for BLE stack to sync
    vTaskDelay(pdMS_TO_TICKS(500));

    for (;;) {
        printf("auris> ");
        fflush(stdout);
        read_line(line, sizeof(line));

        if (strncmp(line, "scan", 4) == 0) {
            uint16_t sec = 10;
            if (line[4] == ' ')
                sec = (uint16_t)atoi(line + 5);
            printf("[scan] scanning %d seconds...\n", sec);
            ble_scan_clear();
            ble_scan_start(sec);

            vTaskDelay(pdMS_TO_TICKS(sec * 1000 + 500));
            ble_scan_print_results();

        } else if (strncmp(line, "conn", 4) == 0 && line[4] == ' ') {
            int idx = atoi(line + 5);
            const ble_scan_results_t *res = ble_scan_get_results();
            if (idx < 0 || idx >= res->count) {
                printf("invalid index\n");
                continue;
            }
            const ble_scan_device_t *d = &res->devices[idx];
            if (!d->connectable) {
                printf("device not connectable\n");
                continue;
            }
            printf("[conn] connecting to %s...\n",
                   d->name[0] ? d->name : "(unknown)");
            ble_connect_to(d->addr, d->addr_type);

        } else if (strcmp(line, "disc") == 0) {
            ble_disconnect();

        } else if (strcmp(line, "list") == 0) {
            ble_scan_print_results();

        } else if (line[0] != '\0') {
            printf("unknown command: %s\n", line);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_device_name_set("Auris-MP3");

    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(cmd_loop, "cmd", 4096, NULL, 5, NULL);
}
