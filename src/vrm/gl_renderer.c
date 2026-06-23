/**
 * @file gl_renderer.c
 * @brief SDL2 + OpenGL 3.3 core renderer with GPU skinning for VRM models.
 *
 * Supports up to VRM_MAX_BONES bones via a Texture Buffer Object (TBO).
 * Each bone matrix (4x4 float) is stored as 4 texels in an RGBA32F TBO.
 */

#include "gl_renderer.h"
#include "vrm_loader.h"
#include "mat4_util.h"
#include "emotion/emotion.h"
#include "spring_bone/spring_bone.h"
#include "lip_sync.h"
#include "skybox.h"
#include "settings_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <dirent.h>
#include <string.h>
#include <libgen.h>

#include "tuya_kconfig.h"
#include "svc_ai_player.h"

/* ================================================================== */
/*  Global state — shared with other modules via vrm_viewer_set_speaking */
/* ================================================================== */

/** Emotion context pointer — valid only while vrm_viewer_run() is executing. */
static emotion_ctx_t *s_emo_ctx = NULL;

/** Lip sync context — persistent, initialised in vrm_viewer_run(). */
static lip_sync_ctx_t s_lip_sync_ctx;

/* ------------------------------------------------------------------ */
/*  Animation switch request (written by any thread, read by render)  */
/* ------------------------------------------------------------------ */

#define ANIM_NAME_MAX 64

/** Pending animation name.  Empty string = no pending request. */
static char             s_anim_req_name[ANIM_NAME_MAX];
/** Set to 1 by vrm_viewer_play_animation(), cleared by the render loop. */
static volatile int     s_anim_req_pending = 0;
/** External animation requests should play once, then return to idle_normal. */
static volatile int     s_anim_req_return_to_idle = 0;

/** Last SDL_GetTicks() when user interaction (MCP request / TTS) occurred.
 *  Written by any thread, read by the render loop. */
static volatile Uint32  s_last_interact_ticks = 0;

/* ------------------------------------------------------------------ */
/*  Emotion request (written by any thread, read by render)           */
/* ------------------------------------------------------------------ */

#define EMO_NAME_MAX 64

static char            s_emo_req_name[EMO_NAME_MAX];
static float           s_emo_req_intensity = 1.0f;
static float           s_emo_req_speed     = 0.0f;
static volatile int    s_emo_req_pending   = 0;

/* ------------------------------------------------------------------ */
/*  BlendShape override request (written by any thread, read by render) */
/* ------------------------------------------------------------------ */

#define BS_REQ_MAX 16

typedef struct {
    char  name[64];
    float weight;
} bs_req_entry_t;

static bs_req_entry_t  s_bs_req_entries[BS_REQ_MAX];
static int             s_bs_req_count   = 0;
static volatile int    s_bs_req_pending = 0;
static volatile int    s_bs_clear_pending = 0;

/* ------------------------------------------------------------------ */
/*  Model / scene reload request (written by settings UI)             */
/* ------------------------------------------------------------------ */

#define RELOAD_PATH_MAX 1024

static char         s_reload_model_path[RELOAD_PATH_MAX];
static volatile int s_reload_model_pending = 0;

static char         s_reload_scene_dir[RELOAD_PATH_MAX];
static volatile int s_reload_scene_pending = 0;

/* (Subtitle display is now handled by the LVGL settings panel) */

/* ================================================================== */
/*  Chained audio consumer — forwards PCM to speaker + lip sync      */
/* ================================================================== */

/* Forward-declare the default speaker consumer defined in consumer_speaker.c */
extern AI_PLAYER_CONSUMER_T g_consumer_speaker;

/** Handle obtained from the underlying speaker consumer's open() call. */
static PLAYER_CONSUMER_HANDLE s_speaker_handle = NULL;

static OPERATE_RET __lc_open(PLAYER_CONSUMER_HANDLE *handle)
{
    OPERATE_RET rt = g_consumer_speaker.open(&s_speaker_handle);
    *handle = (PLAYER_CONSUMER_HANDLE)&s_lip_sync_ctx;
    return rt;
}

static OPERATE_RET __lc_close(PLAYER_CONSUMER_HANDLE handle)
{
    (void)handle;
    vrm_viewer_set_speaking(0);
    return g_consumer_speaker.close(s_speaker_handle);
}

static OPERATE_RET __lc_start(PLAYER_CONSUMER_HANDLE handle)
{
    (void)handle;
    vrm_viewer_set_speaking(1);
    return g_consumer_speaker.start(s_speaker_handle);
}

static OPERATE_RET __lc_write(PLAYER_CONSUMER_HANDLE handle,
                               const void *buf, uint32_t len)
{
    (void)handle;
    /* Feed decoded PCM to lip sync analysis (16 kHz, 16-bit, mono) */
    lip_sync_feed_pcm(&s_lip_sync_ctx, buf, len, 1, 16);
    /* Forward to hardware speaker */
    return g_consumer_speaker.write(s_speaker_handle, buf, len);
}

static OPERATE_RET __lc_stop(PLAYER_CONSUMER_HANDLE handle)
{
    (void)handle;
    vrm_viewer_set_speaking(0);
    return g_consumer_speaker.stop(s_speaker_handle);
}

static OPERATE_RET __lc_set_volume(PLAYER_CONSUMER_HANDLE handle,
                                    uint32_t volume)
{
    (void)handle;
    return g_consumer_speaker.set_volume(s_speaker_handle, volume);
}

static AI_PLAYER_CONSUMER_T s_lip_sync_consumer = {
    .open       = __lc_open,
    .close      = __lc_close,
    .start      = __lc_start,
    .write      = __lc_write,
    .stop       = __lc_stop,
    .set_volume = __lc_set_volume,
};

/* ================================================================== */
/*  Idle behavior — configuration                                      */
/* ================================================================== */

typedef struct {
    const char *name;   /**< Animation name (VRMA filename without extension) */
    int         weight; /**< Relative weight for random selection */
} idle_anim_entry_t;

static const idle_anim_entry_t s_idle_pool[] = {
    { "idle_normal",        10 },
    { "idle_boring",         3 },
    { "happy_idle",          3 },
    { "thinking",            2 },
    { "squat",               1 },
    { "show",                1 },
};
#define IDLE_POOL_COUNT  (int)(sizeof(s_idle_pool) / sizeof(s_idle_pool[0]))

static const char *s_oneshot_anims[] = {
    "say_hello", "standing_greeting", "wave",
    "excited", "joy", "crying",
    "show", "squat", "shoot", "bier",
    "look_around", "thinking",
    NULL
};

static const emotion_id_t s_idle_emotions[] = {
    EMOTION_NEUTRAL, EMOTION_NEUTRAL, EMOTION_NEUTRAL,
    EMOTION_HAPPY, EMOTION_HAPPY,
    EMOTION_WINK,
    EMOTION_THINKING, EMOTION_THINKING,
    EMOTION_COOL,
    EMOTION_RELAXED, EMOTION_RELAXED,
    EMOTION_SILLY,
    EMOTION_CONFUSED,
    EMOTION_LOVING,
};
#define IDLE_EMOTION_COUNT  (int)(sizeof(s_idle_emotions) / sizeof(s_idle_emotions[0]))

#define IDLE_SWITCH_MIN_SEC    8.0f
#define IDLE_SWITCH_MAX_SEC   18.0f
#define IDLE_EMOTION_MIN_SEC   8.0f
#define IDLE_EMOTION_MAX_SEC  16.0f
#define IDLE_EMOTION_HOLD_SEC  3.0f
#define IDLE_INTERACT_COOLDOWN 5.0f
#define ANIM_SWITCH_BLEND_SEC  0.60f

/* ================================================================== */
/*  Public: speaking control (called from any thread)                 */
/* ================================================================== */

/**
 * Grace period (ms) after TTS_STOP before lip sync is actually disabled.
 * Allows the speaker's audio buffer to drain so the last sentence still
 * has visible lip movement.
 */
#define SPEAKING_FADE_MS 500

static volatile Uint32 s_speaking_stop_ticks   = 0;
static volatile int    s_speaking_stop_pending  = 0;

void vrm_viewer_set_speaking(int speaking)
{
    s_last_interact_ticks = SDL_GetTicks();
    emotion_ctx_t *ctx = s_emo_ctx;
    if (!ctx) return;

    if (speaking) {
        s_speaking_stop_pending = 0;
        emotion_set_speaking(ctx, 1);
    } else {
        s_speaking_stop_ticks  = SDL_GetTicks();
        s_speaking_stop_pending = 1;
    }
}

void vrm_viewer_play_animation(const char *name)
{
    if (!name || name[0] == '\0') return;
    s_last_interact_ticks = SDL_GetTicks();
    strncpy(s_anim_req_name, name, ANIM_NAME_MAX - 1);
    s_anim_req_name[ANIM_NAME_MAX - 1] = '\0';
    s_anim_req_return_to_idle = 1;
    s_anim_req_pending = 1;
}

void vrm_viewer_set_emotion(const char *name, float intensity, float speed)
{
    if (!name || name[0] == '\0') return;
    s_last_interact_ticks = SDL_GetTicks();
    strncpy(s_emo_req_name, name, EMO_NAME_MAX - 1);
    s_emo_req_name[EMO_NAME_MAX - 1] = '\0';
    s_emo_req_intensity = intensity;
    s_emo_req_speed     = speed;
    s_emo_req_pending   = 1;
}

void vrm_viewer_set_blendshape(const char *expr_name, float weight)
{
    if (!expr_name || expr_name[0] == '\0') return;
    s_last_interact_ticks = SDL_GetTicks();
    int idx = s_bs_req_count;
    if (idx >= BS_REQ_MAX) return;
    strncpy(s_bs_req_entries[idx].name, expr_name, 63);
    s_bs_req_entries[idx].name[63] = '\0';
    s_bs_req_entries[idx].weight   = weight;
    s_bs_req_count = idx + 1;
    s_bs_req_pending = 1;
}

void vrm_viewer_clear_blendshapes(void)
{
    s_last_interact_ticks = SDL_GetTicks();
    s_bs_clear_pending = 1;
}

void vrm_viewer_set_subtitle(const char *text)
{
    settings_overlay_set_subtitle(text);
}

void vrm_viewer_reload_model(const char *model_path)
{
    if (!model_path || model_path[0] == '\0') {
        return;
    }
    strncpy(s_reload_model_path, model_path, RELOAD_PATH_MAX - 1);
    s_reload_model_path[RELOAD_PATH_MAX - 1] = '\0';
    s_reload_model_pending = 1;
}

void vrm_viewer_reload_scene(const char *scene_dir)
{
    if (!scene_dir) {
        s_reload_scene_dir[0] = '\0';
    } else {
        strncpy(s_reload_scene_dir, scene_dir, RELOAD_PATH_MAX - 1);
        s_reload_scene_dir[RELOAD_PATH_MAX - 1] = '\0';
    }
    s_reload_scene_pending = 1;
}

#ifdef VRM_WINDOW_WIDTH
#define INIT_WIDTH  VRM_WINDOW_WIDTH
#else
#define INIT_WIDTH  1024
#endif

