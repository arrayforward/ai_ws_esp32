/**
 * @file convai_ws.c
 * @brief ConvAI SDK re-implementation for ESP32 (ESP-IDF) over WebSocket.
 *
 * Implements the device side of the convai.v1 wire protocol
 * (cloud_gateway/docs/cloud_gateway/protocol.md) using
 * esp_websocket_client, exposing the same public API as the
 * original precompiled ConvAI SDK (convai_api.h).
 *
 * Memory design (see docs/05-memory-optimization-100kb.md):
 *  - all audio buffers are static (no per-frame malloc)
 *  - uplink: 20ms frames -> encode -> static TX queue -> sender task
 *  - downlink: WS frame -> decode -> static RX message ring -> pump task
 *  - both absorb network/system stalls: full -> drop oldest, never OOM
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "convai_api.h"
#include "convai_protocol.h"
#include "convai_codec.h"
#include "convai_ring.h"
#include "convai_limits.h"

static const char *TAG = "convai";

#define CONVAI_WS_SUBPROTOCOL   "convai.v1"
#define CONVAI_DEFAULT_URL      "ws://192.168.1.100:9000/"
#define CONVAI_DEFAULT_CODEC    CONVAI_CODEC_G711A

/* ---------------- static memory (the whole point: no per-frame malloc) -- */

/* work buffers */
static uint8_t  s_enc_buf[CONVAI_ENC_BUF_BYTES];                 /* app thread */
static int16_t  s_dec_pcm[CONVAI_DEC_PCM_SAMPLES];               /* ws task */

/* TX queue (static FreeRTOS queue) */
typedef struct {
    uint16_t len;
    uint8_t  data[CONVAI_TX_SLOT_BYTES];
} tx_frame_t;
static uint8_t     s_tx_storage[CONVAI_TX_QUEUE_FRAMES * sizeof(tx_frame_t)];
static StaticQueue_t s_tx_queue_cb;

/* RX jitter ring arena */
static uint8_t     s_rx_arena[CONVAI_RX_RING_BYTES];

typedef struct {
    char server_url[128];
    char product_id[64];
    char product_key[64];
    char product_secret[80];
    char device_name[64];

    const convai_codec_t *codec;
    convai_codec_state_t *codec_state;

    char agent_id[80];
    char *startup_params;       /* config_update JSON sent after hello_ack */

    convai_event_handler_t handler;
    void *user_data;

    esp_websocket_client_handle_t ws;
    bool      started;
    bool      session_ready;
    uint32_t  tx_seq;
    uint32_t  audio_seq;
    convai_status_e status;

    /* uplink: app thread -> TX queue -> sender task */
    QueueHandle_t  tx_queue;
    TaskHandle_t   send_task;
    uint32_t       tx_drops;
    uint32_t       tx_high_water;

    /* downlink: ws task -> RX ring -> pump task */
    convai_ring_t  rx_ring;
    SemaphoreHandle_t rx_mutex;
    SemaphoreHandle_t rx_sem;      /* counting: messages available */
    TaskHandle_t   pump_task;
    volatile bool  tasks_running;
} convai_engine_s;

/* ---------------- helpers ---------------- */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void emit_event(convai_engine_s *e, convai_event_code_e code, const char *details)
{
    if (e->handler.on_convai_event) {
        convai_event_t ev = { .code = code, .data.details = details };
        e->handler.on_convai_event(e, &ev, e->user_data);
    }
}

static void emit_status(convai_engine_s *e, convai_status_e st)
{
    e->status = st;
    if (e->handler.on_convai_conversation_status) {
        e->handler.on_convai_conversation_status(e, st, e->user_data);
    }
}

static void emit_message(convai_engine_s *e, const void *data, size_t len, bool is_binary)
{
    if (e->handler.on_convai_message_data) {
        convai_message_info_t info = { .is_binary = is_binary };
        e->handler.on_convai_message_data(e, data, len, &info, e->user_data);
    }
}

static void emit_audio(convai_engine_s *e, const void *data, size_t len)
{
    if (e->handler.on_convai_audio_data) {
        convai_audio_frame_info_t info = { .data_type = CONVAI_AUDIO_DATA_TYPE_PCM16 };
        e->handler.on_convai_audio_data(e, data, len, &info, e->user_data);
    }
}

