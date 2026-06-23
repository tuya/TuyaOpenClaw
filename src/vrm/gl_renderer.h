/**
 * @file gl_renderer.h
 * @brief OpenGL + SDL2 renderer for VRM models with skeletal animation.
 */

#ifndef GL_RENDERER_H
#define GL_RENDERER_H

#include <SDL2/SDL.h>

/* ---------------------------------------------------------------------------
 * Viewer entry point
 * --------------------------------------------------------------------------- */

/**
 * @brief Open an SDL2/OpenGL window, load the VRM model, optionally load VRMA
 * animations from a directory, and enter the render loop.
 *
 * Controls:
 *   Left-drag    : rotate model   (disabled when camera is locked)
 *   Left / Right : switch animation
 *   Space        : pause / resume animation
 *   R            : reset model rotation
 *   Gear icon    : toggle settings panel (top-left corner)
 *   ESC / close  : quit
 *
 * @param[in] model_path  Path to a .vrm / .glb / .pmx / .fbx model file.
 * @param[in] vrma_dir    Directory containing .vrma animation files (NULL = none).
 * @return 0 on clean exit, -1 on error.
 */
int vrm_viewer_run(const char *model_path, const char *vrma_dir);

/* ---------------------------------------------------------------------------
 * Thread-safe control API (callable from any thread)
 * --------------------------------------------------------------------------- */

/**
 * @brief Start or stop the speaking / lip-sync overlay on the VRM model.
 * @param[in] speaking  1 = TTS started, 0 = TTS stopped.
 * @return none
 */
void vrm_viewer_set_speaking(int speaking);

/**
 * @brief Request the avatar to play a named VRMA animation.
 * @param[in] name  Animation name (VRMA filename without extension).
 * @return none
 */
void vrm_viewer_play_animation(const char *name);

/**
 * @brief Set the avatar's facial expression by emotion name.
 * @param[in] name       Emotion name (case-insensitive).
 * @param[in] intensity  Strength [0, 1].
 * @param[in] speed      Transition speed (0 = preset default).
 * @return none
 */
void vrm_viewer_set_emotion(const char *name, float intensity, float speed);

/**
 * @brief Set a direct BlendShape override for a VRM expression.
 * @param[in] expr_name  Expression name.
 * @param[in] weight     Target weight [0, 1].
 * @return none
 */
void vrm_viewer_set_blendshape(const char *expr_name, float weight);

/**
 * @brief Clear all direct BlendShape overrides.
 * @return none
 */
void vrm_viewer_clear_blendshapes(void);

/**
 * @brief Set the subtitle text shown on the LVGL display.
 *
 * The subtitle is only displayed when the subtitle toggle is enabled in the
 * settings UI.  Pass NULL or "" to clear.
 *
 * @param[in] text  UTF-8 subtitle string.
 * @return none
 */
void vrm_viewer_set_subtitle(const char *text);

/**
 * @brief Request a model reload (hot-swap).
 *
 * The render loop picks up the request on the next frame, frees the old
 * model + GPU resources, loads the new model, and re-creates everything.
 *
 * @param[in] model_path  Full path to the new model file.
 * @return none
 */
void vrm_viewer_reload_model(const char *model_path);

/**
 * @brief Request a scene (skybox) reload.
 * @param[in] scene_dir  Path to the new scene directory ("" = no skybox).
 * @return none
 */
void vrm_viewer_reload_scene(const char *scene_dir);

#endif /* GL_RENDERER_H */
