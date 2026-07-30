/**
 * @file test_main.c
 * @brief Host-side (linux target) unit tests for the convai.v1 protocol
 *        codec (convai_protocol.c) and the G.711A codec
 *        (convai_codec_g711a.c).
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "unity.h"
#include "cJSON.h"

#include "convai_protocol.h"
#include "convai_codec_g711a.h"
#include "convai_codec.h"

/* ================= G.711A codec tests ================= */

static void test_g711a_encode_silence(void)
{
    /* PCM 0 must encode to A-law 0xD5 (ITU-T G.711) */
    int16_t pcm[4] = {0, 0, 0, 0};
    uint8_t out[4] = {0};
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(0, convai_g711a_encode((const uint8_t *)pcm, sizeof(pcm),
                                                 1, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xD5, out[i]);
    }
}

static void test_g711a_decode_silence_code(void)
{
    /* A-law 0xD5 decodes to a tiny positive value (8 in this implementation) */
    uint8_t enc[1] = {0xD5};
    uint8_t pcm[2] = {0};
    size_t pcm_len = 0;

    TEST_ASSERT_EQUAL_INT(0, convai_g711a_decode(enc, 1, pcm, sizeof(pcm), &pcm_len));
    TEST_ASSERT_EQUAL_size_t(2, pcm_len);
    int16_t v = (int16_t)(pcm[0] | (pcm[1] << 8));
    TEST_ASSERT_EQUAL_INT16(8, v);
}

static void test_g711a_roundtrip(void)
{
    const int16_t samples[] = {0, 100, -100, 1000, -1000, 8000, -8000,
                               16000, -16000, 30000, -30000, 32767, -32768};
    const size_t n = sizeof(samples) / sizeof(samples[0]);
    uint8_t enc[32] = {0};
    uint8_t dec[64] = {0};
    size_t enc_len = 0, dec_len = 0;

    TEST_ASSERT_EQUAL_INT(0, convai_g711a_encode((const uint8_t *)samples, n * 2,
                                                 1, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_size_t(n, enc_len);

    TEST_ASSERT_EQUAL_INT(0, convai_g711a_decode(enc, enc_len,
                                                 dec, sizeof(dec), &dec_len));
    TEST_ASSERT_EQUAL_size_t(n * 2, dec_len);

    for (size_t i = 0; i < n; i++) {
        int16_t v = (int16_t)(dec[i * 2] | (dec[i * 2 + 1] << 8));
        /* sign must be preserved (except near zero) */
        if (samples[i] > 100)  TEST_ASSERT_GREATER_THAN_INT16(0, v);
        if (samples[i] < -100) TEST_ASSERT_LESS_THAN_INT16(0, v);
        /* A-law quantization error: within one quantization step (~1/16 of value) */
        int32_t tol = 40 + abs(samples[i]) / 8;
        int32_t err = abs((int32_t)v - (int32_t)samples[i]);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(tol, err);
    }
}

static void test_g711a_encode_param_errors(void)
{
    int16_t pcm[2] = {0, 0};
    uint8_t out[2];
    size_t out_len;

    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_encode(NULL, 4, 1, out, 2, &out_len));
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_encode((const uint8_t *)pcm, 4, 1, NULL, 2, &out_len));
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_encode((const uint8_t *)pcm, 4, 1, out, 2, NULL));
    /* odd input length */
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_encode((const uint8_t *)pcm, 3, 1, out, 2, &out_len));
    /* output buffer too small */
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_encode((const uint8_t *)pcm, 4, 1, out, 1, &out_len));
}

static void test_g711a_decode_param_errors(void)
{
    uint8_t enc[2] = {0xD5, 0xD5};
    uint8_t pcm[4];
    size_t pcm_len;

    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_decode(NULL, 2, pcm, 4, &pcm_len));
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_decode(enc, 2, NULL, 4, &pcm_len));
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_decode(enc, 2, pcm, 4, NULL));
    /* output buffer too small: 2 bytes encoded need 4 bytes PCM */
    TEST_ASSERT_EQUAL_INT(-1, convai_g711a_decode(enc, 2, pcm, 3, &pcm_len));
}

