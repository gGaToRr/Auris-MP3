#include "ble_gatt_svc.h"
#include "ble_connect.h"
#include "log/auris_log.h"
#include "player/player_ctrl.h"
#include "player_status.h"

#include <string.h>
#include <stdio.h>

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ── Auris custom service UUID ──
// a1b2c3d4-0001-4000-8000-00805f9b34fb
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

// Notification RX (phone → ESP): write only, encrypted
// a1b2c3d4-0002-4000-8000-00805f9b34fb
static const ble_uuid128_t chr_notif_rx_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x02, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

// Notification TX (ESP → phone): notify, encrypted
// a1b2c3d4-0003-4000-8000-00805f9b34fb
static const ble_uuid128_t chr_notif_tx_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x03, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static uint16_t s_notif_tx_handle;
static uint16_t s_conn_handle;
static bool     s_subscribed;
static bool     s_encrypted;

static const char *WELCOME_MSG = "Bienvenue sur Auris";

static void try_send_validation(void)
{
    if (!s_encrypted || !s_subscribed)
        return;

    ALOG_I("GATT", "Encrypted + subscribed → sending welcome");
    int rc = ble_gatt_svc_notify((const uint8_t *)WELCOME_MSG,
                                 (uint16_t)strlen(WELCOME_MSG));
    if (rc == 0)
        ALOG_I("GATT", "Welcome sent to conn %d", s_conn_handle);
    else
        ALOG_W("GATT", "Welcome notify failed: %d", rc);
}

// ── Status broadcaster task ──

static const char *phase_str(player_phase_t p)
{
    switch (p) {
    case PLAYER_PHASE_IDLE:     return "IDLE";
    case PLAYER_PHASE_LOADING:  return "LOADING";
    case PLAYER_PHASE_PLAYING:  return "PLAYING";
    case PLAYER_PHASE_FINISHED: return "FINISHED";
    case PLAYER_PHASE_ERROR:    return "ERROR";
    default:                    return "UNKNOWN";
    }
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void status_broadcast_task(void *arg)
{
    (void)arg;
    player_status_t prev = {0};
    bool have_prev = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!s_subscribed)
            continue;

        player_status_t st;
        player_status_get(&st);

        bool changed = !have_prev
            || st.phase != prev.phase
            || st.samples_played / (st.sample_rate ? st.sample_rate : 1)
               != prev.samples_played / (prev.sample_rate ? prev.sample_rate : 1)
            || strcmp(st.path, prev.path) != 0;

        if (!changed)
            continue;

        uint32_t pos_sec = st.sample_rate ? st.samples_played / st.sample_rate : 0;
        uint8_t vol = player_ctrl_get_volume();
        int idx = player_ctrl_track_index();
        int total = player_ctrl_track_count();

        // STATUS:phase|filename|pos_s|dur_s|samplerate|bitrate|vol|idx|count
        char frame[128];
        snprintf(frame, sizeof(frame), "STATUS:%s|%s|%lu|%lu|%lu|%lu|%u|%d|%d",
                 phase_str(st.phase),
                 st.path[0] ? basename_of(st.path) : "",
                 (unsigned long)pos_sec,
                 (unsigned long)st.total_seconds,
                 (unsigned long)st.sample_rate,
                 (unsigned long)st.bitrate_kbps,
                 vol, idx, total);

        ble_gatt_svc_notify((const uint8_t *)frame, (uint16_t)strlen(frame));

        prev = st;
        have_prev = true;
    }
}

// ── Characteristic callbacks ──