/* Switch codec; frees/allocates instance state. */
static int set_codec_internal(convai_engine_s *e, convai_codec_id_e id)
{
    const convai_codec_t *c = convai_codec_get(id);
    if (!c) {
        ESP_LOGE(TAG, "codec %d not supported in this build", id);
        return CONVAI_ERR_NOT_SUPPORTED;
    }
    if (e->codec && e->codec->deinit && e->codec_state) {
        e->codec->deinit(e->codec_state);
    }
    free(e->codec_state);
    e->codec_state = NULL;
    if (c->state_size > 0) {
        e->codec_state = calloc(1, c->state_size);
        if (!e->codec_state) return CONVAI_ERR_OUT_OF_MEMORY;
    }
    e->codec = c;
    if (c->init) c->init(e->codec_state);
    ESP_LOGI(TAG, "codec switched to %s (id=%d, sr=%d)", c->name, c->id, c->sample_rate);
    return CONVAI_OK;
}

static int send_envelope(convai_engine_s *e, const char *type, const char *body_json)
{
    if (!e->ws || !esp_websocket_client_is_connected(e->ws)) {
        return CONVAI_ERR_CONNECTION_LOST;
    }
    char *out = convai_proto_build_envelope(type, body_json, ++e->tx_seq, now_ms());
    if (!out) return CONVAI_ERR_OUT_OF_MEMORY;

    ESP_LOGD(TAG, ">> %s", out);
    int rc = esp_websocket_client_send_text(e->ws, out, strlen(out), pdMS_TO_TICKS(2000));
    free(out);
    return rc < 0 ? CONVAI_ERR_NETWORK : CONVAI_OK;
}

/* ---------------- uplink: sender task ---------------- */

static void send_one_frame(convai_engine_s *e, const uint8_t *payload, size_t len)
{
    uint8_t hdr[CONVAI_AUDIO_HDR_LEN];
    convai_proto_audio_hdr_pack(hdr, CONVAI_AUDIO_OP_FRAME, ++e->audio_seq, now_ms());
    int rc = esp_websocket_client_send_bin_partial(e->ws, (const char *)hdr,
                                                   CONVAI_AUDIO_HDR_LEN, pdMS_TO_TICKS(2000));
    if (rc >= 0) {
        rc = esp_websocket_client_send_cont_msg(e->ws, (const char *)payload, len,
                                                pdMS_TO_TICKS(2000));
    }
    if (rc >= 0) {
        esp_websocket_client_send_fin(e->ws, pdMS_TO_TICKS(2000));
    }
}

