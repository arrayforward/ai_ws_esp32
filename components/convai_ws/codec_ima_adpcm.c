/**
 * @file codec_ima_adpcm.c
 * @brief IMA/DVI ADPCM codec (4 bits/sample, 4:1 compression).
 *
 * Extremely low CPU cost — table-driven, no multiply in the hot loop —
 * making it suitable for low-power MCUs. Mono, low nibble first.
 * Encoder/decoder state (predictor + step index) is kept per instance
 * so continuous streams stay coherent across frames.
 */
#include <string.h>
#include "convai_codec.h"

typedef struct {
    int32_t predictor;
    int32_t step_index;
    uint8_t nibble_buf;   /* pending low nibble */
    int     have_nibble;
} ima_state_t;

static const int16_t s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static int ima_init(convai_codec_state_t *state)
{
    ima_state_t *s = (ima_state_t *)state;
    s->predictor = 0;
    s->step_index = 0;
    s->nibble_buf = 0;
    s->have_nibble = 0;
    return 0;
}

/* Encode one sample to a 4-bit ADPCM nibble, updating state. */
static uint8_t ima_encode_sample(ima_state_t *s, int16_t sample)
{
    int32_t step = s_step_table[s->step_index];
    int32_t diff = (int32_t)sample - s->predictor;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int32_t vpdiff = step >> 3;
    if (diff >= step)      { nibble |= 4; diff -= step;     vpdiff += step; }
    if (diff >= (step>>1)) { nibble |= 2; diff -= step>>1;  vpdiff += step>>1; }
    if (diff >= (step>>2)) { nibble |= 1;                    vpdiff += step>>2; }

    s->predictor += (nibble & 8) ? -vpdiff : vpdiff;
    if (s->predictor > 32767)  s->predictor = 32767;
    if (s->predictor < -32768) s->predictor = -32768;

    s->step_index += s_index_table[nibble];
    if (s->step_index < 0)  s->step_index = 0;
    if (s->step_index > 88) s->step_index = 88;

    return nibble & 0x0F;
}

/* Decode one 4-bit nibble, updating state. */
static int16_t ima_decode_sample(ima_state_t *s, uint8_t nibble)
{
    int32_t step = s_step_table[s->step_index];
    int32_t vpdiff = step >> 3;

    if (nibble & 4) vpdiff += step;
    if (nibble & 2) vpdiff += step >> 1;
    if (nibble & 1) vpdiff += step >> 2;

    s->predictor += (nibble & 8) ? -vpdiff : vpdiff;
    if (s->predictor > 32767)  s->predictor = 32767;
    if (s->predictor < -32768) s->predictor = -32768;

    s->step_index += s_index_table[nibble & 0x0F];
    if (s->step_index < 0)  s->step_index = 0;
    if (s->step_index > 88) s->step_index = 88;

    return (int16_t)s->predictor;
}

static int ima_encode(convai_codec_state_t *state,
                      const int16_t *pcm, size_t pcm_samples,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    ima_state_t *s = (ima_state_t *)state;
    if (!pcm || !out || !out_len) return -1;
    if (out_cap < (pcm_samples + 1) / 2) return -1;

    size_t out_i = 0;
    for (size_t i = 0; i < pcm_samples; i++) {
        uint8_t nib = ima_encode_sample(s, pcm[i]);
        if (!s->have_nibble) {
            s->nibble_buf = nib;        /* low nibble first */
            s->have_nibble = 1;
        } else {
            out[out_i++] = (uint8_t)(s->nibble_buf | (nib << 4));
            s->have_nibble = 0;
        }
    }
    if (s->have_nibble) {               /* odd sample count: flush */
        out[out_i++] = s->nibble_buf;
        s->have_nibble = 0;
    }
    *out_len = out_i;
    return 0;
}

static int ima_decode(convai_codec_state_t *state,
                      const uint8_t *enc, size_t enc_len,
                      int16_t *pcm, size_t pcm_cap, size_t *pcm_samples)
{
    ima_state_t *s = (ima_state_t *)state;
    if (!enc || !pcm || !pcm_samples) return -1;
    if (pcm_cap < enc_len * 2) return -1;

    size_t n = 0;
    for (size_t i = 0; i < enc_len; i++) {
        pcm[n++] = ima_decode_sample(s, enc[i] & 0x0F);
        pcm[n++] = ima_decode_sample(s, (enc[i] >> 4) & 0x0F);
    }
    *pcm_samples = n;
    return 0;
}

static size_t ima_mem_usage(convai_codec_state_t *state)
{
    (void)state;
    return sizeof(ima_state_t);
}

static const convai_codec_t s_codec = {
    .id = CONVAI_CODEC_IMA_ADPCM,
    .name = "ima_adpcm",
    .sample_rate = 8000,
    .state_size = sizeof(ima_state_t),
    .init = ima_init,
    .encode = ima_encode,
    .decode = ima_decode,
    .deinit = 0,
    .mem_usage = ima_mem_usage,
};

const convai_codec_t *convai_codec_ima_adpcm(void) { return &s_codec; }
