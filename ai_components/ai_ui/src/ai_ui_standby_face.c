/**
 * @file ai_ui_standby_face.c
 * @brief Standby face animation UI implementation.
 *
 * This file provides the standby face UI with animated expressions,
 * including blinking eyes, emotion cycling, and smooth transitions.
 *
 * Features:
 *   - Animated eyes with blinking effect
 *   - Multiple standby emotions (happy, relaxed, curious, sleepy)
 *   - Smooth state transitions (idle, listening, thinking, responding)
 *   - LVGL-based rendering for hardware acceleration
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"

#if defined(ENABLE_LIBLVGL) && (ENABLE_LIBLVGL == 1)

#include "lvgl.h"
#include "lv_vendor.h"

#include "ai_ui_standby_face.h"
#include "ai_ui_icon_font.h"
#include "lang_config.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* Animation timing (ms) */
#define BLINK_INTERVAL_MIN      2000    /* Minimum time between blinks */
#define BLINK_INTERVAL_MAX      5000    /* Maximum time between blinks */
#define BLINK_DURATION          150     /* How long eyes stay closed */
#define EMOTION_CYCLE_INTERVAL  8000    /* Cycle emotions every 8 seconds */

/* Eye dimensions (scaled for 240x320 screen) */
#define FACE_BG_COLOR           lv_color_hex(0x1A1A2E)  /* Dark blue background */
#define EYE_COLOR               lv_color_hex(0x4ECDC4)  /* Cyan/teal eyes */
#define EYE_GLOW_COLOR          lv_color_hex(0x45B7AA)  /* Slightly darker glow */

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *face_container;
    lv_obj_t *left_eye;
    lv_obj_t *right_eye;
    lv_obj_t *mouth;
    lv_obj_t *hint_label;
    lv_obj_t *status_label;

    /* Animation state */
    STANDBY_FACE_STATE_E state;
    STANDBY_EMO_TYPE_E current_emotion;
    bool is_blinking;
    bool is_visible;

    /* LVGL timers */
    lv_timer_t *blink_timer;
    lv_timer_t *emotion_timer;
    lv_timer_t *anim_timer;
} STANDBY_FACE_CTX_T;

/***********************************************************
***********************variable define**********************/
static STANDBY_FACE_CTX_T sg_ctx = {0};

/* Emotion configurations - eye shapes and sizes */
typedef struct {
    int eye_width;
    int eye_height;
    int eye_radius;
    int eye_spacing;
    int eye_y_offset;
    const char *hint_text;
} emotion_config_t;

static const emotion_config_t emotion_config[STANDBY_EMO_MAX] = {
    /* HAPPY - slightly squinted, upbeat */
    {40, 35, 15, 25, -10, LANG_STANDBY_HINT_HAPPY},
    /* RELAXED - normal, calm */
    {45, 40, 18, 30, 0, LANG_STANDBY_HINT_RELAXED},
    /* CURIOUS - wide, alert */
    {40, 45, 12, 25, -5, LANG_STANDBY_HINT_CURIOUS},
    /* SLEEPY - half closed */
    {40, 20, 15, 25, 10, LANG_STANDBY_HINT_SLEEPY},
    /* EXCITED - wide open */
    {50, 50, 20, 25, -15, LANG_STANDBY_HINT_EXCITED},
};

/***********************************************************
***********************function define**********************/

/**
 * @brief Get random blink interval
 */
static uint32_t __get_random_blink_interval(void)
{
    uint32_t range = BLINK_INTERVAL_MAX - BLINK_INTERVAL_MIN;
    return BLINK_INTERVAL_MIN + tal_system_get_random(range);
}

/**
 * @brief Update eye appearance based on current emotion
 */
static void __update_eye_shape(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) {
        return;
    }

    const emotion_config_t *cfg = &emotion_config[sg_ctx.current_emotion];
    lv_coord_t screen_width = lv_obj_get_width(sg_ctx.screen);
    lv_coord_t center_x = screen_width / 2;
    lv_coord_t center_y = lv_obj_get_height(sg_ctx.screen) / 2 + cfg->eye_y_offset;

    lv_vendor_disp_lock();

    /* Calculate eye positions */
    lv_coord_t left_eye_x = center_x - cfg->eye_spacing - cfg->eye_width / 2;
    lv_coord_t right_eye_x = center_x + cfg->eye_spacing - cfg->eye_width / 2;
    lv_coord_t eye_y = center_y - cfg->eye_height / 2;

    /* Update left eye */
    lv_obj_set_pos(sg_ctx.left_eye, left_eye_x, eye_y);
    lv_obj_set_size(sg_ctx.left_eye, cfg->eye_width, cfg->eye_height);

    /* Update right eye */
    lv_obj_set_pos(sg_ctx.right_eye, right_eye_x, eye_y);
    lv_obj_set_size(sg_ctx.right_eye, cfg->eye_width, cfg->eye_height);

    /* Update hint text */
    if (sg_ctx.hint_label) {
        lv_label_set_text(sg_ctx.hint_label, cfg->hint_text);
    }

    lv_vendor_disp_unlock();
}