static void test_g711a_stereo_planar_layout(void)
{
    /* planar stereo [L0,L1, R0,R1] -> channel-blocked output [enc(L0),enc(L1), enc(R0),enc(R1)] */
    int16_t pcm[4] = {0, 0, 1000, 1000};   /* L = silence, R = tone */
    uint8_t out[4] = {0};
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(0, convai_g711a_encode((const uint8_t *)pcm, sizeof(pcm),
                                                 2, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    /* left channel block = silence */
    TEST_ASSERT_EQUAL_UINT8(0xD5, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD5, out[1]);
    /* right channel block differs from silence */
    TEST_ASSERT_TRUE(out[2] != 0xD5 || out[3] != 0xD5);
    TEST_ASSERT_EQUAL_UINT8(out[2], out[3]);
}

/* ================= protocol envelope tests ================= */

static void test_envelope_build_parse_roundtrip(void)
{
    const char *body = "{\"product_id\":\"pid\",\"audio_codec\":1}";
    char *json = convai_proto_build_envelope("hello", body, 42, 1737034800123ULL);
    TEST_ASSERT_NOT_NULL(json);

    convai_envelope_t env;
    TEST_ASSERT_EQUAL_INT(0, convai_proto_parse_envelope(json, strlen(json), &env));
    TEST_ASSERT_EQUAL_STRING("hello", env.type);
    TEST_ASSERT_EQUAL_UINT32(42, env.seq);
    TEST_ASSERT_EQUAL_UINT64(1737034800123ULL, env.ts);
    TEST_ASSERT_NOT_NULL(env.body);

    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(env.body, "product_id");
    TEST_ASSERT_TRUE(cJSON_IsString(pid));
    TEST_ASSERT_EQUAL_STRING("pid", pid->valuestring);
    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(env.body, "audio_codec");
    TEST_ASSERT_TRUE(cJSON_IsNumber(codec));
    TEST_ASSERT_EQUAL_INT(1, codec->valueint);

    convai_proto_envelope_free(&env);
    free(json);
}

static void test_envelope_null_body_becomes_empty_object(void)
{
    char *json = convai_proto_build_envelope("bye", NULL, 1, 0);
    TEST_ASSERT_NOT_NULL(json);

    convai_envelope_t env;
    TEST_ASSERT_EQUAL_INT(0, convai_proto_parse_envelope(json, strlen(json), &env));
    TEST_ASSERT_EQUAL_STRING("bye", env.type);
    TEST_ASSERT_NOT_NULL(env.body);
    TEST_ASSERT_TRUE(cJSON_IsObject(env.body));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(env.body));

    convai_proto_envelope_free(&env);
    free(json);
}

static void test_envelope_parse_invalid(void)
{
    convai_envelope_t env;

    /* not JSON */
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_parse_envelope("not json", 8, &env));
    /* JSON but missing type */
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_parse_envelope("{\"seq\":1}", 9, &env));
    /* NULL args */
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_parse_envelope(NULL, 4, &env));
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_parse_envelope("{}", 2, NULL));
}

static void test_envelope_non_object_body_ignored(void)
{
    const char *json = "{\"type\":\"status\",\"seq\":2,\"ts\":5,\"body\":\"listening\"}";
    convai_envelope_t env;
    TEST_ASSERT_EQUAL_INT(0, convai_proto_parse_envelope(json, strlen(json), &env));
    TEST_ASSERT_EQUAL_STRING("status", env.type);
    TEST_ASSERT_EQUAL_UINT32(2, env.seq);
    TEST_ASSERT_EQUAL_UINT64(5, env.ts);
    TEST_ASSERT_NULL(env.body);   /* string body is not an object -> NULL */
    convai_proto_envelope_free(&env);
}

