/**
 * @file convai_limits.h
 * @brief Static memory sizing constants for the convai_ws component.
 *
 * Single source of truth for the <100KB subsystem budget — shared by
 * the firmware and by the host-side unit tests (budget assertions use
 * exactly these values).
 */
#ifndef CONVAI_LIMITS_H
#define CONVAI_LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- static work buffers ---- */
#define CONVAI_ENC_BUF_BYTES     1024   /* encode output for one 20ms frame */
#define CONVAI_DEC_PCM_SAMPLES   2048   /* decode output cap (samples) */

/* ---- TX (uplink) frame queue ---- */
#ifndef CONFIG_CONVAI_TX_QUEUE_FRAMES
#define CONVAI_TX_QUEUE_FRAMES   16
#else
#define CONVAI_TX_QUEUE_FRAMES   CONFIG_CONVAI_TX_QUEUE_FRAMES
#endif
#define CONVAI_TX_SLOT_BYTES     768    /* max encoded 20ms frame (PCM16 640B) */

/* ---- RX (downlink) jitter ring ---- */
#ifndef CONFIG_CONVAI_RX_RING_KB
#define CONVAI_RX_RING_KB        24
#else
#define CONVAI_RX_RING_KB        CONFIG_CONVAI_RX_RING_KB
#endif
#define CONVAI_RX_RING_BYTES     (CONVAI_RX_RING_KB * 1024)

/* ---- task stacks (bytes) ---- */
#ifndef CONFIG_CONVAI_WS_TASK_STACK
#define CONVAI_WS_TASK_STACK     4096
#else
#define CONVAI_WS_TASK_STACK     CONFIG_CONVAI_WS_TASK_STACK
#endif
#define CONVAI_PUMP_TASK_STACK   3072
#define CONVAI_SEND_TASK_STACK   3072

/* ---- budget accounting estimates (for tests/docs) ---- */
#define CONVAI_STATIC_POOL_BYTES                                          \
    (CONVAI_ENC_BUF_BYTES + CONVAI_DEC_PCM_SAMPLES * 2 +                  \
     CONVAI_TX_QUEUE_FRAMES * (CONVAI_TX_SLOT_BYTES + 4) +                \
     CONVAI_RX_RING_BYTES)

#define CONVAI_TASK_STACK_BYTES                                           \
    (CONVAI_WS_TASK_STACK + CONVAI_PUMP_TASK_STACK + CONVAI_SEND_TASK_STACK)

#define CONVAI_CODEC_PEAK_BYTES  (18 * 1024 + 15 * 1024)  /* opus enc+dec */
#define CONVAI_JSON_WORKSET_BYTES 2048
#define CONVAI_ENGINE_BYTES      1024
#define CONVAI_TLS_TUNED_BYTES   (4 * 1024 * 2 + 4 * 1024) /* in+out+hs */

#define CONVAI_BUDGET_LIMIT      (100 * 1024)

#define CONVAI_SUBTOTAL_WS                                                \
    (CONVAI_STATIC_POOL_BYTES + CONVAI_TASK_STACK_BYTES +                 \
     CONVAI_CODEC_PEAK_BYTES + CONVAI_JSON_WORKSET_BYTES +                \
     CONVAI_ENGINE_BYTES)

#define CONVAI_SUBTOTAL_WSS  (CONVAI_SUBTOTAL_WS + CONVAI_TLS_TUNED_BYTES)

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_LIMITS_H */
