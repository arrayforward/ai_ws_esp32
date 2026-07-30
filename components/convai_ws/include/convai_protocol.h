/**
 * @file convai_protocol.h
 * @brief Pure-C encode/decode helpers for the convai.v1 wire protocol.
 *
 * Platform-independent: only depends on cJSON. Used by the ESP-IDF
 * transport (convai_ws.c) and by host-side unit tests.
 *
 * Text envelope: {"type":..., "seq":..., "ts":..., "body":{...}}
 * Binary audio:  13-byte header (u8 op, u32 BE seq, u64 BE ts) + payload
 */
#ifndef CONVAI_PROTOCOL_H
#define CONVAI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#include "convai_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Binary audio ops (protocol.md) */
#define CONVAI_AUDIO_OP_FRAME   0x10
#define CONVAI_AUDIO_OP_START   0x11
#define CONVAI_AUDIO_OP_END     0x12
#define CONVAI_AUDIO_OP_CANCEL  0x13
#define CONVAI_AUDIO_HDR_LEN    13

/* Parsed text envelope. `body` is owned by the caller and must be freed
 * with cJSON_Delete(). `type` points into a fixed buffer. */
typedef struct {
    char     type[32];
    uint32_t seq;
    uint64_t ts;
    cJSON   *body;      /* NULL when absent or not an object */
} convai_envelope_t;

/**
 * Build a text envelope JSON string.
 * @param type      message type string (e.g. "hello")
 * @param body_json optional raw JSON object string for "body" (may be NULL)
 * @param seq       sequence number
 * @param ts        timestamp in ms
 * @return malloc'd string (caller frees), or NULL on error
 */
char *convai_proto_build_envelope(const char *type, const char *body_json,
                                  uint32_t seq, uint64_t ts);

/**
 * Parse a text envelope.
 * @return 0 on success (out filled), -1 on parse error
 */
int convai_proto_parse_envelope(const char *data, size_t len, convai_envelope_t *out);

/** Free resources held by a parsed envelope. */
void convai_proto_envelope_free(convai_envelope_t *env);

/** Map a status string to convai_status_e (unknown -> CONVAI_STATUS_IDLE). */
convai_status_e convai_proto_status_from_str(const char *s);

/** Pack a 13-byte binary audio header. */
void convai_proto_audio_hdr_pack(uint8_t hdr[CONVAI_AUDIO_HDR_LEN],
                                 uint8_t op, uint32_t seq, uint64_t ts);

/** Unpack a 13-byte binary audio header. @return 0 on success, -1 if len < 13. */
int convai_proto_audio_hdr_unpack(const uint8_t *data, size_t len,
                                  uint8_t *op, uint32_t *seq, uint64_t *ts);

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_PROTOCOL_H */
