/**
 * @file emotion.c
 * @brief Emotion preset system — smooth expression transitions with procedural
 *        blink for VRM models.
 *
 * Each emotion is a set of VRM expression name→weight bindings.  The system
 * smoothly interpolates from the current weights to the target weights using
 * exponential lerp, and layers a procedural blink on top.
 */

#include "emotion.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

/* ================================================================== */
/*  Built-in presets                                                   */
/* ================================================================== */

/**
 * Preset table — one entry per emotion_id_t.
 *
 * Each preset lists the VRM expression names it drives and the target weight
 * for each.  VRM 1.0 preset expression names:
 *   happy, angry, sad, relaxed, surprised,
 *   aa, ih, ou, ee, oh,
 *   blink, blinkLeft, blinkRight,
 *   lookUp, lookDown, lookLeft, lookRight,
 *   neutral
 *
 * VRM 0.x commonly uses:  joy, angry, sorrow, fun, A, I, U, E, O, Blink
 *
 * We list BOTH 1.0 and 0.x names so the resolver picks whichever exists.
 */

static emotion_preset_t s_presets[EMOTION__COUNT] = {
    /* EMOTION_NEUTRAL */
    {
        .display_name     = "Neutral",
        .bind_count       = 0,
        .transition_speed = 5.0f,
        .binds            = { {{0}} },
    },

    /* EMOTION_HAPPY */
    {
        .display_name     = "Happy",
        .bind_count       = 5,
        .transition_speed = 5.0f,
        .binds = {
            { .expr_name = "happy",   .expr_index = -1, .weight = 1.0f },
            { .expr_name = "joy",     .expr_index = -1, .weight = 1.0f },  /* VRM 0.x */
            { .expr_name = "aa",      .expr_index = -1, .weight = 0.3f },
            { .expr_name = "A",       .expr_index = -1, .weight = 0.3f },  /* VRM 0.x */
            { .expr_name = "a",       .expr_index = -1, .weight = 0.3f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_SAD */
    {
        .display_name     = "Sad",
        .bind_count       = 5,
        .transition_speed = 3.0f,
        .binds = {
            { .expr_name = "sad",     .expr_index = -1, .weight = 1.0f },
            { .expr_name = "sorrow",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x */
            { .expr_name = "ee",      .expr_index = -1, .weight = 0.2f },
            { .expr_name = "E",       .expr_index = -1, .weight = 0.2f },
            { .expr_name = "e",       .expr_index = -1, .weight = 0.2f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_ANGRY */
    {
        .display_name     = "Angry",
        .bind_count       = 6,
        .transition_speed = 6.0f,
        .binds = {
            { .expr_name = "angry",   .expr_index = -1, .weight = 1.0f },
            { .expr_name = "aa",      .expr_index = -1, .weight = 0.4f },
            { .expr_name = "A",       .expr_index = -1, .weight = 0.4f },
            { .expr_name = "a",       .expr_index = -1, .weight = 0.4f },  /* VRM 0.x preset */
            { .expr_name = "ih",      .expr_index = -1, .weight = 0.15f },
            { .expr_name = "i",       .expr_index = -1, .weight = 0.15f }, /* VRM 0.x preset */
        },
    },

    /* EMOTION_SURPRISED */
    {
        .display_name     = "Surprised",
        .bind_count       = 6,
        .transition_speed = 8.0f,
        .binds = {
            { .expr_name = "surprised", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "aa",        .expr_index = -1, .weight = 0.6f },
            { .expr_name = "A",         .expr_index = -1, .weight = 0.6f },
            { .expr_name = "a",         .expr_index = -1, .weight = 0.6f }, /* VRM 0.x preset */
            { .expr_name = "oh",        .expr_index = -1, .weight = 0.5f },
            { .expr_name = "o",         .expr_index = -1, .weight = 0.5f }, /* VRM 0.x preset */
        },
    },

    /* EMOTION_WINK */
    {
        .display_name     = "Wink",
        .bind_count       = 6,
        .transition_speed = 8.0f,
        .binds = {
            { .expr_name = "blinkLeft",  .expr_index = -1, .weight = 1.0f },  /* VRM 1.0 */
            { .expr_name = "blink_l",    .expr_index = -1, .weight = 1.0f },  /* VRM 0.x */
            { .expr_name = "happy",      .expr_index = -1, .weight = 0.4f },
            { .expr_name = "joy",        .expr_index = -1, .weight = 0.4f },  /* VRM 0.x */
            { .expr_name = "aa",         .expr_index = -1, .weight = 0.15f },
            { .expr_name = "a",          .expr_index = -1, .weight = 0.15f }, /* VRM 0.x */
        },
    },

    /* EMOTION_THINKING */
    {
        .display_name     = "Thinking",
        .bind_count       = 4,
        .transition_speed = 4.0f,
        .binds = {
            { .expr_name = "sad",      .expr_index = -1, .weight = 0.15f },
            { .expr_name = "sorrow",   .expr_index = -1, .weight = 0.15f },
            { .expr_name = "lookUp",   .expr_index = -1, .weight = 0.25f },
            { .expr_name = "ou",       .expr_index = -1, .weight = 0.10f },
        },
    },

    /* EMOTION_COOL */
    {
        .display_name     = "Cool",
        .bind_count       = 5,
        .transition_speed = 5.0f,
        .binds = {
            { .expr_name = "happy",     .expr_index = -1, .weight = 0.35f },
            { .expr_name = "joy",       .expr_index = -1, .weight = 0.35f },
            { .expr_name = "blinkRight",.expr_index = -1, .weight = 0.20f },
            { .expr_name = "blink_r",   .expr_index = -1, .weight = 0.20f },
            { .expr_name = "ih",        .expr_index = -1, .weight = 0.08f },
        },
    },

    /* EMOTION_RELAXED */
    {
        .display_name     = "Relaxed",
        .bind_count       = 6,
        .transition_speed = 3.0f,
        .binds = {
            { .expr_name = "relaxed",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "fun",      .expr_index = -1, .weight = 1.0f },
            { .expr_name = "happy",    .expr_index = -1, .weight = 0.15f },
            { .expr_name = "joy",      .expr_index = -1, .weight = 0.15f },
            { .expr_name = "blink",    .expr_index = -1, .weight = 0.35f },
            { .expr_name = "Blink",    .expr_index = -1, .weight = 0.35f },
        },
    },

    /* EMOTION_EMBARRASSED */
    {
        .display_name     = "Embarrassed",
        .bind_count       = 6,
        .transition_speed = 6.0f,
        .binds = {
            { .expr_name = "happy",      .expr_index = -1, .weight = 0.35f },
            { .expr_name = "joy",        .expr_index = -1, .weight = 0.35f },
            { .expr_name = "surprised",  .expr_index = -1, .weight = 0.20f },
            { .expr_name = "sad",        .expr_index = -1, .weight = 0.10f },
            { .expr_name = "blinkRight", .expr_index = -1, .weight = 0.15f },
            { .expr_name = "lookDown",   .expr_index = -1, .weight = 0.20f },
        },
    },

    /* EMOTION_CONFIDENT */
    {
        .display_name     = "Confident",
        .bind_count       = 4,
        .transition_speed = 5.0f,
        .binds = {
            { .expr_name = "happy",  .expr_index = -1, .weight = 0.50f },
            { .expr_name = "joy",    .expr_index = -1, .weight = 0.50f },
            { .expr_name = "aa",     .expr_index = -1, .weight = 0.10f },
            { .expr_name = "A",      .expr_index = -1, .weight = 0.10f },
        },
    },

    /* EMOTION_SLEEP */
    {
        .display_name     = "Sleep",
        .bind_count       = 4,
        .transition_speed = 2.0f,
        .binds = {
            { .expr_name = "blink",    .expr_index = -1, .weight = 0.95f },
            { .expr_name = "Blink",    .expr_index = -1, .weight = 0.95f },
            { .expr_name = "relaxed",  .expr_index = -1, .weight = 0.60f },
            { .expr_name = "fun",      .expr_index = -1, .weight = 0.60f },
        },
    },

    /* EMOTION_SILLY */
    {
        .display_name     = "Silly",
        .bind_count       = 6,
        .transition_speed = 7.0f,
        .binds = {
            { .expr_name = "happy",    .expr_index = -1, .weight = 0.60f },
            { .expr_name = "joy",      .expr_index = -1, .weight = 0.60f },
            { .expr_name = "blinkLeft",.expr_index = -1, .weight = 0.70f },
            { .expr_name = "blink_l",  .expr_index = -1, .weight = 0.70f },
            { .expr_name = "aa",       .expr_index = -1, .weight = 0.30f },
            { .expr_name = "A",        .expr_index = -1, .weight = 0.30f },
        },
    },

    /* EMOTION_CONFUSED */
    {
        .display_name     = "Confused",
        .bind_count       = 6,
        .transition_speed = 5.0f,
        .binds = {
            { .expr_name = "surprised", .expr_index = -1, .weight = 0.30f },
            { .expr_name = "sad",       .expr_index = -1, .weight = 0.25f },
            { .expr_name = "sorrow",    .expr_index = -1, .weight = 0.25f },
            { .expr_name = "lookLeft",  .expr_index = -1, .weight = 0.25f },
            { .expr_name = "oh",        .expr_index = -1, .weight = 0.15f },
            { .expr_name = "O",         .expr_index = -1, .weight = 0.15f },
        },
    },

    /* EMOTION_LOVING */
    {
        .display_name     = "Loving",
        .bind_count       = 6,
        .transition_speed = 4.0f,
        .binds = {
            { .expr_name = "happy",   .expr_index = -1, .weight = 0.80f },
            { .expr_name = "joy",     .expr_index = -1, .weight = 0.80f },
            { .expr_name = "relaxed", .expr_index = -1, .weight = 0.40f },
            { .expr_name = "fun",     .expr_index = -1, .weight = 0.40f },
            { .expr_name = "blink",   .expr_index = -1, .weight = 0.15f },
            { .expr_name = "Blink",   .expr_index = -1, .weight = 0.15f },
        },
    },

    /* EMOTION_LAUGHING */
    {
        .display_name     = "Laughing",
        .bind_count       = 4,
        .transition_speed = 7.0f,
        .binds = {
            { .expr_name = "happy",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "joy",    .expr_index = -1, .weight = 1.0f },
            { .expr_name = "aa",     .expr_index = -1, .weight = 0.70f },
            { .expr_name = "A",      .expr_index = -1, .weight = 0.70f },
        },
    },

    /* EMOTION_SHOCKED */
    {
        .display_name     = "Shocked",
        .bind_count       = 4,
        .transition_speed = 10.0f,
        .binds = {
            { .expr_name = "surprised", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "aa",        .expr_index = -1, .weight = 0.50f },
            { .expr_name = "A",         .expr_index = -1, .weight = 0.50f },
            { .expr_name = "lookUp",    .expr_index = -1, .weight = 0.15f },
        },
    },

    /* EMOTION_FEARFUL */
    {
        .display_name     = "Fearful",
        .bind_count       = 4,
        .transition_speed = 7.0f,
        .binds = {
            { .expr_name = "sad",       .expr_index = -1, .weight = 0.50f },
            { .expr_name = "sorrow",    .expr_index = -1, .weight = 0.50f },
            { .expr_name = "surprised", .expr_index = -1, .weight = 0.50f },
            { .expr_name = "oh",        .expr_index = -1, .weight = 0.20f },
        },
    },

    /* EMOTION_KISSY */
    {
        .display_name     = "Kissy",
        .bind_count       = 6,
        .transition_speed = 6.0f,
        .binds = {
            { .expr_name = "ou",    .expr_index = -1, .weight = 0.80f },
            { .expr_name = "U",     .expr_index = -1, .weight = 0.80f },
            { .expr_name = "happy", .expr_index = -1, .weight = 0.20f },
            { .expr_name = "joy",   .expr_index = -1, .weight = 0.20f },
            { .expr_name = "blink", .expr_index = -1, .weight = 0.20f },
            { .expr_name = "Blink", .expr_index = -1, .weight = 0.20f },
        },
    },

    /* EMOTION_DELICIOUS */
    {
        .display_name     = "Delicious",
        .bind_count       = 8,
        .transition_speed = 5.0f,
        .binds = {
            { .expr_name = "happy",   .expr_index = -1, .weight = 0.60f },
            { .expr_name = "joy",     .expr_index = -1, .weight = 0.60f },
            { .expr_name = "relaxed", .expr_index = -1, .weight = 0.30f },
            { .expr_name = "fun",     .expr_index = -1, .weight = 0.30f },
            { .expr_name = "aa",      .expr_index = -1, .weight = 0.20f },
            { .expr_name = "A",       .expr_index = -1, .weight = 0.20f },
            { .expr_name = "blink",   .expr_index = -1, .weight = 0.20f },
            { .expr_name = "Blink",   .expr_index = -1, .weight = 0.20f },
        },
    },

    /* EMOTION_LIP_AA */
    {
        .display_name     = "Lip: AA",
        .bind_count       = 3,
        .transition_speed = 12.0f,
        .binds = {
            { .expr_name = "aa", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "A",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "a",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_LIP_IH */
    {
        .display_name     = "Lip: IH",
        .bind_count       = 3,
        .transition_speed = 12.0f,
        .binds = {
            { .expr_name = "ih", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "I",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "i",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_LIP_OU */
    {
        .display_name     = "Lip: OU",
        .bind_count       = 3,
        .transition_speed = 12.0f,
        .binds = {
            { .expr_name = "ou", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "U",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "u",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_LIP_EE */
    {
        .display_name     = "Lip: EE",
        .bind_count       = 3,
        .transition_speed = 12.0f,
        .binds = {
            { .expr_name = "ee", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "E",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "e",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x preset */
        },
    },

    /* EMOTION_LIP_OH */
    {
        .display_name     = "Lip: OH",
        .bind_count       = 3,
        .transition_speed = 12.0f,
        .binds = {
            { .expr_name = "oh", .expr_index = -1, .weight = 1.0f },
            { .expr_name = "O",  .expr_index = -1, .weight = 1.0f },
            { .expr_name = "o",  .expr_index = -1, .weight = 1.0f },  /* VRM 0.x preset */
        },
    },
};

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

/** Resolve all expression names in presets against the model. */
static void __resolve_preset_indices(vrm_model_t *model)
{
    for (int p = 0; p < EMOTION__COUNT; p++) {
        emotion_preset_t *preset = &s_presets[p];
        for (int b = 0; b < preset->bind_count; b++) {
            preset->binds[b].expr_index =
                vrm_find_expression(model, preset->binds[b].expr_name);
        }
    }
}

/** Exponential lerp: smoothly approach target. */
static inline float __exp_lerp(float current, float target, float speed, float dt)
{
    float t = 1.0f - expf(-speed * dt);
    return current + (target - current) * t;
}

#define BLINK_INTERVAL_MIN      2.0f
#define BLINK_INTERVAL_MAX      6.0f
#define BLINK_INTERVAL_SPEAK_MIN 1.5f
#define BLINK_INTERVAL_SPEAK_MAX 3.5f
#define BLINK_DOUBLE_CHANCE     0.15f
#define BLINK_DOUBLE_GAP        0.12f

/**
 * @brief Pick a random blink interval.
 * @param[in] speaking  Non-zero when the avatar is speaking (blinks faster).
 * @return Interval in seconds until the next blink.
 */
static float __rand_blink_interval(int speaking)
{
    float lo = speaking ? BLINK_INTERVAL_SPEAK_MIN : BLINK_INTERVAL_MIN;
    float hi = speaking ? BLINK_INTERVAL_SPEAK_MAX : BLINK_INTERVAL_MAX;
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void emotion_init(emotion_ctx_t *ctx, vrm_model_t *model)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->model            = model;
    ctx->current_emotion  = EMOTION_NEUTRAL;
    ctx->previous_emotion = EMOTION_NEUTRAL;
    ctx->blend_t          = 1.0f; /* already at target */
    ctx->transition_speed = s_presets[EMOTION_NEUTRAL].transition_speed;

    /* Procedural blink defaults */
    ctx->auto_blink       = 1;
    ctx->blink_timer      = 0.0f;
    ctx->blink_interval   = __rand_blink_interval(0);
    ctx->blink_weight     = 0.0f;
    ctx->blink_expr_index = -1;
    ctx->blink_double     = 0;
    ctx->blink_double_gap = BLINK_DOUBLE_GAP;

    ctx->total_time = 0.0f;

    ctx->intensity = 1.0f;
    memset(ctx->smooth_weights, 0, sizeof(ctx->smooth_weights));
    memset(ctx->override_active, 0, sizeof(ctx->override_active));
    ctx->has_overrides = 0;

    /* Resolve expression names against the loaded model */
    __resolve_preset_indices(model);

    /* Find blink expression — try multiple name variants */
    ctx->blink_expr_index = vrm_find_expression(model, "blink");
    if (ctx->blink_expr_index < 0)
        ctx->blink_expr_index = vrm_find_expression(model, "Blink");
    if (ctx->blink_expr_index < 0)
        ctx->blink_expr_index = vrm_find_expression(model, "blink_l");

    /* Log resolved presets */
    printf("[emotion] initialized — %u model expressions available\n", model->expression_count);
    for (int p = 0; p < EMOTION__COUNT; p++) {
        emotion_preset_t *preset = &s_presets[p];
        int resolved = 0;
        for (int b = 0; b < preset->bind_count; b++)
            if (preset->binds[b].expr_index >= 0) resolved++;
        if (preset->bind_count > 0)
            printf("[emotion]   %-12s : %d/%d binds resolved\n",
                   preset->display_name, resolved, preset->bind_count);
    }
    if (ctx->blink_expr_index >= 0)
        printf("[emotion]   auto-blink: expr[%d]\n", ctx->blink_expr_index);
    else
        printf("[emotion]   auto-blink: no 'blink' expression found\n");

    /* Resolve lip-sync expression indices */
    ctx->speaking    = 0;
    ctx->speak_timer = 0.0f;
    ctx->speak_phase = 0.0f;

    /* aa */
    ctx->lip_expr_aa = vrm_find_expression(model, "aa");
    if (ctx->lip_expr_aa < 0) ctx->lip_expr_aa = vrm_find_expression(model, "A");
    if (ctx->lip_expr_aa < 0) ctx->lip_expr_aa = vrm_find_expression(model, "a");
    /* ih */
    ctx->lip_expr_ih = vrm_find_expression(model, "ih");
    if (ctx->lip_expr_ih < 0) ctx->lip_expr_ih = vrm_find_expression(model, "I");
    if (ctx->lip_expr_ih < 0) ctx->lip_expr_ih = vrm_find_expression(model, "i");
    /* ou */
    ctx->lip_expr_ou = vrm_find_expression(model, "ou");
    if (ctx->lip_expr_ou < 0) ctx->lip_expr_ou = vrm_find_expression(model, "U");
    if (ctx->lip_expr_ou < 0) ctx->lip_expr_ou = vrm_find_expression(model, "u");
    /* ee */
    ctx->lip_expr_ee = vrm_find_expression(model, "ee");
    if (ctx->lip_expr_ee < 0) ctx->lip_expr_ee = vrm_find_expression(model, "E");
    if (ctx->lip_expr_ee < 0) ctx->lip_expr_ee = vrm_find_expression(model, "e");
    /* oh */
    ctx->lip_expr_oh = vrm_find_expression(model, "oh");
    if (ctx->lip_expr_oh < 0) ctx->lip_expr_oh = vrm_find_expression(model, "O");
    if (ctx->lip_expr_oh < 0) ctx->lip_expr_oh = vrm_find_expression(model, "o");

    printf("[emotion]   lip-sync: aa=%d ih=%d ou=%d ee=%d oh=%d\n",
           ctx->lip_expr_aa, ctx->lip_expr_ih, ctx->lip_expr_ou,
           ctx->lip_expr_ee, ctx->lip_expr_oh);
}

void emotion_set(emotion_ctx_t *ctx, emotion_id_t id)
{
    emotion_set_ex(ctx, id, 0.0f);
}

void emotion_set_ex(emotion_ctx_t *ctx, emotion_id_t id, float speed)
{
    if ((int)id < 0 || (int)id >= EMOTION__COUNT) return;
    if (id == ctx->current_emotion) return;

    ctx->previous_emotion = ctx->current_emotion;
    ctx->current_emotion  = id;
    ctx->blend_t          = 0.0f;
    ctx->transition_speed = (speed > 0.0f) ? speed
                                            : s_presets[id].transition_speed;

}

void emotion_update(emotion_ctx_t *ctx, float dt)
{
    if (!ctx || !ctx->model || dt <= 0.0f) return;

    ctx->total_time += dt;

    /* ---- 1. Compute target weights from current emotion preset ---- */
    float target_weights[VRM_MAX_EXPRESSIONS];
    memset(target_weights, 0, sizeof(target_weights));

    const emotion_preset_t *preset = &s_presets[ctx->current_emotion];
    for (int b = 0; b < preset->bind_count; b++) {
        int idx = preset->binds[b].expr_index;
        if (idx >= 0 && (uint32_t)idx < ctx->model->expression_count) {
            /* Take the maximum if multiple binds map to the same expression
             * (can happen when we list both VRM 0.x and 1.0 names) */
            if (preset->binds[b].weight > target_weights[idx])
                target_weights[idx] = preset->binds[b].weight;
        }
    }

    /* ---- 1b. Apply intensity scaling ---- */
    for (uint32_t i = 0; i < ctx->model->expression_count; i++) {
        target_weights[i] *= ctx->intensity;
    }

    /* ---- 1c. Apply direct BlendShape overrides from AI ---- */
    if (ctx->has_overrides) {
        for (uint32_t i = 0; i < ctx->model->expression_count; i++) {
            if (ctx->override_active[i]) {
                target_weights[i] = ctx->override_weights[i];
            }
        }
    }

    /* ---- 2. Smooth interpolation toward target ---- */
    float speed = ctx->transition_speed;
    for (uint32_t i = 0; i < ctx->model->expression_count; i++) {
        ctx->smooth_weights[i] = __exp_lerp(ctx->smooth_weights[i],
                                             target_weights[i], speed, dt);
        /* Snap to zero / one when very close */
        if (ctx->smooth_weights[i] < 0.001f) ctx->smooth_weights[i] = 0.0f;
        if (ctx->smooth_weights[i] > 0.999f) ctx->smooth_weights[i] = 1.0f;
    }

    /* Update blend_t for external queries */
    ctx->blend_t += speed * dt;
    if (ctx->blend_t > 1.0f) ctx->blend_t = 1.0f;

    /* ---- 3. Procedural blink (randomized, 2-6s idle / 1.5-3.5s speaking) ---- */
    if (ctx->auto_blink && ctx->blink_expr_index >= 0) {
        int emotion_uses_eyes = 0;
        {
            static const char *eye_names[] = {
                "blink", "Blink", "blinkLeft", "blinkRight",
                "blink_l", "blink_r", "wink", NULL
            };
            for (int b = 0; b < preset->bind_count; b++) {
                for (int e = 0; eye_names[e]; e++) {
                    if (strcmp(preset->binds[b].expr_name, eye_names[e]) == 0 &&
                        preset->binds[b].weight > 0.01f) {
                        emotion_uses_eyes = 1;
                        break;
                    }
                }
                if (emotion_uses_eyes) break;
            }
        }

        if (!emotion_uses_eyes) {
            ctx->blink_timer += dt;

            /*
             * Blink waveform: close -> hold -> open
             *   close: 0.06s   hold: 0.06s   open: 0.08s  = 0.20s total
             */
            float t_close   = 0.06f;
            float t_hold    = 0.06f;
            float t_open    = 0.08f;
            float blink_dur = t_close + t_hold + t_open;

            if (ctx->blink_timer < blink_dur) {
                float t = ctx->blink_timer;
                if (t < t_close) {
                    ctx->blink_weight = t / t_close;
                } else if (t < t_close + t_hold) {
                    ctx->blink_weight = 1.0f;
                } else {
                    ctx->blink_weight = 1.0f - (t - t_close - t_hold) / t_open;
                }
            } else {
                ctx->blink_weight = 0.0f;

                float wait = (ctx->blink_double > 0)
                           ? ctx->blink_double_gap
                           : ctx->blink_interval;

                if (ctx->blink_timer >= blink_dur + wait) {
                    ctx->blink_timer = 0.0f;

                    if (ctx->blink_double > 0) {
                        ctx->blink_double--;
                    } else {
                        ctx->blink_interval = __rand_blink_interval(ctx->speaking);
                        if ((float)rand() / (float)RAND_MAX < BLINK_DOUBLE_CHANCE) {
                            ctx->blink_double = 1;
                        }
                    }
                }
            }

            int bi = ctx->blink_expr_index;
            if (ctx->blink_weight > ctx->smooth_weights[bi]) {
                ctx->smooth_weights[bi] = ctx->blink_weight;
            }
        } else {
            ctx->blink_weight = 0.0f;
        }
    }

    /* ---- 4. Speaking overlay — audio-driven or procedural fallback ---- */
    if (ctx->speaking) {
        float w_aa = 0.0f, w_ih = 0.0f, w_ou = 0.0f, w_ee = 0.0f, w_oh = 0.0f;

        if (ctx->lip_sync) {
            /* --- Audio-driven path: read weights from PCM energy analysis --- */
            lip_sync_get_weights(ctx->lip_sync,
                                 &w_aa, &w_ih, &w_ou, &w_ee, &w_oh);
        } else {
            /* --- Procedural fallback: cycle A→I→U→E→O (no audio context) --- */
            ctx->speak_timer += dt;

            const float vowel_dur = 0.12f;
            const float cycle_dur = vowel_dur * 5.0f;
            const float pause_dur = 0.15f;
            const float total_dur = cycle_dur + pause_dur;
            float phase = fmodf(ctx->speak_timer, total_dur);

            float w[5] = {0};
            if (phase < cycle_dur) {
                float slot_f = phase / vowel_dur;
                int   slot   = (int)slot_f;
                float frac   = slot_f - (float)slot;
                if (slot > 4) slot = 4;
                w[slot] = sinf(3.14159265f * frac);
                if (frac > 0.5f && slot < 4) {
                    float rise = (frac - 0.5f) * 2.0f;
                    w[slot + 1] = sinf(3.14159265f * 0.5f * rise);
                }
            }
            w_aa = w[0]; w_ih = w[1]; w_ou = w[2]; w_ee = w[4]; w_oh = w[3];
        }

        /* Layer vowel weights on top of smooth_weights */
        int lip_idx[5] = {
            ctx->lip_expr_aa, ctx->lip_expr_ih, ctx->lip_expr_ou,
            ctx->lip_expr_ee, ctx->lip_expr_oh
        };
        float vowel_w[5] = { w_aa, w_ih, w_ou, w_ee, w_oh };
        for (int v = 0; v < 5; v++) {
            if (lip_idx[v] >= 0 && vowel_w[v] > ctx->smooth_weights[lip_idx[v]])
                ctx->smooth_weights[lip_idx[v]] = vowel_w[v];
        }
    }

    /* ---- 5. Write final weights to model ---- */
    for (uint32_t i = 0; i < ctx->model->expression_count; i++) {
        ctx->model->expression_weights[i] = ctx->smooth_weights[i];
    }
}

void emotion_set_speaking(emotion_ctx_t *ctx, int speaking)
{
    if (!ctx) return;
    if (ctx->speaking == speaking) return;
    ctx->speaking = speaking;
    if (speaking) {
        ctx->speak_timer = 0.0f;
        if (ctx->lip_sync)
            lip_sync_reset(ctx->lip_sync);
    }
}

void emotion_set_lip_sync(emotion_ctx_t *ctx, lip_sync_ctx_t *ls)
{
    if (!ctx) return;
    ctx->lip_sync = ls;
}

/**
 * @brief Case-insensitive string comparison helper.
 * @return 0 if equal
 */
static int __strcasecmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/** Name → emotion_id_t mapping table. */
static const struct {
    const char  *name;
    emotion_id_t id;
} s_emotion_name_map[] = {
    { "neutral",     EMOTION_NEUTRAL },
    { "happy",       EMOTION_HAPPY },
    { "sad",         EMOTION_SAD },
    { "angry",       EMOTION_ANGRY },
    { "surprised",   EMOTION_SURPRISED },
    { "surprise",    EMOTION_SURPRISED },
    { "wink",        EMOTION_WINK },
    { "thinking",    EMOTION_THINKING },
    { "cool",        EMOTION_COOL },
    { "relaxed",     EMOTION_RELAXED },
    { "embarrassed", EMOTION_EMBARRASSED },
    { "confident",   EMOTION_CONFIDENT },
    { "sleep",       EMOTION_SLEEP },
    { "sleepy",      EMOTION_SLEEP },
    { "silly",       EMOTION_SILLY },
    { "confused",    EMOTION_CONFUSED },
    { "loving",      EMOTION_LOVING },
    { "love",        EMOTION_LOVING },
    { "laughing",    EMOTION_LAUGHING },
    { "laugh",       EMOTION_LAUGHING },
    { "shocked",     EMOTION_SHOCKED },
    { "shock",       EMOTION_SHOCKED },
    { "fearful",     EMOTION_FEARFUL },
    { "fear",        EMOTION_FEARFUL },
    { "scared",      EMOTION_FEARFUL },
    { "kissy",       EMOTION_KISSY },
    { "kiss",        EMOTION_KISSY },
    { "delicious",   EMOTION_DELICIOUS },
    { "yummy",       EMOTION_DELICIOUS },
    { NULL,          EMOTION_NEUTRAL },
};

int emotion_find_by_name(const char *name)
{
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; s_emotion_name_map[i].name; i++) {
        if (__strcasecmp(name, s_emotion_name_map[i].name) == 0) {
            return (int)s_emotion_name_map[i].id;
        }
    }
    return -1;
}

void emotion_set_intensity(emotion_ctx_t *ctx, float intensity)
{
    if (!ctx) return;
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    ctx->intensity = intensity;
}

void emotion_set_override(emotion_ctx_t *ctx, int expr_index, float weight)
{
    if (!ctx) return;
    if (expr_index < 0 || expr_index >= VRM_MAX_EXPRESSIONS) return;
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 1.0f) weight = 1.0f;
    ctx->override_weights[expr_index] = weight;
    ctx->override_active[expr_index]  = 1;
    ctx->has_overrides = 1;
}

void emotion_clear_overrides(emotion_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx->override_active, 0, sizeof(ctx->override_active));
    ctx->has_overrides = 0;
}

const char *emotion_name(emotion_id_t id)
{
    if ((int)id < 0 || (int)id >= EMOTION__COUNT) return "Unknown";
    return s_presets[id].display_name;
}

const emotion_preset_t *emotion_get_preset(emotion_id_t id)
{
    if ((int)id < 0 || (int)id >= EMOTION__COUNT) return NULL;
    return &s_presets[id];
}

void emotion_shutdown(emotion_ctx_t *ctx)
{
    if (!ctx) return;
    printf("[emotion] shutdown\n");
    memset(ctx, 0, sizeof(*ctx));
}
