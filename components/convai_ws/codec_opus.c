/**
 * @file codec_opus.c
 * @brief Opus codec adapter (optional, CONFIG_CONVAI_ENABLE_OPUS).
 *
 * 16 kHz mono, 20 ms frames, low complexity — suitable for ESP32-S3.
 * Encoder and decoder are allocated lazily so that a half-duplex
 * direction never pays for the other side's memory. When disabled at
 * build time, convai_codec_get(CONVAI_CODEC_OPUS) returns NULL.
 */
#include "sdkconfig.h"

#ifdef CONFIG_CONVAI_ENABLE_OPUS

#include <string.h>
#include "opus.h"
#include "convai_codec.h"

#define OPUS_SAMPLE_RATE   16000
#define OPUS_CHANNELS      1
#define OPUS_BITRATE       16000
#define OPUS_COMPLEXITY    1
#define OPUS_MAX_PACKET    256

typedef struct {
    OpusEncoder *enc;
    OpusDecoder *dec;
} opus_state_t;

static int opus_init(convai_codec_state_t *state)
{
    opus_state_t *s = (opus_state_t *)state;
    s->enc = 0;
    s->dec = 0;
    return 0;
}

static void opus_deinit(convai_codec_state_t *state)
{
    opus_state_t *s = (opus_state_t *)state;
    if (s->enc) { opus_encoder_destroy(s->enc); s->enc = 0; }
    if (s->dec) { opus_decoder_destroy(s->dec); s->dec = 0; }
}

static size_t opus_mem_usage(convai_codec_state_t *state)
{
    opus_state_t *s = (opus_state_t *)state;
    size_t n = sizeof(opus_state_t);
    if (s->enc) n += (size_t)opus_encoder_get_size(OPUS_CHANNELS);
    if (s->dec) n += (size_t)opus_decoder_get_size(OPUS_CHANNELS);
    return n;
}

static int opus_enc_impl(convai_codec_state_t *state,
                         const int16_t *pcm, size_t pcm_samples,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    opus_state_t *s = (opus_state_t *)state;
    if (!s || !pcm || !out || !out_len) return -1;

    if (!s->enc) {
        int err = 0;
        /* RESTRICTED_LOWDELAY (CELT-only): lowest latency, and avoids the
         * SILK encoder path which is unstable in this vendored 1.6.1 tree. */
        s->enc = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                     OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK || !s->enc) { s->enc = 0; return -1; }
        opus_encoder_ctl(s->enc, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(s->enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
        opus_encoder_ctl(s->enc, OPUS_SET_VBR(0));
    }

    if (out_cap > OPUS_MAX_PACKET) out_cap = OPUS_MAX_PACKET;
    opus_int32 n = opus_encode(s->enc, pcm, (int)pcm_samples, out, (opus_int32)out_cap);
    if (n < 0) return -1;
    *out_len = (size_t)n;
    return 0;
}

static int opus_dec_impl(convai_codec_state_t *state,
                         const uint8_t *enc, size_t enc_len,
                         int16_t *pcm, size_t pcm_cap, size_t *pcm_samples)
{
    opus_state_t *s = (opus_state_t *)state;
    if (!s || !enc || !pcm || !pcm_samples) return -1;

    if (!s->dec) {
        int err = 0;
        s->dec = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &err);
        if (err != OPUS_OK || !s->dec) { s->dec = 0; return -1; }
    }

    int n = opus_decode(s->dec, enc, (opus_int32)enc_len, pcm, (int)pcm_cap, 0);
    if (n < 0) return -1;
    *pcm_samples = (size_t)n;
    return 0;
}

static const convai_codec_t s_codec = {
    .id = CONVAI_CODEC_OPUS,
    .name = "opus",
    .sample_rate = OPUS_SAMPLE_RATE,
    .state_size = sizeof(opus_state_t),
    .init = opus_init,
    .encode = opus_enc_impl,
    .decode = opus_dec_impl,
    .deinit = opus_deinit,
    .mem_usage = opus_mem_usage,
};

const convai_codec_t *convai_codec_opus(void) { return &s_codec; }

#endif /* CONFIG_CONVAI_ENABLE_OPUS */
