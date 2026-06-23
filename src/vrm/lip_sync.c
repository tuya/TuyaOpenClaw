/**
 * @file lip_sync.c
 * @brief Audio-driven lip sync — PCM energy + spectral-split analysis.
 *
 * See lip_sync.h for the algorithm description.
 */

#include "lip_sync.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Tuning constants                                                   */
/* ------------------------------------------------------------------ */

/**
 * Low-pass filter cut-off (Hz).
 * Below this frequency: voiced fundamentals, low formants → open vowels.
 * Above this frequency: upper formants, fricatives → bright / narrow vowels.
 */
#define LP_CUTOFF_HZ     600.0f

/**
 * Energy smoothing time-constant (seconds).
 * Controls how fast the mouth opens/closes in response to audio.
 * Shorter = more responsive; longer = smoother.
 */
#define ENERGY_TAU_S     0.04f      /* ~40 ms */

/**
 * Vowel-weight smoothing time-constant (seconds).
 * A second smoothing stage applied to the individual vowel weights
 * so expression transitions are never jerky.
 */
#define WEIGHT_TAU_S     0.03f      /* ~30 ms */

/**
 * Normalisation target for RMS energy → openness mapping.
 * PCM peak is 32767 (16-bit).  A comfortable speech level is around
 * 20–30 % of full scale, so we normalise against that.
 */
#define ENERGY_NORM      6000.0f    /* RMS at which openness = 1.0 */

/**
 * Minimum energy below which we treat the signal as silence.
 * Prevents tiny noise bursts from animating the mouth.
 */
#define ENERGY_FLOOR     200.0f     /* ≈ 0.6 % of full scale */

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Clamp x to [lo, hi]. */
static inline float __clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/**
 * Exponential-lerp: smoothly approach target.
 *   y[n] = y[n-1] + (1 - exp(-dt/tau)) * (target - y[n-1])
 *
 * For a fixed block size we precompute the factor once in the main loop.
 */
static inline float __exp_lerp(float current, float target, float factor)
{
    return current + factor * (target - current);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void lip_sync_init(lip_sync_ctx_t *ctx, int sample_rate)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->sample_rate = (sample_rate > 0) ? sample_rate : 16000;
}

