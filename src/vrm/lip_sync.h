/**
 * @file lip_sync.h
 * @brief Audio-driven lip sync: feeds decoded PCM samples and produces per-frame
 *        vowel weights (aa / ih / ou / ee / oh) for VRM expression blending.
 *
 * Thread model:
 *   - Producer (audio output thread): calls lip_sync_feed_pcm()
 *   - Consumer (render/main thread):  calls lip_sync_get_weights()
 *
 * Shared weights are stored as volatile float so the compiler never caches them
 * in registers across threads.  On ARM/x86 a naturally-aligned 32-bit store is
 * atomic at the hardware level, so no mutex is required for this use-case.
 *
 * Algorithm (per ~10 ms block):
 *   1. Compute RMS energy  →  overall mouth openness
 *   2. First-order IIR low-pass (fc ≈ 600 Hz)  →  low-band energy
 *   3. High-band energy = total − low-band
 *   4. hp_ratio = E_high / E_total  (0 = voiced/open, 1 = fricative/bright)
 *   5. Map to vowel weights:
 *        aa = openness × (1 − 0.8 × hp_ratio)   — open, low-frequency
 *        ih = openness × 0.6 × hp_ratio          — narrow, bright
 *        ou = openness × 0.4 × (1 − hp_ratio)   — rounded-close
 *        ee = openness × 0.4 × hp_ratio          — front-close
 *        oh = openness × 0.5 × (1 − hp_ratio)   — rounded-open
 *   6. Exponential-lerp smooth toward target weights (~40 ms τ)
 */

#ifndef LIP_SYNC_H
#define LIP_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Context                                                            */
/* ================================================================== */

typedef struct {
    /* ---- IIR filter state (audio thread) ---- */
    float lp_state;         /**< First-order LP filter accumulator */

    /* ---- Smoothed energy (audio thread) ---- */
    float smooth_energy;    /**< Exponential-lerp of RMS energy [0,1] */

    /* ---- Per-block raw targets (audio thread, written) ---- */
    float target_aa;
    float target_ih;
    float target_ou;
    float target_ee;
    float target_oh;

    /* ---- Smoothed vowel weights (render thread, read) ---- */
    volatile float w_aa;    /**< Mouth open wide — あ  [0,1] */
    volatile float w_ih;    /**< Mouth narrow    — い  [0,1] */
    volatile float w_ou;    /**< Mouth round     — う  [0,1] */
    volatile float w_ee;    /**< Mouth half-open — え  [0,1] */
    volatile float w_oh;    /**< Mouth round-open— お  [0,1] */

    /* ---- Configuration ---- */
    int sample_rate;        /**< Samples per second (default 16000) */
} lip_sync_ctx_t;

/* ================================================================== */
/*  API                                                                */
/* ================================================================== */

/**
 * Initialise the context. Call once before feeding audio.
 * @param ctx         Context to initialise.
 * @param sample_rate PCM sample rate in Hz (typically 16000).
 */
void lip_sync_init(lip_sync_ctx_t *ctx, int sample_rate);

/**
 * Feed a block of decoded PCM to update vowel weights.
 * Call from the audio output (consumer) thread.
 *
 * @param ctx      Lip sync context.
 * @param buf      Pointer to raw PCM bytes.
 * @param byte_len Number of bytes in buf.
 * @param channels 1 = mono, 2 = stereo (stereo is downmixed by averaging).
 * @param bits     Bits per sample (only 16 is supported; others are ignored).
 */
void lip_sync_feed_pcm(lip_sync_ctx_t *ctx,
                       const void *buf, uint32_t byte_len,
                       int channels, int bits);

/**
 * Read current vowel weights. Call from the render thread.
 * All output pointers are optional (may be NULL).
 */
void lip_sync_get_weights(const lip_sync_ctx_t *ctx,
                          float *aa, float *ih, float *ou,
                          float *ee, float *oh);

/**
 * Reset all weights to zero (e.g., when speaking stops).
 */
void lip_sync_reset(lip_sync_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIP_SYNC_H */
