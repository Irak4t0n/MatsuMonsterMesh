// SPDX-License-Identifier: GPL-3.0-or-later

#include "meshtastic_lora.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_hosted_custom.h"   // esp_hosted_set_custom_callback / send_custom —
                                 // the old-API path that tanmatsu-lora 0.0.1
                                 // sends through. We MUST register our own
                                 // dispatcher (radio_callback below) before
                                 // any tanmatsu-lora call — without it, the
                                 // P4-side send path returns 1 (no route).
#include "esp_hosted.h"          // esp_hosted_connect_to_slave — force the
                                 // SDIO transport to (re)handshake the C6 once
                                 // the BSP is up. The constructor-time
                                 // esp_hosted_init() that runs before app_main
                                 // tried to power-cycle the radio via the BSP
                                 // already, but bsp_power_set_radio_state()
                                 // fails silently because the coprocessor
                                 // handle is NULL pre-bsp_device_initialize.
                                 // We have to redo that step here.
#include "bsp/power.h"           // bsp_power_set_radio_state for the actual
                                 // C6 power-cycle. The launcher kills C6
                                 // power right before AppFS-chaining to us,
                                 // so we boot with the radio dead.
#include "lora.h"

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *TAG = "ml";

// Internal RX queue depth — tanmatsu-lora owns the queue; we just ask for
// "enough" headroom so we don't drop frames if we sit idle for a few
// hundred ms. Each entry is a ~258-byte struct so 16 = 4 KB.
#define RX_QUEUE_DEPTH 16

static bool                    s_up    = false;
static QueueHandle_t           s_pktq  = NULL;
static meshtastic_lora_stats_t s_stats = {0};
static TaskHandle_t            s_rx_task = NULL;

// RX task: dequeues lora_protocol_lora_packet_t entries that the
// tanmatsu-lora driver enqueues whenever the C6 reports PACKET_RX, then
// re-publishes them on our own queue so the upper layers can pop them
// without depending on the tanmatsu-lora types directly.
static QueueHandle_t s_raw_out_q = NULL;

typedef struct {
    uint8_t length;
    uint8_t data[ML_RAW_MAX_LEN];
} raw_out_t;

// esp-hosted custom-data callback. Fires whenever the C6 emits a custom
// event back to us via its `generate_custom_event(ESP_PRIV_EVENT_LORA, ...)`
// path. The launcher's reference flow (and meshcore's, and the Meshtastic
// Tanmatsu UI's) dispatches type==1 (ESP_PRIV_EVENT_LORA) into
// `lora_transaction_receive`, which feeds tanmatsu-lora's transaction
// semaphore so synchronous calls like `lora_set_config` can complete.
//
// Registering this is mandatory — without it, tanmatsu-lora's transaction
// layer has no way to deliver responses, and `esp_hosted_send_custom` itself
// reports failure (returns 1) because no recipient is configured.
static void mlora_custom_callback(uint8_t type, uint8_t *payload,
                                  uint16_t payload_length)
{
    if (type == 1) {
        // type 1 = ESP_PRIV_EVENT_LORA — packet from the C6's lora_protocol
        // server (either a SET_CONFIG ack/nack, GET_STATUS reply, or an
        // inbound PACKET_RX). tanmatsu-lora's receive function routes it
        // either into the response-buffer (for sync calls) or onto the
        // rx packet queue.
        lora_transaction_receive(payload, payload_length);
    }
    // Other event types (badgelink, echo, init) are ignored here — they
    // belong to other subsystems and we never asked for them.
}