void lip_sync_feed_pcm(lip_sync_ctx_t *ctx,
                       const void *buf, uint32_t byte_len,
                       int channels, int bits)
{
    if (!ctx || !buf || byte_len == 0) return;

    /* Only 16-bit PCM is handled */
    if (bits != 16) return;

    const int16_t *pcm = (const int16_t *)buf;
    uint32_t total_samples = byte_len / sizeof(int16_t);
    if (total_samples == 0) return;

    /* Number of frames (stereo → 2 samples per frame) */
    uint32_t frames = total_samples / (uint32_t)channels;
    if (frames == 0) return;

    /* ----------------------------------------------------------------
     * 1. Compute RMS energy and IIR low-pass simultaneously.
     *    For stereo we average both channels to form a mono signal.
     * ---------------------------------------------------------------- */

    /*
     * First-order IIR LP:  lp[n] = alpha * x[n] + (1-alpha) * lp[n-1]
     * alpha = 1 - exp(-2π * fc / fs)
     */
    const float alpha =
        1.0f - expf(-6.28318f * LP_CUTOFF_HZ / (float)ctx->sample_rate);

    double sum_sq_total = 0.0;
    double sum_sq_lp    = 0.0;
    float  lp_state     = ctx->lp_state;

    for (uint32_t f = 0; f < frames; f++) {
        /* Downmix to mono */
        float mono;
        if (channels == 1) {
            mono = (float)pcm[f];
        } else {
            mono = ((float)pcm[f * 2] + (float)pcm[f * 2 + 1]) * 0.5f;
        }

        /* IIR LP filter */
        lp_state = alpha * mono + (1.0f - alpha) * lp_state;

        sum_sq_total += (double)mono    * (double)mono;
        sum_sq_lp    += (double)lp_state * (double)lp_state;
    }

    ctx->lp_state = lp_state;

    float rms_total = sqrtf((float)(sum_sq_total / frames));
    float rms_lp    = sqrtf((float)(sum_sq_lp    / frames));

    /* High-band RMS: approximate as sqrt(max(total² - lp², 0)) */
    float rms_sq_hp = rms_total * rms_total - rms_lp * rms_lp;
    float rms_hp    = (rms_sq_hp > 0.0f) ? sqrtf(rms_sq_hp) : 0.0f;

    /* ----------------------------------------------------------------
     * 2. Openness: normalise total RMS, apply floor.
     * ---------------------------------------------------------------- */
    float openness = 0.0f;
    if (rms_total > ENERGY_FLOOR) {
        openness = __clampf(rms_total / ENERGY_NORM, 0.0f, 1.0f);
    }

    /* Smooth energy (exponential approach) */
    float block_dt  = (float)frames / (float)ctx->sample_rate;
    float e_factor  = 1.0f - expf(-block_dt / ENERGY_TAU_S);
    ctx->smooth_energy = __exp_lerp(ctx->smooth_energy, openness, e_factor);
    float eff_open = ctx->smooth_energy;

    /* ----------------------------------------------------------------
     * 3. Spectral ratio: hp_ratio ∈ [0,1]
     *    0 = all energy is low-frequency (voiced, open vowels)
     *    1 = all energy is high-frequency (fricatives, narrow mouth)
     * ---------------------------------------------------------------- */
    float hp_ratio = 0.0f;
    if (rms_total > ENERGY_FLOOR) {
        hp_ratio = __clampf(rms_hp / rms_total, 0.0f, 1.0f);
    }

    /* ----------------------------------------------------------------
     * 4. Compute target vowel weights.
     *    Weights intentionally do NOT sum to 1; they are additive
     *    blending layers on top of the base face pose.
     * ---------------------------------------------------------------- */
    float taa = eff_open * (1.0f - 0.8f * hp_ratio);        /* open, LF */
    float tih = eff_open * 0.6f * hp_ratio;                  /* narrow, HF */
    float tou = eff_open * 0.4f * (1.0f - hp_ratio);        /* round-close */
    float tee = eff_open * 0.4f * hp_ratio;                  /* front-close */
    float toh = eff_open * 0.5f * (1.0f - hp_ratio);        /* round-open */

    /* Clamp all targets */
    ctx->target_aa = __clampf(taa, 0.0f, 1.0f);
    ctx->target_ih = __clampf(tih, 0.0f, 1.0f);
    ctx->target_ou = __clampf(tou, 0.0f, 1.0f);
    ctx->target_ee = __clampf(tee, 0.0f, 1.0f);
    ctx->target_oh = __clampf(toh, 0.0f, 1.0f);

    /* ----------------------------------------------------------------
     * 5. Smooth vowel weights and write to volatile output fields.
     *    The render thread reads these without a lock; on ARM/x86
     *    aligned 32-bit stores are atomic so there is no tearing.
     * ---------------------------------------------------------------- */
    float w_factor = 1.0f - expf(-block_dt / WEIGHT_TAU_S);

    ctx->w_aa = __exp_lerp(ctx->w_aa, ctx->target_aa, w_factor);
    ctx->w_ih = __exp_lerp(ctx->w_ih, ctx->target_ih, w_factor);
    ctx->w_ou = __exp_lerp(ctx->w_ou, ctx->target_ou, w_factor);
    ctx->w_ee = __exp_lerp(ctx->w_ee, ctx->target_ee, w_factor);
    ctx->w_oh = __exp_lerp(ctx->w_oh, ctx->target_oh, w_factor);
}

void lip_sync_get_weights(const lip_sync_ctx_t *ctx,
                          float *aa, float *ih, float *ou,
                          float *ee, float *oh)
{
    if (!ctx) return;
    if (aa) *aa = ctx->w_aa;
    if (ih) *ih = ctx->w_ih;
    if (ou) *ou = ctx->w_ou;
    if (ee) *ee = ctx->w_ee;
    if (oh) *oh = ctx->w_oh;
}

void lip_sync_reset(lip_sync_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->lp_state     = 0.0f;
    ctx->smooth_energy = 0.0f;
    ctx->target_aa = ctx->target_ih = ctx->target_ou =
    ctx->target_ee = ctx->target_oh = 0.0f;
    ctx->w_aa = ctx->w_ih = ctx->w_ou = ctx->w_ee = ctx->w_oh = 0.0f;
}
