/**
 * @file skybox.c
 * @brief Cubemap skybox implementation — loads 6 face images and renders a skybox cube.
 * @version 1.0
 * @date 2025-03-25
 * @copyright Copyright (c) Tuya Inc.
 */

#include "skybox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb/stb_image.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

#define SKYBOX_FACE_COUNT  6
#define SKYBOX_PATH_MAX    1024

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */

static const char *s_skybox_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "uniform mat4 u_vp;\n"
    "out vec3 v_texcoord;\n"
    "void main() {\n"
    "    v_texcoord = a_pos;\n"
    "    vec4 pos = u_vp * vec4(a_pos, 1.0);\n"
    "    gl_Position = pos.xyww;\n"
    "}\n";

static const char *s_skybox_fs =
    "#version 140\n"
    "in vec3 v_texcoord;\n"
    "out vec4 frag_color;\n"
    "uniform samplerCube u_skybox;\n"
    "void main() {\n"
    "    frag_color = texture(u_skybox, v_texcoord);\n"
    "}\n";

/* Cube vertex data — inward-facing (camera is inside the cube) */
static const float s_skybox_vertices[] = {
    /* positions — each face is 2 triangles (6 vertices) */
    /* +X (right) */
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
    /* -X (left) */
    -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    /* +Y (top) */
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    /* -Y (bottom) */
    -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,
    /* +Z (front) */
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    /* -Z (back) */
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
};

/*
 * Face names to search for in the scene directory.
 * OpenGL cubemap target order: +X, -X, +Y, -Y, +Z, -Z.
 * Each row is a set of alternative base names (without extension).
 */
static const char *s_face_names[SKYBOX_FACE_COUNT][3] = {
    { "right",  "px", "posx" },   /* GL_TEXTURE_CUBE_MAP_POSITIVE_X */
    { "left",   "nx", "negx" },   /* GL_TEXTURE_CUBE_MAP_NEGATIVE_X */
    { "top",    "py", "posy" },   /* GL_TEXTURE_CUBE_MAP_POSITIVE_Y */
    { "bottom", "ny", "negy" },   /* GL_TEXTURE_CUBE_MAP_NEGATIVE_Y */
    { "front",  "pz", "posz" },   /* GL_TEXTURE_CUBE_MAP_POSITIVE_Z */
    { "back",   "nz", "negz" },   /* GL_TEXTURE_CUBE_MAP_NEGATIVE_Z */
};

static const char *s_extensions[] = { ".jpg", ".jpeg", ".png", ".bmp", ".tga", NULL };

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */

/**
 * @brief Compile a single shader stage
 * @param[in] type  GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
 * @param[in] src   GLSL source string
 * @return shader handle, or 0 on failure
 */
static GLuint __compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[skybox] shader compile error:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/**
 * @brief Link a vertex + fragment shader into a program
 * @param[in] vs_src  Vertex shader source
 * @param[in] fs_src  Fragment shader source
 * @return program handle, or 0 on failure
 */
static GLuint __link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = __compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) {
        return 0;
    }
    GLuint fs = __compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    int ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[skybox] program link error:\n%s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/**
 * @brief Try to find a face image file in the given directory
 * @param[out] out_path  Buffer to receive the found path
 * @param[in]  out_size  Size of out_path buffer
 * @param[in]  dir       Scene directory path
 * @param[in]  face_idx  Face index (0..5)
 * @return 0 if found, -1 if no matching file exists
 */
static int __find_face_file(char *out_path, size_t out_size,
                            const char *dir, int face_idx)
{
    for (int n = 0; n < 3; n++) {
        for (int e = 0; s_extensions[e] != NULL; e++) {
            snprintf(out_path, out_size, "%s/%s%s",
                     dir, s_face_names[face_idx][n], s_extensions[e]);
            FILE *fp = fopen(out_path, "rb");
            if (fp) {
                fclose(fp);
                return 0;
            }
        }
    }
    return -1;
}

/**
 * @brief Initialize skybox from a scene directory containing cubemap face images
 * @param[out] skybox  Skybox context to initialize
 * @param[in]  dir     Path to the scene directory
 * @return 0 on success, -1 on failure
 */