static void test_status_from_str(void)
{
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_IDLE, convai_proto_status_from_str("idle"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_LISTENING, convai_proto_status_from_str("listening"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_THINKING, convai_proto_status_from_str("thinking"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_ANSWERING, convai_proto_status_from_str("answering"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_INTERRUPTED, convai_proto_status_from_str("interrupted"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_ANSWER_FINISHED, convai_proto_status_from_str("answer_finished"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_IDLE, convai_proto_status_from_str("bogus"));
    TEST_ASSERT_EQUAL_INT(CONVAI_STATUS_IDLE, convai_proto_status_from_str(NULL));
}

static void test_audio_hdr_roundtrip(void)
{
    uint8_t hdr[CONVAI_AUDIO_HDR_LEN];
    convai_proto_audio_hdr_pack(hdr, CONVAI_AUDIO_OP_FRAME, 0xDEADBEEF, 0x0102030405060708ULL);

    /* verify big-endian layout on the wire */
    TEST_ASSERT_EQUAL_UINT8(0x10, hdr[0]);
    TEST_ASSERT_EQUAL_UINT8(0xDE, hdr[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, hdr[2]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, hdr[3]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, hdr[4]);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), hdr[5 + i]);
    }

    uint8_t op; uint32_t seq; uint64_t ts;
    TEST_ASSERT_EQUAL_INT(0, convai_proto_audio_hdr_unpack(hdr, sizeof(hdr), &op, &seq, &ts));
    TEST_ASSERT_EQUAL_UINT8(CONVAI_AUDIO_OP_FRAME, op);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, seq);
    TEST_ASSERT_EQUAL_UINT64(0x0102030405060708ULL, ts);
}

static void test_audio_hdr_unpack_short_buffer(void)
{
    uint8_t hdr[CONVAI_AUDIO_HDR_LEN - 1] = {0};
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_audio_hdr_unpack(hdr, sizeof(hdr), NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, convai_proto_audio_hdr_unpack(NULL, 20, NULL, NULL, NULL));
}

/* ================= codec registry tests ================= */

static void test_registry_builtin_codecs(void)
{
    TEST_ASSERT_NOT_NULL(convai_codec_get(CONVAI_CODEC_PCM16));
    TEST_ASSERT_NOT_NULL(convai_codec_get(CONVAI_CODEC_G711A));
    TEST_ASSERT_NOT_NULL(convai_codec_get(CONVAI_CODEC_G711U));
    TEST_ASSERT_NOT_NULL(convai_codec_get(CONVAI_CODEC_IMA_ADPCM));
    TEST_ASSERT_NOT_NULL(convai_codec_get(CONVAI_CODEC_OPUS));
    TEST_ASSERT_NULL(convai_codec_get(CONVAI_CODEC_MAX));
    TEST_ASSERT_EQUAL_size_t(5, convai_codec_count());
}

static void test_registry_by_name_and_iteration(void)
{
    TEST_ASSERT_NOT_NULL(convai_codec_by_name("pcm16"));
    TEST_ASSERT_NOT_NULL(convai_codec_by_name("g711a"));
    TEST_ASSERT_NOT_NULL(convai_codec_by_name("g711u"));
    TEST_ASSERT_NOT_NULL(convai_codec_by_name("ima_adpcm"));
    TEST_ASSERT_NOT_NULL(convai_codec_by_name("opus"));
    TEST_ASSERT_NULL(convai_codec_by_name("bogus"));
    TEST_ASSERT_NULL(convai_codec_by_name(NULL));

    for (size_t i = 0; i < convai_codec_count(); i++) {
        const convai_codec_t *c = convai_codec_at(i);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_NOT_NULL(c->name);
        TEST_ASSERT_NOT_NULL(c->encode);
        TEST_ASSERT_NOT_NULL(c->decode);
    }
    TEST_ASSERT_NULL(convai_codec_at(convai_codec_count()));
}

/* ================= PCM16 codec tests ================= */

static void test_pcm16_passthrough(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_PCM16);
    int16_t in[8] = {0, 1, -1, 1000, -1000, 32767, -32768, 42};
    uint8_t enc[16];
    int16_t dec[8];
    size_t enc_len = 0, dec_n = 0;

    TEST_ASSERT_EQUAL_INT(0, c->encode(NULL, in, 8, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_size_t(16, enc_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(in, enc, 16));

    TEST_ASSERT_EQUAL_INT(0, c->decode(NULL, enc, enc_len, dec, 8, &dec_n));
    TEST_ASSERT_EQUAL_size_t(8, dec_n);
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, dec, 8);

    /* errors */
    TEST_ASSERT_EQUAL_INT(-1, c->encode(NULL, in, 8, enc, 4, &enc_len));
    TEST_ASSERT_EQUAL_INT(-1, c->decode(NULL, enc, 3, dec, 8, &dec_n));
    TEST_ASSERT_EQUAL_INT(-1, c->decode(NULL, enc, 16, dec, 4, &dec_n));
}

/* ================= G.711U codec tests ================= */

static void test_g711u_silence_and_roundtrip(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_G711U);
    int16_t in[4] = {0, 0, 0, 0};
    uint8_t enc[4];
    int16_t dec[4];
    size_t enc_len = 0, dec_n = 0;

    TEST_ASSERT_EQUAL_INT(0, c->encode(NULL, in, 4, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_size_t(4, enc_len);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xFF, enc[i]);   /* u-law zero = 0xFF */
    }

    TEST_ASSERT_EQUAL_INT(0, c->decode(NULL, enc, enc_len, dec, 4, &dec_n));
    TEST_ASSERT_EQUAL_size_t(4, dec_n);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_INT16_WITHIN(4, 0, dec[i]);
    }
}

