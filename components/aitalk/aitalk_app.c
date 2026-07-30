/**
 * @file aitalk_app.c
 * @brief AItalk application core (GUI-agnostic), ported from WS63
 *        goldieos apps/AItalk/main_app.cpp.
 *
 * Port mapping (原 -> 本文件):
 *  - play_type 状态机 (play_task, 307-333行)  -> aitalk_on_sdk_status
 *  - EMOTION_* 枚举与 current_emotion          -> aitalk_emotion_e + s_app.emotion
 *  - 云端 function_call 情绪下发               -> aitalk_on_sdk_message
 *    (WS63 经 convai_bridge_on_message 收 JSON, 本实现同源)
 *  - MsgQueue/add_msg 广播队列 (79-129行)      -> aitalk_push/pop_chat_msg (1500B->512B)
 *  - update_avatar_ui 动画计数逻辑             -> aitalk_tick (只保留计数, GUI 剥离)
 *  - avatar_id 男/女形象                       -> s_app.avatar_id
 */
#include <string.h>

#include "cJSON.h"

#include "aitalk_app.h"

typedef struct {
    int  uid;
    char buf[AITALK_MAX_MSG_LEN];
} aitalk_msg_t;

typedef struct {
    /* playback/emotion state */
    aitalk_play_type_e play_type;
    aitalk_emotion_e   emotion;
    int                avatar_id;
    int                sdk_started;
    int                frame;
    int                dir;          /* happy bounce direction (原 dir) */

    /* chat queue */
    aitalk_msg_t       msgs[AITALK_MAX_MSG_NUM];
    int                head, tail, count;

    /* ui callback */
    aitalk_ui_cb       ui_cb;
    void              *ui_ud;
} aitalk_app_s;

static aitalk_app_s s_app;

/* ---------------- lifecycle ---------------- */

void aitalk_init(void)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.play_type = AITALK_PLAY_SLEEP;
    s_app.emotion = AITALK_EMOTION_NEUTRAL;
}

void aitalk_deinit(void)
{
    memset(&s_app, 0, sizeof(s_app));
}

/* ---------------- SDK event handling ---------------- */

void aitalk_on_sdk_status(convai_status_e status)
{
    /* 原 play_task 映射: 未启动/IDLE -> SLEEP; ANSWERING -> SPEAK; 其他 -> SILENCE */
    if (!s_app.sdk_started || status == CONVAI_STATUS_IDLE) {
        s_app.play_type = AITALK_PLAY_SLEEP;
    } else if (status == CONVAI_STATUS_ANSWERING) {
        s_app.play_type = AITALK_PLAY_SPEAK;
    } else {
        s_app.play_type = AITALK_PLAY_SILENCE;
    }
}

static aitalk_emotion_e emotion_from_str(const char *s)
{
    if (!strcmp(s, "happy"))  return AITALK_EMOTION_HAPPY;
    if (!strcmp(s, "angry"))  return AITALK_EMOTION_ANGRY;
    if (!strcmp(s, "sad"))    return AITALK_EMOTION_SAD;
    if (!strcmp(s, "doubt"))  return AITALK_EMOTION_DOUBT;
    return AITALK_EMOTION_NEUTRAL;
}

int aitalk_on_sdk_message(const char *json, size_t len,
                          char *call_id_out, size_t call_id_cap)
{
    if (!json || !len) return 0;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return 0;

    int handled = 0;
    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(root, "calls");
    const cJSON *body  = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (!cJSON_IsArray(calls) && body) {
        /* envelope 形态: {"type":"function_call","body":{"calls":[...]}} */
        calls = cJSON_GetObjectItemCaseSensitive(body, "calls");
    }
    if (cJSON_IsArray(calls)) {
        const cJSON *call;
        cJSON_ArrayForEach(call, calls) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(call, "name");
            if (!cJSON_IsString(name) || strcmp(name->valuestring, "emotion")) continue;

            const cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "arguments");
            if (cJSON_IsString(args)) {
                cJSON *a = cJSON_Parse(args->valuestring);
                const cJSON *emo = a ? cJSON_GetObjectItemCaseSensitive(a, "emotion") : NULL;
                if (cJSON_IsString(emo)) {
                    s_app.emotion = emotion_from_str(emo->valuestring);
                    handled = 1;
                }
                if (a) cJSON_Delete(a);
            }
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(call, "call_id");
            if (handled && call_id_out && cJSON_IsString(cid)) {
                strncpy(call_id_out, cid->valuestring, call_id_cap - 1);
                call_id_out[call_id_cap - 1] = 0;
            }
        }
    }
    cJSON_Delete(root);
    return handled;
}

