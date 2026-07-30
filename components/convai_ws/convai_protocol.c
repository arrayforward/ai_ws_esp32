#include "convai_protocol.h"

#include <string.h>
#include <stdlib.h>

char *convai_proto_build_envelope(const char *type, const char *body_json,
                                  uint32_t seq, uint64_t ts)
{
    if (!type) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddNumberToObject(root, "ts", (double)ts);

    cJSON *body = NULL;
    if (body_json) body = cJSON_Parse(body_json);
    cJSON_AddItemToObject(root, "body", body ? body : cJSON_CreateObject());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

int convai_proto_parse_envelope(const char *data, size_t len, convai_envelope_t *out)
{
    if (!data || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return -1;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return -1;
    }
    strncpy(out->type, type->valuestring, sizeof(out->type) - 1);

    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(seq)) out->seq = (uint32_t)seq->valuedouble;

    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (cJSON_IsNumber(ts)) out->ts = (uint64_t)ts->valuedouble;

    cJSON *body = cJSON_DetachItemFromObjectCaseSensitive(root, "body");
    out->body = (body && cJSON_IsObject(body)) ? body : NULL;
    if (body && !out->body) cJSON_Delete(body);

    cJSON_Delete(root);
    return 0;
}

void convai_proto_envelope_free(convai_envelope_t *env)
{
    if (env && env->body) {
        cJSON_Delete(env->body);
        env->body = NULL;
    }
}

convai_status_e convai_proto_status_from_str(const char *s)
{
    if (!s) return CONVAI_STATUS_IDLE;
    if (!strcmp(s, "listening"))       return CONVAI_STATUS_LISTENING;
    if (!strcmp(s, "thinking"))        return CONVAI_STATUS_THINKING;
    if (!strcmp(s, "answering"))       return CONVAI_STATUS_ANSWERING;
    if (!strcmp(s, "interrupted"))     return CONVAI_STATUS_INTERRUPTED;
    if (!strcmp(s, "answer_finished")) return CONVAI_STATUS_ANSWER_FINISHED;
    return CONVAI_STATUS_IDLE;
}

void convai_proto_audio_hdr_pack(uint8_t hdr[CONVAI_AUDIO_HDR_LEN],
                                 uint8_t op, uint32_t seq, uint64_t ts)
{
    hdr[0] = op;
    hdr[1] = (seq >> 24) & 0xFF;
    hdr[2] = (seq >> 16) & 0xFF;
    hdr[3] = (seq >> 8) & 0xFF;
    hdr[4] = seq & 0xFF;
    for (int i = 0; i < 8; i++) {
        hdr[5 + i] = (uint8_t)(ts >> (56 - 8 * i));
    }
}

int convai_proto_audio_hdr_unpack(const uint8_t *data, size_t len,
                                  uint8_t *op, uint32_t *seq, uint64_t *ts)
{
    if (!data || len < CONVAI_AUDIO_HDR_LEN) return -1;
    if (op)  *op = data[0];
    if (seq) *seq = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                    ((uint32_t)data[3] << 8) | (uint32_t)data[4];
    if (ts) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | data[5 + i];
        *ts = v;
    }
    return 0;
}