static void test_g711u_roundtrip(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_G711U);
    const int16_t samples[] = {100, -100, 1000, -1000, 8000, -8000, 30000, -30000};
    const size_t n = sizeof(samples) / sizeof(samples[0]);
    uint8_t enc[16];
    int16_t dec[16];
    size_t enc_len = 0, dec_n = 0;

    TEST_ASSERT_EQUAL_INT(0, c->encode(NULL, samples, n, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_INT(0, c->decode(NULL, enc, enc_len, dec, n, &dec_n));
    TEST_ASSERT_EQUAL_size_t(n, dec_n);

    for (size_t i = 0; i < n; i++) {
        if (samples[i] > 0) TEST_ASSERT_GREATER_THAN_INT16(0, dec[i]);
        if (samples[i] < 0) TEST_ASSERT_LESS_THAN_INT16(0, dec[i]);
        int32_t tol = 200 + abs(samples[i]) / 8;
        TEST_ASSERT_INT32_WITHIN(tol, samples[i], dec[i]);
    }
}

/* ================= IMA-ADPCM codec tests ================= */

static void test_ima_adpcm_compression_ratio(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_IMA_ADPCM);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)c->state_size);

    void *st = calloc(1, c->state_size);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL_INT(0, c->init(st));

    int16_t in[160];   /* 20 ms @ 8 kHz */
    memset(in, 0, sizeof(in));
    uint8_t enc[160];
    size_t enc_len = 0;

    TEST_ASSERT_EQUAL_INT(0, c->encode(st, in, 160, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_size_t(80, enc_len);   /* exactly 4:1 */
    free(st);
}

static void test_ima_adpcm_roundtrip(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_IMA_ADPCM);

    void *enc_st = calloc(1, c->state_size);
    void *dec_st = calloc(1, c->state_size);
    c->init(enc_st);
    c->init(dec_st);

    /* slow sine-ish ramp: ADPCM tracks smooth signals well */
    int16_t in[128];
    for (int i = 0; i < 128; i++) {
        in[i] = (int16_t)(8000 * sin(i * 0.1));
    }
    uint8_t enc[128];
    int16_t dec[256];
    size_t enc_len = 0, dec_n = 0;

    TEST_ASSERT_EQUAL_INT(0, c->encode(enc_st, in, 128, enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_size_t(64, enc_len);
    TEST_ASSERT_EQUAL_INT(0, c->decode(dec_st, enc, enc_len, dec, 256, &dec_n));
    TEST_ASSERT_EQUAL_size_t(128, dec_n);

    /* after initial convergence, error stays bounded (~2 step sizes) */
    for (int i = 16; i < 128; i++) {
        TEST_ASSERT_INT32_WITHIN(3000, in[i], dec[i]);
    }
    free(enc_st);
    free(dec_st);
}

/* ================= dynamic codec switching ================= */

static void test_dynamic_codec_switch(void)
{
    /* Simulate what the engine does on convai_set_codec: swap descriptors
     * and re-init fresh state between frames. */
    const convai_codec_id_e ids[] = {
        CONVAI_CODEC_PCM16, CONVAI_CODEC_G711A,
        CONVAI_CODEC_G711U, CONVAI_CODEC_IMA_ADPCM,
    };
    int16_t tone[160];
    for (int i = 0; i < 160; i++) {
        tone[i] = (int16_t)(12000 * ((i % 8) < 4 ? 1 : -1));
    }

    for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        const convai_codec_t *c = convai_codec_get(ids[k]);
        TEST_ASSERT_NOT_NULL(c);

        void *st = c->state_size ? calloc(1, c->state_size) : NULL;
        if (c->init) TEST_ASSERT_EQUAL_INT(0, c->init(st));

        uint8_t enc[512];
        size_t enc_len = 0;
        TEST_ASSERT_EQUAL_INT(0, c->encode(st, tone, 160, enc, sizeof(enc), &enc_len));
        TEST_ASSERT_GREATER_THAN_size_t(0, enc_len);

        void *dst = c->state_size ? calloc(1, c->state_size) : NULL;
        if (c->init) c->init(dst);
        int16_t dec[512];
        size_t dec_n = 0;
        TEST_ASSERT_EQUAL_INT(0, c->decode(dst, enc, enc_len, dec, 512, &dec_n));
        TEST_ASSERT_GREATER_THAN_size_t(0, dec_n);

        if (c->deinit && st) c->deinit(st);
        if (c->deinit && dst) c->deinit(dst);
        free(st);
        free(dst);
    }
}

/* ================= Opus codec tests ================= */

#define OPUS_FRAME_SAMPLES 320   /* 20 ms @ 16 kHz */

static void test_opus_encode_decode_roundtrip(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_OPUS);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(16000, c->sample_rate);

    void *enc_st = calloc(1, c->state_size);
    void *dec_st = calloc(1, c->state_size);
    TEST_ASSERT_NOT_NULL(enc_st);
    TEST_ASSERT_NOT_NULL(dec_st);
    TEST_ASSERT_EQUAL_INT(0, c->init(enc_st));
    TEST_ASSERT_EQUAL_INT(0, c->init(dec_st));

    /* 1 kHz sine, amplitude 8000 */
    int16_t in[OPUS_FRAME_SAMPLES];
    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        in[i] = (int16_t)(8000 * sin(2.0 * M_PI * 1000 * i / 16000));
    }

    /* stream 10 frames; verify each decodes to the right length, then
     * check energy on the later frames (codec has look-ahead warmup) */
    double e_in = 0, e_dec = 0;
    for (int f = 0; f < 10; f++) {
        uint8_t enc[256];
        size_t enc_len = 0;
        TEST_ASSERT_EQUAL_INT(0, c->encode(enc_st, in, OPUS_FRAME_SAMPLES,
                                           enc, sizeof(enc), &enc_len));
        TEST_ASSERT_GREATER_THAN_size_t(0, enc_len);

        int16_t dec[OPUS_FRAME_SAMPLES * 2];
        size_t dec_n = 0;
        TEST_ASSERT_EQUAL_INT(0, c->decode(dec_st, enc, enc_len, dec,
                                           OPUS_FRAME_SAMPLES * 2, &dec_n));
        TEST_ASSERT_EQUAL_size_t(OPUS_FRAME_SAMPLES, dec_n);

        if (f >= 5) {
            for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
                e_in  += (double)in[i] * in[i];
                e_dec += (double)dec[i] * dec[i];
            }
        }
    }

    /* Opus is lossy: steady-state decoded energy must be close to the
     * original (within 50%..150%) — proves encode->decode worked. */
    printf("[opus] steady-state energy in=%.0f dec=%.0f\n", e_in, e_dec);
    TEST_ASSERT_TRUE(e_dec > e_in * 0.5);
    TEST_ASSERT_TRUE(e_dec < e_in * 1.5);

    if (c->deinit) { c->deinit(enc_st); c->deinit(dec_st); }
    free(enc_st);
    free(dec_st);
}