void aitalk_set_sdk_started(int started) { s_app.sdk_started = started; }
void aitalk_set_avatar(int avatar_id)    { s_app.avatar_id = avatar_id ? 1 : 0; }

/* ---------------- chat message queue ---------------- */

void aitalk_push_chat_msg(int uid, const char *msg, int msg_len)
{
    if (!msg || s_app.count >= AITALK_MAX_MSG_NUM) return;   /* 满则丢新(原行为) */
    if (msg_len > AITALK_MAX_MSG_LEN - 1) msg_len = AITALK_MAX_MSG_LEN - 1;

    aitalk_msg_t *m = &s_app.msgs[s_app.tail];
    m->uid = uid;
    memset(m->buf, 0, AITALK_MAX_MSG_LEN);
    memcpy(m->buf, msg, msg_len);
    s_app.tail = (s_app.tail + 1) % AITALK_MAX_MSG_NUM;
    s_app.count++;
}

int aitalk_pop_chat_msg(int *uid_out, char *out, int out_cap)
{
    if (s_app.count == 0 || !out || out_cap < 1) return 0;
    aitalk_msg_t *m = &s_app.msgs[s_app.head];
    if (uid_out) *uid_out = m->uid;
    strncpy(out, m->buf, out_cap - 1);
    out[out_cap - 1] = 0;
    s_app.head = (s_app.head + 1) % AITALK_MAX_MSG_NUM;
    s_app.count--;
    return 1;
}

int aitalk_chat_msg_count(void) { return s_app.count; }

/* ---------------- UI tick (动画计数, 原 update_avatar_ui 的计数部分) ---- */

void aitalk_tick(void)
{
    int *count = &s_app.frame;

    switch (s_app.play_type) {
    case AITALK_PLAY_SLEEP:                      /* 睡眠呼吸 8 帧循环 */
        *count = (*count + 1) % 8;
        break;
    case AITALK_PLAY_SILENCE:                    /* 平时 15 帧眨眼周期 */
        if (++(*count) == 15) *count = 0;
        break;
    case AITALK_PLAY_SPEAK:
        switch (s_app.emotion) {
        case AITALK_EMOTION_HAPPY:               /* 0..5 往返弹跳 */
            if (s_app.dir == 1) { if (++(*count) >= 5) { *count = 5; s_app.dir = 0; } }
            else                { if (--(*count) <= 0) { *count = 0; s_app.dir = 1; } }
            break;
        case AITALK_EMOTION_ANGRY:
        case AITALK_EMOTION_DOUBT:               /* 8 帧循环 */
            if (++(*count) >= 8) *count = 0;
            break;
        case AITALK_EMOTION_SAD:                 /* 20 帧循环(中间眨眼) */
            if (++(*count) >= 20) *count = 0;
            break;
        default:
            *count = 0;
            break;
        }
        break;
    }

    if (s_app.ui_cb) {
        aitalk_ui_state_t st = {
            .play_type = s_app.play_type,
            .emotion   = s_app.emotion,
            .avatar_id = s_app.avatar_id,
            .frame     = s_app.frame,
        };
        s_app.ui_cb(&st, s_app.ui_ud);
    }
}

void aitalk_set_ui_callback(aitalk_ui_cb cb, void *user_data)
{
    s_app.ui_cb = cb;
    s_app.ui_ud = user_data;
}

/* ---------------- getters ---------------- */

aitalk_play_type_e aitalk_get_play_type(void) { return s_app.play_type; }
aitalk_emotion_e   aitalk_get_emotion(void)   { return s_app.emotion; }
int                aitalk_get_frame(void)     { return s_app.frame; }
size_t             aitalk_mem_usage(void)     { return AITALK_MEM_BYTES; }
