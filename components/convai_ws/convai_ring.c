#include "convai_ring.h"

#include <string.h>

#define HDR_LEN 4   /* magic u8 + reserved u8 + len u16 LE */

static void wr_u8(convai_ring_t *r, size_t *pos, uint8_t v)
{
    r->buf[*pos] = v;
    *pos = (*pos + 1) % r->cap;
}

static uint8_t rd_u8(const convai_ring_t *r, size_t pos)
{
    return r->buf[pos % r->cap];
}

/* Read header at ring head: returns payload length, or 0xFFFF if empty. */
static uint16_t peek_msg_len(const convai_ring_t *r)
{
    if (r->used < HDR_LEN) return 0xFFFF;
    if (rd_u8(r, r->head) != CONVAI_RING_MSG_MAGIC) return 0xFFFE; /* corrupt */
    return (uint16_t)(rd_u8(r, r->head + 2) | (rd_u8(r, r->head + 3) << 8));
}

static void advance(convai_ring_t *r, size_t n)
{
    r->head = (r->head + n) % r->cap;
    r->used -= n;
}

/* Evict the oldest whole message. */
static void evict_oldest(convai_ring_t *r)
{
    uint16_t len = peek_msg_len(r);
    if (len == 0xFFFF || len == 0xFFFE) {
        /* empty or corrupt: reset */
        r->head = r->tail = r->used = 0;
        return;
    }
    advance(r, HDR_LEN + len);
    r->drops++;
}

void convai_ring_init(convai_ring_t *r, uint8_t *arena, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = arena;
    r->cap = cap;
}

int convai_ring_push(convai_ring_t *r, const uint8_t *data, uint16_t len)
{
    size_t need = HDR_LEN + len;
    if (!r || !r->buf || need > r->cap) {
        if (r) r->drops++;
        return -1;
    }
    while (r->used + need > r->cap) {
        evict_oldest(r);
    }
    wr_u8(r, &r->tail, CONVAI_RING_MSG_MAGIC);
    wr_u8(r, &r->tail, 0);
    wr_u8(r, &r->tail, (uint8_t)(len & 0xFF));
    wr_u8(r, &r->tail, (uint8_t)(len >> 8));
    for (uint16_t i = 0; i < len; i++) {
        wr_u8(r, &r->tail, data[i]);
    }
    r->used += need;
    if (r->used > r->high_water) r->high_water = (uint32_t)r->used;
    return 0;
}

int convai_ring_pop(convai_ring_t *r, uint8_t *out, uint16_t out_cap, uint16_t *out_len)
{
    uint16_t len = peek_msg_len(r);
    if (len == 0xFFFF) return 1;            /* empty */
    if (len == 0xFFFE) {                    /* corrupt: reset, treat as empty */
        r->head = r->tail = r->used = 0;
        return 1;
    }
    if (len > out_cap) return -1;

    advance(r, HDR_LEN);
    for (uint16_t i = 0; i < len; i++) {
        out[i] = rd_u8(r, r->head);
        r->head = (r->head + 1) % r->cap;
    }
    r->used -= len;
    *out_len = len;
    return 0;
}

size_t convai_ring_used(const convai_ring_t *r)        { return r->used; }
uint32_t convai_ring_drops(const convai_ring_t *r)     { return r->drops; }
uint32_t convai_ring_high_water(const convai_ring_t *r){ return r->high_water; }

void convai_ring_clear(convai_ring_t *r)
{
    r->head = r->tail = r->used = 0;
}
