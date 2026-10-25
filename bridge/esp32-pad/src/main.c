// RetroPad bridge.
//
// Two Bluetooth LE roles at once:
// - central: finds a game controller (Xbox Wireless), bonds with it, and
//   subscribes to its HID input reports;
// - peripheral: advertises the RetroPad service, which the watch app reads.
//   watchOS apps can't read HID devices, but they can read this.
//
// BOOT button held for 3 s: forget bonded controllers (to pair another one).
// A status line is logged every 10 s.

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "pad_state.h"

void ble_store_config_init(void);

#define DEVICE_NAME "RetroPad"
#define BOOT_BUTTON_GPIO 9
#define FORGET_HOLD_MS 3000
#define MAX_REPORTS 8

#define UUID16_HID_SERVICE 0x1812
#define UUID16_HID_REPORT 0x2A4D
#define APPEARANCE_GAMEPAD 0x03C4

static const char *TAG = "retropad";

// 6d3c0001-5f8e-4b7a-9c1e-2a7b52504144 and ...0002...: the RetroPad service
// and its state characteristic (struct pad_state, read + notify).
static const ble_uuid128_t pad_service_uuid = BLE_UUID128_INIT(
    0x44, 0x41, 0x50, 0x52, 0x7b, 0x2a, 0x1e, 0x9c, 0x7a, 0x4b, 0x8e, 0x5f, 0x01, 0x00, 0x3c, 0x6d);
static const ble_uuid128_t pad_state_uuid = BLE_UUID128_INIT(
    0x44, 0x41, 0x50, 0x52, 0x7b, 0x2a, 0x1e, 0x9c, 0x7a, 0x4b, 0x8e, 0x5f, 0x02, 0x00, 0x3c, 0x6d);

static uint8_t own_addr_type;
static uint16_t pad_conn = BLE_HS_CONN_HANDLE_NONE;   // the controller (we are central)
static uint16_t watch_conn = BLE_HS_CONN_HANDLE_NONE; // the watch (we are peripheral)
static bool watch_subscribed;
static uint16_t state_val_handle;
static struct pad_state state = { .version = PAD_STATE_VERSION };

static uint16_t hid_start, hid_end;
static uint16_t report_handles[MAX_REPORTS];
static int report_count, subscribe_index;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void start_advertising(void);

// ---------------------------------------------------------------------------
// Peripheral side: the RetroPad service for the watch

static int state_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, &state, sizeof(state)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &pad_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &pad_state_uuid.u,
                .access_cb = state_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &state_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void publish_state(void) {
    state.seq++;
    if (watch_conn != BLE_HS_CONN_HANDLE_NONE && watch_subscribed) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&state, sizeof(state));
        if (om != NULL) {
            ble_gatts_notify_custom(watch_conn, state_val_handle, om);
        }
    }
}

static void start_advertising(void) {
    if (watch_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &pad_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields: %d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s for the watch", DEVICE_NAME);
    }
}

// ---------------------------------------------------------------------------
// Central side: the game controller

static bool looks_like_gamepad(const struct ble_gap_disc_desc *disc, char *name, size_t name_size) {
    struct ble_hs_adv_fields fields;
    name[0] = '\0';
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }
    if (fields.name != NULL && fields.name_len > 0) {
        size_t n = fields.name_len < name_size - 1 ? fields.name_len : name_size - 1;
        memcpy(name, fields.name, n);
        name[n] = '\0';
    }
    bool hid = false;
    for (int i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == UUID16_HID_SERVICE) {
            hid = true;
        }
    }
    bool gamepad = fields.appearance_is_present && fields.appearance == APPEARANCE_GAMEPAD;
    return (hid && gamepad) || strstr(name, "Xbox") != NULL;
}

static void start_scan(void) {
    if (pad_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params params = { 0 };
    params.passive = 0; // active, so scan responses (with names) arrive too
    params.filter_duplicates = 1;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start: %d", rc);
    } else {
        ESP_LOGI(TAG, "looking for a controller (hold its pair button if it was never paired)");
    }
}

static void subscribe_next(uint16_t conn);

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    ESP_LOGI(TAG, "report %d notifications: %s", subscribe_index, error->status == 0 ? "on" : "failed");
    subscribe_index++;
    subscribe_next(conn);
    return 0;
}

// The CCCD of each report characteristic sits right after its value handle.
static void subscribe_next(uint16_t conn) {
    if (subscribe_index >= report_count) {
        ESP_LOGI(TAG, "controller ready (%d input reports)", report_count);
        state.flags |= PAD_FLAG_CONTROLLER;
        publish_state();
        return;
    }
    static const uint8_t enable_notify[2] = { 0x01, 0x00 };
    ble_gattc_write_flat(conn, report_handles[subscribe_index] + 1, enable_notify, sizeof(enable_notify),
                         on_cccd_written, NULL);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0) {
        if (ble_uuid_u16(&chr->uuid.u) == UUID16_HID_REPORT && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)
            && report_count < MAX_REPORTS) {
            report_handles[report_count++] = chr->val_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        subscribe_index = 0;
        subscribe_next(conn);
    }
    return 0;
}

