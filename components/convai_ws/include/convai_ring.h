/**
 * @file convai_ring.h
 * @brief Message-oriented ring buffer (pure C, host-testable).
 *
 * Stores whole messages (e.g. one decoded audio frame) in a fixed
 * static arena. When full, the OLDEST whole messages are evicted —
 * never partial ones — so stream decoders (ADPCM/Opus) never see a
 * truncated frame sequence. All counters are exposed for watermark
 * observability. No heap allocation, no OS dependencies.
 */
#ifndef CONVAI_RING_H
#define CONVAI_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONVAI_RING_MSG_MAGIC 0xA5

typedef struct {
    uint8_t *buf;        /* caller-provided arena (static) */
    size_t   cap;        /* arena size in bytes */
    size_t   head;       /* read position */
    size_t   tail;       /* write position */
    size_t   used;       /* bytes occupied (headers + payloads) */
    uint32_t drops;      /* whole messages evicted since init */
    uint32_t high_water; /* max 'used' ever reached */
} convai_ring_t;

/** Init the ring over a caller-provided arena. */
void convai_ring_init(convai_ring_t *r, uint8_t *arena, size_t cap);

/**
 * Push one message. Evicts oldest whole messages until it fits.
 * @return 0 on success, -1 if len exceeds arena capacity
 *         (message can never fit; it is dropped and counted)
 */
int convai_ring_push(convai_ring_t *r, const uint8_t *data, uint16_t len);

/**
 * Pop the oldest message.
 * @return 0 on success (*out_len set), 1 if ring empty, -1 if out_cap too small
 */
int convai_ring_pop(convai_ring_t *r, uint8_t *out, uint16_t out_cap, uint16_t *out_len);

/** Current occupancy in bytes (headers included). */
size_t convai_ring_used(const convai_ring_t *r);

/** Messages evicted since init. */
uint32_t convai_ring_drops(const convai_ring_t *r);

/** High-water mark of occupancy in bytes. */
uint32_t convai_ring_high_water(const convai_ring_t *r);

/** Reset to empty (counters preserved). */
void convai_ring_clear(convai_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_RING_H */
