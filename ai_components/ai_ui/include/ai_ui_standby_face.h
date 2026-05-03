/**
 * @file ai_ui_standby_face.h
 * @brief Standby face animation UI interface definitions.
 *
 * This header provides function declarations for the standby face UI,
 * including animated expressions (blinking eyes, emotion cycling) and
 * transition to chat interface.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __AI_UI_STANDBY_FACE_H__
#define __AI_UI_STANDBY_FACE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Standby face animation states */
typedef enum {
    STANDBY_FACE_STATE_IDLE,        /* Normal idle - blinking */
    STANDBY_FACE_STATE_LISTENING,   /* Listening - wide eyes */
    STANDBY_FACE_STATE_THINKING,    /* Thinking - looking around */
    STANDBY_FACE_STATE_RESPONDING,  /* Responding - excited */
    STANDBY_FACE_STATE_SLEEPING,    /* Sleeping - closed eyes */
} STANDBY_FACE_STATE_E;

/* Emotion types for standby face */
typedef enum {
    STANDBY_EMO_HAPPY,
    STANDBY_EMO_RELAXED,
    STANDBY_EMO_CURIOUS,
    STANDBY_EMO_SLEEPY,
    STANDBY_EMO_EXCITED,
    STANDBY_EMO_MAX,
} STANDBY_EMO_TYPE_E;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize standby face UI.
 *
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_init(void);

/**
 * @brief Deinitialize standby face UI.
 *
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_deinit(void);

/**
 * @brief Show standby face UI on screen.
 *
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_show(void);

/**
 * @brief Hide standby face UI.
 *
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_hide(void);

/**
 * @brief Set standby face animation state.
 *
 * @param state Animation state (idle, listening, thinking, etc.)
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_set_state(STANDBY_FACE_STATE_E state);

/**
 * @brief Set standby face emotion.
 *
 * @param emotion Emotion type
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_ui_standby_face_set_emotion(STANDBY_EMO_TYPE_E emotion);

/**
 * @brief Start blinking animation.
 */
void ai_ui_standby_face_start_blink(void);

/**
 * @brief Stop blinking animation.
 */
void ai_ui_standby_face_stop_blink(void);

/**
 * @brief Check if standby face is currently visible.
 *
 * @return true if visible, false otherwise
 */
bool ai_ui_standby_face_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* __AI_UI_STANDBY_FACE_H__ */
