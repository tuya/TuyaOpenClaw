/**
 * @file settings_overlay.h
 * @brief Lightweight OpenGL overlay for VRM viewer settings and subtitles.
 *
 * Renders a 2D settings panel and subtitle bar directly in the OpenGL window,
 * using a single shader program and batched draw calls.  All GL state is saved
 * before rendering and restored afterward so the 3D pipeline is unaffected.
 *
 * @version 1.0
 * @date 2025-03-26
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef SETTINGS_OVERLAY_H
#define SETTINGS_OVERLAY_H

#include <SDL2/SDL.h>

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

typedef void (*overlay_str_cb_t)(const char *value, void *user_data);
typedef void (*overlay_int_cb_t)(int value, void *user_data);

typedef struct {
    const char       *model_dir;
    const char       *current_model;
    const char       *scene_parent_dir;
    const char       *current_scene;
    char            **anim_names;
    int               anim_count;
    int               active_anim;
    int               camera_locked;
    int               subtitle_enabled;
    int               fullscreen;
    int               spectator;
    overlay_str_cb_t  on_model_change;
    overlay_int_cb_t  on_anim_change;
    overlay_str_cb_t  on_scene_change;
    overlay_int_cb_t  on_camera_lock;
    overlay_int_cb_t  on_subtitle;
    overlay_int_cb_t  on_fullscreen;
    overlay_int_cb_t  on_spectator;
    void             *user_data;
} settings_overlay_cfg_t;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Create GL resources and populate file lists.
 * @param[in] cfg  Initial configuration and callbacks.
 * @return 0 on success, -1 on error.
 */
int  settings_overlay_init(const settings_overlay_cfg_t *cfg);

/**
 * @brief Free GL resources.
 * @return none
 */
void settings_overlay_destroy(void);

/**
 * @brief Process an SDL event; returns 1 if the overlay consumed it.
 * @param[in] ev    SDL event.
 * @param[in] win_w Window width in pixels.
 * @param[in] win_h Window height in pixels.
 * @return 1 if consumed, 0 otherwise.
 */
int  settings_overlay_handle_event(const SDL_Event *ev, int win_w, int win_h);

/**
 * @brief Render the overlay on top of the current framebuffer.
 *
 * Call after the 3D scene has been drawn but before SDL_GL_SwapWindow.
 * Saves and restores all GL state.
 *
 * @param[in] win_w Window width in pixels.
 * @param[in] win_h Window height in pixels.
 * @return none
 */
void settings_overlay_render(int win_w, int win_h);

void settings_overlay_toggle(void);
int  settings_overlay_is_visible(void);
int  settings_overlay_camera_locked(void);
int  settings_overlay_subtitle_enabled(void);
int  settings_overlay_fullscreen(void);
int  settings_overlay_spectator(void);
void settings_overlay_set_subtitle(const char *text);
void settings_overlay_update_anims(char **names, int count, int active);

#endif /* SETTINGS_OVERLAY_H */