static void rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "rx task started");
    lora_protocol_lora_packet_t pkt;
    for (;;) {
        // Block up to 1s on the tanmatsu-lora queue so we never spin hot.
        if (xQueueReceive(s_pktq, &pkt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        s_stats.rx_packets    += 1;
        s_stats.rx_bytes_total += pkt.length;
        ESP_LOGI(TAG, "rx %u bytes", (unsigned)pkt.length);

        // Forward into our own queue. If full, drop the oldest so the
        // newest arrival isn't lost.
        raw_out_t out;
        out.length = pkt.length > ML_RAW_MAX_LEN ? ML_RAW_MAX_LEN : pkt.length;
        memcpy(out.data, pkt.data, out.length);
        if (xQueueSend(s_raw_out_q, &out, 0) != pdTRUE) {
            raw_out_t drop;
            xQueueReceive(s_raw_out_q, &drop, 0);
            xQueueSend(s_raw_out_q, &out, 0);
            ESP_LOGW(TAG, "out queue full, dropped oldest");
        }
    }
}

// Bring the C6 radio back up and (re)establish the ESP-Hosted SDIO
// transport. Background:
//
//  * The launcher's `prepare_device_for_app_launch()` calls
//    `bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF)` right before
//    chaining to our AppFS app. So when our app boots, the C6 is dead.
//
//  * `esp_hosted_init()` runs from a C-runtime constructor (see
//    port_esp_hosted_host_init.c) BEFORE app_main, BEFORE
//    bsp_device_initialize. Inside, the SDIO driver tries to power-cycle
//    the C6 via `hosted_sdio_reset_slave_callback` →
//    `bsp_power_set_radio_state(...)`, but that fails silently because
//    the coprocessor I²C handle is still NULL at constructor time.
//
//  * Net effect at app_main entry: SDIO peripheral configured on the P4
//    side, C6 still powered off, transport_state stuck below
//    TRANSPORT_TX_ACTIVE, so every `esp_hosted_tx` returns ESP_FAIL —
//    which the lora_protocol layer surfaces as cfg_err=-1.
//
// Fix: after the BSP is initialised, we do the power cycle ourselves and
// then call `esp_hosted_connect_to_slave()` (which is just
// `transport_drv_reconfigure()` under the hood). That re-runs
// `ensure_slave_bus_ready` — this time the BSP-backed power call
// actually toggles the rail — and polls `is_transport_tx_ready()` until
// the C6 firmware boots and completes the SDIO handshake.
static esp_err_t bring_up_transport(void)
{
    bsp_radio_state_t cur = BSP_POWER_RADIO_STATE_OFF;
    esp_err_t gs_err = bsp_power_get_radio_state(&cur);
    if (gs_err == ESP_OK && cur != BSP_POWER_RADIO_STATE_OFF) {
        ESP_LOGI(TAG, "C6 was in state %d, turning OFF first", (int)cur);
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "powering C6 up (APPLICATION)");
    esp_err_t ps_err = bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    if (ps_err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_power_set_radio_state(APPLICATION): %s",
                 esp_err_to_name(ps_err));
        return ps_err;
    }
    // C6 firmware boot to "ready for SDIO" is ~1 s. Mirror the
    // wifi_remote.c reset-callback wait of 1200 ms.
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "esp_hosted_connect_to_slave()...");
    int rc_err = esp_hosted_connect_to_slave();
    if (rc_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave returned %d", rc_err);
        return rc_err;
    }
    ESP_LOGI(TAG, "SDIO transport up");
    return ESP_OK;
}