int skybox_init(skybox_t *skybox, const char *dir)
{
    if (!skybox || !dir || dir[0] == '\0') {
        return -1;
    }

    memset(skybox, 0, sizeof(skybox_t));

    /* Load all 6 face images */
    glGenTextures(1, &skybox->cubemap_tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->cubemap_tex);

    int loaded_count = 0;
    for (int i = 0; i < SKYBOX_FACE_COUNT; i++) {
        char path[SKYBOX_PATH_MAX];
        if (__find_face_file(path, sizeof(path), dir, i) != 0) {
            fprintf(stderr, "[skybox] missing face %d (%s/%s/...)\n",
                    i, s_face_names[i][0], s_face_names[i][1]);
            continue;
        }

        int w, h, channels;
        stbi_set_flip_vertically_on_load(0);
        unsigned char *pixels = stbi_load(path, &w, &h, &channels, 3);
        if (!pixels) {
            fprintf(stderr, "[skybox] failed to load: %s\n", path);
            continue;
        }

        GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)i;
        glTexImage2D(target, 0, GL_RGB, w, h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, pixels);
        printf("[skybox] loaded face %d: %s (%dx%d)\n", i, path, w, h);

        stbi_image_free(pixels);
        loaded_count++;
    }

    if (loaded_count < SKYBOX_FACE_COUNT) {
        fprintf(stderr, "[skybox] only %d of %d faces loaded — skybox disabled\n",
                loaded_count, SKYBOX_FACE_COUNT);
        glDeleteTextures(1, &skybox->cubemap_tex);
        skybox->cubemap_tex = 0;
        return -1;
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    /* Create cube VAO/VBO */
    glGenVertexArrays(1, &skybox->vao);
    glGenBuffers(1, &skybox->vbo);
    glBindVertexArray(skybox->vao);
    glBindBuffer(GL_ARRAY_BUFFER, skybox->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_skybox_vertices),
                 s_skybox_vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          3 * (GLsizei)sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    /* Compile shaders */
    skybox->program = __link_program(s_skybox_vs, s_skybox_fs);
    if (!skybox->program) {
        skybox_destroy(skybox);
        return -1;
    }
    skybox->u_vp = glGetUniformLocation(skybox->program, "u_vp");
    glUseProgram(skybox->program);
    glUniform1i(glGetUniformLocation(skybox->program, "u_skybox"), 0);
    glUseProgram(0);

    skybox->loaded = 1;
    printf("[skybox] cubemap skybox ready (%d faces)\n", SKYBOX_FACE_COUNT);
    return 0;
}

/**
 * @brief Render the skybox
 * @param[in] skybox    Skybox context (must be loaded)
 * @param[in] view_mat  4x4 column-major view matrix
 * @param[in] proj_mat  4x4 column-major projection matrix
 * @return none
 */
void skybox_draw(const skybox_t *skybox, const float view_mat[16],
                 const float proj_mat[16])
{
    if (!skybox || !skybox->loaded) {
        return;
    }

    /* Strip translation from view matrix (keep only rotation) */
    float view_rot[16];
    memcpy(view_rot, view_mat, sizeof(float) * 16);
    view_rot[12] = 0.0f;
    view_rot[13] = 0.0f;
    view_rot[14] = 0.0f;

    /* VP = projection * view_rotation */
    float vp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += proj_mat[k * 4 + r] * view_rot[c * 4 + k];
            }
            vp[c * 4 + r] = sum;
        }
    }

    glDepthFunc(GL_LEQUAL);
    glUseProgram(skybox->program);
    glUniformMatrix4fv(skybox->u_vp, 1, GL_FALSE, vp);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->cubemap_tex);
    glBindVertexArray(skybox->vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    glDepthFunc(GL_LESS);
}

/**
 * @brief Release all GPU resources held by the skybox
 * @param[in] skybox  Skybox context
 * @return none
 */
void skybox_destroy(skybox_t *skybox)
{
    if (!skybox) {
        return;
    }
    if (skybox->cubemap_tex) {
        glDeleteTextures(1, &skybox->cubemap_tex);
    }
    if (skybox->vao) {
        glDeleteVertexArrays(1, &skybox->vao);
    }
    if (skybox->vbo) {
        glDeleteBuffers(1, &skybox->vbo);
    }
    if (skybox->program) {
        glDeleteProgram(skybox->program);
    }
    memset(skybox, 0, sizeof(skybox_t));
}
