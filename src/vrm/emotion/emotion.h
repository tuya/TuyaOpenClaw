/**
 * @file emotion.h
 * @brief Emotion system for VRM models — predefined expression presets with
 *        smooth transitions, procedural blink, and lip-sync placeholders.
 *
 * Usage:
 *   1. emotion_init(&ctx, &model)        — after model is loaded
 *   2. emotion_set(&ctx, EMOTION_HAPPY)  — trigger an emotion
 *   3. emotion_update(&ctx, dt)          — call each frame
 *   4. emotion_shutdown(&ctx)            — cleanup
 *
 * The system drives model->expression_weights[] via smooth lerp transitions
 * and layers procedural blink on top.
 */

#ifndef EMOTION_H
#define EMOTION_H

#include "vrm_loader.h"
#include "lip_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Emotion IDs                                                        */
/* ================================================================== */

typedef enum {
    EMOTION_NEUTRAL = 0,   /**< Rest face — all expressions zero */
    EMOTION_HAPPY,         /**< Joy / smile */
    EMOTION_SAD,           /**< Sadness */
    EMOTION_ANGRY,         /**< Anger */
    EMOTION_SURPRISED,     /**< Surprise / shock */
    EMOTION_WINK,          /**< Wink — one eye closed */

    /* Extended emotions for AI-driven control */
    EMOTION_THINKING,      /**< Contemplative / pondering */
    EMOTION_COOL,          /**< Casual confidence */
    EMOTION_RELAXED,       /**< Calm and at ease */
    EMOTION_EMBARRASSED,   /**< Flustered / bashful */
    EMOTION_CONFIDENT,     /**< Self-assured smile */
    EMOTION_SLEEP,         /**< Drowsy / sleeping */
    EMOTION_SILLY,         /**< Playful / goofy */
    EMOTION_CONFUSED,      /**< Bewildered / uncertain */
    EMOTION_LOVING,        /**< Affectionate / adoring */
    EMOTION_LAUGHING,      /**< Open-mouth laughing */
    EMOTION_SHOCKED,       /**< Shock / disbelief */
    EMOTION_FEARFUL,       /**< Scared / anxious */
    EMOTION_KISSY,         /**< Blowing a kiss */
    EMOTION_DELICIOUS,     /**< Savoring / yummy */

    /* Lip-sync vowels (Japanese あいうえお) */
    EMOTION_LIP_AA,        /**< Mouth open wide — あ */
    EMOTION_LIP_IH,        /**< Mouth narrow — い */
    EMOTION_LIP_OU,        /**< Mouth round — う */
    EMOTION_LIP_EE,        /**< Mouth half-open — え */
    EMOTION_LIP_OH,        /**< Mouth round-open — お */

    EMOTION__COUNT
} emotion_id_t;

/* ================================================================== */
/*  Configuration                                                      */
/* ================================================================== */

/** Maximum number of VRM expressions that a single emotion can blend. */
#define EMOTION_MAX_BINDS  8

/** Maximum number of direct BlendShape overrides from AI. */
#define EMOTION_MAX_OVERRIDES 16

/** A single binding: "set VRM expression <name> to <weight>". */
typedef struct {
    char   expr_name[64];   /**< VRM expression name (e.g. "happy", "blink") */
    int    expr_index;      /**< Resolved index into model->expressions, or -1 */
    float  weight;          /**< Target weight [0, 1] */
} emotion_bind_t;

/** Definition of one emotion preset. */
typedef struct {
    const char    *display_name;              /**< Human-readable name */
    emotion_bind_t binds[EMOTION_MAX_BINDS];  /**< Expression bindings */
    int            bind_count;                /**< Number of active bindings */
    float          transition_speed;          /**< Lerp speed (higher = faster) */
} emotion_preset_t;

/* ================================================================== */
/*  Runtime context                                                    */
/* ================================================================== */