/**
 * @brief Blink animation - close eyes
 */
static void __blink_close(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) {
        return;
    }

    sg_ctx.is_blinking = true;

    lv_vendor_disp_lock();

    /* Animate to thin line (closed eye) */
    const emotion_config_t *cfg = &emotion_config[sg_ctx.current_emotion];
    lv_coord_t closed_height = 4;  /* Thin line for closed eye */

    /* Smooth animation using LVGL anim */
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, sg_ctx.left_eye);
    lv_anim_set_values(&anim, cfg->eye_height, closed_height);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_time(&anim, BLINK_DURATION / 2);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    lv_anim_set_var(&anim, sg_ctx.right_eye);
    lv_anim_start(&anim);

    lv_vendor_disp_unlock();
}

/**
 * @brief Blink animation - open eyes
 */
static void __blink_open(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) {
        return;
    }

    lv_vendor_disp_lock();

    const emotion_config_t *cfg = &emotion_config[sg_ctx.current_emotion];

    /* Animate back to normal height */
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, sg_ctx.left_eye);
    lv_anim_set_values(&anim, 4, cfg->eye_height);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_time(&anim, BLINK_DURATION / 2);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    lv_anim_set_var(&anim, sg_ctx.right_eye);
    lv_anim_start(&anim);

    lv_vendor_disp_unlock();

    sg_ctx.is_blinking = false;
}

/**
 * @brief Blink timer callback
 */
static void __blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!sg_ctx.is_visible) {
        return;
    }

    if (!sg_ctx.is_blinking) {
        /* Start blink - close eyes */
        __blink_close();

        /* Schedule open after BLINK_DURATION */
        if (sg_ctx.blink_timer) {
            lv_timer_set_cb(sg_ctx.blink_timer, __blink_timer_cb);
            lv_timer_reset(sg_ctx.blink_timer);
            lv_timer_set_period(sg_ctx.blink_timer, BLINK_DURATION);
        }
    } else {
        /* End blink - open eyes */
        __blink_open();

        /* Reset timer for next blink */
        if (sg_ctx.blink_timer) {
            lv_timer_set_period(sg_ctx.blink_timer, __get_random_blink_interval());
        }
    }
}

/**
 * @brief Emotion cycle timer callback
 */
static void __emotion_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!sg_ctx.is_visible || sg_ctx.state != STANDBY_FACE_STATE_IDLE) {
        return;
    }

    /* Cycle to next emotion */
    sg_ctx.current_emotion = (sg_ctx.current_emotion + 1) % STANDBY_EMO_MAX;
    __update_eye_shape();
}

/**
 * @brief Create eye object with glow effect
 */
