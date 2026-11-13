#include "ble_connect.h"
#include "ble_gatt_svc.h"
#include "log/auris_log.h"

#include <string.h>
#include <stdio.h>

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"

static uint16_t s_conn_handle;
static bool     s_connected;
static ble_connect_on_disconnect_fn s_on_disconnect;

static bool is_already_bonded(const ble_addr_t *addr)
{
    int count;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    if (count == 0)
        return false;

    struct ble_store_value_sec val;
    struct ble_store_key_sec   key = {
        .peer_addr     = *addr,
        .idx           = 0,
    };
    return (ble_store_read_our_sec(&key, &val) == 0 && val.ltk_present);
}

int ble_connect_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected   = true;
            ble_gatt_svc_on_connect(s_conn_handle);

            struct ble_gap_conn_desc desc;
            ble_gap_conn_find(s_conn_handle, &desc);

            ALOG_I("BLE", "Connected handle=%d peer=%02X:%02X:%02X:%02X:%02X:%02X type=%s",
                   s_conn_handle,
                   desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                   desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                   desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                   desc.peer_id_addr.type == 0 ? "public" :
                   desc.peer_id_addr.type == 1 ? "random" :
                   desc.peer_id_addr.type == 2 ? "rpa+public" :
                   desc.peer_id_addr.type == 3 ? "rpa+random" : "?");

            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0)
                ALOG_W("BLE", "Security initiate failed: %d", rc);
        } else {
            ALOG_W("BLE", "Connect failed: status=%d", event->connect.status);
            s_connected = false;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        struct ble_gap_conn_desc *d = &event->disconnect.conn;
        ALOG_I("BLE", "Disconnected peer=%02X:%02X:%02X:%02X:%02X:%02X reason=0x%02X",
               d->peer_id_addr.val[5], d->peer_id_addr.val[4],
               d->peer_id_addr.val[3], d->peer_id_addr.val[2],
               d->peer_id_addr.val[1], d->peer_id_addr.val[0],
               event->disconnect.reason);
        s_connected   = false;
        s_conn_handle = 0;
        ble_gatt_svc_on_disconnect();
        if (s_on_disconnect)
            s_on_disconnect();
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (event->enc_change.status == 0) {
            ALOG_I("BLE", "Encrypted auth=%s bonded=%s keysize=%d",
                   desc.sec_state.authenticated ? "yes" : "no",
                   desc.sec_state.bonded ? "yes" : "no",
                   desc.sec_state.key_size);
            ble_gatt_svc_on_encrypted(event->enc_change.conn_handle);
        } else {
            ALOG_W("BLE", "Encryption failed: status=%d", event->enc_change.status);
        }
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Anti-BLERP: reject re-pairing on an already-bonded link.
        // A legitimate peer never needs to re-pair from scratch once bonded.
        // Accepting would let an attacker replace the stored LTK.
        ALOG_W("BLE", "Re-pairing attempt REJECTED (anti-BLERP)");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            printf("[sec] numeric comparison: %06lu — auto-confirm\n",
                   (unsigned long)event->passkey.params.numcmp);
            struct ble_sm_io pk = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1 };
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ALOG_I("BLE", "Subscribe conn=%d notify=%d",
               event->subscribe.conn_handle,
               event->subscribe.cur_notify);
        ble_gatt_svc_on_subscribe(event->subscribe.conn_handle,
                                  event->subscribe.cur_notify);
        break;

    case BLE_GAP_EVENT_MTU:
        ALOG_I("BLE", "MTU updated conn=%d mtu=%d",
               event->mtu.conn_handle, event->mtu.value);
        break;
    }
    return 0;
}

int ble_connect_to(const uint8_t addr[6], uint8_t addr_type)
{
    ble_addr_t peer = { .type = addr_type };
    memcpy(peer.val, addr, 6);

    // If already bonded, we'll just reconnect — encryption resumes from
    // stored LTK, no new pairing needed. REPEAT_PAIRING guards the rest.
    if (is_already_bonded(&peer))
        printf("[conn] peer already bonded — will use stored keys\n");

    // Stop any running scan before connecting
    ble_gap_disc_cancel();

    int rc = ble_gap_connect(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT,
                             &peer, 5000, NULL, ble_connect_gap_event_cb, NULL);
    if (rc != 0)
        printf("[conn] gap_connect failed: %d\n", rc);

    return rc;
}

void ble_disconnect(void)
{
    if (s_connected)
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

bool ble_connect_is_connected(void)
{
    return s_connected;
}

uint16_t ble_connect_get_handle(void)
{
    return s_conn_handle;
}

void ble_connect_set_on_disconnect(ble_connect_on_disconnect_fn fn)
{
    s_on_disconnect = fn;
}
