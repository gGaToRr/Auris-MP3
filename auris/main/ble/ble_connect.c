#include "ble_connect.h"

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

            struct ble_gap_conn_desc desc;
            ble_gap_conn_find(s_conn_handle, &desc);

            printf("\n========== CONNECTED ==========\n");
            printf("  handle      : %d\n", s_conn_handle);
            printf("  peer addr   : %02X:%02X:%02X:%02X:%02X:%02X\n",
                   desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                   desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                   desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            printf("  addr type   : %s\n",
                   desc.peer_id_addr.type == 0 ? "public" :
                   desc.peer_id_addr.type == 1 ? "random" :
                   desc.peer_id_addr.type == 2 ? "rpa+public" :
                   desc.peer_id_addr.type == 3 ? "rpa+random" : "unknown");
            printf("  our addr    : %02X:%02X:%02X:%02X:%02X:%02X\n",
                   desc.our_id_addr.val[5], desc.our_id_addr.val[4],
                   desc.our_id_addr.val[3], desc.our_id_addr.val[2],
                   desc.our_id_addr.val[1], desc.our_id_addr.val[0]);
            printf("  conn itvl   : %d (x1.25ms = %.1fms)\n",
                   desc.conn_itvl, desc.conn_itvl * 1.25);
            printf("  latency     : %d\n", desc.conn_latency);
            printf("  timeout     : %d (x10ms = %dms)\n",
                   desc.supervision_timeout, desc.supervision_timeout * 10);
            printf("  encrypted   : %s\n", desc.sec_state.encrypted ? "yes" : "no");
            printf("  authenticated: %s\n", desc.sec_state.authenticated ? "yes" : "no");
            printf("  bonded      : %s\n", desc.sec_state.bonded ? "yes" : "no");
            printf("  role        : %s\n", desc.role == 0 ? "central" : "peripheral");
            printf("===============================\n\n");

            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0)
                printf("[sec] security initiate failed: %d\n", rc);
        } else {
            printf("[conn] connect failed: status=%d\n", event->connect.status);
            s_connected = false;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        struct ble_gap_conn_desc *d = &event->disconnect.conn;
        printf("\n========== DISCONNECTED ==========\n");
        printf("  peer addr : %02X:%02X:%02X:%02X:%02X:%02X\n",
               d->peer_id_addr.val[5], d->peer_id_addr.val[4],
               d->peer_id_addr.val[3], d->peer_id_addr.val[2],
               d->peer_id_addr.val[1], d->peer_id_addr.val[0]);
        printf("  reason    : 0x%02X (%d)\n",
               event->disconnect.reason, event->disconnect.reason);
        printf("==================================\n\n");
        s_connected   = false;
        s_conn_handle = 0;
        if (s_on_disconnect)
            s_on_disconnect();
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (event->enc_change.status == 0) {
            printf("\n---------- ENCRYPTION ON ----------\n");
            printf("  encrypted    : yes\n");
            printf("  authenticated: %s\n", desc.sec_state.authenticated ? "yes" : "no");
            printf("  bonded       : %s\n", desc.sec_state.bonded ? "yes" : "no");
            printf("  key size     : %d\n", desc.sec_state.key_size);
            printf("-----------------------------------\n\n");
        } else {
            printf("[sec] encryption failed: status=%d\n", event->enc_change.status);
        }
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Anti-BLERP: reject re-pairing on an already-bonded link.
        // A legitimate peer never needs to re-pair from scratch once bonded.
        // Accepting would let an attacker replace the stored LTK.
        printf("[sec] re-pairing attempt REJECTED (anti-BLERP)\n");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // SC numeric comparison — auto-confirm for now (no display).
        // TODO: when we have a display, show the passkey for user confirm.
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            printf("[sec] numeric comparison: %06lu — auto-confirm\n",
                   (unsigned long)event->passkey.params.numcmp);
            struct ble_sm_io pk = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1 };
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
        }
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
