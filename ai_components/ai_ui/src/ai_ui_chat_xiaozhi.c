/**
 * @file ai_ui_chat_xiaozhi.c
 * @brief Xiaozhi-style chat UI with standby face animation.
 *
 * This file provides a combined UI that shows a cute animated standby face
 * when idle, and switches to a chat interface during conversation.
 * Features:
 *   - Animated standby face with blinking eyes
 *   - Automatic switching between standby and chat modes
 *   - Smooth LVGL animations
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"

#if defined(ENABLE_AI_CHAT_GUI_XIAOZHI) && (ENABLE_AI_CHAT_GUI_XIAOZHI == 1)

#include "lvgl.h"
#include "lv_vendor.h"

#include "font_awesome_symbols.h"
#include "ai_ui_manage.h"
#include "ai_ui_icon_font.h"
#include "ai_ui_chat_xiaozhi.h"

#include "lang_config.h"

/***********************************************************
************************macro define************************
***********************************************************/
/* Animation timing (ms) */
#define BLINK_INTERVAL_MIN      2000
#define BLINK_INTERVAL_MAX      5000
#define BLINK_DURATION          150
#define EMOTION_CYCLE_INTERVAL  10000   /* 10 seconds between emotion changes */
#define IDLE_TIMEOUT_MS         30000   /* Switch to standby after 30s idle */

/* Eye colors */
#define FACE_BG_COLOR           lv_color_hex(0x1A1A2E)
#define EYE_COLOR               lv_color_hex(0x4ECDC4)
#define EYE_GLOW_COLOR          lv_color_hex(0x45B7AA)

/* UI states */
typedef enum {
    UI_STATE_STANDBY,       /* Showing standby face */
    UI_STATE_CHAT,          /* Showing chat interface */
} UI_STATE_E;

/***********************************************************
***********************typedef define***********************/
typedef struct {
    /* Standby face objects */
    lv_obj_t *standby_container;
    lv_obj_t *left_eye;
    lv_obj_t *right_eye;
    lv_obj_t *hint_label;

    /* Chat objects */
    lv_obj_t *chat_container;
    lv_obj_t *status_bar;
    lv_obj_t *emotion_label;
    lv_obj_t *chat_message_label;
    lv_obj_t *status_label;
    lv_obj_t *network_label;
    lv_obj_t *chat_mode_label;

    /* State management */
    UI_STATE_E current_state;
    XIAOZHI_EMO_TYPE_E current_emotion;
    bool is_blinking;
    bool is_streaming;

    /* LVGL timers */
    lv_timer_t *blink_timer;
    lv_timer_t *emotion_timer;
    lv_timer_t *idle_timer;

    /* Theme colors */
    lv_color_t bg_color;
    lv_color_t text_color;
    lv_color_t user_bubble_color;
    lv_color_t assistant_bubble_color;
} XIAOZHI_UI_CTX_T;

/***********************************************************
***********************variable define**********************/
static XIAOZHI_UI_CTX_T sg_ctx = {0};
static AI_UI_FONT_LIST_T sg_font = {0};

/* Emotion configurations - eye shapes */
typedef struct {
    int eye_width;
    int eye_height;
    int eye_spacing;
    int eye_y_offset;
    const char *hint_text;
} emotion_config_t;

static const emotion_config_t emotion_configs[XIAOZHI_EMO_MAX] = {
    {45, 40, 30, 0, LANG_STANDBY_HINT_HAPPY},      /* HAPPY */
    {40, 35, 25, 5, LANG_STANDBY_HINT_RELAXED},    /* RELAXED */
    {50, 45, 25, -5, LANG_STANDBY_HINT_CURIOUS},   /* CURIOUS */
    {40, 20, 30, 10, LANG_STANDBY_HINT_SLEEPY},   /* SLEEPY */
    {50, 50, 25, -10, LANG_STANDBY_HINT_EXCITED},  /* EXCITED */
};

/***********************************************************
***********************helper functions********************/

static uint32_t __get_random_blink_interval(void)
{
    uint32_t range = BLINK_INTERVAL_MAX - BLINK_INTERVAL_MIN;
    return BLINK_INTERVAL_MIN + tal_system_get_random(range);
}