esp_err_t meshtastic_lora_begin(void)
{
    if (!s_up) {
        // Step 0: get the C6 powered and the SDIO transport handshaked.
        // Without this, every subsequent `esp_hosted_send_custom` call
        // returns ESP_FAIL because `is_transport_tx_ready()` is still
        // false. See bring_up_transport() above for the gory details.
        esp_err_t tr_err = bring_up_transport();
        if (tr_err != ESP_OK) {
            s_stats.last_init_err = (int32_t)tr_err;
            s_stats.last_init_ms  = now_ms();
            return tr_err;
        }

        // Register the esp-hosted custom-data callback FIRST — before
        // lora_init, before any lora_* call. Without this, the P4-side
        // `esp_hosted_send_custom(1, ...)` has no receive-side handler
        // bound and bails out with return code 1 (no route). This is the
        // step meshcore / tanmatsu-meshtastic-ui / the launcher all do
        // explicitly before lora_init — it's not done internally by
        // tanmatsu-lora 0.0.1.
        esp_err_t cb_err = esp_hosted_set_custom_callback(mlora_custom_callback);
        if (cb_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_set_custom_callback: %s",
                     esp_err_to_name(cb_err));
            s_stats.last_init_err = (int32_t)cb_err;
            return cb_err;
        }
        ESP_LOGI(TAG, "esp_hosted custom callback registered");

        esp_err_t init_err = lora_init(RX_QUEUE_DEPTH);
        s_stats.last_init_err = (int32_t)init_err;
        s_stats.last_init_ms  = now_ms();
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "lora_init: %s", esp_err_to_name(init_err));
            return init_err;
        }
        s_pktq = lora_get_packet_queue();
        if (!s_pktq) {
            ESP_LOGE(TAG, "lora_get_packet_queue returned NULL");
            s_stats.last_init_err = (int32_t)ESP_FAIL;
            return ESP_FAIL;
        }
        s_raw_out_q = xQueueCreate(RX_QUEUE_DEPTH, sizeof(raw_out_t));
        if (!s_raw_out_q) {
            ESP_LOGE(TAG, "xQueueCreate failed");
            s_stats.last_init_err = (int32_t)ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(rx_task, "mlrora_rx", 4 * 1024, NULL, 5,
                        &s_rx_task) != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate rx_task failed");
            s_stats.last_init_err = (int32_t)ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        s_stats.init_ok = 1;
        s_up = true;
    }

    lora_protocol_config_params_t cfg = {
        .frequency                  = ML_LONGFAST_US_FREQ_HZ,
        .spreading_factor           = ML_LONGFAST_SF,
        .bandwidth                  = ML_LONGFAST_BW_KHZ,
        .coding_rate                = ML_LONGFAST_CR,
        .sync_word                  = ML_LONGFAST_SYNC_WORD,
        .preamble_length            = ML_LONGFAST_PREAMBLE,
        .power                      = ML_LONGFAST_TX_POWER_DBM,
        .ramp_time                  = 0,
        .crc_enabled                = true,
        .invert_iq                  = false,
        .low_data_rate_optimization = false,
    };

    // Timing fix: the ESP-Hosted SDIO transport isn't TX_ACTIVE until the
    // C6 emits its "Start Data Path" event ~5 seconds into boot. monster_init
    // runs much earlier — our first `lora_set_config` historically fires at
    // P4 ms ≈ 1700, well before the link is live, and the C6 NACKs (cfg_err=1)
    // because the request never properly reaches lora_protocol_handle_packet.
    //
    // Retry loop: try every 500 ms for up to ~12 s. Record only the LAST
    // attempt's err code in stats — so post-success, `lora_stats` shows
    // cfg_err=0 cleanly. Each retry costs ~2 s in the worst case (the
    // tanmatsu-lora layer's own RPC-ack timeout), so the actual elapsed
    // time on a slow link can be 4-6 s. Tunable via ML_CFG_MAX_TRIES.
    #define ML_CFG_MAX_TRIES   24      // 24 * 500ms = 12 s total
    #define ML_CFG_RETRY_MS    500
    esp_err_t cfg_err = ESP_FAIL;
    for (int attempt = 0; attempt < ML_CFG_MAX_TRIES; ++attempt) {
        cfg_err = lora_set_config(&cfg);
        s_stats.last_config_err = (int32_t)cfg_err;
        s_stats.last_config_ms  = now_ms();
        if (cfg_err == ESP_OK) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "lora_set_config OK on attempt %d", attempt + 1);
            }
            break;
        }
        ESP_LOGW(TAG, "lora_set_config attempt %d: %s (retrying)",
                 attempt + 1, esp_err_to_name(cfg_err));
        vTaskDelay(pdMS_TO_TICKS(ML_CFG_RETRY_MS));
    }
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "lora_set_config: gave up after %d tries (last: %s)",
                 ML_CFG_MAX_TRIES, esp_err_to_name(cfg_err));
        return cfg_err;
    }
    s_stats.configs_applied += 1;
    ESP_LOGI(TAG, "config: %u Hz SF%u BW%u kHz CR4/%u power %u dBm",
             (unsigned)ML_LONGFAST_US_FREQ_HZ,
             (unsigned)ML_LONGFAST_SF,
             (unsigned)ML_LONGFAST_BW_KHZ,
             (unsigned)ML_LONGFAST_CR,
             (unsigned)ML_LONGFAST_TX_POWER_DBM);

    // Same retry pattern for set_mode — though if set_config worked,
    // set_mode is on the same channel and should succeed immediately.
    esp_err_t mode_err = ESP_FAIL;
    for (int attempt = 0; attempt < 6; ++attempt) {
        mode_err = lora_set_mode(LORA_PROTOCOL_MODE_RX);
        s_stats.last_mode_err = (int32_t)mode_err;
        s_stats.last_mode_ms  = now_ms();
        if (mode_err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "lora_set_mode RX: %s", esp_err_to_name(mode_err));
        return mode_err;
    }
    ESP_LOGI(TAG, "radio in RX mode — Meshtastic LongFast US 907.125 MHz");
    return ESP_OK;
}

bool meshtastic_lora_is_up(void)
{
    return s_up;
}