#ifdef VRM_WINDOW_HEIGHT
#define INIT_HEIGHT VRM_WINDOW_HEIGHT
#else
#define INIT_HEIGHT 768
#endif

/* ================================================================== */
/*  Shader sources                                                     */
/* ================================================================== */

static const char *s_model_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_normal;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec4 a_bone_ids;\n"
    "layout(location=4) in vec4 a_bone_weights;\n"
    "\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "uniform int  u_skinned;\n"
    "uniform samplerBuffer u_bone_tbo;\n"
    "\n"
    "out vec2 v_uv;\n"
    "out vec3 v_normal;\n"
    "\n"
    "mat4 getBoneMatrix(int idx) {\n"
    "    int base = idx * 4;\n"
    "    return mat4(\n"
    "        texelFetch(u_bone_tbo, base + 0),\n"
    "        texelFetch(u_bone_tbo, base + 1),\n"
    "        texelFetch(u_bone_tbo, base + 2),\n"
    "        texelFetch(u_bone_tbo, base + 3)\n"
    "    );\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 pos = vec4(a_pos, 1.0);\n"
    "    vec4 norm4 = vec4(a_normal, 0.0);\n"
    "\n"
    "    if (u_skinned != 0) {\n"
    "        mat4 skin = mat4(0.0);\n"
    "        ivec4 ids = ivec4(a_bone_ids);\n"
    "        skin += a_bone_weights.x * getBoneMatrix(ids.x);\n"
    "        skin += a_bone_weights.y * getBoneMatrix(ids.y);\n"
    "        skin += a_bone_weights.z * getBoneMatrix(ids.z);\n"
    "        skin += a_bone_weights.w * getBoneMatrix(ids.w);\n"
    "        pos  = skin * pos;\n"
    "        norm4 = skin * norm4;\n"
    "    }\n"
    "\n"
    "    gl_Position = u_mvp * pos;\n"
    "    v_uv        = a_uv;\n"
    "    v_normal    = normalize(mat3(u_model) * norm4.xyz);\n"
    "}\n";

static const char *s_model_fs =
    "#version 140\n"
    "in vec2 v_uv;\n"
    "in vec3 v_normal;\n"
    "out vec4 frag_color;\n"
    "uniform vec4      u_base_color;\n"
    "uniform sampler2D u_texture;\n"
    "uniform int       u_has_texture;\n"
    "uniform vec3      u_light_dir;\n"
    "void main() {\n"
    "    vec4 base;\n"
    "    if (u_has_texture != 0) {\n"
    "        base = texture(u_texture, v_uv) * u_base_color;\n"
    "    } else {\n"
    "        base = u_base_color;\n"
    "    }\n"
    "    if (base.a < 0.1) discard;\n"
    "\n"
    "    vec3 n = normalize(v_normal);\n"
    "    float NdotL = dot(n, u_light_dir);\n"
    "    float lit = smoothstep(-0.02, 0.05, NdotL - 0.1);\n"
    "    vec3 shadow = base.rgb * vec3(0.62, 0.58, 0.72);\n"
    "    frag_color = vec4(mix(shadow, base.rgb, lit), base.a);\n"
    "}\n";

/* ---- grid shader ---- */
static const char *s_grid_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "uniform mat4 u_mvp;\n"
    "out vec3 v_pos;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "    v_pos = a_pos;\n"
    "}\n";

static const char *s_grid_fs =
    "#version 140\n"
    "in vec3 v_pos;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    float dist  = length(v_pos.xz) / 16.0;\n"
    "    float alpha = clamp(1.0 - dist * 0.7, 0.0, 1.0);\n"
    "    frag_color  = vec4(0.35, 0.40, 0.55, alpha * 0.35);\n"
    "}\n";

/* ---- background gradient shader ---- */
static const char *s_bg_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.999, 1.0);\n"
    "    v_uv = a_pos * 0.5 + 0.5;\n"
    "}\n";

static const char *s_bg_fs =
    "#version 140\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    vec3 col_bot = vec3(0.08, 0.08, 0.14);\n"
    "    vec3 col_top = vec3(0.12, 0.13, 0.22);\n"
    "    vec3 bg = mix(col_bot, col_top, v_uv.y);\n"
    "    frag_color = vec4(bg, 1.0);\n"
    "}\n";

/* ---- shadow depth shader (renders model from light's view) ---- */
static const char *s_shadow_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=3) in vec4 a_bone_ids;\n"
    "layout(location=4) in vec4 a_bone_weights;\n"
    "uniform mat4 u_light_mvp;\n"
    "uniform int  u_skinned;\n"
    "uniform samplerBuffer u_bone_tbo;\n"
    "mat4 getBoneMatrix(int idx) {\n"
    "    int base = idx * 4;\n"
    "    return mat4(\n"
    "        texelFetch(u_bone_tbo, base + 0),\n"
    "        texelFetch(u_bone_tbo, base + 1),\n"
    "        texelFetch(u_bone_tbo, base + 2),\n"
    "        texelFetch(u_bone_tbo, base + 3)\n"
    "    );\n"
    "}\n"
    "void main() {\n"
    "    vec4 pos = vec4(a_pos, 1.0);\n"
    "    if (u_skinned != 0) {\n"
    "        mat4 skin = mat4(0.0);\n"
    "        ivec4 ids = ivec4(a_bone_ids);\n"
    "        skin += a_bone_weights.x * getBoneMatrix(ids.x);\n"
    "        skin += a_bone_weights.y * getBoneMatrix(ids.y);\n"
    "        skin += a_bone_weights.z * getBoneMatrix(ids.z);\n"
    "        skin += a_bone_weights.w * getBoneMatrix(ids.w);\n"
    "        pos = skin * pos;\n"
    "    }\n"
    "    gl_Position = u_light_mvp * pos;\n"
    "}\n";

static const char *s_shadow_fs =
    "#version 140\n"
    "void main() {}\n";

/* ---- ground shadow receiver shader ---- */
static const char *s_ground_shadow_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_light_vp;\n"
    "uniform mat4 u_ground_mat;\n"
    "out vec4 v_light_pos;\n"
    "out vec3 v_world;\n"
    "void main() {\n"
    "    vec4 world = u_ground_mat * vec4(a_pos, 1.0);\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "    v_light_pos = u_light_vp * world;\n"
    "    v_world = world.xyz;\n"
    "}\n";

static const char *s_ground_shadow_fs =
    "#version 140\n"
    "in vec4 v_light_pos;\n"
    "in vec3 v_world;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_shadow_map;\n"
    "uniform vec3 u_center;\n"
    "uniform float u_radius;\n"
    "void main() {\n"
    "    vec3 proj = v_light_pos.xyz / v_light_pos.w;\n"
    "    proj = proj * 0.5 + 0.5;\n"
    "    if (proj.x < 0.0 || proj.x > 1.0 ||\n"
    "        proj.y < 0.0 || proj.y > 1.0 ||\n"
    "        proj.z > 1.0) discard;\n"
    "    float shadow = 0.0;\n"
    "    vec2 texel = 1.0 / vec2(textureSize(u_shadow_map, 0));\n"
    "    float bias = 0.003;\n"
    "    for (int x = -2; x <= 2; x++) {\n"
    "        for (int y = -2; y <= 2; y++) {\n"
    "            float d = texture(u_shadow_map, proj.xy + vec2(x,y)*texel).r;\n"
    "            shadow += (proj.z - bias > d) ? 1.0 : 0.0;\n"
    "        }\n"
    "    }\n"
    "    shadow /= 25.0;\n"
    "    if (shadow < 0.01) discard;\n"
    "    float dist = length(v_world.xz - u_center.xz) / u_radius;\n"
    "    float fade = smoothstep(1.0, 0.2, dist);\n"
    "    frag_color = vec4(0.0, 0.0, 0.05, shadow * 0.45 * fade);\n"
    "}\n";

/* ---- inverted-hull outline shader ---- */
static const char *s_outline_vs =
    "#version 140\n"
    "#extension GL_ARB_explicit_attrib_location : enable\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_normal;\n"
    "layout(location=3) in vec4 a_bone_ids;\n"
    "layout(location=4) in vec4 a_bone_weights;\n"
    "\n"
    "uniform mat4 u_mvp;\n"
    "uniform int  u_skinned;\n"
    "uniform float u_outline_width;\n"
    "uniform samplerBuffer u_bone_tbo;\n"
    "\n"
    "mat4 getBoneMatrix(int idx) {\n"
    "    int base = idx * 4;\n"
    "    return mat4(\n"
    "        texelFetch(u_bone_tbo, base + 0),\n"
    "        texelFetch(u_bone_tbo, base + 1),\n"
    "        texelFetch(u_bone_tbo, base + 2),\n"
    "        texelFetch(u_bone_tbo, base + 3)\n"
    "    );\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 pos = vec4(a_pos, 1.0);\n"
    "    vec4 norm4 = vec4(a_normal, 0.0);\n"
    "\n"
    "    if (u_skinned != 0) {\n"
    "        mat4 skin = mat4(0.0);\n"
    "        ivec4 ids = ivec4(a_bone_ids);\n"
    "        skin += a_bone_weights.x * getBoneMatrix(ids.x);\n"
    "        skin += a_bone_weights.y * getBoneMatrix(ids.y);\n"
    "        skin += a_bone_weights.z * getBoneMatrix(ids.z);\n"
    "        skin += a_bone_weights.w * getBoneMatrix(ids.w);\n"
    "        pos  = skin * pos;\n"
    "        norm4 = skin * norm4;\n"
    "    }\n"
    "\n"
    "    vec4 clip = u_mvp * pos;\n"
    "    vec3 cn = mat3(u_mvp) * normalize(norm4.xyz);\n"
    "    vec2 d = normalize(cn.xy) * u_outline_width * clip.w;\n"
    "    clip.xy += d;\n"
    "    gl_Position = clip;\n"
    "}\n";

static const char *s_outline_fs =
    "#version 140\n"
    "out vec4 frag_color;\n"
    "uniform vec4 u_outline_color;\n"
    "void main() {\n"
    "    frag_color = u_outline_color;\n"
    "}\n";

/* ================================================================== */
/*  GL helpers                                                         */
/* ================================================================== */

typedef struct {
    GLuint   vao;
    GLuint   vbo;
    GLuint   ebo;
    GLuint   tex_id;
    uint32_t index_count;
    uint32_t vertex_count;
    float    color[4];
    int      has_texture;
    int      has_bones;
    int      has_morph;
} gpu_mesh_t;

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
        fprintf(stderr, "[gl] shader compile error:\n%s\n", log);
    }
    return s;
}