static void __update_eye_positions(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) return;

    const emotion_config_t *cfg = &emotion_configs[sg_ctx.current_emotion];
    lv_coord_t center_x = LV_HOR_RES / 2;
    lv_coord_t center_y = LV_VER_RES / 2 + cfg->eye_y_offset;

    lv_coord_t left_x = center_x - cfg->eye_spacing - cfg->eye_width / 2;
    lv_coord_t right_x = center_x + cfg->eye_spacing - cfg->eye_width / 2;
    lv_coord_t eye_y = center_y - cfg->eye_height / 2;

    lv_obj_set_pos(sg_ctx.left_eye, left_x, eye_y);
    lv_obj_set_size(sg_ctx.left_eye, cfg->eye_width, cfg->eye_height);

    lv_obj_set_pos(sg_ctx.right_eye, right_x, eye_y);
    lv_obj_set_size(sg_ctx.right_eye, cfg->eye_width, cfg->eye_height);

    if (sg_ctx.hint_label) {
        lv_label_set_text(sg_ctx.hint_label, cfg->hint_text);
    }
}

static void __blink_animation(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) return;

    const emotion_config_t *cfg = &emotion_configs[sg_ctx.current_emotion];
    lv_coord_t closed_height = 4;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_time(&anim, BLINK_DURATION / 2);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);

    /* Close eyes */
    sg_ctx.is_blinking = true;
    lv_anim_set_var(&anim, sg_ctx.left_eye);
    lv_anim_set_values(&anim, cfg->eye_height, closed_height);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_start(&anim);

    lv_anim_set_var(&anim, sg_ctx.right_eye);
    lv_anim_start(&anim);
}

static void __unblink_animation(void)
{
    if (!sg_ctx.left_eye || !sg_ctx.right_eye) return;

    const emotion_config_t *cfg = &emotion_configs[sg_ctx.current_emotion];

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_time(&anim, BLINK_DURATION / 2);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);

    /* Open eyes */
    lv_anim_set_var(&anim, sg_ctx.left_eye);
    lv_anim_set_values(&anim, 4, cfg->eye_height);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_start(&anim);

    lv_anim_set_var(&anim, sg_ctx.right_eye);
    lv_anim_start(&anim);

    sg_ctx.is_blinking = false;
}

static void __blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (sg_ctx.current_state != UI_STATE_STANDBY) return;

    if (!sg_ctx.is_blinking) {
        __blink_animation();
        lv_timer_set_period(sg_ctx.blink_timer, BLINK_DURATION);
    } else {
        __unblink_animation();
        lv_timer_set_period(sg_ctx.blink_timer, __get_random_blink_interval());
    }
}

static void __emotion_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (sg_ctx.current_state != UI_STATE_STANDBY) return;

    /* Cycle to next emotion */
    sg_ctx.current_emotion = (sg_ctx.current_emotion + 1) % XIAOZHI_EMO_MAX;
    __update_eye_positions();
}