static int on_notif_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0)
        return 0;

    uint8_t buf[256];
    uint16_t copy_len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    os_mbuf_copydata(ctxt->om, 0, copy_len, buf);
    buf[copy_len] = '\0';

    ALOG_I("GATT", "RX conn=%d len=%d data=%s", conn_handle, copy_len, buf);

    // ── Command dispatch ──
    const char *resp = NULL;

    if (strcmp((char *)buf, "PLAY") == 0) {
        player_ctrl_send(PLAYER_CMD_PLAY);
        resp = "OK:PLAY";
    } else if (strcmp((char *)buf, "PAUSE") == 0) {
        player_ctrl_send(PLAYER_CMD_PAUSE);
        resp = "OK:PAUSE";
    } else if (strcmp((char *)buf, "RESTART") == 0) {
        player_ctrl_send(PLAYER_CMD_RESTART);
        resp = "OK:RESTART";
    } else if (strcmp((char *)buf, "NEXT") == 0) {
        player_ctrl_send(PLAYER_CMD_NEXT);
        resp = "OK:NEXT";
    } else if (strcmp((char *)buf, "PREV") == 0) {
        player_ctrl_send(PLAYER_CMD_PREV);
        resp = "OK:PREV";
    } else if (strcmp((char *)buf, "VOL+") == 0) {
        player_ctrl_send(PLAYER_CMD_VOL_UP);
        resp = "OK:VOL+";
    } else if (strcmp((char *)buf, "VOL-") == 0) {
        player_ctrl_send(PLAYER_CMD_VOL_DOWN);
        resp = "OK:VOL-";
    } else if (strcmp((char *)buf, "UNPAIR") == 0) {
        ALOG_W("GATT", "Erasing all bonding data");
        ble_store_clear();
        ble_connect_allow_repairing();
        resp = "OK:UNPAIR";
        ble_gatt_svc_notify((const uint8_t *)resp, (uint16_t)strlen(resp));
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (resp && s_subscribed)
        ble_gatt_svc_notify((const uint8_t *)resp, (uint16_t)strlen(resp));

    return 0;
}

static int on_notif_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

// ── GATT service table ──

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &chr_notif_rx_uuid.u,
                .access_cb  = on_notif_rx_write,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid       = &chr_notif_tx_uuid.u,
                .access_cb  = on_notif_tx_access,
                .val_handle = &s_notif_tx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 },
        },
    },
    { 0 },
};

// ── Public API ──

void ble_gatt_svc_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        printf("[gatt] count_cfg failed: %d\n", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        printf("[gatt] add_svcs failed: %d\n", rc);
        return;
    }

    xTaskCreate(status_broadcast_task, "ble_status", 4096, NULL, 3, NULL);
    printf("[gatt] Auris service registered\n");
}

int ble_gatt_svc_notify(const uint8_t *data, uint16_t len)
{
    if (s_notif_tx_handle == 0) {
        printf("[gatt] TX handle not ready\n");
        return -1;
    }
    if (!s_subscribed) {
        printf("[gatt] peer not subscribed\n");
        return -2;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om)
        return -3;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_notif_tx_handle, om);
    if (rc != 0)
        printf("[gatt] notify failed: %d\n", rc);

    return rc;
}

void ble_gatt_svc_on_connect(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    s_subscribed  = false;
    s_encrypted   = false;
}

void ble_gatt_svc_on_disconnect(void)
{
    s_conn_handle = 0;
    s_subscribed  = false;
    s_encrypted   = false;
    ALOG_I("GATT", "State reset (disconnected)");
}

void ble_gatt_svc_on_subscribe(uint16_t conn_handle, bool subscribed)
{
    if (conn_handle != s_conn_handle)
        return;

    s_subscribed = subscribed;
    ALOG_I("GATT", "Peer %s notifications",
           subscribed ? "subscribed to" : "unsubscribed from");

    if (subscribed)
        try_send_validation();
}

void ble_gatt_svc_on_encrypted(uint16_t conn_handle)
{
    if (conn_handle != s_conn_handle)
        return;

    s_encrypted = true;
    ALOG_I("GATT", "Encryption active on conn %d", conn_handle);

    if (s_subscribed)
        try_send_validation();
}

bool ble_gatt_svc_is_subscribed(void)
{
    return s_subscribed;
}
