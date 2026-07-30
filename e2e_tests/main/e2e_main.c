/**
 * @file e2e_main.c
 * @brief End-to-end device-cloud test on the linux target.
 *
 * Links the REAL convai_ws component (same code as the firmware) and
 * runs a scripted scenario against a live convai.v1 gateway
 * (router + mock ASR/LLM/TTS backends on loopback):
 *
 *   1. convai_create/start -> expect CONNECTED + LISTENING
 *   2. send 30 x 20ms PCM16 tone frames
 *   3. expect: AI reply text envelope, >=1 TTS audio frame (Start..End),
 *      statuses thinking/answering/answer_finished
 *   4. run for codecs: g711a (8k) and opus (16k)
 *
 * Exit code 0 = all scenarios pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

#include "convai_api.h"
#include "convai_codec.h"

static const char *TAG = "e2e";

#define ROUTER_URL      "ws://127.0.0.1:9000/"
#define CONNECT_TIMEOUT_MS   15000
#define TURN_TIMEOUT_MS      30000
#define UPLINK_FRAMES        30

/* ---- observed state (filled by callbacks) ---- */
typedef struct {
    bool connected;
    bool got_text;
    bool saw_thinking;
    bool saw_answering;
    bool saw_answer_finished;
    bool saw_listening;
    int  audio_frames;
    int  audio_bytes;
    char reply[256];
} obs_t;

static obs_t g_obs;

static void reset_obs(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
}

static void on_event(convai_engine_t e, convai_event_t *ev, void *ud)
{
    (void)e; (void)ud;
    if (ev->code == CONVAI_EV_CONNECTED) g_obs.connected = true;
    ESP_LOGI(TAG, "[cb] event %d (%s)", ev->code, ev->data.details ? ev->data.details : "");
}

static void on_status(convai_engine_t e, convai_status_e s, void *ud)
{
    (void)e; (void)ud;
    switch (s) {
    case CONVAI_STATUS_LISTENING:       g_obs.saw_listening = true; break;
    case CONVAI_STATUS_THINKING:        g_obs.saw_thinking = true; break;
    case CONVAI_STATUS_ANSWERING:       g_obs.saw_answering = true; break;
    case CONVAI_STATUS_ANSWER_FINISHED: g_obs.saw_answer_finished = true; break;
    default: break;
    }
    ESP_LOGI(TAG, "[cb] status %d", s);
}

static void on_audio(convai_engine_t e, const void *data, size_t len,
                     const convai_audio_frame_info_t *info, void *ud)
{
    (void)e; (void)data; (void)info; (void)ud;
    g_obs.audio_frames++;
    g_obs.audio_bytes += (int)len;
}

static void on_message(convai_engine_t e, const void *data, size_t len,
                       const convai_message_info_t *info, void *ud)
{
    (void)e; (void)info; (void)ud;
    size_t n = len < sizeof(g_obs.reply) - 1 ? len : sizeof(g_obs.reply) - 1;
    memcpy(g_obs.reply, data, n);
    g_obs.reply[n] = 0;
    /* the AI reply arrives as a "text" envelope */
    if (strstr(g_obs.reply, "\"text\"") || strstr(g_obs.reply, "小荷")) {
        g_obs.got_text = true;
    }
    ESP_LOGI(TAG, "[cb] message: %s", g_obs.reply);
}