static void test_opus_packet_sizes(void)
{
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_OPUS);
    void *st = calloc(1, c->state_size);
    c->init(st);

    /* 16 kbps CBR, 20 ms frame -> ~40 bytes per packet */
    int16_t silence[OPUS_FRAME_SAMPLES] = {0};
    uint8_t enc[256];
    size_t enc_len = 0;
    TEST_ASSERT_EQUAL_INT(0, c->encode(st, silence, OPUS_FRAME_SAMPLES,
                                       enc, sizeof(enc), &enc_len));
    TEST_ASSERT_LESS_OR_EQUAL_size_t(60, enc_len);

    if (c->deinit) c->deinit(st);
    free(st);
}

/* ================= memory budget (< 64 KB per direction) ================= */

#define CODEC_MEM_LIMIT (64 * 1024)

static size_t codec_mem(const convai_codec_t *c, void *st)
{
    return c->mem_usage ? c->mem_usage(st) : 0;
}

static void check_codec_memory(convai_codec_id_e id, int sample_rate)
{
    const convai_codec_t *c = convai_codec_get(id);
    TEST_ASSERT_NOT_NULL(c);

    int frame = sample_rate / 50;   /* 20 ms */
    int16_t *tone = malloc(frame * sizeof(int16_t));
    for (int i = 0; i < frame; i++) {
        tone[i] = (int16_t)(8000 * sin(2.0 * M_PI * 440 * i / sample_rate));
    }
    uint8_t *enc = malloc(frame * 2 + 256);
    int16_t *dec = malloc((frame * 2 + 256) * sizeof(int16_t));

    /* ---- encode side: fresh state, one frame, measure ---- */
    void *st = c->state_size ? calloc(1, c->state_size) : NULL;
    if (c->init) TEST_ASSERT_EQUAL_INT(0, c->init(st));
    size_t enc_len = 0;
    TEST_ASSERT_EQUAL_INT(0, c->encode(st, tone, frame, enc, frame * 2 + 256, &enc_len));
    size_t enc_mem = codec_mem(c, st);
    printf("[mem] %-10s encode-side: %u bytes\n", c->name, (unsigned)enc_mem);
    TEST_ASSERT_LESS_THAN_UINT32(CODEC_MEM_LIMIT, enc_mem);
    if (c->deinit && st) c->deinit(st);
    free(st);

    /* ---- decode side: fresh state, one frame, measure ---- */
    st = c->state_size ? calloc(1, c->state_size) : NULL;
    if (c->init) TEST_ASSERT_EQUAL_INT(0, c->init(st));
    size_t dec_n = 0;
    TEST_ASSERT_EQUAL_INT(0, c->decode(st, enc, enc_len, dec, frame * 2 + 256, &dec_n));
    size_t dec_mem = codec_mem(c, st);
    printf("[mem] %-10s decode-side: %u bytes\n", c->name, (unsigned)dec_mem);
    TEST_ASSERT_LESS_THAN_UINT32(CODEC_MEM_LIMIT, dec_mem);
    if (c->deinit && st) c->deinit(st);
    free(st);

    free(tone);
    free(enc);
    free(dec);
}