static void send_task_fn(void *arg)
{
    convai_engine_s *e = arg;
    tx_frame_t f;
    while (e->tasks_running) {
        if (xQueueReceive(e->tx_queue, &f, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!e->tasks_running) break;
        if (e->session_ready && e->ws && esp_websocket_client_is_connected(e->ws)) {
            send_one_frame(e, f.data, f.len);
        }
    }
    e->send_task = NULL;
    vTaskDelete(NULL);
}

/* Queue one encoded frame; full -> drop oldest (never blocks recorder). */
static int tx_push(convai_engine_s *e, const uint8_t *payload, size_t len)
{
    if (len > CONVAI_TX_SLOT_BYTES) return CONVAI_ERR_INVALID_PARAM;
    if (uxQueueSpacesAvailable(e->tx_queue) == 0) {
        tx_frame_t drop;
        xQueueReceive(e->tx_queue, &drop, 0);   /* drop oldest */
        e->tx_drops++;
    }
    tx_frame_t f = { .len = (uint16_t)len };
    memcpy(f.data, payload, len);
    if (xQueueSend(e->tx_queue, &f, 0) != pdTRUE) {
        e->tx_drops++;
        return CONVAI_ERR_NETWORK;
    }
    UBaseType_t used = CONVAI_TX_QUEUE_FRAMES - uxQueueSpacesAvailable(e->tx_queue);
    if (used > e->tx_high_water) e->tx_high_water = used;
    return CONVAI_OK;
}

/* ---------------- downlink: pump task ---------------- */

static void pump_task_fn(void *arg)
{
    convai_engine_s *e = arg;
    static uint8_t msg[CONVAI_DEC_PCM_SAMPLES * 2];
    uint16_t msg_len;
    while (e->tasks_running) {
        if (xSemaphoreTake(e->rx_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!e->tasks_running) break;
        xSemaphoreTake(e->rx_mutex, portMAX_DELAY);
        int rc = convai_ring_pop(&e->rx_ring, msg, sizeof(msg), &msg_len);
        xSemaphoreGive(e->rx_mutex);
        if (rc == 0) {
            emit_audio(e, msg, msg_len);   /* app playback may block here; ring absorbs */
        }
    }
    e->pump_task = NULL;
    vTaskDelete(NULL);
}

static void rx_push(convai_engine_s *e, const uint8_t *pcm, size_t len)
{
    xSemaphoreTake(e->rx_mutex, portMAX_DELAY);
    convai_ring_push(&e->rx_ring, pcm, (uint16_t)len);
    xSemaphoreGive(e->rx_mutex);
    xSemaphoreGive(e->rx_sem);
}

/* ---------------- incoming data ---------------- */

static void handle_text(convai_engine_s *e, const char *data, int len)
{
    convai_envelope_t env;
    if (convai_proto_parse_envelope(data, len, &env) != 0) {
        ESP_LOGW(TAG, "invalid text frame: %.*s", len > 120 ? 120 : len, data);
        return;
    }

    if (!strcmp(env.type, "hello_ack")) {
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(env.body, "session_id");
        ESP_LOGI(TAG, "session created: %s", cJSON_IsString(sid) ? sid->valuestring : "?");
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(env.body, "audio_config");
        const cJSON *cn = ac ? cJSON_GetObjectItemCaseSensitive(ac, "codec") : NULL;
        if (cJSON_IsString(cn)) {
            const convai_codec_t *want = convai_codec_by_name(cn->valuestring);
            if (want && want != e->codec) {
                set_codec_internal(e, want->id);
            }
        }
        e->session_ready = true;
        emit_event(e, CONVAI_EV_CONNECTED, "session established");
        emit_status(e, CONVAI_STATUS_LISTENING);
        if (e->startup_params && e->startup_params[0]) {
            send_envelope(e, "config_update", e->startup_params);
        }
    } else if (!strcmp(env.type, "hello_err")) {
        char *msg = env.body ? cJSON_PrintUnformatted(env.body) : NULL;
        ESP_LOGE(TAG, "auth rejected: %s", msg ? msg : "");
        emit_event(e, CONVAI_EV_FAILED, msg);
        free(msg);
    } else if (!strcmp(env.type, "status")) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(env.body, "status");
        if (cJSON_IsString(st)) {
            emit_status(e, convai_proto_status_from_str(st->valuestring));
        }
    } else if (!strcmp(env.type, "event")) {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(env.body, "event");
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(env.body, "details");
        const char *evs = cJSON_IsString(ev) ? ev->valuestring : "";
        const char *dts = cJSON_IsString(dt) ? dt->valuestring : NULL;
        if (!strcmp(evs, "connected"))         emit_event(e, CONVAI_EV_CONNECTED, dts);
        else if (!strcmp(evs, "disconnected")) emit_event(e, CONVAI_EV_DISCONNECTED, dts);
        else if (!strcmp(evs, "updated"))      emit_event(e, CONVAI_EV_UPDATED, dts);
        else                                   emit_event(e, CONVAI_EV_FAILED, dts);
    } else if (!strcmp(env.type, "pong")) {
        /* keepalive reply, ignore */
    } else {
        ESP_LOGD(TAG, "<< %s: %.*s", env.type, len > 120 ? 120 : len, data);
        emit_message(e, data, len, false);
    }
    convai_proto_envelope_free(&env);
}

static void handle_binary(convai_engine_s *e, const uint8_t *data, int len)
{
    uint8_t op;
    if (convai_proto_audio_hdr_unpack(data, len, &op, NULL, NULL) != 0) {
        ESP_LOGW(TAG, "short binary frame (%d bytes)", len);
        return;
    }
    switch (op) {
    case CONVAI_AUDIO_OP_FRAME: {
        const uint8_t *enc = data + CONVAI_AUDIO_HDR_LEN;
        size_t enc_len = len - CONVAI_AUDIO_HDR_LEN;
        if (!e->codec || !e->codec->decode) break;

        size_t samples = 0;
        if (e->codec->decode(e->codec_state, enc, enc_len,
                             s_dec_pcm, CONVAI_DEC_PCM_SAMPLES, &samples) != 0) {
            ESP_LOGW(TAG, "%s decode failed/overflow (%zu bytes), frame dropped",
                     e->codec->name, enc_len);
            break;
        }
        if (samples > 0) {
            rx_push(e, (const uint8_t *)s_dec_pcm, samples * sizeof(int16_t));
        }
        break;
    }
    case CONVAI_AUDIO_OP_START:
        ESP_LOGI(TAG, "TTS stream start");
        emit_status(e, CONVAI_STATUS_ANSWERING);
        break;
    case CONVAI_AUDIO_OP_END:
        ESP_LOGI(TAG, "TTS stream end");
        emit_status(e, CONVAI_STATUS_ANSWER_FINISHED);
        break;
    default:
        ESP_LOGW(TAG, "unknown audio op 0x%02x", op);
        break;
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    convai_engine_s *e = (convai_engine_s *)arg;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected, sending hello");
        {
            char body[512];
            snprintf(body, sizeof(body),
                     "{\"product_id\":\"%s\",\"product_key\":\"%s\","
                     "\"product_secret\":\"%s\",\"device_name\":\"%s\","
                     "\"audio_codec\":%d,\"sample_rate\":%d}",
                     e->product_id, e->product_key, e->product_secret,
                     e->device_name,
                     e->codec ? e->codec->id : CONVAI_DEFAULT_CODEC,
                     e->codec ? e->codec->sample_rate : 8000);
            send_envelope(e, "hello", body);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS disconnected");
        e->session_ready = false;
        emit_event(e, CONVAI_EV_DISCONNECTED, "transport closed");
        emit_status(e, CONVAI_STATUS_IDLE);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1) {
            handle_text(e, (const char *)d->data_ptr, d->data_len);
        } else if (d->op_code == 0x2) {
            handle_binary(e, (const uint8_t *)d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        emit_event(e, CONVAI_EV_FAILED, "transport error");
        break;
    default:
        break;
    }
}

/* ---------------- public API ---------------- */

int convai_create(convai_engine_t *handle,
                  const char *config_json,
                  const convai_event_handler_t *handler,
                  void *user_data)
{
    if (!handle || !config_json || !handler) return CONVAI_ERR_INVALID_PARAM;

    convai_engine_s *e = calloc(1, sizeof(convai_engine_s));
    if (!e) return CONVAI_ERR_OUT_OF_MEMORY;

    strncpy(e->server_url, CONVAI_DEFAULT_URL, sizeof(e->server_url) - 1);

    cJSON *root = cJSON_Parse(config_json);
    if (!root) { free(e); return CONVAI_ERR_INVALID_JSON; }
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    const cJSON *ws   = cJSON_GetObjectItemCaseSensitive(root, "ws");
    const cJSON *item;
#define COPY_STR(field, dst) \
    if ((item = cJSON_GetObjectItemCaseSensitive(info, field)) && cJSON_IsString(item)) \
        strncpy(e->dst, item->valuestring, sizeof(e->dst) - 1);
    if (info) {
        COPY_STR("product_id", product_id);
        COPY_STR("product_key", product_key);
        COPY_STR("product_secret", product_secret);
        COPY_STR("device_name", device_name);
    }
#undef COPY_STR
    int codec_id = CONVAI_DEFAULT_CODEC;
    if (ws) {
        if ((item = cJSON_GetObjectItemCaseSensitive(ws, "url")) && cJSON_IsString(item)) {
            strncpy(e->server_url, item->valuestring, sizeof(e->server_url) - 1);
        }
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(ws, "audio");
        if (audio && (item = cJSON_GetObjectItemCaseSensitive(audio, "codec")) && cJSON_IsNumber(item)) {
            codec_id = item->valueint;
        }
    }
    cJSON_Delete(root);

    if (!e->product_key[0] || !e->device_name[0]) {
        ESP_LOGE(TAG, "config incomplete: device_name/product_key required");
        free(e);
        return CONVAI_ERR_CONFIG_INCOMPLETE;
    }

    memcpy(&e->handler, handler, sizeof(e->handler));
    e->user_data = user_data;
    e->status = CONVAI_STATUS_IDLE;

    if (set_codec_internal(e, (convai_codec_id_e)codec_id) != CONVAI_OK) {
        ESP_LOGW(TAG, "codec %d unavailable, falling back to g711a", codec_id);
        set_codec_internal(e, CONVAI_DEFAULT_CODEC);
    }

    *handle = e;
    ESP_LOGI(TAG, "engine created (server=%s, device=%s)", e->server_url, e->device_name);
    return CONVAI_OK;
}

void convai_destroy(convai_engine_t handle)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return;
    if (e->started) convai_stop(handle);
    free(e->startup_params);
    if (e->codec && e->codec->deinit && e->codec_state) {
        e->codec->deinit(e->codec_state);
    }
    free(e->codec_state);
    free(e);
}

int convai_start(convai_engine_t handle, const convai_opt_t *opt)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (e->started) return CONVAI_ERR_ALREADY_STARTED;

    if (opt && opt->agent_id) {
        strncpy(e->agent_id, opt->agent_id, sizeof(e->agent_id) - 1);
    }
    if (opt && opt->params) {
        free(e->startup_params);
        e->startup_params = strdup(opt->params);
    }

    /* --- uplink TX queue + downlink RX ring (all static) --- */
    e->tx_queue = xQueueCreateStatic(CONVAI_TX_QUEUE_FRAMES, sizeof(tx_frame_t),
                                     s_tx_storage, &s_tx_queue_cb);
    if (!e->tx_queue) return CONVAI_ERR_OUT_OF_MEMORY;
    convai_ring_init(&e->rx_ring, s_rx_arena, sizeof(s_rx_arena));
    e->rx_mutex = xSemaphoreCreateMutex();
    e->rx_sem   = xSemaphoreCreateCounting(64, 0);
    if (!e->rx_mutex || !e->rx_sem) return CONVAI_ERR_OUT_OF_MEMORY;

    e->tasks_running = true;
    BaseType_t ok = xTaskCreate(send_task_fn, "convai_tx", CONVAI_SEND_TASK_STACK,
                                e, 5, &e->send_task);
    if (ok == pdPASS) {
        ok = xTaskCreate(pump_task_fn, "convai_rx", CONVAI_PUMP_TASK_STACK,
                         e, 5, &e->pump_task);
    }
    if (ok != pdPASS) {
        e->tasks_running = false;
        vSemaphoreDelete(e->rx_mutex);
        vSemaphoreDelete(e->rx_sem);
        return CONVAI_ERR_OUT_OF_MEMORY;
    }

    esp_websocket_client_config_t cfg = {
        .uri = e->server_url,
        .subprotocol = CONVAI_WS_SUBPROTOCOL,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .task_stack = CONVAI_WS_TASK_STACK,
    };
    if (!strncmp(e->server_url, "wss://", 6)) {
#ifdef CONVAI_WSS_CUSTOM_CA
        extern const uint8_t server_ca_pem_start[] asm("_binary_server_ca_pem_start");
        cfg.cert_pem = (const char *)server_ca_pem_start;
        ESP_LOGI(TAG, "wss: using embedded custom CA");
#elif defined(CONFIG_CONVAI_WSS_CA_BUNDLE) && CONFIG_CONVAI_WSS_CA_BUNDLE
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "wss: using built-in CA bundle");
#endif
#if defined(CONFIG_CONVAI_WSS_SKIP_CN_CHECK) && CONFIG_CONVAI_WSS_SKIP_CN_CHECK
        cfg.skip_cert_common_name_check = true;
#endif
    }
    e->ws = esp_websocket_client_init(&cfg);
    if (!e->ws) {
        e->tasks_running = false;
        return CONVAI_ERR_INIT_FAILED;
    }

    esp_websocket_register_events(e->ws, WEBSOCKET_EVENT_ANY, ws_event_handler, e);

    esp_err_t err = esp_websocket_client_start(e->ws);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(e->ws);
        e->ws = NULL;
        e->tasks_running = false;
        return CONVAI_ERR_NETWORK;
    }

    e->started = true;
    ESP_LOGI(TAG, "engine started (agent_id=%s)", e->agent_id);
    return CONVAI_OK;
}

