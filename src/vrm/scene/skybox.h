/**
 * @file skybox.h
 * @brief Cubemap skybox renderer for the VRM viewer.
 * @version 1.0
 * @date 2025-03-25
 * @copyright Copyright (c) Tuya Inc.
 */

#ifndef __SKYBOX_H__
#define __SKYBOX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <GL/glew.h>

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

typedef struct {
    GLuint cubemap_tex;
    GLuint vao;
    GLuint vbo;
    GLuint program;
    GLint  u_vp;
    int    loaded;
} skybox_t;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialize skybox from a scene directory containing cubemap face images.
 *
 * The directory must contain 6 images for the cube faces. Supported naming:
 *   - right/left/top/bottom/front/back  (.jpg/.png/.bmp/.tga)
 *   - px/nx/py/ny/pz/nz                (.jpg/.png/.bmp/.tga)
 *   - posx/negx/posy/negy/posz/negz    (.jpg/.png/.bmp/.tga)
 *
 * If any face image is missing, the skybox is marked as not loaded (skybox->loaded == 0)
 * and the caller should fall back to the gradient background.
 *
 * @param[out] skybox  Skybox context to initialize
 * @param[in]  dir     Path to the scene directory
 * @return 0 on success, -1 on failure (missing images or GL error)
 */
int skybox_init(skybox_t *skybox, const char *dir);

/**
 * @brief Render the skybox.
 *
 * Must be called after glClear() and before drawing the scene.
 * Disables depth writes so the skybox always appears behind everything.
 *
 * @param[in] skybox    Skybox context (must be loaded)
 * @param[in] view_mat  4x4 column-major view matrix
 * @param[in] proj_mat  4x4 column-major projection matrix
 * @return none
 */
void skybox_draw(const skybox_t *skybox, const float view_mat[16],
                 const float proj_mat[16]);

/**
 * @brief Release all GPU resources held by the skybox.
 * @param[in] skybox  Skybox context
 * @return none
 */
void skybox_destroy(skybox_t *skybox);

#ifdef __cplusplus
}
#endif

#endif /* __SKYBOX_H__ */