static int on_hid_service(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == 0) {
        hid_start = svc->start_handle;
        hid_end = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (hid_start == 0) {
            ESP_LOGE(TAG, "controller has no HID service");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            ble_gattc_disc_all_chrs(conn, hid_start, hid_end, on_chr, NULL);
        }
    }
    return 0;
}

static void on_report(const uint8_t *data, uint16_t len) {
    static uint8_t last_buttons[4];
    // Log raw reports when the button bytes change (sticks change constantly).
    if (len >= 16 && memcmp(last_buttons, &data[12], 4) != 0) {
        memcpy(last_buttons, &data[12], 4);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_INFO);
    }
    struct pad_state next = state;
    if (!pad_parse_xbox_report(data, len, &next)) {
        ESP_LOGI(TAG, "other report, %d bytes", len);
        return;
    }
    if (memcmp(&next.buttons, &state.buttons, sizeof(state) - 2) != 0) {
        if (next.buttons != state.buttons) {
            ESP_LOGI(TAG, "buttons 0x%04x  L(%d,%d) R(%d,%d) LT %d RT %d", next.buttons, next.lx, next.ly,
                     next.rx, next.ry, next.lt, next.rt);
        }
        state = next;
        publish_state();
    }
}

// ---------------------------------------------------------------------------
// GAP events for both roles

static int gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    char name[32];

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (looks_like_gamepad(&event->disc, name, sizeof(name))) {
            ESP_LOGI(TAG, "found controller \"%s\", connecting", name);
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "connect: %d", rc);
                start_scan();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            start_scan();
            start_advertising();
            return 0;
        }
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (desc.role == BLE_GAP_ROLE_MASTER) {
            pad_conn = event->connect.conn_handle;
            hid_start = hid_end = 0;
            report_count = 0;
            ESP_LOGI(TAG, "controller connected, securing the link");
            ble_gap_security_initiate(pad_conn);
        } else {
            watch_conn = event->connect.conn_handle;
            watch_subscribed = false;
            ESP_LOGI(TAG, "watch connected");
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == pad_conn) {
            ESP_LOGW(TAG, "controller disconnected (%d)", event->disconnect.reason);
            pad_conn = BLE_HS_CONN_HANDLE_NONE;
            struct pad_state released = { .version = PAD_STATE_VERSION, .seq = state.seq, .flags = 0 };
            state = released;
            publish_state();
            start_scan();
        } else if (event->disconnect.conn.conn_handle == watch_conn) {
            ESP_LOGW(TAG, "watch disconnected (%d)", event->disconnect.reason);
            watch_conn = BLE_HS_CONN_HANDLE_NONE;
            watch_subscribed = false;
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle == pad_conn) {
            if (event->enc_change.status == 0) {
                ESP_LOGI(TAG, "controller paired, reading its reports");
                ble_gattc_disc_svc_by_uuid(pad_conn, BLE_UUID16_DECLARE(UUID16_HID_SERVICE), on_hid_service, NULL);
            } else {
                ESP_LOGE(TAG, "pairing failed: %d", event->enc_change.status);
                ble_gap_terminate(pad_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The controller lost our bond (e.g. paired elsewhere): start over.
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == pad_conn && !event->notify_rx.indication) {
            uint8_t buf[64];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            on_report(buf, len);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == state_val_handle) {
            watch_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "watch %s", watch_subscribed ? "subscribed" : "unsubscribed");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Startup and the BOOT button

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_advertising();
    start_scan();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void boot_button_task(void *param) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // Every 2 s: advertising can stop without an event while the link to
        // the controller is set up, so restart it if no watch is connected.
        if (++tick % 40 == 0) {
            if (watch_conn == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
                ESP_LOGW(TAG, "advertising had stopped, restarting it");
                start_advertising();
            }
            if (tick % 200 == 0) {
                ESP_LOGI(TAG, "status: controller %s, watch %s, advertising %s",
                         pad_conn != BLE_HS_CONN_HANDLE_NONE ? "connected" : "searching",
                         watch_conn != BLE_HS_CONN_HANDLE_NONE ? (watch_subscribed ? "subscribed" : "connected") : "none",
                         ble_gap_adv_active() ? "on" : "off");
            }
        }
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 50;
            if (held_ms == FORGET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held: forgetting paired controllers");
                ble_store_clear();
                if (pad_conn != BLE_HS_CONN_HANDLE_NONE) {
                    ble_gap_terminate(pad_conn, BLE_ERR_REM_USER_CONN_TERM);
                }
            }
        } else {
            held_ms = 0;
        }
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // "Just works" bonding, as game controllers expect.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_services));
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    xTaskCreate(boot_button_task, "boot_button", 3072, NULL, 5, NULL);
}