int convai_stop(convai_engine_t handle)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started) return CONVAI_OK;

    send_envelope(e, "bye", NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_websocket_client_stop(e->ws);
    esp_websocket_client_destroy(e->ws);
    e->ws = NULL;

    /* stop tasks: flag off + wake them */
    e->tasks_running = false;
    xSemaphoreGive(e->rx_sem);
    tx_frame_t kick = {0};
    xQueueSend(e->tx_queue, &kick, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (e->send_task) vTaskDelete(e->send_task);
    if (e->pump_task) vTaskDelete(e->pump_task);
    e->send_task = e->pump_task = NULL;
    vSemaphoreDelete(e->rx_mutex);
    vSemaphoreDelete(e->rx_sem);
    e->rx_mutex = e->rx_sem = NULL;

    e->started = false;
    e->session_ready = false;
    e->status = CONVAI_STATUS_IDLE;
    ESP_LOGI(TAG, "engine stopped");
    return CONVAI_OK;
}

int convai_update(convai_engine_t handle, const char *session_update_json)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!session_update_json) return CONVAI_ERR_INVALID_PARAM;
    return send_envelope(e, "config_update", session_update_json);
}

int convai_send_audio(convai_engine_t handle,
                      const void *data_ptr,
                      size_t data_len,
                      const convai_audio_frame_info_t *info_ptr)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started || !e->session_ready) return CONVAI_ERR_SESSION_NOT_READY;
    if (!data_ptr || !data_len || !e->tx_queue) return CONVAI_ERR_INVALID_PARAM;
    if (!e->codec || !e->codec->encode) return CONVAI_ERR_NOT_SUPPORTED;

    /* pre-encoded passthrough when caller already used the active codec */
    bool passthrough = info_ptr && ((int)info_ptr->data_type == (int)e->codec->id)
                       && info_ptr->data_type != CONVAI_AUDIO_DATA_TYPE_PCM16;
    if (passthrough) {
        return tx_push(e, data_ptr, data_len);
    }

    /* input: mono PCM16 -> encode per 20ms frame into static queue */
    size_t frame_samples = (size_t)e->codec->sample_rate / 50;
    const int16_t *pcm = data_ptr;
    size_t total = data_len / 2;
    for (size_t off = 0; off < total; off += frame_samples) {
        size_t n = total - off;
        if (n > frame_samples) n = frame_samples;
        size_t enc_len = 0;
        if (e->codec->encode(e->codec_state, pcm + off, n,
                             s_enc_buf, sizeof(s_enc_buf), &enc_len) != 0 || enc_len == 0) {
            ESP_LOGW(TAG, "%s encode failed", e->codec->name);
            return CONVAI_ERR_MEDIA;
        }
        int rc = tx_push(e, s_enc_buf, enc_len);
        if (rc != CONVAI_OK) return rc;
    }
    return CONVAI_OK;
}

