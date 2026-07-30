/**
 * @file aitalk_app.h
 * @brief AItalk application core (GUI-agnostic), ported from WS63
 *        goldieos apps/AItalk/main_app.cpp.
 *
 * Contains only the protocol-stack/SDK-related logic of the original
 * app: SDK status -> playback type mapping, cloud function_call
 * emotion handling, and the chat message queue. All GUI/tiny_gui
 * calls are removed; a UI layer (LVGL) can attach later via the
 * getters and the tick callback.
 *
 * Memory: fully static (no heap), see AITALK_MEM_BYTES.
 */
#ifndef AITALK_APP_H
#define AITALK_APP_H

#include <stddef.h>

#include "convai_types.h"   /* convai_status_e */

#ifdef __cplusplus
extern "C" {
#endif

/* playback types (原 PLAY_TYPE_*) */
typedef enum {
    AITALK_PLAY_SILENCE = 0,
    AITALK_PLAY_SPEAK,
    AITALK_PLAY_SLEEP,
} aitalk_play_type_e;

/* emotions (原 EMOTION_*) */
typedef enum {
    AITALK_EMOTION_NEUTRAL = 0,
    AITALK_EMOTION_HAPPY,
    AITALK_EMOTION_ANGRY,
    AITALK_EMOTION_SAD,
    AITALK_EMOTION_DOUBT,
} aitalk_emotion_e;

/* chat message queue sizing (原 MAX_MSG_NUM / MAX_CHAT_MSG_LEN 的裁剪版:
 * 1500B -> 512B, 云端文本回复一般不超过 500 字符) */
#define AITALK_MAX_MSG_NUM   4
#define AITALK_MAX_MSG_LEN   512

/* total static bytes used by this module (for the <100KB budget) */
#define AITALK_MEM_BYTES     (AITALK_MAX_MSG_NUM * AITALK_MAX_MSG_LEN + 64)

/**
 * Optional UI callback, invoked by aitalk_tick() every animation frame
 * with the current logical eye/avatar state. Register NULL to disable.
 */
typedef struct {
    aitalk_play_type_e play_type;
    aitalk_emotion_e   emotion;
    int                avatar_id;    /* 0=female, 1=male (原 avatar_id) */
    int                frame;        /* animation frame counter */
} aitalk_ui_state_t;

typedef void (*aitalk_ui_cb)(const aitalk_ui_state_t *state, void *user_data);

/* ---- lifecycle ---- */
void aitalk_init(void);
void aitalk_deinit(void);

/* ---- SDK event entry points (wire to convai callbacks) ---- */

/** Map SDK conversation status to playback type (原 play_task 中的映射). */
void aitalk_on_sdk_status(convai_status_e status);

/**
 * Feed a cloud message envelope (on_convai_message_data payload).
 * Detects function_call "emotion" and updates the current emotion.
 * @param call_id_out  optional; when a function call was handled,
 *                     receives the call_id that MUST be answered with
 *                     function_call_output (protocol requirement).
 * @return 1 if a function call was handled, 0 otherwise
 */
int aitalk_on_sdk_message(const char *json, size_t len,
                          char *call_id_out, size_t call_id_cap);

/** Set SDK started flag (false -> playback forced to SLEEP). */
void aitalk_set_sdk_started(int started);

/** Set avatar id (0=female, 1=male). */
void aitalk_set_avatar(int avatar_id);

/* ---- chat message queue (原 add_msg/MsgQueue 广播消息队列) ---- */

/** Push a broadcast chat message (drop-newest when full, 与原行为一致). */
void aitalk_push_chat_msg(int uid, const char *msg, int msg_len);

/** Pop the oldest message. @return 1 if a message was popped. */
int aitalk_pop_chat_msg(int *uid_out, char *out, int out_cap);

/** Number of queued messages. */
int aitalk_chat_msg_count(void);

/* ---- UI tick ---- */

/**
 * Drive one animation tick (call every 200 ms, 原 play_task 周期).
 * Advances frame counters and invokes the registered UI callback.
 */
void aitalk_tick(void);

void aitalk_set_ui_callback(aitalk_ui_cb cb, void *user_data);

/* ---- getters (for UI/tests) ---- */
aitalk_play_type_e aitalk_get_play_type(void);
aitalk_emotion_e   aitalk_get_emotion(void);
int                aitalk_get_frame(void);
size_t             aitalk_mem_usage(void);

#ifdef __cplusplus
}
#endif

#endif /* AITALK_APP_H */
