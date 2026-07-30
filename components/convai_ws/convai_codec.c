/**
 * @file convai_codec.c
 * @brief Codec registry for the pluggable audio codec layer.
 */
#include "sdkconfig.h"

#include <string.h>
#include "convai_codec.h"

extern const convai_codec_t *convai_codec_pcm16(void);
extern const convai_codec_t *convai_codec_g711a(void);
extern const convai_codec_t *convai_codec_g711u(void);
extern const convai_codec_t *convai_codec_ima_adpcm(void);
#ifdef CONFIG_CONVAI_ENABLE_OPUS
extern const convai_codec_t *convai_codec_opus(void);
#endif

const convai_codec_t *convai_codec_get(convai_codec_id_e id)
{
    switch (id) {
    case CONVAI_CODEC_PCM16:     return convai_codec_pcm16();
    case CONVAI_CODEC_G711A:     return convai_codec_g711a();
    case CONVAI_CODEC_G711U:     return convai_codec_g711u();
    case CONVAI_CODEC_IMA_ADPCM: return convai_codec_ima_adpcm();
#ifdef CONFIG_CONVAI_ENABLE_OPUS
    case CONVAI_CODEC_OPUS:      return convai_codec_opus();
#endif
    default:                     return NULL;
    }
}

size_t convai_codec_count(void)
{
    size_t n = 0;
    for (int i = 0; i < CONVAI_CODEC_MAX; i++) {
        if (convai_codec_get((convai_codec_id_e)i)) n++;
    }
    return n;
}

const convai_codec_t *convai_codec_at(size_t index)
{
    size_t n = 0;
    for (int i = 0; i < CONVAI_CODEC_MAX; i++) {
        const convai_codec_t *c = convai_codec_get((convai_codec_id_e)i);
        if (c && n++ == index) return c;
    }
    return NULL;
}

const convai_codec_t *convai_codec_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < convai_codec_count(); i++) {
        const convai_codec_t *c = convai_codec_at(i);
        if (c && !strcmp(c->name, name)) return c;
    }
    return NULL;
}