int convai_send_message(convai_engine_t handle,
                        const void *data_ptr,
                        size_t data_len,
                        const convai_message_info_t *info_ptr)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    (void)info_ptr;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started || !e->session_ready) return CONVAI_ERR_SESSION_NOT_READY;
    if (!data_ptr || !data_len) return CONVAI_ERR_INVALID_PARAM;
    if (!e->ws || !esp_websocket_client_is_connected(e->ws)) {
        return CONVAI_ERR_CONNECTION_LOST;
    }

    const char *s = (const char *)data_ptr;
    /* A complete envelope JSON is forwarded verbatim; plain text is
     * wrapped into a ping envelope body {"msg":"<text>"}. */
    if (s[0] == '{') {
        int rc = esp_websocket_client_send_text(e->ws, s, data_len, pdMS_TO_TICKS(2000));
        return rc < 0 ? CONVAI_ERR_NETWORK : CONVAI_OK;
    }

    cJSON *body = cJSON_CreateObject();
    if (!body) return CONVAI_ERR_OUT_OF_MEMORY;
    cJSON_AddStringToObject(body, "msg", s);
    char *bj = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!bj) return CONVAI_ERR_OUT_OF_MEMORY;

    int ret = send_envelope(e, "ping", bj);
    free(bj);
    return ret;
}

