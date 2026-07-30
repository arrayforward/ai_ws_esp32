/**
 * @file convai_codec.h
 * @brief Pluggable audio codec layer for the ConvAI SDK.
 *
 * All codecs share one interface operating on mono 16-bit PCM.
 * The active codec can be switched at runtime (convai_set_codec)
 * and is negotiated with the gateway via the `audio_codec` field
 * of the `hello` message.
 *
 * Built-in codecs (wire id in parentheses):
 *   - PCM16      (0)  pass-through, no compression
 *   - G.711A     (1)  ITU-T A-law, 2:1, very low CPU
 *   - G.711U     (2)  ITU-T u-law, 2:1, very low CPU
 *   - IMA-ADPCM  (3)  4:1, trivial CPU, common on low-end MCUs
 *   - Opus       (4)  high quality, optional (CONFIG_CONVAI_ENABLE_OPUS)
 */
#ifndef CONVAI_CODEC_H
#define CONVAI_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Codec identifiers (sent on the wire as hello.audio_codec). */
typedef enum {
    CONVAI_CODEC_PCM16     = 0,
    CONVAI_CODEC_G711A     = 1,
    CONVAI_CODEC_G711U     = 2,
    CONVAI_CODEC_IMA_ADPCM = 3,
    CONVAI_CODEC_OPUS      = 4,
    CONVAI_CODEC_MAX
} convai_codec_id_e;

/** Opaque per-instance codec state (sized by codec->state_size). */
typedef void convai_codec_state_t;

typedef struct {
    convai_codec_id_e id;
    const char       *name;
    /** Native sample rate the codec expects (informational). */
    int               sample_rate;
    /** Bytes to allocate for per-instance state (0 = stateless). */
    size_t            state_size;

    /** Initialise / reset instance state (may be NULL). */
    int  (*init)(convai_codec_state_t *state);

    /**
     * Encode mono PCM16 to the codec format.
     * @param pcm         input samples
     * @param pcm_samples number of 16-bit samples
     * @param out         output buffer
     * @param out_cap     output capacity in bytes
     * @param out_len     actual encoded bytes
     * @return 0 on success, -1 on error
     */
    int  (*encode)(convai_codec_state_t *state,
                   const int16_t *pcm, size_t pcm_samples,
                   uint8_t *out, size_t out_cap, size_t *out_len);

    /**
     * Decode to mono PCM16.
     * @param enc         encoded input
     * @param enc_len     encoded bytes
     * @param pcm         output sample buffer
     * @param pcm_cap     output capacity in samples
     * @param pcm_samples actual decoded samples
     * @return 0 on success, -1 on error
     */
    int  (*decode)(convai_codec_state_t *state,
                   const uint8_t *enc, size_t enc_len,
                   int16_t *pcm, size_t pcm_cap, size_t *pcm_samples);

    /**
     * Release resources held by instance state (may be NULL).
     * The state memory itself is freed by the caller afterwards.
     */
    void (*deinit)(convai_codec_state_t *state);

    /**
     * Current heap memory used by this instance, in bytes
     * (excluding the state struct itself unless noted). May be NULL,
     * meaning zero extra memory.
     */
    size_t (*mem_usage)(convai_codec_state_t *state);
} convai_codec_t;

/**
 * Look up a codec by id.
 * @return codec descriptor, or NULL if unsupported / disabled at build time
 */
const convai_codec_t *convai_codec_get(convai_codec_id_e id);

/** Number of codecs compiled in. */
size_t convai_codec_count(void);

/** Iterate compiled-in codecs (index < convai_codec_count()). */
const convai_codec_t *convai_codec_at(size_t index);

/** Find a codec by name (e.g. "opus"), or NULL. */
const convai_codec_t *convai_codec_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_CODEC_H */
