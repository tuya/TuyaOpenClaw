/**
 * @file vrm_viewer.c
 * @brief VRM 3D model renderer — OpenGL + SDL2 backend.
 *
 * Renders a 3D model in an SDL2+OpenGL window with skeletal animation.
 * Model path is configured via menuconfig (CONFIG_VRM_MODEL_PATH).
 * Animation directory is configured via menuconfig (CONFIG_VRM_ANIM_DIR).
 *
 * Controls:
 *   Left-drag    — rotate model
 *   Left / Right — switch animation
 *   Space        — pause / resume animation
 *   R            — reset model rotation
 *   Gear icon    — toggle settings panel (top-left corner)
 *   ESC / ×      — quit
 *
 * Entry point is provided by tuya_main.c; this file only contains the
 * renderer implementation (vrm_viewer_run).
 */

#include "tal_api.h"
#include "tkl_output.h"
#include "tuya_kconfig.h"
#include "gl_renderer.h"
