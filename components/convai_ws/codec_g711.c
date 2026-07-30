/**
 * @file codec_g711.c
 * @brief G.711 codec adapters (A-law + u-law) for the pluggable codec layer.
 *
 * A-law reuses the ITU-T implementation in convai_codec_g711a.c.
 * u-law is implemented here (standard G.711 mu-law).
 */
#include "convai_codec.h"
#include "convai_codec_g711a.h"

/* ---------------- A-law adapter (delegates to convai_codec_g711a.c) -------- */

static int g711a_encode(convai_codec_state_t *st,
                        const int16_t *pcm, size_t pcm_samples,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)st;
    return convai_g711a_encode((const uint8_t *)pcm, pcm_samples * 2, 1,
                               out, out_cap, out_len);
}

static int g711a_decode(convai_codec_state_t *st,
                        const uint8_t *enc, size_t enc_len,
                        int16_t *pcm, size_t pcm_cap, size_t *pcm_samples)
{
    (void)st;
    size_t out_bytes = 0;
    int rc = convai_g711a_decode(enc, enc_len, (uint8_t *)pcm, pcm_cap * 2, &out_bytes);
    if (rc == 0) *pcm_samples = out_bytes / 2;
    return rc;
}

/* ---------------- u-law (ITU-T G.711 mu-law) ---------------- */

#define MULAW_BIAS  132
#define MULAW_CLIP  32635

static uint8_t pcm16_to_mulaw(int16_t pcm_val)
{
    int mask;
    int seg;
    uint8_t uval;

    if (pcm_val < 0) {
        pcm_val = (int16_t)(MULAW_BIAS - pcm_val);
        mask = 0x7F;
    } else {
        pcm_val = (int16_t)(MULAW_BIAS + pcm_val);
        mask = 0xFF;
    }
    if (pcm_val > MULAW_CLIP) pcm_val = MULAW_CLIP;

    /* segment search */
    seg = 7;
    {
        static const int16_t seg_end[8] = {
            0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF
        };
        for (seg = 0; seg < 8; seg++) {
            if (pcm_val <= seg_end[seg]) break;
        }
        if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    }
    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

static int16_t mulaw_to_pcm16(uint8_t u_val)
{
    u_val = (uint8_t)~u_val;
    int t = ((u_val & 0x0F) << 3) + MULAW_BIAS;
    t <<= (u_val & 0x70) >> 4;
    return (int16_t)((u_val & 0x80) ? (MULAW_BIAS - t) : (t - MULAW_BIAS));
}

static int g711u_encode(convai_codec_state_t *st,
                        const int16_t *pcm, size_t pcm_samples,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)st;
    if (!pcm || !out || !out_len) return -1;
    if (out_cap < pcm_samples) return -1;
    for (size_t i = 0; i < pcm_samples; i++) {
        out[i] = pcm16_to_mulaw(pcm[i]);
    }
    *out_len = pcm_samples;
    return 0;
}

static int g711u_decode(convai_codec_state_t *st,
                        const uint8_t *enc, size_t enc_len,
                        int16_t *pcm, size_t pcm_cap, size_t *pcm_samples)
{
    (void)st;
    if (!enc || !pcm || !pcm_samples) return -1;
    if (pcm_cap < enc_len) return -1;
    for (size_t i = 0; i < enc_len; i++) {
        pcm[i] = mulaw_to_pcm16(enc[i]);
    }
    *pcm_samples = enc_len;
    return 0;
}

/* ---------------- descriptors ---------------- */

static const convai_codec_t s_codec_a = {
    .id = CONVAI_CODEC_G711A,
    .name = "g711a",
    .sample_rate = 8000,
    .state_size = 0,
    .init = 0,
    .encode = g711a_encode,
    .decode = g711a_decode,
};

static const convai_codec_t s_codec_u = {
    .id = CONVAI_CODEC_G711U,
    .name = "g711u",
    .sample_rate = 8000,
    .state_size = 0,
    .init = 0,
    .encode = g711u_encode,
    .decode = g711u_decode,
};

const convai_codec_t *convai_codec_g711a(void) { return &s_codec_a; }
const convai_codec_t *convai_codec_g711u(void) { return &s_codec_u; }
