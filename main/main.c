/**
 * @file main.c
 * @brief Goldie ESP32 demo: WiFi + ConvAI (convai.v1 over WebSocket).
 *
 * Flow:
 *   1. Connect to WiFi (menuconfig GOLDIE_WIFI_SSID/PASSWORD)
 *   2. convai_create() with product info (menuconfig GOLDIE_*)
 *   3. convai_start() -> WS connect -> hello -> hello_ack
 *   4. Send a "hello world" text message to the gateway
 *   5. Log all status/event/message/audio callbacks
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "convai_api.h"

static const char *TAG = "goldie";

/* ---------------- WiFi ---------------- */

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static int retries = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++retries < 10) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "WiFi disconnected, retry %d", retries);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_GOLDIE_WIFI_SSID,
            .password = CONFIG_GOLDIE_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* ---------------- ConvAI callbacks ---------------- */

static const char *status_str(convai_status_e s)
{
    switch (s) {
    case CONVAI_STATUS_IDLE:            return "IDLE";
    case CONVAI_STATUS_LISTENING:       return "LISTENING";
    case CONVAI_STATUS_THINKING:        return "THINKING";
    case CONVAI_STATUS_ANSWERING:       return "ANSWERING";
    case CONVAI_STATUS_INTERRUPTED:     return "INTERRUPTED";
    case CONVAI_STATUS_ANSWER_FINISHED: return "ANSWER_FINISHED";
    default:                            return "?";
    }
}

static void on_event(convai_engine_t e, convai_event_t *ev, void *ud)
{
    (void)e; (void)ud;
    const char *names[] = { "CONNECTED", "DISCONNECTED", "FAILED", "UPDATED" };
    ESP_LOGI(TAG, "[EVENT] %s (%s)",
             ev->code <= CONVAI_EV_UPDATED ? names[ev->code] : "?",
             ev->data.details ? ev->data.details : "");
}

static void on_status(convai_engine_t e, convai_status_e s, void *ud)
{
    (void)e; (void)ud;
    ESP_LOGI(TAG, "[STATUS] %s", status_str(s));
}

static void on_audio(convai_engine_t e, const void *data, size_t len,
                     const convai_audio_frame_info_t *info, void *ud)
{
    (void)e; (void)data; (void)info; (void)ud;
    /* decoded mono PCM16; TODO: play through I2S */
    ESP_LOGD(TAG, "[AUDIO] %zu bytes PCM16 TTS", len);
}

static void on_message(convai_engine_t e, const void *data, size_t len,
                       const convai_message_info_t *info, void *ud)
{
    (void)e; (void)info; (void)ud;
    ESP_LOGI(TAG, "[MESSAGE] %.*s", (int)len, (const char *)data);
}

/* ---------------- app_main ---------------- */

static const char *STARTUP_CONFIG =
    "{"
        "\"config\":{"
            "\"llm_config\":{"
                "\"system_messages\":["
                    "\"你的名字叫小荷，你可以帮小朋友解决小烦恼哦。\""
                "]"
            "},"
            "\"tts_config\":{"
                "\"provider_params\":{"
                    "\"audio\":{"
                        "\"voice_type\":\"Chinese (Mandarin)_Warm_Girl\""
                    "}"
                "}"
            "}"
        "}"
    "}";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "connecting WiFi '%s' ...", CONFIG_GOLDIE_WIFI_SSID);
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed, restarting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    char config_json[768];
    snprintf(config_json, sizeof(config_json),
             "{"
                 "\"info\":{"
                     "\"product_id\":\"%s\","
                     "\"product_key\":\"%s\","
                     "\"product_secret\":\"%s\","
                     "\"device_name\":\"%s\""
                 "},"
                 "\"ws\":{"
                     "\"url\":\"%s\","
                     "\"audio\":{\"codec\":1}"
                 "}"
             "}",
             CONFIG_GOLDIE_PRODUCT_ID, CONFIG_GOLDIE_PRODUCT_KEY,
             CONFIG_GOLDIE_PRODUCT_SECRET, CONFIG_GOLDIE_DEVICE_NAME,
             CONFIG_GOLDIE_SERVER_URL);

    convai_event_handler_t handler = {
        .on_convai_event = on_event,
        .on_convai_conversation_status = on_status,
        .on_convai_audio_data = on_audio,
        .on_convai_message_data = on_message,
    };

    convai_engine_t engine = NULL;
    ret = convai_create(&engine, config_json, &handler, NULL);
    if (ret != CONVAI_OK) {
        ESP_LOGE(TAG, "convai_create failed: %s", convai_err_2_str(ret));
        return;
    }
    ESP_LOGI(TAG, "ConvAI SDK version: %s", convai_get_version());

    convai_opt_t opt = {
        .mode = CONVAI_MODE_WS,
        .agent_id = CONFIG_GOLDIE_AGENT_ID,
        .params = STARTUP_CONFIG,
    };
    ret = convai_start(engine, &opt);
    if (ret != CONVAI_OK) {
        ESP_LOGE(TAG, "convai_start failed: %s", convai_err_2_str(ret));
        convai_destroy(engine);
        return;
    }

    /* Wait for the session, then send "hello world" text to the gateway */
    vTaskDelay(pdMS_TO_TICKS(3000));
    const char *hello = "hello world";
    ret = convai_send_message(engine, hello, strlen(hello), NULL);
    ESP_LOGI(TAG, "send hello world -> %s", convai_err_2_str(ret));

    /* Demo: runtime codec switch + periodic tone uplink + memory report */
    const int codecs[] = {
        CONVAI_AUDIO_DATA_TYPE_PCM16,
        CONVAI_AUDIO_DATA_TYPE_G711A,
        CONVAI_AUDIO_DATA_TYPE_G711U,
        CONVAI_AUDIO_DATA_TYPE_IMA_ADPCM,
        CONVAI_AUDIO_DATA_TYPE_OPUS,
    };
    int16_t tone[160];
    for (int i = 0; i < 160; i++) {
        tone[i] = (int16_t)(12000 * ((i % 8) < 4 ? 1 : -1));
    }
    while (true) {
        for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
            ret = convai_set_codec(engine, codecs[i]);
            if (ret == CONVAI_OK) {
                ESP_LOGI(TAG, "codec = %d, sending 20ms PCM16 tone", convai_get_codec(engine));
                convai_send_audio(engine, tone, sizeof(tone), NULL);
            } else {
                ESP_LOGW(TAG, "codec %d unavailable: %s", codecs[i], convai_err_2_str(ret));
            }
            convai_mem_report(engine);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