static lv_obj_t *__create_eye(lv_obj_t *parent)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_set_style_bg_color(eye, EYE_COLOR, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(eye, EYE_GLOW_COLOR, 0);
    lv_obj_set_style_shadow_width(eye, 15, 0);
    lv_obj_set_style_shadow_spread(eye, 3, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    return eye;
}

/***********************************************************
***********************UI creation************************/

static void __create_standby_ui(void)
{
    /* Standby container (full screen) */
    sg_ctx.standby_container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(sg_ctx.standby_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(sg_ctx.standby_container, FACE_BG_COLOR, 0);
    lv_obj_set_style_border_width(sg_ctx.standby_container, 0, 0);
    lv_obj_clear_flag(sg_ctx.standby_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sg_ctx.standby_container, LV_OBJ_FLAG_HIDDEN);

    /* Create eyes */
    sg_ctx.left_eye = __create_eye(sg_ctx.standby_container);
    sg_ctx.right_eye = __create_eye(sg_ctx.standby_container);

    /* Hint label */
    sg_ctx.hint_label = lv_label_create(sg_ctx.standby_container);
    lv_obj_set_style_text_color(sg_ctx.hint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(sg_ctx.hint_label, sg_font.text, 0);
    lv_obj_align(sg_ctx.hint_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    __update_eye_positions();
}

static void __create_chat_ui(void)
{
    /* Chat container (full screen) */
    sg_ctx.chat_container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(sg_ctx.chat_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(sg_ctx.chat_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sg_ctx.chat_container, 0, 0);
    lv_obj_set_style_border_width(sg_ctx.chat_container, 0, 0);
    lv_obj_set_style_bg_color(sg_ctx.chat_container, sg_ctx.bg_color, 0);
    lv_obj_add_flag(sg_ctx.chat_container, LV_OBJ_FLAG_HIDDEN);

    /* Status bar */
    sg_ctx.status_bar = lv_obj_create(sg_ctx.chat_container);
    lv_obj_set_size(sg_ctx.status_bar, LV_HOR_RES, sg_font.text->line_height);
    lv_obj_set_style_radius(sg_ctx.status_bar, 0, 0);
    lv_obj_set_style_bg_color(sg_ctx.status_bar, sg_ctx.bg_color, 0);

    /* Content area */
    lv_obj_t *content = lv_obj_create(sg_ctx.chat_container);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_width(content, LV_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_bg_color(content, sg_ctx.bg_color, 0);

    /* Emotion label (large emoji) */
    sg_ctx.emotion_label = lv_label_create(content);
    lv_obj_set_style_text_font(sg_ctx.emotion_label, sg_font.emoji, 0);
    lv_label_set_text(sg_ctx.emotion_label, sg_font.emoji_list[0].emo_icon);

    /* Chat message label */
    sg_ctx.chat_message_label = lv_label_create(content);
    lv_label_set_text(sg_ctx.chat_message_label, "");
    lv_obj_set_width(sg_ctx.chat_message_label, LV_HOR_RES * 0.9);
    lv_obj_set_height(sg_ctx.chat_message_label, LV_VER_RES * 0.4);
    lv_label_set_long_mode(sg_ctx.chat_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(sg_ctx.chat_message_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Status label */
    sg_ctx.status_label = lv_label_create(sg_ctx.status_bar);
    lv_obj_set_style_text_color(sg_ctx.status_label, sg_ctx.text_color, 0);
    lv_label_set_text(sg_ctx.status_label, INITIALIZING);
    lv_obj_center(sg_ctx.status_label);

    /* Network icon */
    sg_ctx.network_label = lv_label_create(sg_ctx.status_bar);
    lv_obj_set_style_text_font(sg_ctx.network_label, sg_font.icon, 0);
    lv_obj_set_style_text_color(sg_ctx.network_label, sg_ctx.text_color, 0);
    lv_obj_align(sg_ctx.network_label, LV_ALIGN_RIGHT_MID, -5, 0);

    /* Chat mode label */
    sg_ctx.chat_mode_label = lv_label_create(sg_ctx.status_bar);
    lv_obj_set_style_text_color(sg_ctx.chat_mode_label, sg_ctx.text_color, 0);
    lv_obj_align(sg_ctx.chat_mode_label, LV_ALIGN_LEFT_MID, 5, 0);
}

/***********************************************************
***********************state management********************/

static void __switch_to_standby(void)
{
    if (sg_ctx.current_state == UI_STATE_STANDBY) return;

    sg_ctx.current_state = UI_STATE_STANDBY;

    lv_obj_add_flag(sg_ctx.chat_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sg_ctx.standby_container, LV_OBJ_FLAG_HIDDEN);

    /* Start animations */
    if (sg_ctx.blink_timer) {
        lv_timer_set_period(sg_ctx.blink_timer, __get_random_blink_interval());
        lv_timer_resume(sg_ctx.blink_timer);
    }
    if (sg_ctx.emotion_timer) {
        lv_timer_resume(sg_ctx.emotion_timer);
    }
}

static void __switch_to_chat(void)
{
    if (sg_ctx.current_state == UI_STATE_CHAT) return;

    sg_ctx.current_state = UI_STATE_CHAT;

    /* Stop standby animations */
    if (sg_ctx.blink_timer) lv_timer_pause(sg_ctx.blink_timer);
    if (sg_ctx.emotion_timer) lv_timer_pause(sg_ctx.emotion_timer);

    lv_obj_add_flag(sg_ctx.standby_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sg_ctx.chat_container, LV_OBJ_FLAG_HIDDEN);
}

/***********************************************************
***********************UI interface functions****************/

static void __ui_font_init(void)
{
    sg_font.text       = ai_ui_get_text_font();
    sg_font.icon       = ai_ui_get_icon_font();
    sg_font.emoji      = ai_ui_get_emo_font();
    sg_font.emoji_list = ai_ui_get_emo_list();
}

static OPERATE_RET __ui_init(void)
{
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_start(5, 1024*8);

    lv_vendor_disp_lock();

    __ui_font_init();

    /* Theme colors */
    sg_ctx.bg_color = lv_color_white();
    sg_ctx.text_color = lv_color_black();
    sg_ctx.user_bubble_color = lv_color_hex(0x95EC69);
    sg_ctx.assistant_bubble_color = lv_color_white();

    /* Create UIs */
    __create_standby_ui();
    __create_chat_ui();

    /* Create timers */
    sg_ctx.blink_timer = lv_timer_create(__blink_timer_cb, BLINK_INTERVAL_MAX, NULL);
    lv_timer_pause(sg_ctx.blink_timer);

    sg_ctx.emotion_timer = lv_timer_create(__emotion_timer_cb, EMOTION_CYCLE_INTERVAL, NULL);
    lv_timer_pause(sg_ctx.emotion_timer);

    /* Start in standby mode */
    sg_ctx.current_emotion = XIAOZHI_EMO_HAPPY;
    __switch_to_standby();

    lv_vendor_disp_unlock();

    return OPRT_OK;
}

static void __ui_set_user_msg(char *text)
{
    if (!sg_ctx.chat_message_label) return;

    __switch_to_chat();

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.chat_message_label, text);
    lv_obj_set_style_bg_color(sg_ctx.chat_message_label, sg_ctx.user_bubble_color, 0);
    lv_obj_set_style_text_color(sg_ctx.chat_message_label, sg_ctx.text_color, 0);
    lv_vendor_disp_unlock();
}

static void __ui_set_ai_msg(char *text)
{
    if (!sg_ctx.chat_message_label) return;

    __switch_to_chat();

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.chat_message_label, text);
    lv_obj_set_style_bg_color(sg_ctx.chat_message_label, sg_ctx.assistant_bubble_color, 0);
    lv_obj_set_style_text_color(sg_ctx.chat_message_label, sg_ctx.text_color, 0);
    lv_vendor_disp_unlock();
}

static void __ui_set_ai_msg_stream_start(void)
{
    __switch_to_chat();
    sg_ctx.is_streaming = true;

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.chat_message_label, "");
    lv_vendor_disp_unlock();
}

static void __ui_set_ai_msg_stream_data(char *text)
{
    if (!sg_ctx.chat_message_label || !sg_ctx.is_streaming) return;

    lv_vendor_disp_lock();
    lv_label_ins_text(sg_ctx.chat_message_label, LV_LABEL_POS_LAST, text);
    lv_vendor_disp_unlock();
}

static void __ui_set_ai_msg_stream_end(void)
{
    sg_ctx.is_streaming = false;
}

static void __ui_set_system_msg(char *text)
{
    if (!sg_ctx.chat_message_label) return;

    __switch_to_chat();

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.chat_message_label, text);
    lv_obj_set_style_text_color(sg_ctx.chat_message_label, lv_color_hex(0x666666), 0);
    lv_vendor_disp_unlock();
}

static void __ui_set_emotion(char *emotion)
{
    if (!sg_ctx.emotion_label) return;

    char *emo_icon = sg_font.emoji_list[0].emo_icon;
    for (int i = 0; i < FONT_EMO_ICON_MAX_NUM; i++) {
        if (strcmp(emotion, sg_font.emoji_list[i].emo_name) == 0) {
            emo_icon = sg_font.emoji_list[i].emo_icon;
            break;
        }
    }

    lv_vendor_disp_lock();
    lv_obj_set_style_text_font(sg_ctx.emotion_label, sg_font.emoji, 0);
    lv_label_set_text(sg_ctx.emotion_label, emo_icon);
    lv_vendor_disp_unlock();
}

static void __ui_set_status(char *status)
{
    if (!sg_ctx.status_label) return;

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.status_label, status);
    lv_vendor_disp_unlock();
}

static void __ui_set_notification(char *notification)
{
    if (!sg_ctx.status_label) return;

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.status_label, notification);
    lv_vendor_disp_unlock();
}

static void __ui_set_network(AI_UI_WIFI_STATUS_E wifi_status)
{
    if (!sg_ctx.network_label) return;

    char *wifi_icon = ai_ui_get_wifi_icon(wifi_status);

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.network_label, wifi_icon);
    lv_vendor_disp_unlock();
}

static void __ui_set_chat_mode(char *chat_mode)
{
    if (!sg_ctx.chat_mode_label || !chat_mode) return;

    lv_vendor_disp_lock();
    lv_label_set_text(sg_ctx.chat_mode_label, chat_mode);
    lv_vendor_disp_unlock();
}

/***********************************************************
***********************public interface**********************/

OPERATE_RET ai_ui_chat_xiaozhi_register(void)
{
    AI_UI_INTFS_T intfs;

    memset(&intfs, 0, sizeof(AI_UI_INTFS_T));

    intfs.disp_init                = __ui_init;
    intfs.disp_user_msg            = __ui_set_user_msg;
    intfs.disp_ai_msg              = __ui_set_ai_msg;
    intfs.disp_ai_msg_stream_start = __ui_set_ai_msg_stream_start;
    intfs.disp_ai_msg_stream_data  = __ui_set_ai_msg_stream_data;
    intfs.disp_ai_msg_stream_end   = __ui_set_ai_msg_stream_end;
    intfs.disp_system_msg          = __ui_set_system_msg;
    intfs.disp_emotion             = __ui_set_emotion;
    intfs.disp_ai_mode_state       = __ui_set_status;
    intfs.disp_notification        = __ui_set_notification;
    intfs.disp_wifi_state          = __ui_set_network;
    intfs.disp_ai_chat_mode        = __ui_set_chat_mode;

    return ai_ui_register(&intfs);
}

#endif /* ENABLE_AI_CHAT_GUI_XIAOZHI */
