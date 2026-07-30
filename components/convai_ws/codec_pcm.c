/**
 * @file codec_pcm.c
 * @brief PCM16 pass-through codec (no compression).
 */
#include <string.h>
#include "convai_codec.h"

static int pcm_encode(convai_codec_state_t *st,
                      const int16_t *pcm, size_t pcm_samples,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)st;
    if (!pcm || !out || !out_len) return -1;
    size_t need = pcm_samples * 2;
    if (out_cap < need) return -1;
    memcpy(out, pcm, need);
    *out_len = need;
    return 0;
}

static int pcm_decode(convai_codec_state_t *st,
                      const uint8_t *enc, size_t enc_len,
                      int16_t *pcm, size_t pcm_cap, size_t *pcm_samples)
{
    (void)st;
    if (!enc || !pcm || !pcm_samples) return -1;
    if (enc_len % 2 != 0) return -1;
    size_t samples = enc_len / 2;
    if (pcm_cap < samples) return -1;
    memcpy(pcm, enc, enc_len);
    *pcm_samples = samples;
    return 0;
}

static const convai_codec_t s_codec = {
    .id = CONVAI_CODEC_PCM16,
    .name = "pcm16",
    .sample_rate = 8000,
    .state_size = 0,
    .init = 0,
    .encode = pcm_encode,
    .decode = pcm_decode,
};

const convai_codec_t *convai_codec_pcm16(void) { return &s_codec; }