typedef struct {
    vrm_model_t    *model;              /**< Pointer to VRM model (not owned) */

    /* Current state */
    emotion_id_t    current_emotion;    /**< Active emotion ID */
    emotion_id_t    previous_emotion;   /**< Previous emotion (for blending) */
    float           blend_t;            /**< Transition progress [0, 1] */
    float           transition_speed;   /**< Current transition speed */

    /* Intensity multiplier for the active emotion [0, 1] */
    float           intensity;

    /* Per-expression current weights (smooth) */
    float           smooth_weights[VRM_MAX_EXPRESSIONS];

    /* Direct BlendShape overrides from AI (applied on top of emotion targets) */
    float           override_weights[VRM_MAX_EXPRESSIONS];
    uint8_t         override_active[VRM_MAX_EXPRESSIONS];
    int             has_overrides;

    /* Procedural blink */
    int             auto_blink;         /**< Enable procedural blink (default: 1) */
    float           blink_timer;        /**< Time since last blink started */
    float           blink_interval;     /**< Current interval until next blink */
    float           blink_weight;       /**< Current blink weight [0, 1] */
    int             blink_expr_index;   /**< Resolved "blink" expression index */
    int             blink_double;       /**< >0: remaining double-blink count */
    float           blink_double_gap;   /**< Short gap between double-blinks */

    /* Speaking (lip-sync) */
    int             speaking;           /**< 1 = speaking active */
    float           speak_timer;        /**< Accumulated speaking time (used for fallback only) */
    float           speak_phase;        /**< Vowel phase (fallback only) */
    int             lip_expr_aa;        /**< Resolved expr index for aa/a */
    int             lip_expr_ih;        /**< Resolved expr index for ih/i */
    int             lip_expr_ou;        /**< Resolved expr index for ou/u */
    int             lip_expr_ee;        /**< Resolved expr index for ee/e */
    int             lip_expr_oh;        /**< Resolved expr index for oh/o */

    /** Optional audio-driven lip sync context.
     *  When non-NULL, replaces the procedural A-I-U-E-O cycle with real
     *  PCM-energy-based weights.  Set via emotion_set_lip_sync(). */
    lip_sync_ctx_t *lip_sync;

    /* Wall clock */
    float           total_time;         /**< Accumulated time in seconds */
} emotion_ctx_t;

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

/**
 * Initialize the emotion context. Resolves expression names against the model.
 * @param ctx    Emotion context to initialize.
 * @param model  Loaded VRM model with expressions.
 */
void emotion_init(emotion_ctx_t *ctx, vrm_model_t *model);

/**
 * Set the active emotion. Triggers a smooth transition from the current state.
 * @param ctx    Emotion context.
 * @param id     Emotion preset ID.
 */
void emotion_set(emotion_ctx_t *ctx, emotion_id_t id);

/**
 * Set emotion with a custom transition speed override.
 * @param speed  Lerp speed (e.g. 6.0 = fast, 2.0 = slow). 0 = use preset default.
 */
void emotion_set_ex(emotion_ctx_t *ctx, emotion_id_t id, float speed);

/**
 * Update the emotion system. Call once per frame.
 * Smoothly interpolates expression weights and applies procedural blink.
 * Updates model->expression_weights[] in-place.
 * @param ctx  Emotion context.
 * @param dt   Delta time in seconds since last frame.
 */
void emotion_update(emotion_ctx_t *ctx, float dt);

/**
 * Toggle speaking state.
 * When lip_sync is NULL, falls back to procedural A-I-U-E-O cycling.
 * When lip_sync is set, weights come from audio PCM analysis.
 * @param ctx       Emotion context.
 * @param speaking  1 = start speaking, 0 = stop.
 */
void emotion_set_speaking(emotion_ctx_t *ctx, int speaking);

/**
 * Bind an audio-driven lip sync context.
 * Pass NULL to revert to procedural cycling.
 * @param ctx  Emotion context.
 * @param ls   Lip sync context (not owned; must outlive ctx).
 */
void emotion_set_lip_sync(emotion_ctx_t *ctx, lip_sync_ctx_t *ls);

/**
 * @brief Set the global intensity for the active emotion.
 * @param[in] ctx       Emotion context.
 * @param[in] intensity Value in [0, 1]. 1.0 = full strength (default).
 */
void emotion_set_intensity(emotion_ctx_t *ctx, float intensity);

/**
 * @brief Look up an emotion ID by its display name (case-insensitive).
 * @param[in] name  Emotion name string (e.g. "happy", "thinking").
 * @return Matching emotion_id_t, or -1 if not found.
 */
int emotion_find_by_name(const char *name);

/**
 * @brief Set a direct BlendShape override for a specific expression index.
 *        Overrides take priority over the emotion preset for that expression.
 * @param[in] ctx        Emotion context.
 * @param[in] expr_index Expression index in the VRM model.
 * @param[in] weight     Target weight [0, 1].
 */
void emotion_set_override(emotion_ctx_t *ctx, int expr_index, float weight);

/**
 * @brief Clear all direct BlendShape overrides (return to emotion-driven mode).
 * @param[in] ctx  Emotion context.
 */
void emotion_clear_overrides(emotion_ctx_t *ctx);

/**
 * @brief Get the display name of an emotion.
 * @param[in] id  Emotion ID.
 * @return Human-readable name string.
 */
const char *emotion_name(emotion_id_t id);

/**
 * @brief Get the preset definition (read-only).
 * @param[in] id  Emotion ID.
 * @return Pointer to preset, or NULL if invalid.
 */
const emotion_preset_t *emotion_get_preset(emotion_id_t id);

/**
 * @brief Shutdown / cleanup the emotion context.
 * @param[in] ctx  Emotion context.
 */
void emotion_shutdown(emotion_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EMOTION_H */