esp_err_t meshtastic_lora_send_raw(const uint8_t *data, size_t len)
{
    if (!s_up)                return ESP_ERR_INVALID_STATE;
    if (!data || len == 0)    return ESP_ERR_INVALID_ARG;
    if (len > ML_RAW_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    // SX1262 at CR=4/5 NACKs any packet shorter than 8 bytes — fail fast
    // here with a recognisable code rather than letting it round-trip to
    // the C6 and come back as ESP_FAIL.
    if (len < ML_RAW_MIN_LEN) return ESP_ERR_INVALID_SIZE;

    lora_protocol_lora_packet_t pkt;
    pkt.length = (uint8_t)len;
    memcpy(pkt.data, data, len);

    s_stats.tx_attempted += 1;
    s_stats.last_tx_ms    = now_ms();

    // The C6's `tanmatsu-radio` firmware rejects `PACKET_TX` (with NACK,
    // which surfaces here as ESP_FAIL) if the SX1262 is in continuous RX.
    // Standard SX1262 sequence is:
    //   STANDBY_RC → SET_PAYLOAD → SET_TX → wait DONE → STANDBY_RC → RX
    // The C6 firmware handles the middle three steps internally, but we
    // have to take the chip out of RX ourselves first, then re-arm RX
    // after the send completes. (`meshtastic_lora_begin` always parks
    // the chip in RX once config succeeds, so without this step every
    // `lora_send` after init fails.)
    esp_err_t pre_err = lora_set_mode(LORA_PROTOCOL_MODE_STANDBY_RC);
    s_stats.last_tx_pre_mode_err = (int32_t)pre_err;
    if (pre_err != ESP_OK) {
        // Couldn't even leave RX — abort. Don't bump tx_err: we never
        // actually tried to TX.
        ESP_LOGW(TAG, "lora_set_mode STANDBY_RC pre-TX: %s",
                 esp_err_to_name(pre_err));
        s_stats.last_tx_err = (int32_t)pre_err;
        return pre_err;
    }

    // Brief settle delay after switching from RX to STANDBY_RC.
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t err = lora_send_packet(&pkt);
    s_stats.last_tx_err = (int32_t)err;
    s_stats.last_tx_len = (uint16_t)len;
    if (err != ESP_OK) {
        s_stats.tx_err += 1;
        ESP_LOGW(TAG, "lora_send_packet: %s", esp_err_to_name(err));
    } else {
        s_stats.tx_ok += 1;
        ESP_LOGI(TAG, "tx %u bytes ok", (unsigned)len);
    }

    // Re-arm RX regardless of whether the TX succeeded — leaving the chip
    // in STANDBY would make us deaf to inbound traffic.
    esp_err_t post_err = lora_set_mode(LORA_PROTOCOL_MODE_RX);
    s_stats.last_tx_post_mode_err = (int32_t)post_err;
    if (post_err != ESP_OK) {
        ESP_LOGW(TAG, "lora_set_mode RX post-TX: %s",
                 esp_err_to_name(post_err));
    }
    return err;
}

bool meshtastic_lora_pop_raw(uint8_t *out_buf, size_t max_len, size_t *out_len)
{
    if (!s_up || !out_buf || !out_len || !s_raw_out_q) return false;
    raw_out_t entry;
    if (xQueueReceive(s_raw_out_q, &entry, 0) != pdTRUE) return false;
    size_t copy = entry.length;
    if (copy > max_len) copy = max_len;
    memcpy(out_buf, entry.data, copy);
    *out_len = copy;
    return true;
}

void meshtastic_lora_get_stats(meshtastic_lora_stats_t *out)
{
    if (!out) return;
    *out = s_stats;
}

static inline void probe_record(meshtastic_lora_probe_result_t *r,
                                const char *label, esp_err_t err)
{
    if (r->probe_count >= MESHTASTIC_LORA_PROBE_MAX) return;
    r->entries[r->probe_count].label  = label;
    r->entries[r->probe_count].result = (int32_t)err;
    r->probe_count++;
}

void meshtastic_lora_probe(meshtastic_lora_probe_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // 1. get_status — read-only; should succeed iff the SDIO-RPC channel is
    //    usable at all. Tells us if the link itself works.
    lora_protocol_status_params_t st = {0};
    esp_err_t e_status = lora_get_status(&st);
    probe_record(out, "get_status", e_status);

    // 2. get_config — also read-only; returns whatever config the C6 has.
    //    Useful to confirm we're seeing the launcher's last-applied state.
    lora_protocol_config_params_t cur = {0};
    esp_err_t e_get_cfg = lora_get_config(&cur);
    probe_record(out, "get_config", e_get_cfg);
    if (e_get_cfg == ESP_OK) {
        out->cur_frequency = cur.frequency;
        out->cur_sf        = cur.spreading_factor;
        out->cur_bw        = cur.bandwidth;
        out->cur_cr        = cur.coding_rate;
    }

    // 3. set_mode STANDBY_RC — minimal-payload write op; tells us if any
    //    mutating call works at all.
    esp_err_t e_mode_sb = lora_set_mode(LORA_PROTOCOL_MODE_STANDBY_RC);
    probe_record(out, "set_mode SB", e_mode_sb);

    // 4. set_config — echo the C6's current config back. If get_config
    //    returned OK, this echoes valid values. If THIS still fails, it's
    //    a transport / RPC framing problem, not a params problem.
    esp_err_t e_echo = ESP_ERR_INVALID_STATE;
    if (e_get_cfg == ESP_OK) {
        e_echo = lora_set_config(&cur);
    }
    probe_record(out, "set_config echo", e_echo);

    // 5. set_mode RX — restore the radio to a useful state so this probe
    //    doesn't leave us deaf.
    esp_err_t e_mode_rx = lora_set_mode(LORA_PROTOCOL_MODE_RX);
    probe_record(out, "set_mode RX", e_mode_rx);
}