static bool wait_for(bool *cond, int timeout_ms)
{
    int waited = 0;
    while (!*cond && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return *cond;
}

static int run_scenario(int codec_id, const char *codec_name)
{
    char cfg[768];
    snprintf(cfg, sizeof(cfg),
             "{\"info\":{\"product_id\":\"pid\",\"product_key\":\"pk\","
             "\"product_secret\":\"ps\",\"device_name\":\"e2e-dev\"},"
             "\"ws\":{\"url\":\"%s\",\"audio\":{\"codec\":%d}}}",
             ROUTER_URL, codec_id);

    convai_event_handler_t h = {
        .on_convai_event = on_event,
        .on_convai_conversation_status = on_status,
        .on_convai_audio_data = on_audio,
        .on_convai_message_data = on_message,
    };
    convai_engine_t eng = NULL;
    int rc = convai_create(&eng, cfg, &h, NULL);
    if (rc != CONVAI_OK) {
        ESP_LOGE(TAG, "[%s] convai_create failed: %s", codec_name, convai_err_2_str(rc));
        return 1;
    }

    convai_opt_t opt = {
        .mode = CONVAI_MODE_WS,
        .agent_id = "agent_e2e",
        .params = NULL,
    };
    rc = convai_start(eng, &opt);
    if (rc != CONVAI_OK) {
        ESP_LOGE(TAG, "[%s] convai_start failed: %s", codec_name, convai_err_2_str(rc));
        convai_destroy(eng);
        return 1;
    }

    int fails = 0;
    if (!wait_for(&g_obs.connected, CONNECT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "[%s] FAIL: no CONNECTED within %d ms", codec_name, CONNECT_TIMEOUT_MS);
        fails++;
        goto out;
    }
    if (!g_obs.saw_listening) {
        ESP_LOGE(TAG, "[%s] FAIL: no LISTENING status", codec_name);
        fails++;
    }

    /* uplink: 30 x 20ms PCM16 tone */
    {
        int sr = (codec_id == CONVAI_AUDIO_DATA_TYPE_OPUS) ? 16000 : 8000;
        int frame = sr / 50;
        int16_t *tone = malloc(frame * 2 * UPLINK_FRAMES);
        for (int i = 0; i < frame * UPLINK_FRAMES; i++) {
            tone[i] = (int16_t)(10000 * ((i % 16) < 8 ? 1 : -1));
        }
        rc = convai_send_audio(eng, tone, frame * 2 * UPLINK_FRAMES, NULL);
        free(tone);
        if (rc != CONVAI_OK) {
            ESP_LOGE(TAG, "[%s] FAIL: send_audio %s", codec_name, convai_err_2_str(rc));
            fails++;
            goto out;
        }
    }

    /* wait for the AI turn to complete */
    if (!wait_for(&g_obs.saw_answer_finished, TURN_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "[%s] FAIL: turn did not complete in %d ms", codec_name, TURN_TIMEOUT_MS);
        fails++;
        goto out;
    }
    if (!g_obs.saw_thinking)  { ESP_LOGE(TAG, "[%s] FAIL: no thinking", codec_name);  fails++; }
    if (!g_obs.saw_answering) { ESP_LOGE(TAG, "[%s] FAIL: no answering", codec_name); fails++; }
    if (!g_obs.got_text)      { ESP_LOGE(TAG, "[%s] FAIL: no AI reply text", codec_name); fails++; }
    if (g_obs.audio_frames < 5) {
        ESP_LOGE(TAG, "[%s] FAIL: only %d audio frames", codec_name, g_obs.audio_frames);
        fails++;
    }

    ESP_LOGI(TAG, "[%s] turn stats: frames=%d bytes=%d reply=%.80s",
             codec_name, g_obs.audio_frames, g_obs.audio_bytes, g_obs.reply);

out:
    convai_stop(eng);
    convai_destroy(eng);
    ESP_LOGI(TAG, "[%s] scenario %s", codec_name, fails ? "FAILED" : "PASSED");
    return fails;
}

void app_main(void)
{
    esp_log_level_set("convai", ESP_LOG_INFO);
    int fails = 0;

    ESP_LOGI(TAG, "=== scenario 1/2: g711a ===");
    reset_obs();
    fails += run_scenario(CONVAI_AUDIO_DATA_TYPE_G711A, "g711a");

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "=== scenario 2/2: opus ===");
    reset_obs();
    fails += run_scenario(CONVAI_AUDIO_DATA_TYPE_OPUS, "opus");

    printf("\n==========================================\n");
    printf("E2E RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    printf("==========================================\n");
    fflush(stdout);
    exit(fails ? 1 : 0);
}