static void test_memory_pcm16_under_64kb(void)     { check_codec_memory(CONVAI_CODEC_PCM16, 8000); }
static void test_memory_g711a_under_64kb(void)     { check_codec_memory(CONVAI_CODEC_G711A, 8000); }
static void test_memory_g711u_under_64kb(void)     { check_codec_memory(CONVAI_CODEC_G711U, 8000); }
static void test_memory_ima_adpcm_under_64kb(void) { check_codec_memory(CONVAI_CODEC_IMA_ADPCM, 8000); }
static void test_memory_opus_under_64kb(void)      { check_codec_memory(CONVAI_CODEC_OPUS, 16000); }

/* ================= runner ================= */

void app_main(void)
{
    UNITY_BEGIN();

    /* G.711A codec */
    RUN_TEST(test_g711a_encode_silence);
    RUN_TEST(test_g711a_decode_silence_code);
    RUN_TEST(test_g711a_roundtrip);
    RUN_TEST(test_g711a_encode_param_errors);
    RUN_TEST(test_g711a_decode_param_errors);
    RUN_TEST(test_g711a_stereo_planar_layout);

    /* protocol */
    RUN_TEST(test_envelope_build_parse_roundtrip);
    RUN_TEST(test_envelope_null_body_becomes_empty_object);
    RUN_TEST(test_envelope_parse_invalid);
    RUN_TEST(test_envelope_non_object_body_ignored);
    RUN_TEST(test_status_from_str);
    RUN_TEST(test_audio_hdr_roundtrip);
    RUN_TEST(test_audio_hdr_unpack_short_buffer);

    /* codec registry + codecs */
    RUN_TEST(test_registry_builtin_codecs);
    RUN_TEST(test_registry_by_name_and_iteration);
    RUN_TEST(test_pcm16_passthrough);
    RUN_TEST(test_g711u_silence_and_roundtrip);
    RUN_TEST(test_g711u_roundtrip);
    RUN_TEST(test_ima_adpcm_compression_ratio);
    RUN_TEST(test_ima_adpcm_roundtrip);
    RUN_TEST(test_dynamic_codec_switch);

    /* opus */
    RUN_TEST(test_opus_encode_decode_roundtrip);
    RUN_TEST(test_opus_packet_sizes);

    /* memory budget < 64 KB per direction */
    RUN_TEST(test_memory_pcm16_under_64kb);
    RUN_TEST(test_memory_g711a_under_64kb);
    RUN_TEST(test_memory_g711u_under_64kb);
    RUN_TEST(test_memory_ima_adpcm_under_64kb);
    RUN_TEST(test_memory_opus_under_64kb);

    UNITY_END();
}