int convai_set_codec(convai_engine_t handle, int codec_id)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (codec_id < 0 || codec_id >= CONVAI_CODEC_MAX) return CONVAI_ERR_INVALID_PARAM;
    int rc = set_codec_internal(e, (convai_codec_id_e)codec_id);
    if (rc == CONVAI_OK && e->started) {
        /* avoid mixing formats in flight */
        xQueueReset(e->tx_queue);
        xSemaphoreTake(e->rx_mutex, portMAX_DELAY);
        convai_ring_clear(&e->rx_ring);
        xSemaphoreGive(e->rx_mutex);
    }
    return rc;
}

int convai_get_codec(convai_engine_t handle)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e || !e->codec) return -1;
    return (int)e->codec->id;
}

void convai_mem_report(convai_engine_t handle)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return;
    size_t codec_mem = (e->codec && e->codec->mem_usage && e->codec_state)
                       ? e->codec->mem_usage(e->codec_state) : 0;
    ESP_LOGI(TAG, "=== convai memory report ===");
    ESP_LOGI(TAG, "static pool: %u B (enc %d + dec %d + tx %dx%d + rx %d)",
             (unsigned)CONVAI_STATIC_POOL_BYTES,
             CONVAI_ENC_BUF_BYTES, CONVAI_DEC_PCM_SAMPLES * 2,
             CONVAI_TX_QUEUE_FRAMES, CONVAI_TX_SLOT_BYTES + 4, CONVAI_RX_RING_BYTES);
    ESP_LOGI(TAG, "codec: %s, instance %u B",
             e->codec ? e->codec->name : "?", (unsigned)codec_mem);
    if (e->tx_queue) {
        ESP_LOGI(TAG, "tx queue: used %d/%d, high-water %u, drops %u",
                 CONVAI_TX_QUEUE_FRAMES - uxQueueSpacesAvailable(e->tx_queue),
                 CONVAI_TX_QUEUE_FRAMES, (unsigned)e->tx_high_water, (unsigned)e->tx_drops);
    }
    ESP_LOGI(TAG, "rx ring: used %u/%d B, high-water %u B, drops %u",
             (unsigned)convai_ring_used(&e->rx_ring), CONVAI_RX_RING_BYTES,
             (unsigned)convai_ring_high_water(&e->rx_ring),
             (unsigned)convai_ring_drops(&e->rx_ring));
    ESP_LOGI(TAG, "heap free: %u B, min-ever %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

const char *convai_get_version(void)
{
    return "0.2.0-esp32";
}

const char *convai_err_2_str(int err_code)
{
    switch (err_code) {
    case CONVAI_OK:                    return "Success";
    case CONVAI_ERR_UNKNOWN:           return "Unknown error";
    case CONVAI_ERR_INVALID_PARAM:     return "Invalid parameter";
    case CONVAI_ERR_OUT_OF_MEMORY:     return "Memory allocation failed";
    case CONVAI_ERR_NOT_INITIALIZED:   return "Engine not created, call convai_create first";
    case CONVAI_ERR_ALREADY_STARTED:   return "Engine already started";
    case CONVAI_ERR_NOT_STARTED:       return "Engine not started, call convai_start first";
    case CONVAI_ERR_NETWORK:           return "Network connection error";
    case CONVAI_ERR_TIMEOUT:           return "Operation timeout";
    case CONVAI_ERR_PROTOCOL:          return "Protocol encoding or decoding error";
    case CONVAI_ERR_MEDIA:             return "Media encoding or decoding error";
    case CONVAI_ERR_TLS:               return "TLS handshake or encryption error";
    case CONVAI_ERR_PLATFORM:          return "Platform HAL error";
    case CONVAI_ERR_NOT_SUPPORTED:     return "Feature not supported";
    case CONVAI_ERR_INVALID_STATE:     return "Invalid engine state for this operation";
    case CONVAI_ERR_CONNECTION_LOST:   return "Connection to server lost";
    case CONVAI_ERR_INIT_FAILED:       return "Engine initialization failed";
    case CONVAI_ERR_SESSION_NOT_READY: return "Session not ready, call convai_start first";
    case CONVAI_ERR_CONFIG_INCOMPLETE: return "Config incomplete, missing device_name or product_key";
    case CONVAI_ERR_INVALID_JSON:      return "Invalid JSON format or parse failed";
    default:                           return "Unknown error code";
    }
}
