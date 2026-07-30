/**
 * @file convai_ws.c
 * @brief ConvAI SDK re-implementation for ESP32 (ESP-IDF) over WebSocket.
 *
 * Implements the device side of the convai.v1 wire protocol
 * (cloud_gateway/docs/cloud_gateway/protocol.md) using
 * esp_websocket_client, exposing the same public API as the
 * original precompiled ConvAI SDK (convai_api.h).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "convai_api.h"
#include "convai_protocol.h"
#include "convai_codec.h"

static const char *TAG = "convai";

#define CONVAI_WS_SUBPROTOCOL   "convai.v1"
#define CONVAI_DEFAULT_URL      "ws://192.168.1.100:9000/"
#define CONVAI_DEFAULT_CODEC    CONVAI_CODEC_G711A

typedef struct {
    char server_url[128];
    char product_id[64];
    char product_key[64];
    char product_secret[80];
    char device_name[64];

    const convai_codec_t *codec;        /* active codec (runtime switchable) */
    convai_codec_state_t *codec_state;  /* per-instance codec state */

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
        /* honour gateway-requested codec, e.g. audio_config.codec="opus" */
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
        /* text / text_delta / function_call / ack / config_update_(ack|err) /
         * error and unknown types: forward raw JSON to the app */
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
        if (!e->codec || !e->codec->decode) {
            ESP_LOGW(TAG, "no codec, dropping %zu audio bytes", enc_len);
            break;
        }
        /* decode to PCM16; worst case 2 samples/byte (ADPCM) */
        size_t cap = enc_len * 2 + 64;
        int16_t *pcm = malloc(cap * sizeof(int16_t));
        if (!pcm) break;
        size_t samples = 0;
        if (e->codec->decode(e->codec_state, enc, enc_len, pcm, cap, &samples) == 0
            && samples > 0) {
            emit_audio(e, pcm, samples * sizeof(int16_t));
        } else {
            ESP_LOGW(TAG, "%s decode failed (%zu bytes)", e->codec->name, enc_len);
        }
        free(pcm);
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

    esp_websocket_client_config_t cfg = {
        .uri = e->server_url,
        .subprotocol = CONVAI_WS_SUBPROTOCOL,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };
    if (!strncmp(e->server_url, "wss://", 6)) {
#ifdef CONVAI_WSS_CUSTOM_CA
        /* embedded self-signed CA (components/convai_ws/certs/server_ca.pem) */
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
    if (!e->ws) return CONVAI_ERR_INIT_FAILED;

    esp_websocket_register_events(e->ws, WEBSOCKET_EVENT_ANY, ws_event_handler, e);

    esp_err_t err = esp_websocket_client_start(e->ws);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(e->ws);
        e->ws = NULL;
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
    if (!data_ptr || !data_len) return CONVAI_ERR_INVALID_PARAM;
    if (!e->ws || !esp_websocket_client_is_connected(e->ws)) {
        return CONVAI_ERR_CONNECTION_LOST;
    }
    if (!e->codec || !e->codec->encode) return CONVAI_ERR_NOT_SUPPORTED;

    /* Pre-encoded passthrough when the caller already used the active codec */
    const uint8_t *payload = data_ptr;
    size_t payload_len = data_len;
    uint8_t *enc_buf = NULL;

    bool passthrough = info_ptr && ((int)info_ptr->data_type == (int)e->codec->id)
                       && info_ptr->data_type != CONVAI_AUDIO_DATA_TYPE_PCM16;
    if (!passthrough) {
        /* input is mono PCM16; encode with the active codec */
        size_t cap = data_len + 256;   /* all supported codecs shrink or pass through */
        enc_buf = malloc(cap);
        if (!enc_buf) return CONVAI_ERR_OUT_OF_MEMORY;
        if (e->codec->encode(e->codec_state, data_ptr, data_len / 2,
                             enc_buf, cap, &payload_len) != 0 || payload_len == 0) {
            ESP_LOGW(TAG, "%s encode failed", e->codec->name);
            free(enc_buf);
            return CONVAI_ERR_MEDIA;
        }
        payload = enc_buf;
    }

    uint8_t hdr[CONVAI_AUDIO_HDR_LEN];
    convai_proto_audio_hdr_pack(hdr, CONVAI_AUDIO_OP_FRAME, ++e->audio_seq, now_ms());

    int rc = esp_websocket_client_send_bin_partial(e->ws, (const char *)hdr,
                                                   CONVAI_AUDIO_HDR_LEN, pdMS_TO_TICKS(1000));
    if (rc >= 0) {
        rc = esp_websocket_client_send_cont_msg(e->ws, (const char *)payload, payload_len,
                                                pdMS_TO_TICKS(1000));
    }
    if (rc >= 0) {
        rc = esp_websocket_client_send_fin(e->ws, pdMS_TO_TICKS(1000));
    }
    free(enc_buf);
    return rc < 0 ? CONVAI_ERR_NETWORK : CONVAI_OK;
}

int convai_set_codec(convai_engine_t handle, int codec_id)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (codec_id < 0 || codec_id >= CONVAI_CODEC_MAX) return CONVAI_ERR_INVALID_PARAM;
    return set_codec_internal(e, (convai_codec_id_e)codec_id);
}

int convai_get_codec(convai_engine_t handle)
{
    convai_engine_s *e = (convai_engine_s *)handle;
    if (!e || !e->codec) return -1;
    return (int)e->codec->id;
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

const char *convai_get_version(void)
{
    return "0.1.0-esp32";
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