static GLuint __link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = __compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = __compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    int ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] program link error:\n%s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static void __upload_mesh(gpu_mesh_t *gpu, const vrm_mesh_t *mesh,
                          const vrm_model_t *model)
{
    glGenVertexArrays(1, &gpu->vao);
    glGenBuffers(1, &gpu->vbo);
    glGenBuffers(1, &gpu->ebo);

    glBindVertexArray(gpu->vao);

    /* Vertex layout: pos(3)+normal(3)+uv(2)+boneId(4)+boneWt(4) = 16 floats */
    const GLsizei stride = 16 * (GLsizei)sizeof(float);

    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(mesh->vertex_count * 16 * sizeof(float)),
                 mesh->vertices,
                 mesh->morph_target_count > 0 ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);

    /* location 0 = position (3) */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(0);

    /* location 1 = normal (3) */
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    /* location 2 = uv (2) */
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    /* location 3 = bone IDs (4 floats, will be cast to int in shader) */
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          (void *)(8 * sizeof(float)));
    glEnableVertexAttribArray(3);

    /* location 4 = bone weights (4) */
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          (void *)(12 * sizeof(float)));
    glEnableVertexAttribArray(4);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(mesh->index_count * sizeof(uint32_t)),
                 mesh->indices, GL_STATIC_DRAW);

    gpu->index_count = mesh->index_count;
    gpu->vertex_count = mesh->vertex_count;
    memcpy(gpu->color, mesh->color, sizeof(float) * 4);
    gpu->has_bones = mesh->has_bones;
    gpu->has_morph = (mesh->morph_target_count > 0) ? 1 : 0;

    /* ---- Upload texture if available ---- */
    gpu->tex_id = 0;
    gpu->has_texture = 0;

    if (mesh->texture_index >= 0 &&
        (uint32_t)mesh->texture_index < model->texture_count) {
        const vrm_texture_t *tex = &model->textures[mesh->texture_index];
        if (tex->pixels && tex->width > 0 && tex->height > 0) {
            glGenTextures(1, &gpu->tex_id);
            glBindTexture(GL_TEXTURE_2D, gpu->tex_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         tex->width, tex->height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, tex->pixels);
            glGenerateMipmap(GL_TEXTURE_2D);
            gpu->has_texture = 1;
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    glBindVertexArray(0);
}

/* ---- ground grid ---- */
static GLuint s_grid_vao, s_grid_vbo;
static int    s_grid_vert_count;
static GLuint s_bg_vao, s_bg_vbo;

/* ---- shadow map ---- */
#define SHADOW_MAP_SIZE 2048
static GLuint s_shadow_fbo, s_shadow_depth_tex;
static GLuint s_ground_vao, s_ground_vbo, s_ground_ebo;

static void __create_grid(void)
{
    enum { DIVS = 40, MAX_VERTS = (DIVS + 1) * 4 };
    float buf[MAX_VERTS * 3];
    int n = 0;
    float half = 10.0f;
    float step = (half * 2.0f) / DIVS;

    for (int i = 0; i <= DIVS; i++) {
        float t = -half + i * step;
        buf[n++] = -half; buf[n++] = 0.0f; buf[n++] = t;
        buf[n++] =  half; buf[n++] = 0.0f; buf[n++] = t;
        buf[n++] = t; buf[n++] = 0.0f; buf[n++] = -half;
        buf[n++] = t; buf[n++] = 0.0f; buf[n++] =  half;
    }
    s_grid_vert_count = n / 3;

    glGenVertexArrays(1, &s_grid_vao);
    glGenBuffers(1, &s_grid_vbo);
    glBindVertexArray(s_grid_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(float)), buf, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

static void __create_bg_quad(void)
{
    float verts[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenVertexArrays(1, &s_bg_vao);
    glGenBuffers(1, &s_bg_vbo);
    glBindVertexArray(s_bg_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_bg_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

/**
 * @brief Create the shadow map FBO with a depth-only texture
 * @return none
 */
static void __create_shadow_fbo(void)
{
    glGenTextures(1, &s_shadow_depth_tex);
    glBindTexture(GL_TEXTURE_2D, s_shadow_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &s_shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, s_shadow_depth_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gl] shadow FBO incomplete!\n");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/**
 * @brief Create a unit XZ quad (Y=0, from -1..1) for ground shadow receiving
 * @return none
 */
static void __create_ground_quad(void)
{
    float verts[] = {
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
         1.0f, 0.0f,  1.0f,
        -1.0f, 0.0f,  1.0f,
    };
    uint32_t indices[] = { 0, 1, 2, 0, 2, 3 };

    glGenVertexArrays(1, &s_ground_vao);
    glGenBuffers(1, &s_ground_vbo);
    glGenBuffers(1, &s_ground_ebo);
    glBindVertexArray(s_ground_vao);

    glBindBuffer(GL_ARRAY_BUFFER, s_ground_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ground_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
}

/* ================================================================== */
/*  Public entry point                                                 */
/* ================================================================== */

/* Compare function for qsort of filename strings */
static int __cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ================================================================== */
/*  Idle behavior — helpers                                            */
/* ================================================================== */

/**
 * @brief Check if an animation name is one-shot (play once then return to idle)
 * @param[in] name animation name
 * @return 1 if one-shot, 0 if looping
 */
static int __is_oneshot(const char *name)
{
    for (int i = 0; s_oneshot_anims[i]; i++) {
        if (strcmp(name, s_oneshot_anims[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int __find_anim_idx(char **anim_names, int anim_name_count, const char *name)
{
    if (!anim_names || anim_name_count <= 0 || !name || name[0] == '\0') {
        return -1;
    }

    for (int i = 0; i < anim_name_count; i++) {
        if (strcmp(anim_names[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Generate a random float in [lo, hi]
 * @param[in] lo lower bound
 * @param[in] hi upper bound
 * @return random value
 */
static float __rand_range_f(float lo, float hi)
{
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

/**
 * @brief Generate a random interval in milliseconds from a [min, max] seconds range
 * @param[in] min_sec minimum seconds
 * @param[in] max_sec maximum seconds
 * @return interval in milliseconds
 */
static Uint32 __rand_interval_ms(float min_sec, float max_sec)
{
    return (Uint32)(__rand_range_f(min_sec, max_sec) * 1000.0f);
}

/**
 * @brief Detect Y-rotation needed so the model faces the camera (+Z).
 *
 * Uses the humanoid bone map to find "leftUpperArm" and "rightUpperArm"
 * rest-pose world positions. In a correctly oriented model (facing +Z
 * toward the camera at +Z), leftUpperArm.x should be greater than
 * rightUpperArm.x in world space.  If reversed, the model faces -Z
 * and needs a 180-degree Y rotation.
 *
 * @param[in] model loaded VRM model with humanoid_map populated
 * @return rotation angle in radians (0 or M_PI)
 */
static float __detect_model_facing(const vrm_model_t *model)
{
    if (!model->humanoid_map || model->humanoid_map_count == 0 ||
        !model->bones || model->bone_count == 0) {
        return 0.0f;
    }

    int left_bone  = -1;
    int right_bone = -1;

    for (uint32_t i = 0; i < model->humanoid_map_count; i++) {
        const char *hname = model->humanoid_map[i].humanoid_name;
        const char *nname = model->humanoid_map[i].node_name;
        if (!nname[0]) continue;

        int bone_idx = -1;
        for (uint32_t b = 0; b < model->bone_count; b++) {
            if (strcmp(model->bones[b].name, nname) == 0) {
                bone_idx = (int)b;
                break;
            }
        }
        if (bone_idx < 0) continue;

        if (strcmp(hname, "leftUpperArm") == 0)  left_bone  = bone_idx;
        if (strcmp(hname, "rightUpperArm") == 0) right_bone = bone_idx;
    }

    if (left_bone < 0 || right_bone < 0) {
        printf("[vrm] facing detect: missing arm bones, assuming front-facing\n");
        return 0.0f;
    }

    /* Compute rest-pose global transforms */
    uint32_t nb = model->bone_count;
    float *globals = (float *)malloc(nb * 16 * sizeof(float));
    if (!globals) return 0.0f;

    for (uint32_t i = 0; i < nb; i++) {
        if (model->bones[i].parent < 0) {
            memcpy(&globals[i * 16], model->bones[i].local_transform, 16 * sizeof(float));
        } else {
            int p = model->bones[i].parent;
            mat4_multiply(&globals[i * 16], &globals[p * 16],
                          model->bones[i].local_transform);
        }
    }

    /* World X of each arm = column 3 (translation), element [12] of the 4x4 */
    float lx = globals[left_bone  * 16 + 12];
    float rx = globals[right_bone * 16 + 12];

    free(globals);

    /*
     * Camera is at +Z looking in -Z.
     * Model facing +Z (toward camera): leftArm.x > rightArm.x  → OK
     * Model facing -Z (away):          leftArm.x < rightArm.x  → rotate 180°
     */
    if (lx < rx) {
        printf("[vrm] facing detect: model faces -Z (lx=%.3f rx=%.3f), rotating 180°\n",
               lx, rx);
        return (float)M_PI;
    }

    printf("[vrm] facing detect: model faces +Z (lx=%.3f rx=%.3f), no rotation\n",
           lx, rx);
    return 0.0f;
}

/**
 * @brief Weighted random selection of an idle animation from loaded pool
 * @param[in] anim_names loaded animation names array
 * @param[in] anim_name_count number of loaded animations
 * @param[in] last_pick index to skip for variety (-1 = no skip)
 * @return index into anim_names, or -1 if none available
 */
static int __pick_idle_anim(char **anim_names, int anim_name_count, int last_pick)
{
    int avail[IDLE_POOL_COUNT];
    int avail_weights[IDLE_POOL_COUNT];
    int avail_count  = 0;
    int total_weight = 0;

    for (int p = 0; p < IDLE_POOL_COUNT; p++) {
        for (int a = 0; a < anim_name_count; a++) {
            if (strcmp(anim_names[a], s_idle_pool[p].name) == 0) {
                if (a == last_pick) {
                    break;
                }
                avail[avail_count]        = a;
                avail_weights[avail_count] = s_idle_pool[p].weight;
                total_weight += s_idle_pool[p].weight;
                avail_count++;
                break;
            }
        }
    }

    /* Fallback: allow last_pick if it was the only available entry */
    if (avail_count == 0) {
        for (int p = 0; p < IDLE_POOL_COUNT; p++) {
            for (int a = 0; a < anim_name_count; a++) {
                if (strcmp(anim_names[a], s_idle_pool[p].name) == 0) {
                    avail[avail_count]        = a;
                    avail_weights[avail_count] = s_idle_pool[p].weight;
                    total_weight += s_idle_pool[p].weight;
                    avail_count++;
                    break;
                }
            }
        }
    }

    if (avail_count == 0 || total_weight == 0) {
        return -1;
    }

    int roll = rand() % total_weight;
    int sum  = 0;
    for (int i = 0; i < avail_count; i++) {
        sum += avail_weights[i];
        if (roll < sum) {
            return avail[i];
        }
    }
    return avail[avail_count - 1];
}

static int __pick_recovery_idle_anim(char **anim_names, int anim_name_count, int last_pick)
{
    int idx = __find_anim_idx(anim_names, anim_name_count, "idle_normal");
    if (idx >= 0) {
        return idx;
    }

    return __pick_idle_anim(anim_names, anim_name_count, last_pick);
}

/* ================================================================== */
/*  LVGL settings UI callbacks                                         */
/* ================================================================== */

/**
 * @brief LVGL settings callback: user selected a new model.
 * @param[in] path      Full path to the model file
 * @param[in] user_data Unused
 * @return none
 */
static void __on_model_change(const char *path, void *user_data)
{
    (void)user_data;
    vrm_viewer_reload_model(path);
}

/**
 * @brief LVGL settings callback: user selected an animation by index.
 *
 * Uses the same pending-flag mechanism as vrm_viewer_play_animation()
 * to pass the request safely to the render loop.
 */
static volatile int s_sui_anim_req_idx     = -1;
static volatile int s_sui_anim_req_pending = 0;

static void __on_anim_change(int index, void *user_data)
{
    (void)user_data;
    s_sui_anim_req_idx     = index;
    s_sui_anim_req_pending = 1;
}

/**
 * @brief LVGL settings callback: user selected a new scene.
 * @param[in] dir       Scene directory path ("" for no skybox)
 * @param[in] user_data Unused
 * @return none
 */
static void __on_scene_change(const char *dir, void *user_data)
{
    (void)user_data;
    vrm_viewer_reload_scene(dir);
}

/**
 * @brief LVGL settings callback: camera lock toggled.
 * @param[in] locked    1 = locked, 0 = unlocked
 * @param[in] user_data Unused
 * @return none
 */
static void __on_camera_lock(int locked, void *user_data)
{
    (void)user_data;
    (void)locked;
}

/**
 * @brief LVGL settings callback: subtitle toggled.
 * @param[in] enabled   1 = enabled, 0 = disabled
 * @param[in] user_data Unused
 * @return none
 */
static void __on_subtitle_toggle(int enabled, void *user_data)
{
    (void)user_data;
    (void)enabled;
}

/**
 * @brief LVGL settings callback: fullscreen toggled.
 * @param[in] enabled   1 = fullscreen, 0 = windowed
 * @param[in] user_data Pointer to SDL_Window
 * @return none
 */
static void __on_fullscreen_toggle(int enabled, void *user_data)
{
    SDL_Window *win = (SDL_Window *)user_data;
    if (!win) {
        return;
    }
    SDL_SetWindowFullscreen(win,
        enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static float s_spec_yaw      = 0.0f;
static float s_spec_pitch    = 0.15f;
static float s_spec_dist     = 3.0f;
static float s_spec_target[3] = {0};
static int   s_spec_inited   = 0;

/**
 * @brief Settings callback: spectator mode toggled.
 * @param[in] enabled   1 = spectator on, 0 = off
 * @param[in] user_data unused
 * @return none
 */
static void __on_spectator_toggle(int enabled, void *user_data)
{
    (void)user_data;
    if (enabled) {
        s_spec_inited = 0;
    }
}

/**
 * @brief Extract the directory portion of a file path.
 * @param[out] out      Output buffer
 * @param[in]  out_size Buffer size
 * @param[in]  path     File path
 * @return none
 */
static void __path_dirname(char *out, size_t out_size, const char *path)
{
    char tmp[RELOAD_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *d = dirname(tmp);
    strncpy(out, d, out_size - 1);
    out[out_size - 1] = '\0';
}

/**
 * @brief Extract the filename portion of a file path.
 * @param[out] out      Output buffer
 * @param[in]  out_size Buffer size
 * @param[in]  path     File path
 * @return none
 */
static void __path_basename(char *out, size_t out_size, const char *path)
{
    char tmp[RELOAD_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *b = basename(tmp);
    strncpy(out, b, out_size - 1);
    out[out_size - 1] = '\0';
}

int vrm_viewer_run(const char *model_path, const char *vrma_dir)
{
    /* ---- load model ---- */
    vrm_model_t model;
    printf("[vrm_viewer] loading model: %s\n", model_path);
    if (vrm_model_load(&model, model_path) != 0) {
        fprintf(stderr, "[vrm_viewer] failed to load model\n");
        return -1;
    }

    /* ---- scan vrma directory and load all .vrma files ---- */
    uint32_t active_anim = 0;
    int has_anim = 0;

    /* Store animation names for display */
    #define MAX_VRMA_FILES 64
    char *anim_names[MAX_VRMA_FILES];
    int anim_name_count = 0;
    uint32_t first_vrma_anim = 0; /* index of the first VRMA animation in model */

    if (vrma_dir && vrma_dir[0] != '\0') {
        /* Collect .vrma filenames */
        char *vrma_files[MAX_VRMA_FILES];
        int vrma_count = 0;

        DIR *dir = opendir(vrma_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && vrma_count < MAX_VRMA_FILES) {
                const char *name = entry->d_name;
                size_t len = strlen(name);
                if (len > 5 && strcasecmp(name + len - 5, ".vrma") == 0) {
                    vrma_files[vrma_count] = strdup(name);
                    vrma_count++;
                }
            }
            closedir(dir);
        }

        /* Sort alphabetically */
        if (vrma_count > 1)
            qsort(vrma_files, vrma_count, sizeof(char *), __cmp_str);

        printf("[vrm_viewer] found %d VRMA files in %s\n", vrma_count, vrma_dir);
        first_vrma_anim = model.animation_count;

        /* Load each VRMA */
        for (int i = 0; i < vrma_count; i++) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", vrma_dir, vrma_files[i]);
            printf("[vrm_viewer]   [%d] %s ... ", i, vrma_files[i]);
            if (vrm_load_vrma(&model, full_path) == 0) {
                printf("OK (duration=%.1fs)\n", model.animations[model.animation_count - 1].duration);
                /* Store display name (strip .vrma extension) */
                char *dname = strdup(vrma_files[i]);
                size_t dlen = strlen(dname);
                if (dlen > 5) dname[dlen - 5] = '\0';
                anim_names[anim_name_count++] = dname;
            } else {
                printf("FAILED\n");
            }
            free(vrma_files[i]);
        }

        if (anim_name_count > 0) {
            int idle_idx = __find_anim_idx(anim_names, anim_name_count, "idle_normal");
            active_anim = (idle_idx >= 0)
                ? (first_vrma_anim + (uint32_t)idle_idx)
                : first_vrma_anim;
            has_anim = 1;
        }
    }

    /* If model has embedded animations and no VRMA was loaded, use first embedded */
    if (!has_anim && model.animation_count > 0) {
        active_anim = 0;
        has_anim = 1;
    }

    uint32_t total_anims = model.animation_count;

    /* ---- SDL2 + OpenGL init ---- */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[vrm_viewer] SDL_Init failed: %s\n", SDL_GetError());
        vrm_model_free(&model);
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#ifdef VRM_FULLSCREEN
    win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif

    /* Try GL contexts in order: 3.3 Core → 3.1 + MSAA → 3.1 without MSAA */
    typedef struct { int major, minor, profile, msaa; } gl_try_t;
    gl_try_t gl_tries[] = {
        { 3, 3, SDL_GL_CONTEXT_PROFILE_CORE, 4 },
        { 3, 1, 0, 4 },
        { 3, 1, 0, 0 },
    };

    SDL_Window *window = NULL;
    SDL_GLContext gl_ctx = NULL;
    for (int ti = 0; ti < 3; ti++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl_tries[ti].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl_tries[ti].minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gl_tries[ti].profile);
        if (gl_tries[ti].msaa > 0) {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, gl_tries[ti].msaa);
        } else {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        }
        window = SDL_CreateWindow(
            "VRM Viewer — TuyaOpen",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            INIT_WIDTH, INIT_HEIGHT, win_flags);
        if (!window) continue;
        gl_ctx = SDL_GL_CreateContext(window);
        if (gl_ctx) {
            printf("[vrm_viewer] GL context: %d.%d %s MSAA=%d\n",
                   gl_tries[ti].major, gl_tries[ti].minor,
                   gl_tries[ti].profile ? "core" : "compat",
                   gl_tries[ti].msaa);
            break;
        }
        SDL_DestroyWindow(window);
        window = NULL;
    }
    if (!window || !gl_ctx) {
        fprintf(stderr, "[vrm_viewer] Failed to create GL context: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit(); vrm_model_free(&model); return -1;
    }

    SDL_GL_SetSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "[vrm_viewer] glewInit failed\n");
        SDL_GL_DeleteContext(gl_ctx); SDL_DestroyWindow(window);
        SDL_Quit(); vrm_model_free(&model); return -1;
    }

    printf("[vrm_viewer] GL version : %s\n", glGetString(GL_VERSION));
    printf("[vrm_viewer] GL renderer: %s\n", glGetString(GL_RENDERER));

    /* ---- compile shaders ---- */
    GLuint model_prog = __link_program(s_model_vs, s_model_fs);
    GLuint grid_prog  = __link_program(s_grid_vs, s_grid_fs);
    GLuint bg_prog    = __link_program(s_bg_vs, s_bg_fs);
    GLuint shadow_prog = __link_program(s_shadow_vs, s_shadow_fs);
    GLuint ground_shadow_prog = __link_program(s_ground_shadow_vs,
                                               s_ground_shadow_fs);

    /* ---- upload meshes ---- */
    gpu_mesh_t *gpu = (gpu_mesh_t *)calloc(model.mesh_count, sizeof(gpu_mesh_t));
    for (uint32_t i = 0; i < model.mesh_count; i++)
        __upload_mesh(&gpu[i], &model.meshes[i], &model);

    printf("[vrm_viewer] uploaded %u meshes to GPU\n", model.mesh_count);

    __create_grid();
    __create_bg_quad();
    __create_shadow_fbo();
    __create_ground_quad();

    /* ---- Skybox (cubemap) ---- */
    skybox_t skybox;
    memset(&skybox, 0, sizeof(skybox));
#ifdef VRM_SCENE_DIR
    if (VRM_SCENE_DIR[0] != '\0') {
        printf("[vrm_viewer] loading skybox from: %s\n", VRM_SCENE_DIR);
        if (skybox_init(&skybox, VRM_SCENE_DIR) != 0) {
            printf("[vrm_viewer] skybox not available — using gradient background\n");
        }
    }
#endif

    /* ---- Bone matrix TBO ---- */
    GLuint bone_tbo_tex = 0, bone_tbo_buf = 0;
    float *bone_matrices = NULL;
    float *target_bone_matrices = NULL;
    float *blend_from_matrices = NULL;
    uint32_t bone_count = model.bone_count;
    Uint32 anim_blend_start_ticks = 0;
    int anim_blend_active = 0;

    if (bone_count > 0) {
        bone_matrices = (float *)calloc(bone_count * 16, sizeof(float));
        target_bone_matrices = (float *)calloc(bone_count * 16, sizeof(float));
        blend_from_matrices = (float *)calloc(bone_count * 16, sizeof(float));

        /* Initialize to rest pose */
        vrm_rest_pose_matrices(&model, bone_matrices);
        memcpy(target_bone_matrices, bone_matrices, bone_count * 16 * sizeof(float));
        memcpy(blend_from_matrices, bone_matrices, bone_count * 16 * sizeof(float));

        glGenBuffers(1, &bone_tbo_buf);
        glBindBuffer(GL_TEXTURE_BUFFER, bone_tbo_buf);
        glBufferData(GL_TEXTURE_BUFFER,
                     (GLsizeiptr)(bone_count * 16 * sizeof(float)),
                     bone_matrices, GL_DYNAMIC_DRAW);

        glGenTextures(1, &bone_tbo_tex);
        glBindTexture(GL_TEXTURE_BUFFER, bone_tbo_tex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, bone_tbo_buf);

        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);

        printf("[vrm_viewer] bone TBO: %u bones, %zu bytes\n",
               bone_count, (size_t)(bone_count * 16 * sizeof(float)));
    }

    /* ---- auto-detect model facing and compute Y correction ---- */
    float model_y_rot_base = __detect_model_facing(&model);
    float model_y_rot = model_y_rot_base;
    float model_x_rot = 0.0f;

    /* ---- camera state ---- */
    float cam_yaw   = 0.0f;
    float cam_pitch = 0.15f;
    float cam_dist  = model.extent * 2.0f;
    if (cam_dist < 1.0f) cam_dist = 3.0f;
    float cam_target[3] = {
        model.center[0], model.center[1], model.center[2]
    };

    /* ---- GL state ---- */
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_CULL_FACE);

    /* White fallback texture */
    GLuint white_tex;
    {
        uint8_t white_pixel[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &white_tex);
        glBindTexture(GL_TEXTURE_2D, white_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* ---- Morph target info ---- */
    int total_morph_targets = 0;
    for (uint32_t i = 0; i < model.mesh_count; i++)
        total_morph_targets += (int)model.meshes[i].morph_target_count;
    printf("[vrm_viewer] total morph targets across all meshes: %d\n", total_morph_targets);
    printf("[vrm_viewer] expressions: %u\n", model.expression_count);

    /* ---- Emotion system ---- */
    emotion_ctx_t emo_ctx;
    emotion_init(&emo_ctx, &model);
    emotion_id_t cur_emo = EMOTION_NEUTRAL;
    Uint32 last_frame_ticks = SDL_GetTicks();

    /* ---- Audio-driven lip sync ---- */
    lip_sync_init(&s_lip_sync_ctx, 16000);
    emotion_set_lip_sync(&emo_ctx, &s_lip_sync_ctx);

    /* Register the chained consumer so decoded PCM is also fed to lip sync.
     * This replaces the default speaker consumer with a wrapper that both
     * outputs audio to the hardware AND analyses energy for lip sync. */
    tuya_ai_player_set_consumer(&s_lip_sync_consumer);

    /* Publish the emotion context so vrm_viewer_set_speaking() can reach it */
    s_emo_ctx = &emo_ctx;

    /* ---- Spring bone physics ---- */
    spring_bone_ctx_t spring_ctx;
    spring_bone_init(&spring_ctx, &model);

    /* ---- Settings overlay (rendered in this GL window) ---- */
    {
        char model_dir[RELOAD_PATH_MAX];
        char model_basename[256];
        char scene_parent[RELOAD_PATH_MAX];
        char scene_basename[256];

        __path_dirname(model_dir, sizeof(model_dir), model_path);
        __path_basename(model_basename, sizeof(model_basename), model_path);

        scene_parent[0] = '\0';
        scene_basename[0] = '\0';
#ifdef VRM_SCENE_PARENT_DIR
        if (VRM_SCENE_PARENT_DIR[0] != '\0') {
            snprintf(scene_parent, sizeof(scene_parent), "%s", VRM_SCENE_PARENT_DIR);
        } else
#endif
        {
#ifdef VRM_SCENE_DIR
            if (VRM_SCENE_DIR[0] != '\0') {
                __path_dirname(scene_parent, sizeof(scene_parent), VRM_SCENE_DIR);
            }
#endif
        }
#ifdef VRM_SCENE_DIR
        if (VRM_SCENE_DIR[0] != '\0') {
            __path_basename(scene_basename, sizeof(scene_basename), VRM_SCENE_DIR);
        }
#endif

        int init_fullscreen = (SDL_GetWindowFlags(window) &
                               SDL_WINDOW_FULLSCREEN_DESKTOP) ? 1 : 0;

        settings_overlay_cfg_t sui_cfg = {
            .model_dir        = model_dir,
            .current_model    = model_basename,
            .scene_parent_dir = scene_parent,
            .current_scene    = scene_basename,
            .anim_names       = anim_names,
            .anim_count       = anim_name_count,
            .active_anim      = 0,
            .camera_locked    = 0,
            .subtitle_enabled = 1,
            .fullscreen       = init_fullscreen,
            .on_model_change  = __on_model_change,
            .on_anim_change   = __on_anim_change,
            .on_scene_change  = __on_scene_change,
            .on_camera_lock   = __on_camera_lock,
            .on_subtitle      = __on_subtitle_toggle,
            .on_fullscreen    = __on_fullscreen_toggle,
            .on_spectator     = __on_spectator_toggle,
            .user_data        = window,
        };

        settings_overlay_init(&sui_cfg);
    }

    /* ---- main loop ---- */
    bool running   = true;
    bool dragging  = false;
    bool paused    = false;
    int  last_mx   = 0, last_my = 0;
    int  win_w     = INIT_WIDTH, win_h = INIT_HEIGHT;

    Uint32 start_ticks = SDL_GetTicks();

    /* ---- idle behavior state ---- */
    srand((unsigned)SDL_GetTicks());
    s_last_interact_ticks = SDL_GetTicks();
    int     idle_last_pick    = -1;
    int     idle_is_oneshot   = 0;
    int     idle_was_active   = 0;
    Uint32  idle_next_switch  = SDL_GetTicks() + __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
    Uint32  idle_next_emotion = SDL_GetTicks() + __rand_interval_ms(IDLE_EMOTION_MIN_SEC, IDLE_EMOTION_MAX_SEC);
    Uint32  idle_emotion_revert = 0;

#define START_ANIM_BLEND()                                                     \
    do {                                                                       \
        if (bone_count > 0 && bone_matrices && blend_from_matrices) {          \
            memcpy(blend_from_matrices, bone_matrices,                         \
                   bone_count * 16 * sizeof(float));                           \
            anim_blend_start_ticks = SDL_GetTicks();                           \
            anim_blend_active = 1;                                             \
        }                                                                      \
    } while (0)

    while (running) {
        /* ---- events ---- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (settings_overlay_handle_event(&ev, win_w, win_h)) {
                continue;
            }
            int cam_locked = settings_overlay_camera_locked();
            int spectator  = settings_overlay_spectator();

            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    win_w = ev.window.data1;
                    win_h = ev.window.data2;
                    glViewport(0, 0, win_w, win_h);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT ||
                    ev.button.button == SDL_BUTTON_RIGHT ||
                    ev.button.button == SDL_BUTTON_MIDDLE) {
                    dragging = true;
                    last_mx = ev.button.x; last_my = ev.button.y;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT ||
                    ev.button.button == SDL_BUTTON_RIGHT ||
                    ev.button.button == SDL_BUTTON_MIDDLE) {
                    dragging = false;
                }
                break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    int dx = ev.motion.x - last_mx;
                    int dy = ev.motion.y - last_my;
                    if (spectator) {
                        Uint32 btns = SDL_GetMouseState(NULL, NULL);
                        int rmb = btns & SDL_BUTTON(SDL_BUTTON_RIGHT);
                        int mmb = btns & SDL_BUTTON(SDL_BUTTON_MIDDLE);
                        if (rmb || mmb) {
                            float pan_scale = s_spec_dist * 0.002f;
                            float rx = cosf(s_spec_yaw);
                            float rz = -sinf(s_spec_yaw);
                            s_spec_target[0] -= rx * (float)dx * pan_scale;
                            s_spec_target[2] -= rz * (float)dx * pan_scale;
                            s_spec_target[1] += (float)dy * pan_scale;
                        } else {
                            s_spec_yaw   += (float)dx * 0.005f;
                            s_spec_pitch -= (float)dy * 0.005f;
                            if (s_spec_pitch >  1.55f) s_spec_pitch =  1.55f;
                            if (s_spec_pitch < -1.55f) s_spec_pitch = -1.55f;
                        }
                    } else if (!cam_locked) {
                        model_y_rot += dx * 0.005f;
                    }
                    last_mx = ev.motion.x; last_my = ev.motion.y;
                }
                break;
            case SDL_MOUSEWHEEL:
                if (spectator) {
                    float zoom = (ev.wheel.y > 0) ? 0.85f : 1.18f;
                    s_spec_dist *= zoom;
                    if (s_spec_dist < 0.1f) s_spec_dist = 0.1f;
                    if (s_spec_dist > 200.0f) s_spec_dist = 200.0f;
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (ev.key.keysym.sym == SDLK_r) {
                    model_y_rot = model_y_rot_base;
                    model_x_rot = 0.0f;
                }
                if (ev.key.keysym.sym == SDLK_SPACE) paused = !paused;
                if (ev.key.keysym.sym == SDLK_LEFT || ev.key.keysym.sym == SDLK_RIGHT) {
                    int anim_total = anim_name_count > 0 ? anim_name_count : (int)total_anims;
                    if (has_anim && anim_total > 0) {
                        int step = (ev.key.keysym.sym == SDLK_RIGHT) ? 1 : -1;
                        int anim_idx = 0;
                        START_ANIM_BLEND();

                        if (anim_name_count > 0) {
                            if (active_anim >= first_vrma_anim) {
                                anim_idx = (int)(active_anim - first_vrma_anim);
                            }
                            anim_idx = (anim_idx + step + anim_name_count) % anim_name_count;
                            active_anim = first_vrma_anim + (uint32_t)anim_idx;
                            idle_last_pick = anim_idx;
                            idle_is_oneshot = __is_oneshot(anim_names[anim_idx]);
                        } else {
                            anim_idx = ((int)active_anim + step + anim_total) % anim_total;
                            active_anim = (uint32_t)anim_idx;
                            idle_is_oneshot = 0;
                        }

                        has_anim = 1;
                        start_ticks = SDL_GetTicks();
                        s_last_interact_ticks = start_ticks;
                        idle_next_switch = start_ticks +
                            __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                    }
                }
                /* E key: cycle through emotions */
                if (ev.key.keysym.sym == SDLK_e) {
                    cur_emo = (emotion_id_t)((int)cur_emo + 1);
                    if ((int)cur_emo >= EMOTION__COUNT) cur_emo = EMOTION_NEUTRAL;
                    emotion_set(&emo_ctx, cur_emo);
                }
                /* F1-F6: direct emotion set */
                if (ev.key.keysym.sym >= SDLK_F1 && ev.key.keysym.sym <= SDLK_F6) {
                    emotion_id_t eid = (emotion_id_t)(ev.key.keysym.sym - SDLK_F1);
                    if ((int)eid < EMOTION__COUNT) {
                        cur_emo = eid;
                        emotion_set(&emo_ctx, cur_emo);
                    }
                }
                /* B key: toggle auto-blink */
                if (ev.key.keysym.sym == SDLK_b) {
                    emo_ctx.auto_blink = !emo_ctx.auto_blink;
                }
                /* T key: toggle speaking */
                if (ev.key.keysym.sym == SDLK_t) {
                    emotion_set_speaking(&emo_ctx, !emo_ctx.speaking);
                }
                /* P key: toggle spring bone physics */
                if (ev.key.keysym.sym == SDLK_p) {
                    spring_ctx.enabled = !spring_ctx.enabled;
                    if (!spring_ctx.enabled) spring_ctx.initialized = 0;
                }
                /* Spring bone parameter tuning:
                 *   A / S : decrease / increase stiffness
                 *   D / F : decrease / increase drag
                 *   G / H : decrease / increase gravity power
                 *   J / K : decrease / increase hit radius
                 *   L     : reset parameters to VRM defaults
                 */
                if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_s) {
                    float delta = (ev.key.keysym.sym == SDLK_s) ? 0.1f : -0.1f;
                    for (int ci = 0; ci < spring_ctx.chain_count; ci++) {
                        for (int ji = 0; ji < spring_ctx.chains[ci].joint_count; ji++) {
                            spring_ctx.chains[ci].joints[ji].stiffness += delta;
                            if (spring_ctx.chains[ci].joints[ji].stiffness < 0.0f)
                                spring_ctx.chains[ci].joints[ji].stiffness = 0.0f;
                        }
                    }
                    float val = spring_ctx.chain_count > 0 && spring_ctx.chains[0].joint_count > 0
                        ? spring_ctx.chains[0].joints[0].stiffness : 0;
                    (void)val;
                }
                if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_f) {
                    float delta = (ev.key.keysym.sym == SDLK_f) ? 0.05f : -0.05f;
                    for (int ci = 0; ci < spring_ctx.chain_count; ci++) {
                        for (int ji = 0; ji < spring_ctx.chains[ci].joint_count; ji++) {
                            spring_ctx.chains[ci].joints[ji].drag_force += delta;
                            if (spring_ctx.chains[ci].joints[ji].drag_force < 0.0f)
                                spring_ctx.chains[ci].joints[ji].drag_force = 0.0f;
                            if (spring_ctx.chains[ci].joints[ji].drag_force > 1.0f)
                                spring_ctx.chains[ci].joints[ji].drag_force = 1.0f;
                        }
                    }
                    float val = spring_ctx.chain_count > 0 && spring_ctx.chains[0].joint_count > 0
                        ? spring_ctx.chains[0].joints[0].drag_force : 0;
                    (void)val;
                }
                if (ev.key.keysym.sym == SDLK_g || ev.key.keysym.sym == SDLK_h) {
                    float delta = (ev.key.keysym.sym == SDLK_h) ? 0.05f : -0.05f;
                    for (int ci = 0; ci < spring_ctx.chain_count; ci++) {
                        for (int ji = 0; ji < spring_ctx.chains[ci].joint_count; ji++) {
                            spring_ctx.chains[ci].joints[ji].gravity_power += delta;
                            if (spring_ctx.chains[ci].joints[ji].gravity_power < 0.0f)
                                spring_ctx.chains[ci].joints[ji].gravity_power = 0.0f;
                        }
                    }
                    float val = spring_ctx.chain_count > 0 && spring_ctx.chains[0].joint_count > 0
                        ? spring_ctx.chains[0].joints[0].gravity_power : 0;
                    (void)val;
                }
                if (ev.key.keysym.sym == SDLK_j || ev.key.keysym.sym == SDLK_k) {
                    float delta = (ev.key.keysym.sym == SDLK_k) ? 0.005f : -0.005f;
                    for (int ci = 0; ci < spring_ctx.chain_count; ci++) {
                        for (int ji = 0; ji < spring_ctx.chains[ci].joint_count; ji++) {
                            spring_ctx.chains[ci].joints[ji].hit_radius += delta;
                            if (spring_ctx.chains[ci].joints[ji].hit_radius < 0.0f)
                                spring_ctx.chains[ci].joints[ji].hit_radius = 0.0f;
                        }
                    }
                    float val = spring_ctx.chain_count > 0 && spring_ctx.chains[0].joint_count > 0
                        ? spring_ctx.chains[0].joints[0].hit_radius : 0;
                    (void)val;
                }
                if (ev.key.keysym.sym == SDLK_l) {
                    /* Reset spring bone params to VRM defaults */
                    for (int ci = 0; ci < spring_ctx.chain_count; ci++) {
                        if ((uint32_t)ci >= spring_ctx.model->spring_group_count) break;
                        vrm_spring_group_t *sg = &spring_ctx.model->spring_groups[ci];
                        for (int ji = 0; ji < spring_ctx.chains[ci].joint_count; ji++) {
                            if ((uint32_t)ji >= sg->joint_count) break;
                            spring_ctx.chains[ci].joints[ji].stiffness     = sg->joints[ji].stiffness;
                            spring_ctx.chains[ci].joints[ji].drag_force    = sg->joints[ji].drag_force;
                            spring_ctx.chains[ci].joints[ji].gravity_power = sg->joints[ji].gravity_power;
                            spring_ctx.chains[ci].joints[ji].hit_radius    = sg->joints[ji].hit_radius;
                        }
                    }
                    spring_ctx.initialized = 0;
                }
                /* F11: toggle fullscreen */
                if (ev.key.keysym.sym == SDLK_F11) {
                    Uint32 flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window, 0);
                    } else {
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }
                /* 1-9: raw expression toggle (for debugging) */
                if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_9) {
                    int expr_idx = ev.key.keysym.sym - SDLK_1;
                    if ((uint32_t)expr_idx < model.expression_count) {
                        float cur = model.expression_weights[expr_idx];
                        float nw = (cur < 0.5f) ? 1.0f : 0.0f;
                        vrm_set_expression_weight(&model, expr_idx, nw);
                    }
                }
                break;
            default: break;
            }
        }

        /* ---- handle pending animation switch request ---- */
        if (s_anim_req_pending) {
            s_anim_req_pending = 0;
            int req_return_to_idle = s_anim_req_return_to_idle;
            s_anim_req_return_to_idle = 0;
            for (int ni = 0; ni < anim_name_count; ni++) {
                if (strcmp(anim_names[ni], s_anim_req_name) == 0) {
                    START_ANIM_BLEND();
                    active_anim     = first_vrma_anim + (uint32_t)ni;
                    start_ticks     = SDL_GetTicks();
                    has_anim        = 1;
                    idle_last_pick  = ni;
                    idle_is_oneshot = req_return_to_idle || __is_oneshot(anim_names[ni]);
                    idle_next_switch = SDL_GetTicks() +
                        __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                    break;
                }
            }
        }

        /* ---- handle pending emotion request ---- */
        if (s_emo_req_pending) {
            s_emo_req_pending = 0;
            int eid = emotion_find_by_name(s_emo_req_name);
            if (eid >= 0) {
                emotion_clear_overrides(&emo_ctx);
                emotion_set_intensity(&emo_ctx, s_emo_req_intensity);
                emotion_set_ex(&emo_ctx, (emotion_id_t)eid, s_emo_req_speed);
                cur_emo = (emotion_id_t)eid;
            } else {
                fprintf(stderr, "[vrm] emotion not found: %s\n", s_emo_req_name);
            }
        }

        /* ---- handle delayed speaking stop ---- */
        if (s_speaking_stop_pending) {
            Uint32 elapsed = SDL_GetTicks() - s_speaking_stop_ticks;
            if (elapsed >= SPEAKING_FADE_MS) {
                s_speaking_stop_pending = 0;
                emotion_set_speaking(&emo_ctx, 0);
            }
        }

        /* ---- handle pending blendshape overrides ---- */
        if (s_bs_clear_pending) {
            s_bs_clear_pending = 0;
            emotion_clear_overrides(&emo_ctx);
        }
        if (s_bs_req_pending) {
            s_bs_req_pending = 0;
            int count = s_bs_req_count;
            for (int bi = 0; bi < count; bi++) {
                int idx = vrm_find_expression(&model, s_bs_req_entries[bi].name);
                if (idx >= 0) {
                    emotion_set_override(&emo_ctx, idx, s_bs_req_entries[bi].weight);
                }
            }
            s_bs_req_count = 0;
        }

        /* ---- handle model reload ---- */
        if (s_reload_model_pending) {
            s_reload_model_pending = 0;

            /* Free GPU meshes */
            for (uint32_t mi = 0; mi < model.mesh_count; mi++) {
                if (gpu[mi].tex_id) { glDeleteTextures(1, &gpu[mi].tex_id); }
                glDeleteVertexArrays(1, &gpu[mi].vao);
                glDeleteBuffers(1, &gpu[mi].vbo);
                glDeleteBuffers(1, &gpu[mi].ebo);
            }
            free(gpu);
            gpu = NULL;

            /* Free bone TBO */
            if (bone_tbo_tex) { glDeleteTextures(1, &bone_tbo_tex); bone_tbo_tex = 0; }
            if (bone_tbo_buf) { glDeleteBuffers(1, &bone_tbo_buf); bone_tbo_buf = 0; }
            free(bone_matrices); bone_matrices = NULL;
            free(target_bone_matrices); target_bone_matrices = NULL;
            free(blend_from_matrices); blend_from_matrices = NULL;

            /* Shutdown emotion & spring bones */
            s_emo_ctx = NULL;
            emotion_shutdown(&emo_ctx);
            spring_bone_shutdown(&spring_ctx);

            /* Free old model & animation names */
            vrm_model_free(&model);
            for (int ai = 0; ai < anim_name_count; ai++) {
                free(anim_names[ai]);
            }
            anim_name_count = 0;
            has_anim = 0;
            active_anim = 0;
            first_vrma_anim = 0;

            /* Load new model */
            if (vrm_model_load(&model, s_reload_model_path) != 0) {
                fprintf(stderr, "[vrm] reload failed\n");
                running = false;
                continue;
            }

            /* Reload VRMA animations from the same directory */
            if (vrma_dir && vrma_dir[0] != '\0') {
                char *vrma_files_tmp[MAX_VRMA_FILES];
                int vrma_cnt = 0;
                DIR *ddir = opendir(vrma_dir);
                if (ddir) {
                    struct dirent *de;
                    while ((de = readdir(ddir)) != NULL && vrma_cnt < MAX_VRMA_FILES) {
                        size_t nl = strlen(de->d_name);
                        if (nl > 5 && strcasecmp(de->d_name + nl - 5, ".vrma") == 0) {
                            vrma_files_tmp[vrma_cnt++] = strdup(de->d_name);
                        }
                    }
                    closedir(ddir);
                }
                if (vrma_cnt > 1) {
                    qsort(vrma_files_tmp, vrma_cnt, sizeof(char *), __cmp_str);
                }
                first_vrma_anim = model.animation_count;
                for (int vi = 0; vi < vrma_cnt; vi++) {
                    char fp[1024];
                    snprintf(fp, sizeof(fp), "%s/%s", vrma_dir, vrma_files_tmp[vi]);
                    if (vrm_load_vrma(&model, fp) == 0) {
                        char *dn = strdup(vrma_files_tmp[vi]);
                        size_t dl = strlen(dn);
                        if (dl > 5) { dn[dl - 5] = '\0'; }
                        anim_names[anim_name_count++] = dn;
                    }
                    free(vrma_files_tmp[vi]);
                }
                if (anim_name_count > 0) {
                    int idle_idx = __find_anim_idx(anim_names, anim_name_count, "idle_normal");
                    active_anim = (idle_idx >= 0)
                        ? (first_vrma_anim + (uint32_t)idle_idx)
                        : first_vrma_anim;
                    has_anim = 1;
                }
            }
            if (!has_anim && model.animation_count > 0) {
                active_anim = 0;
                has_anim = 1;
            }
            total_anims = model.animation_count;

            /* Re-upload meshes */
            gpu = (gpu_mesh_t *)calloc(model.mesh_count, sizeof(gpu_mesh_t));
            for (uint32_t mi = 0; mi < model.mesh_count; mi++) {
                __upload_mesh(&gpu[mi], &model.meshes[mi], &model);
            }

            /* Recreate bone TBO */
            bone_count = model.bone_count;
            anim_blend_active = 0;
            if (bone_count > 0) {
                bone_matrices = (float *)calloc(bone_count * 16, sizeof(float));
                target_bone_matrices = (float *)calloc(bone_count * 16, sizeof(float));
                blend_from_matrices = (float *)calloc(bone_count * 16, sizeof(float));
                vrm_rest_pose_matrices(&model, bone_matrices);
                memcpy(target_bone_matrices, bone_matrices, bone_count * 16 * sizeof(float));
                memcpy(blend_from_matrices, bone_matrices, bone_count * 16 * sizeof(float));

                glGenBuffers(1, &bone_tbo_buf);
                glBindBuffer(GL_TEXTURE_BUFFER, bone_tbo_buf);
                glBufferData(GL_TEXTURE_BUFFER,
                             (GLsizeiptr)(bone_count * 16 * sizeof(float)),
                             bone_matrices, GL_DYNAMIC_DRAW);
                glGenTextures(1, &bone_tbo_tex);
                glBindTexture(GL_TEXTURE_BUFFER, bone_tbo_tex);
                glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, bone_tbo_buf);
                glBindTexture(GL_TEXTURE_BUFFER, 0);
                glBindBuffer(GL_TEXTURE_BUFFER, 0);
            }

            /* Reinit emotion & spring bones */
            emotion_init(&emo_ctx, &model);
            emotion_set_lip_sync(&emo_ctx, &s_lip_sync_ctx);
            s_emo_ctx = &emo_ctx;
            spring_bone_init(&spring_ctx, &model);

            /* Recalculate morph info */
            total_morph_targets = 0;
            for (uint32_t mi = 0; mi < model.mesh_count; mi++) {
                total_morph_targets += (int)model.meshes[mi].morph_target_count;
            }

            /* Re-detect facing for the new model */
            model_y_rot_base = __detect_model_facing(&model);
            model_y_rot = model_y_rot_base;
            model_x_rot = 0.0f;

            /* Reset camera to new model */
            cam_yaw = 0.0f;
            cam_pitch = 0.15f;
            cam_dist = model.extent * 2.0f;
            if (cam_dist < 1.0f) { cam_dist = 3.0f; }
            cam_target[0] = model.center[0];
            cam_target[1] = model.center[1];
            cam_target[2] = model.center[2];

            settings_overlay_update_anims(anim_names, anim_name_count, 0);

            start_ticks = SDL_GetTicks();
            s_last_interact_ticks = SDL_GetTicks();
            idle_last_pick = -1;
            idle_is_oneshot = 0;
            idle_next_switch = SDL_GetTicks() +
                __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
            idle_next_emotion = SDL_GetTicks() +
                __rand_interval_ms(IDLE_EMOTION_MIN_SEC, IDLE_EMOTION_MAX_SEC);

            continue;
        }

        /* ---- handle scene reload ---- */
        if (s_reload_scene_pending) {
            s_reload_scene_pending = 0;
            skybox_destroy(&skybox);
            memset(&skybox, 0, sizeof(skybox));
            if (s_reload_scene_dir[0] != '\0') {
                skybox_init(&skybox, s_reload_scene_dir);
            }
        }

        /* ---- sync animation selection from LVGL settings ---- */
        if (s_sui_anim_req_pending && anim_name_count > 0) {
            s_sui_anim_req_pending = 0;
            int idx = s_sui_anim_req_idx;
            if (idx >= 0 && idx < anim_name_count) {
                START_ANIM_BLEND();
                active_anim     = first_vrma_anim + (uint32_t)idx;
                start_ticks     = SDL_GetTicks();
                has_anim        = 1;
                idle_last_pick  = idx;
                idle_is_oneshot = __is_oneshot(anim_names[idx]);
                idle_next_switch = SDL_GetTicks() +
                    __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                s_last_interact_ticks = SDL_GetTicks();
            }
        }

        /* ---- idle behavior engine ---- */
        if (has_anim && anim_name_count > 0 && !paused) {
            Uint32 idle_now = SDL_GetTicks();
            float since_interact = (float)(idle_now - s_last_interact_ticks) / 1000.0f;

            /* A. One-shot completion: auto-return to idle_normal after playback */
            if (idle_is_oneshot && active_anim < total_anims) {
                float dur = model.animations[active_anim].duration;
                float elapsed = (float)(idle_now - start_ticks) / 1000.0f;
                if (elapsed >= dur) {
                    int idx = __pick_recovery_idle_anim(anim_names, anim_name_count,
                                                        idle_last_pick);
                    if (idx >= 0) {
                        START_ANIM_BLEND();
                        active_anim     = first_vrma_anim + (uint32_t)idx;
                        start_ticks     = idle_now;
                        idle_last_pick  = idx;
                        idle_is_oneshot = __is_oneshot(anim_names[idx]);
                        idle_next_switch = idle_now +
                            __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                    }
                }
            }

            /* B. Idle mode: animation rotation + random emotions */
            int idle_mode = (since_interact > IDLE_INTERACT_COOLDOWN && !emo_ctx.speaking);

            if (idle_mode && !idle_was_active) {
                idle_next_switch  = idle_now +
                    __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                idle_next_emotion = idle_now +
                    __rand_interval_ms(IDLE_EMOTION_MIN_SEC, IDLE_EMOTION_MAX_SEC);
                idle_emotion_revert = 0;
            }

            if (!idle_mode && idle_was_active) {
                if (cur_emo != EMOTION_NEUTRAL) {
                    emotion_set(&emo_ctx, EMOTION_NEUTRAL);
                    cur_emo = EMOTION_NEUTRAL;
                }
                idle_emotion_revert = 0;
            }

            idle_was_active = idle_mode;

            if (idle_mode) {
                /* Idle animation rotation */
                if (!idle_is_oneshot && (int32_t)(idle_now - idle_next_switch) >= 0) {
                    int idx = __pick_idle_anim(anim_names, anim_name_count, idle_last_pick);
                    if (idx >= 0) {
                        idle_next_switch = idle_now +
                            __rand_interval_ms(IDLE_SWITCH_MIN_SEC, IDLE_SWITCH_MAX_SEC);
                        uint32_t next_anim = first_vrma_anim + (uint32_t)idx;
                        if (active_anim != next_anim) {
                            START_ANIM_BLEND();
                            active_anim     = next_anim;
                            start_ticks     = idle_now;
                            idle_last_pick  = idx;
                            idle_is_oneshot = __is_oneshot(anim_names[idx]);
                        }
                    }
                }

                /* Random emotion micro-variation */
                if ((int32_t)(idle_now - idle_next_emotion) >= 0) {
                    emotion_id_t eid = s_idle_emotions[rand() % IDLE_EMOTION_COUNT];
                    if (eid != cur_emo) {
                        emotion_set(&emo_ctx, eid);
                        cur_emo = eid;
                        if (eid != EMOTION_NEUTRAL) {
                            idle_emotion_revert = idle_now +
                                (Uint32)(IDLE_EMOTION_HOLD_SEC * 1000);
                        }
                    }
                    idle_next_emotion = idle_now +
                        __rand_interval_ms(IDLE_EMOTION_MIN_SEC, IDLE_EMOTION_MAX_SEC);
                }

                /* Revert non-neutral emotion after hold time */
                if (idle_emotion_revert > 0 &&
                    (int32_t)(idle_now - idle_emotion_revert) >= 0) {
                    if (cur_emo != EMOTION_NEUTRAL) {
                        emotion_set(&emo_ctx, EMOTION_NEUTRAL);
                        cur_emo = EMOTION_NEUTRAL;
                    }
                    idle_emotion_revert = 0;
                }
            }
        }

        /* ---- animation time ---- */
        float anim_time = 0.0f;
        if (!paused) {
            anim_time = (float)(SDL_GetTicks() - start_ticks) / 1000.0f;
        }

        /* ---- update bone matrices ---- */
        if (bone_count > 0) {
            float *evaluated_matrices = target_bone_matrices ? target_bone_matrices : bone_matrices;
            if (has_anim) {
                vrm_evaluate_animation(&model, active_anim, anim_time, evaluated_matrices);
            } else {
                vrm_rest_pose_matrices(&model, evaluated_matrices);
            }

            if (evaluated_matrices != bone_matrices) {
                if (anim_blend_active && blend_from_matrices) {
                    float blend_t = (float)(SDL_GetTicks() - anim_blend_start_ticks) /
                                    (ANIM_SWITCH_BLEND_SEC * 1000.0f);
                    if (blend_t >= 1.0f) {
                        memcpy(bone_matrices, evaluated_matrices,
                               bone_count * 16 * sizeof(float));
                        anim_blend_active = 0;
                    } else {
                        float smooth_t = blend_t * blend_t * (3.0f - 2.0f * blend_t);
                        for (uint32_t mi = 0; mi < bone_count * 16; mi++) {
                            bone_matrices[mi] = blend_from_matrices[mi] +
                                (evaluated_matrices[mi] - blend_from_matrices[mi]) * smooth_t;
                        }
                    }
                } else {
                    memcpy(bone_matrices, evaluated_matrices,
                           bone_count * 16 * sizeof(float));
                }
            }

            /* ---- Spring bone physics ---- */
            {
                Uint32 now = SDL_GetTicks();
                float sdt = (float)(now - last_frame_ticks) / 1000.0f;
                if (sdt <= 0.0f) sdt = 0.016f;
                if (sdt > 0.1f) sdt = 0.016f;
                spring_bone_update(&spring_ctx, sdt, bone_matrices);
            }

            /* Upload to TBO */
            glBindBuffer(GL_TEXTURE_BUFFER, bone_tbo_buf);
            glBufferSubData(GL_TEXTURE_BUFFER, 0,
                            (GLsizeiptr)(bone_count * 16 * sizeof(float)),
                            bone_matrices);
            glBindBuffer(GL_TEXTURE_BUFFER, 0);
        }

        /* ---- Emotion & BlendShape / morph target update ---- */
        {
            Uint32 now_ticks = SDL_GetTicks();
            float frame_dt = (float)(now_ticks - last_frame_ticks) / 1000.0f;
            if (frame_dt > 0.1f) frame_dt = 0.1f; /* clamp large spikes */
            last_frame_ticks = now_ticks;

            /* Drive expression weights via emotion system */
            emotion_update(&emo_ctx, frame_dt);

            /* Apply morph target deltas to CPU vertex data */
            if (total_morph_targets > 0) {
                vrm_apply_morph_targets(&model);

                /* Re-upload VBOs for meshes with morph targets */
                for (uint32_t i = 0; i < model.mesh_count; i++) {
                    if (gpu[i].has_morph) {
                        glBindBuffer(GL_ARRAY_BUFFER, gpu[i].vbo);
                        glBufferSubData(GL_ARRAY_BUFFER, 0,
                                        (GLsizeiptr)(gpu[i].vertex_count * 16 * sizeof(float)),
                                        model.meshes[i].vertices);
                    }
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }

        /* ---- model matrix (drag rotation) ---- */
        mat4 view_mat, proj_mat, model_mat, vp, mvp;
        {
            mat4 ry;
            mat4_rotate_y(ry, model_y_rot);
            if (model_x_rot != 0.0f) {
                mat4 rx;
                mat4_rotate_x(rx, model_x_rot);
                mat4_multiply(model_mat, rx, ry);
            } else {
                memcpy(model_mat, ry, sizeof(mat4));
            }
        }

        /* Recompute camera target from rotated model center */
        cam_target[0] = model_mat[0] * model.center[0] + model_mat[4] * model.center[1] + model_mat[8]  * model.center[2];
        cam_target[1] = model_mat[1] * model.center[0] + model_mat[5] * model.center[1] + model_mat[9]  * model.center[2];
        cam_target[2] = model_mat[2] * model.center[0] + model_mat[6] * model.center[1] + model_mat[10] * model.center[2];

        /* ---- camera ---- */
        float eye[3];
        float cam_look_target[3];
        float up[3] = { 0.0f, 1.0f, 0.0f };

        int spectator_on = settings_overlay_spectator();
        if (spectator_on) {
            if (!s_spec_inited) {
                s_spec_yaw   = cam_yaw;
                s_spec_pitch = cam_pitch;
                s_spec_dist  = cam_dist;
                s_spec_target[0] = cam_target[0];
                s_spec_target[1] = cam_target[1];
                s_spec_target[2] = cam_target[2];
                s_spec_inited = 1;
            }

            eye[0] = s_spec_target[0] + s_spec_dist * cosf(s_spec_pitch) * sinf(s_spec_yaw);
            eye[1] = s_spec_target[1] + s_spec_dist * sinf(s_spec_pitch);
            eye[2] = s_spec_target[2] + s_spec_dist * cosf(s_spec_pitch) * cosf(s_spec_yaw);
            cam_look_target[0] = s_spec_target[0];
            cam_look_target[1] = s_spec_target[1];
            cam_look_target[2] = s_spec_target[2];
        } else {
            eye[0] = cam_target[0] + cam_dist * cosf(cam_pitch) * sinf(cam_yaw);
            eye[1] = cam_target[1] + cam_dist * sinf(cam_pitch);
            eye[2] = cam_target[2] + cam_dist * cosf(cam_pitch) * cosf(cam_yaw);
            cam_look_target[0] = cam_target[0];
            cam_look_target[1] = cam_target[1];
            cam_look_target[2] = cam_target[2];
        }

        mat4_look_at(view_mat, eye, cam_look_target, up);
        mat4_perspective(proj_mat, 45.0f * (float)M_PI / 180.0f,
                         (float)win_w / (float)(win_h > 0 ? win_h : 1),
                         0.01f, 1000.0f);

        mat4_multiply(vp, proj_mat, view_mat);
        mat4_multiply(mvp, vp, model_mat);

        /* ---- Light matrix for shadow mapping ---- */
        mat4 light_vp, light_mvp;
        {
            float le = model.extent * 1.5f;
            if (le < 1.0f) le = 1.0f;
            vec3 l_eye = {
                cam_target[0],
                cam_target[1] + le * 2.0f,
                cam_target[2] + le * 2.5f
            };
            vec3 l_at = { cam_target[0], model.bbox_min[1], cam_target[2] };
            vec3 l_up = { 0.0f, 0.0f, -1.0f };
            mat4 lv, lp;
            mat4_look_at(lv, l_eye, l_at, l_up);
            mat4_ortho(lp, -le, le, -le, le, 0.01f, le * 6.0f);
            mat4_multiply(light_vp, lp, lv);
            mat4_multiply(light_mvp, light_vp, model_mat);
        }

        /* ---- Shadow depth pass ---- */
        glBindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
        glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(shadow_prog);
        glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "u_light_mvp"),
                           1, GL_FALSE, light_mvp);
        glUniform1i(glGetUniformLocation(shadow_prog, "u_bone_tbo"), 1);
        if (bone_tbo_tex) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_BUFFER, bone_tbo_tex);
        }
        for (uint32_t i = 0; i < model.mesh_count; i++) {
            glUniform1i(glGetUniformLocation(shadow_prog, "u_skinned"),
                        gpu[i].has_bones);
            glBindVertexArray(gpu[i].vao);
            glDrawElements(GL_TRIANGLES, (GLsizei)gpu[i].index_count,
                           GL_UNSIGNED_INT, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glViewport(0, 0, win_w, win_h);

        /* ---- draw background ---- */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (skybox.loaded) {
            skybox_draw(&skybox, view_mat, proj_mat);
        } else {
            glDisable(GL_DEPTH_TEST);
            glUseProgram(bg_prog);
            glBindVertexArray(s_bg_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glEnable(GL_DEPTH_TEST);
        }

        /* ---- draw grid only when no skybox is active ---- */
        if (!skybox.loaded) {
            glUseProgram(grid_prog);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            {
                mat4 gm, gmvp;
                mat4_scale_uniform(gm, model.extent * 1.5f);
                gm[13] = model.bbox_min[1];
                mat4_multiply(gmvp, vp, gm);
                glUniformMatrix4fv(glGetUniformLocation(grid_prog, "u_mvp"), 1, GL_FALSE, gmvp);
            }
            glBindVertexArray(s_grid_vao);
            glDrawArrays(GL_LINES, 0, s_grid_vert_count);
            glDisable(GL_BLEND);
        }

        /* ---- draw model (toon cel shading) ---- */
        glUseProgram(model_prog);
        glUniformMatrix4fv(glGetUniformLocation(model_prog, "u_mvp"), 1, GL_FALSE, mvp);
        glUniformMatrix4fv(glGetUniformLocation(model_prog, "u_model"), 1, GL_FALSE, model_mat);
        {
            float ld[3] = { 0.0f, 0.15f, 1.0f };
            float len = sqrtf(ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2]);
            ld[0] /= len; ld[1] /= len; ld[2] /= len;
            glUniform3fv(glGetUniformLocation(model_prog, "u_light_dir"), 1, ld);
        }

        glUniform1i(glGetUniformLocation(model_prog, "u_texture"), 0);
        glUniform1i(glGetUniformLocation(model_prog, "u_bone_tbo"), 1);

        if (bone_tbo_tex) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_BUFFER, bone_tbo_tex);
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (uint32_t i = 0; i < model.mesh_count; i++) {
            glUniform4fv(glGetUniformLocation(model_prog, "u_base_color"), 1, gpu[i].color);
            glUniform1i(glGetUniformLocation(model_prog, "u_has_texture"), gpu[i].has_texture);
            glUniform1i(glGetUniformLocation(model_prog, "u_skinned"), gpu[i].has_bones);

            glActiveTexture(GL_TEXTURE0);
            if (gpu[i].has_texture) {
                glBindTexture(GL_TEXTURE_2D, gpu[i].tex_id);
            } else {
                glBindTexture(GL_TEXTURE_2D, white_tex);
            }

            glBindVertexArray(gpu[i].vao);
            glDrawElements(GL_TRIANGLES, (GLsizei)gpu[i].index_count, GL_UNSIGNED_INT, 0);
        }
        glDisable(GL_BLEND);

        /* ---- Ground shadow ---- */
        glUseProgram(ground_shadow_prog);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        {
            float gs = model.extent * 3.0f;
            mat4 ground_mat;
            mat4_identity(ground_mat);
            ground_mat[0]  = gs;
            ground_mat[10] = gs;
            ground_mat[13] = model.bbox_min[1];

            mat4 gmvp;
            mat4_multiply(gmvp, vp, ground_mat);

            glUniformMatrix4fv(
                glGetUniformLocation(ground_shadow_prog, "u_mvp"),
                1, GL_FALSE, gmvp);
            glUniformMatrix4fv(
                glGetUniformLocation(ground_shadow_prog, "u_light_vp"),
                1, GL_FALSE, light_vp);
            glUniformMatrix4fv(
                glGetUniformLocation(ground_shadow_prog, "u_ground_mat"),
                1, GL_FALSE, ground_mat);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_shadow_depth_tex);
            glUniform1i(
                glGetUniformLocation(ground_shadow_prog, "u_shadow_map"), 0);

            vec3 sc = { cam_target[0], 0.0f, cam_target[2] };
            glUniform3fv(
                glGetUniformLocation(ground_shadow_prog, "u_center"), 1, sc);
            glUniform1f(
                glGetUniformLocation(ground_shadow_prog, "u_radius"),
                gs * 0.8f);

            glBindVertexArray(s_ground_vao);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        /* ---- Settings overlay + subtitle (2D on top of 3D) ---- */
        settings_overlay_render(win_w, win_h);

        SDL_GL_SwapWindow(window);
    }

    /* ---- cleanup ---- */
    for (uint32_t i = 0; i < model.mesh_count; i++) {
        if (gpu[i].tex_id) glDeleteTextures(1, &gpu[i].tex_id);
        glDeleteVertexArrays(1, &gpu[i].vao);
        glDeleteBuffers(1, &gpu[i].vbo);
        glDeleteBuffers(1, &gpu[i].ebo);
    }
    free(gpu);
    free(bone_matrices);
    free(target_bone_matrices);
    free(blend_from_matrices);

    if (bone_tbo_tex) glDeleteTextures(1, &bone_tbo_tex);
    if (bone_tbo_buf) glDeleteBuffers(1, &bone_tbo_buf);

    glDeleteTextures(1, &white_tex);
    glDeleteVertexArrays(1, &s_grid_vao);
    glDeleteBuffers(1, &s_grid_vbo);
    glDeleteVertexArrays(1, &s_bg_vao);
    glDeleteBuffers(1, &s_bg_vbo);
    glDeleteFramebuffers(1, &s_shadow_fbo);
    glDeleteTextures(1, &s_shadow_depth_tex);
    glDeleteVertexArrays(1, &s_ground_vao);
    glDeleteBuffers(1, &s_ground_vbo);
    glDeleteBuffers(1, &s_ground_ebo);
    glDeleteProgram(model_prog);
    glDeleteProgram(grid_prog);
    glDeleteProgram(bg_prog);
    glDeleteProgram(shadow_prog);
    glDeleteProgram(ground_shadow_prog);
    skybox_destroy(&skybox);

    settings_overlay_destroy();

    /* Nullify global emotion ctx before shutdown to prevent dangling access */
    s_emo_ctx = NULL;
    /* Restore default speaker consumer */
    tuya_ai_player_set_consumer(NULL);

    emotion_shutdown(&emo_ctx);
    spring_bone_shutdown(&spring_ctx);
    vrm_model_free(&model);

    for (int i = 0; i < anim_name_count; i++)
        free(anim_names[i]);

    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;

#undef START_ANIM_BLEND
}