static lv_obj_t *__create_eye(lv_obj_t *parent)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_set_style_bg_color(eye, EYE_COLOR, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(eye, EYE_GLOW_COLOR, 0);
    lv_obj_set_style_shadow_width(eye, 20, 0);
    lv_obj_set_style_shadow_spread(eye, 5, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    return eye;
}

/**
 * @brief Initialize standby face UI components
 */
static OPERATE_RET __standby_face_create(void)
{
    lv_vendor_disp_lock();

    /* Get active screen */
    sg_ctx.screen = lv_screen_active();
    lv_obj_set_style_bg_color(sg_ctx.screen, FACE_BG_COLOR, 0);

    /* Face container - holds all face elements */
    sg_ctx.face_container = lv_obj_create(sg_ctx.screen);
    lv_obj_set_size(sg_ctx.face_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(sg_ctx.face_container, FACE_BG_COLOR, 0);
    lv_obj_set_style_border_width(sg_ctx.face_container, 0, 0);
    lv_obj_clear_flag(sg_ctx.face_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(sg_ctx.face_container);

    /* Create eyes */
    sg_ctx.left_eye = __create_eye(sg_ctx.face_container);
    sg_ctx.right_eye = __create_eye(sg_ctx.face_container);

    /* Hint label at bottom */
    sg_ctx.hint_label = lv_label_create(sg_ctx.face_container);
    lv_obj_set_style_text_color(sg_ctx.hint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(sg_ctx.hint_label, ai_ui_get_text_font(), 0);
    lv_obj_align(sg_ctx.hint_label, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* Status label for state indication */
    sg_ctx.status_label = lv_label_create(sg_ctx.face_container);
    lv_obj_set_style_text_color(sg_ctx.status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(sg_ctx.status_label, ai_ui_get_text_font(), 0);
    lv_obj_align(sg_ctx.status_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(sg_ctx.status_label, "");

    /* Initial eye positions */
    __update_eye_shape();

    lv_vendor_disp_unlock();

    return OPRT_OK;
}

/**
 * @brief Destroy standby face UI components
 */
static __attribute__((unused)) void __standby_face_destroy(void)
{
    lv_vendor_disp_lock();

    if (sg_ctx.face_container) {
        lv_obj_del(sg_ctx.face_container);
        sg_ctx.face_container = NULL;
        sg_ctx.left_eye = NULL;
        sg_ctx.right_eye = NULL;
        sg_ctx.hint_label = NULL;
        sg_ctx.status_label = NULL;
    }

    lv_vendor_disp_unlock();
}

/***********************************************************
********************public functions************************/

OPERATE_RET ai_ui_standby_face_init(void)
{
    memset(&sg_ctx, 0, sizeof(sg_ctx));

    sg_ctx.state = STANDBY_FACE_STATE_IDLE;
    sg_ctx.current_emotion = STANDBY_EMO_HAPPY;
    sg_ctx.is_blinking = false;
    sg_ctx.is_visible = false;

    /* Create blink timer (initially stopped) */
    sg_ctx.blink_timer = lv_timer_create(__blink_timer_cb, BLINK_INTERVAL_MAX, NULL);
    if (sg_ctx.blink_timer) {
        lv_timer_pause(sg_ctx.blink_timer);
    }

    /* Create emotion cycle timer */
    sg_ctx.emotion_timer = lv_timer_create(__emotion_timer_cb, EMOTION_CYCLE_INTERVAL, NULL);
    if (sg_ctx.emotion_timer) {
        lv_timer_pause(sg_ctx.emotion_timer);
    }

    return OPRT_OK;
}

OPERATE_RET ai_ui_standby_face_deinit(void)
{
    ai_ui_standby_face_hide();

    if (sg_ctx.blink_timer) {
        lv_timer_del(sg_ctx.blink_timer);
        sg_ctx.blink_timer = NULL;
    }

    if (sg_ctx.emotion_timer) {
        lv_timer_del(sg_ctx.emotion_timer);
        sg_ctx.emotion_timer = NULL;
    }

    return OPRT_OK;
}

OPERATE_RET ai_ui_standby_face_show(void)
{
    if (sg_ctx.is_visible) {
        return OPRT_OK;
    }

    /* Create UI if not exists */
    if (!sg_ctx.face_container) {
        __standby_face_create();
    }

    sg_ctx.is_visible = true;

    /* Show container */
    lv_vendor_disp_lock();
    lv_obj_clear_flag(sg_ctx.face_container, LV_OBJ_FLAG_HIDDEN);
    lv_vendor_disp_unlock();

    /* Start animations */
    if (sg_ctx.blink_timer) {
        lv_timer_set_period(sg_ctx.blink_timer, __get_random_blink_interval());
        lv_timer_resume(sg_ctx.blink_timer);
    }
    if (sg_ctx.emotion_timer) {
        lv_timer_resume(sg_ctx.emotion_timer);
    }

    return OPRT_OK;
}

OPERATE_RET ai_ui_standby_face_hide(void)
{
    if (!sg_ctx.is_visible) {
        return OPRT_OK;
    }

    sg_ctx.is_visible = false;

    /* Stop animations */
    if (sg_ctx.blink_timer) {
        lv_timer_pause(sg_ctx.blink_timer);
    }
    if (sg_ctx.emotion_timer) {
        lv_timer_pause(sg_ctx.emotion_timer);
    }

    /* Hide container */
    if (sg_ctx.face_container) {
        lv_vendor_disp_lock();
        lv_obj_add_flag(sg_ctx.face_container, LV_OBJ_FLAG_HIDDEN);
        lv_vendor_disp_unlock();
    }

    return OPRT_OK;
}

OPERATE_RET ai_ui_standby_face_set_state(STANDBY_FACE_STATE_E state)
{
    sg_ctx.state = state;

    if (!sg_ctx.is_visible || !sg_ctx.status_label) {
        return OPRT_OK;
    }

    lv_vendor_disp_lock();

    switch (state) {
        case STANDBY_FACE_STATE_IDLE:
            lv_label_set_text(sg_ctx.status_label, "");
            break;
        case STANDBY_FACE_STATE_LISTENING:
            lv_label_set_text(sg_ctx.status_label, LISTENING);
            break;
        case STANDBY_FACE_STATE_THINKING:
            lv_label_set_text(sg_ctx.status_label, THINKING);
            break;
        case STANDBY_FACE_STATE_RESPONDING:
            lv_label_set_text(sg_ctx.status_label, LANG_RESPONDING);
            break;
        case STANDBY_FACE_STATE_SLEEPING:
            lv_label_set_text(sg_ctx.status_label, LANG_SLEEPING);
            break;
    }

    lv_vendor_disp_unlock();

    return OPRT_OK;
}

OPERATE_RET ai_ui_standby_face_set_emotion(STANDBY_EMO_TYPE_E emotion)
{
    if (emotion >= STANDBY_EMO_MAX) {
        return OPRT_INVALID_PARM;
    }

    sg_ctx.current_emotion = emotion;
    __update_eye_shape();

    return OPRT_OK;
}

void ai_ui_standby_face_start_blink(void)
{
    if (sg_ctx.blink_timer && sg_ctx.is_visible) {
        lv_timer_resume(sg_ctx.blink_timer);
    }
}

void ai_ui_standby_face_stop_blink(void)
{
    if (sg_ctx.blink_timer) {
        lv_timer_pause(sg_ctx.blink_timer);
    }
}

bool ai_ui_standby_face_is_visible(void)
{
    return sg_ctx.is_visible;
}

#endif /* ENABLE_LIBLVGL */
