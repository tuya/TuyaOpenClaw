/**
 * @file settings_overlay.c
 * @brief Lightweight OpenGL overlay for VRM viewer settings and subtitles.
 *
 * Renders entirely with a single shader program and two batched draw calls
 * (colored quads + textured text).  All relevant GL state is saved before
 * rendering and restored afterward so the 3D pipeline is unaffected.
 *
 * @version 1.0
 * @date 2025-03-26
 * @copyright Copyright (c) Tuya Inc.
 */

#include "settings_overlay.h"

#include <GL/glew.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../TuyaOpen/src/liblvgl/v9/lvgl/src/libs/tiny_ttf/stb_truetype_htcw.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define OVL_MAX_FILES      64
#define OVL_MAX_PATH       512
#define OVL_MAX_NAME       128
#define OVL_MAX_VERTS      16384
#define OVL_SUBTITLE_MAX   2048
#define OVL_PANEL_W        340
#define OVL_FONT_SCALE     2
#define OVL_CHAR_W         (8 * OVL_FONT_SCALE)
#define OVL_CHAR_H         (8 * OVL_FONT_SCALE)
#define OVL_PAD            14
#define OVL_ITEM_H         (OVL_CHAR_H + 10)
#define OVL_SECTION_H      (OVL_CHAR_H + 14)
#define OVL_SEP_H          8
#define OVL_BORDER_W       2
#define OVL_SUB_FONT_PX    22.0f
#define OVL_SUB_MAX_GLYPHS 512
#define OVL_SUB_MAX_LINES  2
#define OVL_SUB_MAX_WRAP_LINES 32
#define OVL_SUB_LINE_GAP   6
#define OVL_SUB_MARGIN_X   36

#define OVL_ICON_SIZE      40
#define OVL_ICON_MARGIN    12
#define OVL_SUB_BOTTOM_GAP 26
#define OVL_SUB_INNER_PAD_X 18
#define OVL_SUB_INNER_PAD_Y 12
#define OVL_SUBTITLE_H     78

#define OVL_SUB_REPO_FONT \
    "TuyaOpen/src/liblvgl/v9/lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf"
#define OVL_SUB_SYS_FONT   "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"

#define OVL_ICEILF(v)  ((int)((v) + 0.999f))
#define OVL_IROUNDF(v) ((int)(((v) >= 0.0f) ? ((v) + 0.5f) : ((v) - 0.5f)))

/* ---------------------------------------------------------------------------
 * Embedded 8x8 bitmap font (ASCII 32–126, public domain CP437 style)
 * --------------------------------------------------------------------------- */
static const unsigned char FONT_8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x6c,0x6c,0x24,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6c,0xfe,0x6c,0x6c,0xfe,0x6c,0x00,0x00}, /* # */
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00}, /* $ */
    {0x00,0x66,0xac,0xd8,0x36,0x6a,0xcc,0x00}, /* % */
    {0x38,0x6c,0x68,0x76,0xdc,0xce,0x7b,0x00}, /* & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, /* ( */
    {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00}, /* / */
    {0x7c,0xc6,0xce,0xde,0xf6,0xe6,0x7c,0x00}, /* 0 */
    {0x18,0x38,0x78,0x18,0x18,0x18,0x7e,0x00}, /* 1 */
    {0x7c,0xc6,0x06,0x1c,0x30,0x66,0xfe,0x00}, /* 2 */
    {0x7c,0xc6,0x06,0x3c,0x06,0xc6,0x7c,0x00}, /* 3 */
    {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00}, /* 4 */
    {0xfe,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00}, /* 5 */
    {0x38,0x60,0xc0,0xfc,0xc6,0xc6,0x7c,0x00}, /* 6 */
    {0xfe,0xc6,0x0c,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0x7c,0x00}, /* 8 */
    {0x7c,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00}, /* < */
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00}, /* = */
    {0x60,0x30,0x18,0x0c,0x18,0x30,0x60,0x00}, /* > */
    {0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x7c,0xc6,0xde,0xde,0xdc,0xc0,0x7c,0x00}, /* @ */
    {0x38,0x6c,0xc6,0xc6,0xfe,0xc6,0xc6,0x00}, /* A */
    {0xfc,0xc6,0xc6,0xfc,0xc6,0xc6,0xfc,0x00}, /* B */
    {0x7c,0xc6,0xc0,0xc0,0xc0,0xc6,0x7c,0x00}, /* C */
    {0xf8,0xcc,0xc6,0xc6,0xc6,0xcc,0xf8,0x00}, /* D */
    {0xfe,0xc0,0xc0,0xfc,0xc0,0xc0,0xfe,0x00}, /* E */
    {0xfe,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0x00}, /* F */
    {0x7c,0xc6,0xc0,0xce,0xc6,0xc6,0x7e,0x00}, /* G */
    {0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, /* H */
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x7e,0x00}, /* I */
    {0x1e,0x06,0x06,0x06,0xc6,0xc6,0x7c,0x00}, /* J */
    {0xc6,0xcc,0xd8,0xf0,0xd8,0xcc,0xc6,0x00}, /* K */
    {0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xfe,0x00}, /* L */
    {0xc6,0xee,0xfe,0xd6,0xc6,0xc6,0xc6,0x00}, /* M */
    {0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00}, /* N */
    {0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, /* O */
    {0xfc,0xc6,0xc6,0xfc,0xc0,0xc0,0xc0,0x00}, /* P */
    {0x7c,0xc6,0xc6,0xc6,0xd6,0xcc,0x76,0x00}, /* Q */
    {0xfc,0xc6,0xc6,0xfc,0xd8,0xcc,0xc6,0x00}, /* R */
    {0x7c,0xc6,0xc0,0x7c,0x06,0xc6,0x7c,0x00}, /* S */
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
    {0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, /* U */
    {0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0x00}, /* V */
    {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00}, /* W */
    {0xc6,0xc6,0x6c,0x38,0x6c,0xc6,0xc6,0x00}, /* X */
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x00}, /* Y */
    {0xfe,0x06,0x0c,0x18,0x30,0x60,0xfe,0x00}, /* Z */
    {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, /* [ */
    {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00}, /* \ */
    {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, /* ] */
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe}, /* _ */
    {0x18,0x18,0x0c,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x7c,0x06,0x7e,0xc6,0x7e,0x00}, /* a */
    {0xc0,0xc0,0xfc,0xc6,0xc6,0xc6,0xfc,0x00}, /* b */
    {0x00,0x00,0x7c,0xc6,0xc0,0xc6,0x7c,0x00}, /* c */
    {0x06,0x06,0x7e,0xc6,0xc6,0xc6,0x7e,0x00}, /* d */
    {0x00,0x00,0x7c,0xc6,0xfe,0xc0,0x7c,0x00}, /* e */
    {0x1c,0x36,0x30,0x7c,0x30,0x30,0x30,0x00}, /* f */
    {0x00,0x00,0x7e,0xc6,0xc6,0x7e,0x06,0x7c}, /* g */
    {0xc0,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00}, /* i */
    {0x06,0x00,0x0e,0x06,0x06,0x66,0x66,0x3c}, /* j */
    {0xc0,0xc0,0xcc,0xd8,0xf0,0xd8,0xcc,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, /* l */
    {0x00,0x00,0xec,0xfe,0xd6,0xd6,0xc6,0x00}, /* m */
    {0x00,0x00,0xfc,0xc6,0xc6,0xc6,0xc6,0x00}, /* n */
    {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0x00}, /* o */
    {0x00,0x00,0xfc,0xc6,0xc6,0xfc,0xc0,0xc0}, /* p */
    {0x00,0x00,0x7e,0xc6,0xc6,0x7e,0x06,0x06}, /* q */
    {0x00,0x00,0xdc,0xe6,0xc0,0xc0,0xc0,0x00}, /* r */
    {0x00,0x00,0x7e,0xc0,0x7c,0x06,0xfc,0x00}, /* s */
    {0x30,0x30,0x7c,0x30,0x30,0x36,0x1c,0x00}, /* t */
    {0x00,0x00,0xc6,0xc6,0xc6,0xc6,0x7e,0x00}, /* u */
    {0x00,0x00,0xc6,0xc6,0xc6,0x6c,0x38,0x00}, /* v */
    {0x00,0x00,0xc6,0xd6,0xd6,0xfe,0x6c,0x00}, /* w */
    {0x00,0x00,0xc6,0x6c,0x38,0x6c,0xc6,0x00}, /* x */
    {0x00,0x00,0xc6,0xc6,0xc6,0x7e,0x06,0x7c}, /* y */
    {0x00,0x00,0xfe,0x0c,0x38,0x60,0xfe,0x00}, /* z */
    {0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0x00}, /* { */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

/* ---------------------------------------------------------------------------
 * Shader sources
 * --------------------------------------------------------------------------- */
static const char *OVL_VS =
    "#version 130\n"
    "uniform mat4 u_proj;\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  v_uv  = a_uv;\n"
    "  v_col = a_col;\n"
    "  gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *OVL_FS =
    "#version 130\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_use_tex;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  if (u_use_tex != 0) {\n"
    "    float a = texture(u_tex, v_uv).r;\n"
    "    frag = vec4(v_col.rgb, v_col.a * a);\n"
    "  } else {\n"
    "    frag = v_col;\n"
    "  }\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Vertex type
 * --------------------------------------------------------------------------- */
typedef struct {
    float x, y;
    float u, v;
    float r, g, b, a;
} ovl_vert_t;

/* ---------------------------------------------------------------------------
 * File entry
 * --------------------------------------------------------------------------- */
typedef struct {
    char name[OVL_MAX_NAME];
    char path[OVL_MAX_PATH];
} ovl_file_t;

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
static GLuint     s_prog       = 0;
static GLint      s_u_proj     = -1;
static GLint      s_u_tex      = -1;
static GLint      s_u_use_tex  = -1;
static GLuint     s_vao        = 0;
static GLuint     s_vbo        = 0;
static GLuint     s_font_tex   = 0;

static ovl_vert_t s_verts[OVL_MAX_VERTS];
static int        s_vert_count = 0;

static ovl_file_t s_models[OVL_MAX_FILES];
static int        s_model_count    = 0;
static int        s_model_sel      = -1;
static char       s_model_dir[OVL_MAX_PATH];

static ovl_file_t s_scenes[OVL_MAX_FILES];
static int        s_scene_count    = 0;
static int        s_scene_sel      = -1;
static char       s_scene_dir[OVL_MAX_PATH];

static char     **s_anim_names    = NULL;
static int        s_anim_count    = 0;
static int        s_anim_sel      = -1;

static int        s_visible        = 0;
static int        s_cam_locked     = 0;
static int        s_sub_enabled    = 0;
static int        s_fullscreen     = 0;
static int        s_spectator      = 0;
static float      s_scroll         = 0.0f;
static char       s_subtitle[OVL_SUBTITLE_MAX];

static overlay_str_cb_t s_cb_model = NULL;
static overlay_int_cb_t s_cb_anim  = NULL;
static overlay_str_cb_t s_cb_scene = NULL;
static overlay_int_cb_t s_cb_cam   = NULL;
static overlay_int_cb_t s_cb_sub   = NULL;
static overlay_int_cb_t s_cb_fs    = NULL;
static overlay_int_cb_t s_cb_spec  = NULL;
static void            *s_cb_ud    = NULL;

static GLuint     s_sub_tex         = 0;
static int        s_sub_draw_w      = 0;
static int        s_sub_draw_h      = 0;
static int        s_sub_last_max_w  = -1;
static int        s_sub_dirty       = 1;
static unsigned char *s_sub_pixels  = NULL;
static size_t     s_sub_pixels_cap  = 0;

static stbtt_fontinfo s_sub_font;
static unsigned char *s_sub_font_buf   = NULL;
static int            s_sub_font_ready = 0;
static int            s_sub_font_tried = 0;

typedef struct {
    unsigned int cp;
    int x;
    int y;
    int line;
    int x0, y0, x1, y1;
} ovl_glyph_layout_t;

static int __file_read_all(const char *path, unsigned char **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (!path || path[0] == '\0') {
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    *out = buf;
    *out_size = (size_t)sz;
    return 1;
}

static int __path_exists(const char *path)
{
    return path && access(path, R_OK) == 0;
}

static int __find_repo_font_from(char *base, char *out_path, size_t out_sz)
{
    char dir[OVL_MAX_PATH];
    strncpy(dir, base, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char cand[OVL_MAX_PATH];
        snprintf(cand, sizeof(cand), "%s/%s", dir, OVL_SUB_REPO_FONT);
        if (__path_exists(cand)) {
            strncpy(out_path, cand, out_sz - 1);
            out_path[out_sz - 1] = '\0';
            return 1;
        }

        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) {
            break;
        }
        *slash = '\0';
    }
    return 0;
}

static int __locate_subtitle_font(char *out_path, size_t out_sz, int *out_index)
{
    const char *env = getenv("DUCKYCLAW_SUBTITLE_FONT");
    if (env && env[0] != '\0' && __path_exists(env)) {
        strncpy(out_path, env, out_sz - 1);
        out_path[out_sz - 1] = '\0';
        *out_index = 0;
        printf("[ovl] subtitle font (env): %s\n", out_path);
        return 1;
    }

    /* Search from CWD upward */
    char cwd[OVL_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)) && __find_repo_font_from(cwd, out_path, out_sz)) {
        *out_index = 0;
        printf("[ovl] subtitle font (cwd): %s\n", out_path);
        return 1;
    }

    /* Search from executable's directory upward (/proc/self/exe on Linux) */
    char exe_dir[OVL_MAX_PATH];
    ssize_t elen = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (elen > 0) {
        exe_dir[elen] = '\0';
        char *slash = strrchr(exe_dir, '/');
        if (slash) {
            *slash = '\0';
        }
        if (__find_repo_font_from(exe_dir, out_path, out_sz)) {
            *out_index = 0;
            printf("[ovl] subtitle font (exe): %s\n", out_path);
            return 1;
        }
    }

    /* System font fallbacks — prefer .ttf (TrueType) over .ttc (CFF) */
    static const char *sys_fonts[] = {
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSC-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/TTF/NotoSansCJK-Regular.ttc",
        NULL
    };
    for (int i = 0; sys_fonts[i]; i++) {
        if (__path_exists(sys_fonts[i])) {
            strncpy(out_path, sys_fonts[i], out_sz - 1);
            out_path[out_sz - 1] = '\0';
            /* .ttc collections: index 2 = SC; .ttf files: index 0 */
            const char *ext = strrchr(sys_fonts[i], '.');
            *out_index = (ext && strcmp(ext, ".ttc") == 0) ? 2 : 0;
            printf("[ovl] subtitle font (sys): %s (index=%d)\n", out_path, *out_index);
            return 1;
        }
    }

    fprintf(stderr, "[ovl] subtitle font not found in any search path\n");
    return 0;
}

static int __subtitle_font_init(void)
{
    if (s_sub_font_ready) {
        return 1;
    }
    if (s_sub_font_tried) {
        return 0;
    }
    s_sub_font_tried = 1;

    char path[OVL_MAX_PATH];
    int font_index = 0;
    if (!__locate_subtitle_font(path, sizeof(path), &font_index)) {
        fprintf(stderr, "[ovl] subtitle font not found — "
                "set DUCKYCLAW_SUBTITLE_FONT=/path/to/font.ttf\n");
        return 0;
    }

    unsigned char *buf = NULL;
    size_t buf_sz = 0;
    if (!__file_read_all(path, &buf, &buf_sz)) {
        fprintf(stderr, "[ovl] failed to read subtitle font: %s\n", path);
        return 0;
    }

    printf("[ovl] loaded font file: %s (%zu bytes, index=%d)\n",
           path, buf_sz, font_index);

    int offset = stbtt_GetFontOffsetForIndex(buf, font_index);
    if (offset < 0) {
        fprintf(stderr, "[ovl] font index %d out of range in %s "
                "(try index 0)\n", font_index, path);
        free(buf);
        return 0;
    }

    if (!stbtt_InitFont(&s_sub_font, buf, offset)) {
        fprintf(stderr, "[ovl] stbtt_InitFont failed for %s (index=%d, offset=%d)\n",
                path, font_index, offset);
        free(buf);
        return 0;
    }

    s_sub_font_buf = buf;
    s_sub_font_ready = 1;
    printf("[settings_overlay] subtitle font ready: %s (index=%d)\n",
           path, font_index);
    return 1;
}

static unsigned int __utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)(*pp);
    unsigned int cp = 0xfffdU;

    if (!s || s[0] == '\0') {
        return 0;
    }

    if (s[0] < 0x80) {
        cp = s[0];
        *pp += 1;
        return cp;
    }
    if ((s[0] & 0xe0) == 0xc0 && s[1]) {
        cp = ((unsigned int)(s[0] & 0x1f) << 6) |
             (unsigned int)(s[1] & 0x3f);
        *pp += 2;
        return cp;
    }
    if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) {
        cp = ((unsigned int)(s[0] & 0x0f) << 12) |
             ((unsigned int)(s[1] & 0x3f) << 6) |
             (unsigned int)(s[2] & 0x3f);
        *pp += 3;
        return cp;
    }
    if ((s[0] & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
        cp = ((unsigned int)(s[0] & 0x07) << 18) |
             ((unsigned int)(s[1] & 0x3f) << 12) |
             ((unsigned int)(s[2] & 0x3f) << 6) |
             (unsigned int)(s[3] & 0x3f);
        *pp += 4;
        return cp;
    }

    *pp += 1;
    return cp;
}

static void __subtitle_texture_reset(void)
{
    s_sub_draw_w = 0;
    s_sub_draw_h = 0;
    s_sub_last_max_w = -1;
    s_sub_dirty = 1;
}

static int __ensure_sub_pixel_capacity(size_t need)
{
    if (need <= s_sub_pixels_cap) {
        return 1;
    }
    unsigned char *tmp = (unsigned char *)realloc(s_sub_pixels, need);
    if (!tmp) {
        return 0;
    }
    s_sub_pixels = tmp;
    s_sub_pixels_cap = need;
    return 1;
}

static int __subtitle_build_layout(int max_w, ovl_glyph_layout_t *glyphs,
                                   int *glyph_count, int *out_w, int *out_h,
                                   int *out_baseline)
{
    if (!__subtitle_font_init()) {
        return 0;
    }

    const char *p = s_subtitle;
    float scale = stbtt_ScaleForPixelHeight(&s_sub_font, OVL_SUB_FONT_PX);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&s_sub_font, &ascent, &descent, &line_gap);
    (void)line_gap;
    int baseline = OVL_ICEILF((float)ascent * scale);
    int line_h = OVL_ICEILF((float)(ascent - descent) * scale);
    int line_step = line_h + OVL_SUB_LINE_GAP;

    int line_min[OVL_SUB_MAX_WRAP_LINES];
    int line_max[OVL_SUB_MAX_WRAP_LINES];
    for (int i = 0; i < OVL_SUB_MAX_WRAP_LINES; i++) {
        line_min[i] = 0;
        line_max[i] = 0;
    }

    int line = 0;
    int pen_x = 0;
    int prev_cp = 0;
    int count = 0;

    while (*p && count < OVL_SUB_MAX_GLYPHS) {
        unsigned int cp = __utf8_next(&p);
        if (cp == 0) {
            break;
        }
        if (cp == '\r' || cp == '\n' || cp == '\t') {
            cp = ' ';
        }

        if (cp == ' ' && pen_x == 0) {
            continue;
        }

        int kern = prev_cp ? stbtt_GetCodepointKernAdvance(&s_sub_font, prev_cp, (int)cp) : 0;
        int glyph_x = pen_x + OVL_IROUNDF((float)kern * scale);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&s_sub_font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);

        int trial_min_x = (pen_x == 0) ? (glyph_x + x0) : line_min[line];
        int trial_max_x = (pen_x == 0) ? (glyph_x + x1) : line_max[line];
        if (glyph_x + x0 < trial_min_x) {
            trial_min_x = glyph_x + x0;
        }
        if (glyph_x + x1 > trial_max_x) {
            trial_max_x = glyph_x + x1;
        }

        if (pen_x > 0 && (trial_max_x - trial_min_x) > max_w) {
            line++;
            if (line >= OVL_SUB_MAX_WRAP_LINES) {
                break;
            }
            pen_x = 0;
            prev_cp = 0;
            if (cp == ' ') {
                continue;
            }

            kern = 0;
            glyph_x = 0;
            trial_min_x = glyph_x + x0;
            trial_max_x = glyph_x + x1;
        }

        glyphs[count].cp = cp;
        glyphs[count].x = glyph_x;
        glyphs[count].y = line * line_step;
        glyphs[count].line = line;
        glyphs[count].x0 = x0;
        glyphs[count].y0 = y0;
        glyphs[count].x1 = x1;
        glyphs[count].y1 = y1;
        count++;

        line_min[line] = trial_min_x;
        line_max[line] = trial_max_x;

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&s_sub_font, (int)cp, &advance, &lsb);
        (void)lsb;
        pen_x = glyph_x + OVL_IROUNDF((float)advance * scale);
        prev_cp = (int)cp;
    }

    if (count <= 0) {
        return 0;
    }

    int total_lines = glyphs[count - 1].line + 1;
    int first_line = total_lines > OVL_SUB_MAX_LINES ? (total_lines - OVL_SUB_MAX_LINES) : 0;
    int kept_lines = total_lines - first_line;
    int out_max_w = 0;
    int kept = 0;

    for (int ln = first_line; ln < total_lines; ln++) {
        int w = line_max[ln] - line_min[ln];
        if (w > out_max_w) {
            out_max_w = w;
        }
    }

    for (int i = 0; i < count; i++) {
        if (glyphs[i].line < first_line) {
            continue;
        }
        int ln = glyphs[i].line;
        glyphs[kept] = glyphs[i];
        glyphs[kept].x = glyphs[i].x - line_min[ln];
        glyphs[kept].y = (ln - first_line) * line_step;
        glyphs[kept].line = ln - first_line;
        kept++;
    }

    *glyph_count = kept;
    *out_w = out_max_w;
    *out_h = kept_lines * line_h + (kept_lines - 1) * OVL_SUB_LINE_GAP;
    *out_baseline = baseline;
    return (kept > 0 && *out_w > 0 && *out_h > 0);
}

static int __rebuild_subtitle_texture(int max_w)
{
    if (s_subtitle[0] == '\0') {
        s_sub_draw_w = 0;
        s_sub_draw_h = 0;
        s_sub_last_max_w = max_w;
        s_sub_dirty = 0;
        return 0;
    }

    ovl_glyph_layout_t glyphs[OVL_SUB_MAX_GLYPHS];
    int glyph_count = 0;
    int tex_w = 0;
    int tex_h = 0;
    int baseline = 0;
    if (!__subtitle_build_layout(max_w, glyphs, &glyph_count, &tex_w, &tex_h, &baseline)) {
        s_sub_draw_w = 0;
        s_sub_draw_h = 0;
        s_sub_last_max_w = max_w;
        s_sub_dirty = 0;
        return 0;
    }

    size_t need = (size_t)tex_w * (size_t)tex_h;
    if (!__ensure_sub_pixel_capacity(need)) {
        return 0;
    }
    memset(s_sub_pixels, 0, need);

    float scale = stbtt_ScaleForPixelHeight(&s_sub_font, OVL_SUB_FONT_PX);
    for (int i = 0; i < glyph_count; i++) {
        int gw = glyphs[i].x1 - glyphs[i].x0;
        int gh = glyphs[i].y1 - glyphs[i].y0;
        if (gw <= 0 || gh <= 0) {
            continue;
        }
        int dst_x = glyphs[i].x + glyphs[i].x0;
        int dst_y = glyphs[i].y + baseline + glyphs[i].y0;
        if (dst_x < 0 || dst_y < 0 || dst_x + gw > tex_w || dst_y + gh > tex_h) {
            continue;
        }
        unsigned char *dst = s_sub_pixels + (size_t)dst_y * (size_t)tex_w + (size_t)dst_x;
        stbtt_MakeCodepointBitmap(&s_sub_font, dst, gw, gh, tex_w, scale, scale, (int)glyphs[i].cp);
    }

    if (!s_sub_tex) {
        glGenTextures(1, &s_sub_tex);
    }
    glBindTexture(GL_TEXTURE_2D, s_sub_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, tex_w, tex_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, s_sub_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    s_sub_draw_w = tex_w;
    s_sub_draw_h = tex_h;
    s_sub_last_max_w = max_w;
    s_sub_dirty = 0;
    return 1;
}

static int __subtitle_ascii_fallback(char *out, size_t out_sz, const char *in)
{
    size_t j = 0;
    const char *p = in;
    while (*p && j + 1 < out_sz) {
        unsigned int cp = __utf8_next(&p);
        if (cp == '\r' || cp == '\n' || cp == '\t') {
            out[j++] = ' ';
        } else if (cp >= 32 && cp <= 126) {
            out[j++] = (char)cp;
        } else {
            out[j++] = '?';
        }
    }
    out[j] = '\0';
    return (int)j;
}

/* ---------------------------------------------------------------------------
 * Helpers: push geometry
 * --------------------------------------------------------------------------- */

static void __push_quad(float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1,
                        float r, float g, float b, float a)
{
    if (s_vert_count + 6 > OVL_MAX_VERTS) {
        return;
    }
    ovl_vert_t *v = &s_verts[s_vert_count];
    v[0] = (ovl_vert_t){x0,y0, u0,v0, r,g,b,a};
    v[1] = (ovl_vert_t){x1,y0, u1,v0, r,g,b,a};
    v[2] = (ovl_vert_t){x1,y1, u1,v1, r,g,b,a};
    v[3] = (ovl_vert_t){x0,y0, u0,v0, r,g,b,a};
    v[4] = (ovl_vert_t){x1,y1, u1,v1, r,g,b,a};
    v[5] = (ovl_vert_t){x0,y1, u0,v1, r,g,b,a};
    s_vert_count += 6;
}

static void __push_rect(float x, float y, float w, float h,
                        float r, float g, float b, float a)
{
    __push_quad(x, y, x + w, y + h, 0,0, 0,0, r, g, b, a);
}

static int s_text_batch_start = 0;

static void __push_char(float x, float y, char c,
                        float r, float g, float b, float a)
{
    if (c < 32 || c > 126) {
        return;
    }
    int idx = c - 32;
    int col = idx % 16;
    int row = idx / 16;
    float u0 = col * 8.0f / 128.0f;
    float v0 = row * 8.0f / 48.0f;
    float u1 = u0 + 8.0f / 128.0f;
    float v1 = v0 + 8.0f / 48.0f;
    __push_quad(x, y, x + OVL_CHAR_W, y + OVL_CHAR_H,
                u0, v0, u1, v1, r, g, b, a);
}

static float __push_text(float x, float y, const char *text, int max_chars,
                         float r, float g, float b, float a)
{
    float cx = x;
    for (int i = 0; text[i] && (max_chars <= 0 || i < max_chars); i++) {
        __push_char(cx, y, text[i], r, g, b, a);
        cx += OVL_CHAR_W;
    }
    return cx - x;
}

/**
 * @brief Check if coordinates hit the settings icon button
 * @param[in] mx mouse X
 * @param[in] my mouse Y
 * @return 1 if hit, 0 otherwise
 */
static int __icon_hit(int mx, int my)
{
    return mx >= OVL_ICON_MARGIN &&
           mx <  OVL_ICON_MARGIN + OVL_ICON_SIZE &&
           my >= OVL_ICON_MARGIN &&
           my <  OVL_ICON_MARGIN + OVL_ICON_SIZE;
}

/**
 * @brief Draw the settings icon (hamburger menu, pass 0 only)
 * @return none
 */
static void __draw_settings_icon(void)
{
    float x  = (float)OVL_ICON_MARGIN;
    float y  = (float)OVL_ICON_MARGIN;
    float sz = (float)OVL_ICON_SIZE;

    /* three bars only, no background */
    float bar_w = 20.0f;
    float bar_h = 3.0f;
    float gap   = 5.5f;
    float total = bar_h * 3.0f + gap * 2.0f;
    float bx    = x + (sz - bar_w) * 0.5f;
    float by    = y + (sz - total) * 0.5f;

    for (int i = 0; i < 3; i++) {
        float fy = by + (float)i * (bar_h + gap);
        __push_rect(bx, fy, bar_w, bar_h, 0.85f, 0.90f, 1.0f, 0.85f);
    }
}

/* ---------------------------------------------------------------------------
 * File scanning
 * --------------------------------------------------------------------------- */

static int __is_model_ext(const char *name)
{
    const char *exts[] = {".vrm",".glb",".pmx",".fbx",".gltf",".obj",NULL};
    size_t nlen = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (nlen > elen && strcasecmp(name + nlen - elen, exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void __scan_dir(ovl_file_t *out, int *count, int max,
                       const char *dir, int (*filter)(const char *),
                       int dirs_only)
{
    *count = 0;
    if (!dir || dir[0] == '\0') {
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < max) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (dirs_only) {
            char full[OVL_MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;
            }
            snprintf(out[*count].name, OVL_MAX_NAME, "%s", e->d_name);
            snprintf(out[*count].path, OVL_MAX_PATH, "%s", full);
        } else {
            if (filter && !filter(e->d_name)) {
                continue;
            }
            snprintf(out[*count].name, OVL_MAX_NAME, "%s", e->d_name);
            snprintf(out[*count].path, OVL_MAX_PATH, "%s/%s", dir, e->d_name);
        }
        (*count)++;
    }
    closedir(d);
    for (int i = 0; i < *count - 1; i++) {
        for (int j = i + 1; j < *count; j++) {
            if (strcmp(out[i].name, out[j].name) > 0) {
                ovl_file_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * GL resource management
 * --------------------------------------------------------------------------- */

static GLuint __compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[ovl] shader error: %s\n", log);
    }
    return s;
}

static void __create_font_texture(void)
{
    unsigned char atlas[48][128];
    memset(atlas, 0, sizeof(atlas));
    for (int ci = 0; ci < 95; ci++) {
        int col = ci % 16;
        int row = ci / 16;
        for (int r = 0; r < 8; r++) {
            unsigned char bits = FONT_8X8[ci][r];
            for (int c = 0; c < 8; c++) {
                if (bits & (0x80 >> c)) {
                    atlas[row * 8 + r][col * 8 + c] = 255;
                }
            }
        }
    }
    glGenTextures(1, &s_font_tex);
    glBindTexture(GL_TEXTURE_2D, s_font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 128, 48, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void __init_gl(void)
{
    GLuint vs = __compile(GL_VERTEX_SHADER, OVL_VS);
    GLuint fs = __compile(GL_FRAGMENT_SHADER, OVL_FS);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glBindAttribLocation(s_prog, 2, "a_col");
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    s_u_proj    = glGetUniformLocation(s_prog, "u_proj");
    s_u_tex     = glGetUniformLocation(s_prog, "u_tex");
    s_u_use_tex = glGetUniformLocation(s_prog, "u_use_tex");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_verts), NULL, GL_DYNAMIC_DRAW);

    const GLsizei stride = sizeof(ovl_vert_t);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)(4 * sizeof(float)));

    glBindVertexArray(0);

    __create_font_texture();
}

/* ---------------------------------------------------------------------------
 * Rendering — two-pass geometry build (rects first, then text)
 * --------------------------------------------------------------------------- */

static float __content_height(void)
{
    float h = OVL_PAD;
    h += OVL_SECTION_H + OVL_SEP_H;
    h += OVL_SECTION_H + (s_model_count > 0 ? s_model_count : 1) * OVL_ITEM_H + OVL_SEP_H;
    h += OVL_SECTION_H + (s_anim_count > 0 ? s_anim_count : 1) * OVL_ITEM_H + OVL_SEP_H;
    h += OVL_SECTION_H + (1 + (s_scene_count > 0 ? s_scene_count : 0)) * OVL_ITEM_H + OVL_SEP_H;
    h += OVL_ITEM_H * 4 + OVL_SEP_H;
    h += OVL_ITEM_H + OVL_PAD;
    return h;
}

/**
 * @brief Shared layout walker — computes Y for each item in the panel.
 *
 * @param[in] pass  0 = push colored rects only, 1 = push text only.
 * @param[in] win_w Window width.
 * @param[in] win_h Window height.
 * @return none
 */
static void __push_separator(float y, float pw)
{
    __push_rect(OVL_PAD, y + OVL_SEP_H * 0.5f - 0.5f,
                pw - OVL_PAD * 2, 1.0f, 0.28f, 0.28f, 0.38f, 0.5f);
}

static void __walk_panel(int pass, int win_w, int win_h)
{
    float pw = OVL_PANEL_W;
    float ph = (float)win_h;
    int mc = (int)((pw - OVL_PAD * 2) / OVL_CHAR_W);
    int mc_item = mc - 2;
    float ix = OVL_PAD + 2 * OVL_CHAR_W;

    if (pass == 0) {
        __push_rect(0, 0, pw, ph, 0.06f, 0.06f, 0.12f, 0.94f);
        __push_rect(pw - OVL_BORDER_W, 0, OVL_BORDER_W, ph,
                    0.30f, 0.55f, 0.85f, 0.6f);
    }

    float y = OVL_PAD - s_scroll;

    /* Title */
    if (pass == 1) {
        __push_text(OVL_PAD, y, "SETTINGS", mc, 0.40f, 0.72f, 1.0f, 1.0f);
    }
    y += OVL_SECTION_H;

    if (pass == 0) { __push_separator(y, pw); }
    y += OVL_SEP_H;

    /* --- Models --- */
    if (pass == 1) {
        __push_text(OVL_PAD, y, "> MODEL", mc, 0.94f, 0.72f, 0.20f, 1.0f);
    }
    y += OVL_SECTION_H;
    if (s_model_count == 0) {
        if (pass == 1) {
            __push_text(ix, y + 5, "(empty)", mc_item, 0.40f,0.40f,0.45f,1.0f);
        }
        y += OVL_ITEM_H;
    }
    for (int i = 0; i < s_model_count; i++) {
        int sel = (i == s_model_sel);
        if (pass == 0 && sel) {
            __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                        0.14f, 0.33f, 0.62f, 0.90f);
        }
        if (pass == 1) {
            if (sel) {
                __push_text(OVL_PAD, y + 5, ">", 0, 0.50f,0.80f,1.0f,1.0f);
                __push_text(ix, y + 5, s_models[i].name, mc_item, 1.0f,1.0f,1.0f,1.0f);
            } else {
                __push_text(ix, y + 5, s_models[i].name, mc_item, 0.68f,0.68f,0.72f,1.0f);
            }
        }
        y += OVL_ITEM_H;
    }

    if (pass == 0) { __push_separator(y, pw); }
    y += OVL_SEP_H;

    /* --- Animations --- */
    if (pass == 1) {
        __push_text(OVL_PAD, y, "> ANIMATION", mc, 0.94f, 0.72f, 0.20f, 1.0f);
    }
    y += OVL_SECTION_H;
    if (s_anim_count == 0) {
        if (pass == 1) {
            __push_text(ix, y + 5, "(empty)", mc_item, 0.40f,0.40f,0.45f,1.0f);
        }
        y += OVL_ITEM_H;
    }
    for (int i = 0; i < s_anim_count; i++) {
        int sel = (i == s_anim_sel);
        if (pass == 0 && sel) {
            __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                        0.14f, 0.33f, 0.62f, 0.90f);
        }
        if (pass == 1) {
            const char *n = (s_anim_names && s_anim_names[i]) ? s_anim_names[i] : "???";
            if (sel) {
                __push_text(OVL_PAD, y + 5, ">", 0, 0.50f,0.80f,1.0f,1.0f);
                __push_text(ix, y + 5, n, mc_item, 1.0f,1.0f,1.0f,1.0f);
            } else {
                __push_text(ix, y + 5, n, mc_item, 0.68f,0.68f,0.72f,1.0f);
            }
        }
        y += OVL_ITEM_H;
    }

    if (pass == 0) { __push_separator(y, pw); }
    y += OVL_SEP_H;

    /* --- Scenes --- */
    if (pass == 1) {
        __push_text(OVL_PAD, y, "> SCENE", mc, 0.94f, 0.72f, 0.20f, 1.0f);
    }
    y += OVL_SECTION_H;
    {
        int sel = (s_scene_sel < 0);
        if (pass == 0 && sel) {
            __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                        0.14f, 0.33f, 0.62f, 0.90f);
        }
        if (pass == 1) {
            if (sel) {
                __push_text(OVL_PAD, y + 5, ">", 0, 0.50f,0.80f,1.0f,1.0f);
                __push_text(ix, y + 5, "(none)", mc_item, 1.0f,1.0f,1.0f,1.0f);
            } else {
                __push_text(ix, y + 5, "(none)", mc_item, 0.68f,0.68f,0.72f,1.0f);
            }
        }
        y += OVL_ITEM_H;
    }
    for (int i = 0; i < s_scene_count; i++) {
        int sel = (i == s_scene_sel);
        if (pass == 0 && sel) {
            __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                        0.14f, 0.33f, 0.62f, 0.90f);
        }
        if (pass == 1) {
            if (sel) {
                __push_text(OVL_PAD, y + 5, ">", 0, 0.50f,0.80f,1.0f,1.0f);
                __push_text(ix, y + 5, s_scenes[i].name, mc_item, 1.0f,1.0f,1.0f,1.0f);
            } else {
                __push_text(ix, y + 5, s_scenes[i].name, mc_item, 0.68f,0.68f,0.72f,1.0f);
            }
        }
        y += OVL_ITEM_H;
    }

    if (pass == 0) { __push_separator(y, pw); }
    y += OVL_SEP_H;

    /* --- Toggles --- */
    if (pass == 0 && s_cam_locked) {
        __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                    0.12f, 0.28f, 0.22f, 0.60f);
    }
    if (pass == 1) {
        float cr, cg, cb;
        if (s_cam_locked) { cr = 0.30f; cg = 0.85f; cb = 0.65f; }
        else              { cr = 0.50f; cg = 0.50f; cb = 0.55f; }
        __push_text(OVL_PAD, y + 5,
                    s_cam_locked ? "[#] Lock Camera" : "[ ] Lock Camera",
                    mc, cr, cg, cb, 1.0f);
    }
    y += OVL_ITEM_H;

    if (pass == 0 && s_sub_enabled) {
        __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                    0.12f, 0.28f, 0.22f, 0.60f);
    }
    if (pass == 1) {
        float cr, cg, cb;
        if (s_sub_enabled) { cr = 0.30f; cg = 0.85f; cb = 0.65f; }
        else               { cr = 0.50f; cg = 0.50f; cb = 0.55f; }
        __push_text(OVL_PAD, y + 5,
                    s_sub_enabled ? "[#] Subtitle" : "[ ] Subtitle",
                    mc, cr, cg, cb, 1.0f);
    }
    y += OVL_ITEM_H;

    if (pass == 0 && s_fullscreen) {
        __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                    0.12f, 0.28f, 0.22f, 0.60f);
    }
    if (pass == 1) {
        float cr, cg, cb;
        if (s_fullscreen) { cr = 0.30f; cg = 0.85f; cb = 0.65f; }
        else              { cr = 0.50f; cg = 0.50f; cb = 0.55f; }
        __push_text(OVL_PAD, y + 5,
                    s_fullscreen ? "[#] Fullscreen" : "[ ] Fullscreen",
                    mc, cr, cg, cb, 1.0f);
    }
    y += OVL_ITEM_H;

    if (pass == 0 && s_spectator) {
        __push_rect(OVL_PAD, y + 1, pw - OVL_PAD * 2, OVL_ITEM_H - 2,
                    0.12f, 0.28f, 0.22f, 0.60f);
    }
    if (pass == 1) {
        float cr, cg, cb;
        if (s_spectator) { cr = 0.30f; cg = 0.85f; cb = 0.65f; }
        else             { cr = 0.50f; cg = 0.50f; cb = 0.55f; }
        __push_text(OVL_PAD, y + 5,
                    s_spectator ? "[#] Spectator" : "[ ] Spectator",
                    mc, cr, cg, cb, 1.0f);
    }
    y += OVL_ITEM_H;

    if (pass == 0) { __push_separator(y, pw); }
    y += OVL_SEP_H;

    /* --- Close button --- */
    if (pass == 0) {
        __push_rect(OVL_PAD, y + 2, pw - OVL_PAD * 2, OVL_ITEM_H - 4,
                    0.20f, 0.28f, 0.38f, 1.0f);
    }
    if (pass == 1) {
        float tw = 9.0f * OVL_CHAR_W;
        __push_text((pw - tw) * 0.5f, y + 5, "[ Close ]", 0,
                    0.85f, 0.88f, 0.95f, 1.0f);
    }
}

static void __walk_subtitle(int pass, int win_w, int win_h)
{
    if (!s_sub_enabled || s_subtitle[0] == '\0') {
        return;
    }
    float box_x = (float)OVL_SUB_MARGIN_X;
    float box_w = (float)(win_w - OVL_SUB_MARGIN_X * 2);
    float bar_y = (float)win_h - OVL_SUBTITLE_H - OVL_SUB_BOTTOM_GAP;
    if (box_w <= 0.0f) {
        return;
    }
    if (pass == 0) {
        __push_rect(box_x, bar_y, box_w, OVL_SUBTITLE_H,
                    0.03f, 0.03f, 0.08f, 0.78f);
        __push_rect(box_x, bar_y, box_w, 1.0f,
                    0.30f, 0.55f, 0.85f, 0.42f);
    }
    if (pass == 1) {
        if (__subtitle_font_init()) {
            return;
        }
        char ascii[OVL_SUBTITLE_MAX];
        int len = __subtitle_ascii_fallback(ascii, sizeof(ascii), s_subtitle);
        int max_c_per_line = (int)((box_w - OVL_SUB_INNER_PAD_X * 2) / OVL_CHAR_W);
        if (max_c_per_line < 1) {
            return;
        }
        int max_chars = max_c_per_line * OVL_SUB_MAX_LINES;
        int start = len > max_chars ? (len - max_chars) : 0;
        float tx = box_x + OVL_SUB_INNER_PAD_X;
        float ty = bar_y + OVL_SUB_INNER_PAD_Y;
        for (int line = 0; line < OVL_SUB_MAX_LINES && start < len; line++) {
            __push_text(tx, ty + line * (OVL_CHAR_H + OVL_SUB_LINE_GAP),
                        ascii + start, max_c_per_line, 0.95f, 0.96f, 1.0f, 1.0f);
            start += max_c_per_line;
        }
    }
}

static void __render_subtitle_texture(int win_w, int win_h)
{
    int box_x = OVL_SUB_MARGIN_X;
    int box_w = win_w - OVL_SUB_MARGIN_X * 2;
    int box_y = win_h - OVL_SUBTITLE_H - OVL_SUB_BOTTOM_GAP;
    int max_w = box_w - OVL_SUB_INNER_PAD_X * 2;
    if (max_w <= 0) {
        return;
    }
    if ((s_sub_dirty || s_sub_last_max_w != max_w) &&
        !__rebuild_subtitle_texture(max_w)) {
        return;
    }
    if (!s_sub_tex || s_sub_draw_w <= 0 || s_sub_draw_h <= 0) {
        return;
    }

    float x = (float)box_x + (float)OVL_SUB_INNER_PAD_X +
              ((float)max_w - (float)s_sub_draw_w) * 0.5f;
    if (x < (float)(box_x + OVL_SUB_INNER_PAD_X)) {
        x = (float)(box_x + OVL_SUB_INNER_PAD_X);
    }
    float content_h = (float)OVL_SUBTITLE_H - (float)(OVL_SUB_INNER_PAD_Y * 2);
    float y = (float)box_y + (float)OVL_SUB_INNER_PAD_Y +
              (content_h - (float)s_sub_draw_h) * 0.5f;

    ovl_vert_t quad[6];
    quad[0] = (ovl_vert_t){x, y, 0.0f, 0.0f, 0.95f, 0.96f, 1.0f, 1.0f};
    quad[1] = (ovl_vert_t){x + (float)s_sub_draw_w, y, 1.0f, 0.0f, 0.95f, 0.96f, 1.0f, 1.0f};
    quad[2] = (ovl_vert_t){x + (float)s_sub_draw_w, y + (float)s_sub_draw_h, 1.0f, 1.0f, 0.95f, 0.96f, 1.0f, 1.0f};
    quad[3] = (ovl_vert_t){x, y, 0.0f, 0.0f, 0.95f, 0.96f, 1.0f, 1.0f};
    quad[4] = (ovl_vert_t){x + (float)s_sub_draw_w, y + (float)s_sub_draw_h, 1.0f, 1.0f, 0.95f, 0.96f, 1.0f, 1.0f};
    quad[5] = (ovl_vert_t){x, y + (float)s_sub_draw_h, 0.0f, 1.0f, 0.95f, 0.96f, 1.0f, 1.0f};

    glBindTexture(GL_TEXTURE_2D, s_sub_tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glUniform1i(s_u_use_tex, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---------------------------------------------------------------------------
 * GL state save / restore
 * --------------------------------------------------------------------------- */
typedef struct {
    GLint     prog, vao, vbo, active_tex, tex_2d;
    GLint     blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint     viewport[4];
    GLint     scissor_box[4];
    GLboolean blend, depth, cull, scissor;
} gl_saved_state_t;

static void __save_gl(gl_saved_state_t *st)
{
    glGetIntegerv(GL_CURRENT_PROGRAM,      &st->prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &st->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->vbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE,       &st->active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,   &st->tex_2d);
    glGetIntegerv(GL_BLEND_SRC_RGB,        &st->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,        &st->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,      &st->blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA,      &st->blend_dst_a);
    glGetIntegerv(GL_VIEWPORT,              st->viewport);
    glGetIntegerv(GL_SCISSOR_BOX,           st->scissor_box);
    st->blend   = glIsEnabled(GL_BLEND);
    st->depth   = glIsEnabled(GL_DEPTH_TEST);
    st->cull    = glIsEnabled(GL_CULL_FACE);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void __restore_gl(const gl_saved_state_t *st)
{
    glUseProgram(st->prog);
    glBindVertexArray(st->vao);
    glBindBuffer(GL_ARRAY_BUFFER, st->vbo);
    glActiveTexture(st->active_tex);
    glBindTexture(GL_TEXTURE_2D, st->tex_2d);
    glBlendFuncSeparate(st->blend_src_rgb, st->blend_dst_rgb,
                        st->blend_src_a, st->blend_dst_a);
    glViewport(st->viewport[0], st->viewport[1],
               st->viewport[2], st->viewport[3]);
    glScissor(st->scissor_box[0], st->scissor_box[1],
              st->scissor_box[2], st->scissor_box[3]);
    if (st->blend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (st->depth)   glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->cull)    glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

/* ---------------------------------------------------------------------------
 * Event handling
 * --------------------------------------------------------------------------- */

static int __hit_panel_item(int mx, int my, int win_h,
                            int *section, int *index)
{
    if (mx < 0 || mx >= OVL_PANEL_W) {
        return 0;
    }

    float y = OVL_PAD - s_scroll;
    y += OVL_SECTION_H;    /* title */
    y += OVL_SEP_H;        /* separator */

    /* Models */
    *section = 0;
    y += OVL_SECTION_H;
    int mc = s_model_count > 0 ? s_model_count : 1;
    for (int i = 0; i < mc; i++) {
        if (my >= y && my < y + OVL_ITEM_H) {
            *index = (s_model_count > 0) ? i : -1;
            return 1;
        }
        y += OVL_ITEM_H;
    }
    y += OVL_SEP_H;

    /* Animations */
    *section = 1;
    y += OVL_SECTION_H;
    int ac = s_anim_count > 0 ? s_anim_count : 1;
    for (int i = 0; i < ac; i++) {
        if (my >= y && my < y + OVL_ITEM_H) {
            *index = (s_anim_count > 0) ? i : -1;
            return 1;
        }
        y += OVL_ITEM_H;
    }
    y += OVL_SEP_H;

    /* Scenes */
    *section = 2;
    y += OVL_SECTION_H;
    if (my >= y && my < y + OVL_ITEM_H) {
        *index = -1;
        return 1;
    }
    y += OVL_ITEM_H;
    for (int i = 0; i < s_scene_count; i++) {
        if (my >= y && my < y + OVL_ITEM_H) {
            *index = i;
            return 1;
        }
        y += OVL_ITEM_H;
    }
    y += OVL_SEP_H;

    /* Camera toggle */
    *section = 3;
    *index = 0;
    if (my >= y && my < y + OVL_ITEM_H) {
        return 1;
    }
    y += OVL_ITEM_H;

    /* Subtitle toggle */
    *section = 4;
    if (my >= y && my < y + OVL_ITEM_H) {
        return 1;
    }
    y += OVL_ITEM_H;

    /* Fullscreen toggle */
    *section = 5;
    if (my >= y && my < y + OVL_ITEM_H) {
        return 1;
    }
    y += OVL_ITEM_H;

    /* Spectator toggle */
    *section = 6;
    if (my >= y && my < y + OVL_ITEM_H) {
        return 1;
    }
    y += OVL_ITEM_H;
    y += OVL_SEP_H;

    /* Close button */
    *section = 7;
    if (my >= y && my < y + OVL_ITEM_H) {
        return 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

int settings_overlay_init(const settings_overlay_cfg_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    s_model_dir[0] = '\0';
    s_scene_dir[0] = '\0';
    if (cfg->model_dir) {
        strncpy(s_model_dir, cfg->model_dir, OVL_MAX_PATH - 1);
        s_model_dir[OVL_MAX_PATH - 1] = '\0';
    }
    if (cfg->scene_parent_dir) {
        strncpy(s_scene_dir, cfg->scene_parent_dir, OVL_MAX_PATH - 1);
        s_scene_dir[OVL_MAX_PATH - 1] = '\0';
    }

    s_anim_names = cfg->anim_names;
    s_anim_count = cfg->anim_count;
    s_anim_sel   = cfg->active_anim;
    s_cam_locked   = cfg->camera_locked;
    s_sub_enabled  = cfg->subtitle_enabled;
    s_fullscreen   = cfg->fullscreen;
    s_spectator    = cfg->spectator;
    s_subtitle[0]  = '\0';
    __subtitle_texture_reset();
    s_scroll       = 0.0f;
    s_visible      = 0;

    s_cb_model = cfg->on_model_change;
    s_cb_anim  = cfg->on_anim_change;
    s_cb_scene = cfg->on_scene_change;
    s_cb_cam   = cfg->on_camera_lock;
    s_cb_sub   = cfg->on_subtitle;
    s_cb_fs    = cfg->on_fullscreen;
    s_cb_spec  = cfg->on_spectator;
    s_cb_ud    = cfg->user_data;

    __scan_dir(s_models, &s_model_count, OVL_MAX_FILES,
               s_model_dir, __is_model_ext, 0);
    __scan_dir(s_scenes, &s_scene_count, OVL_MAX_FILES,
               s_scene_dir, NULL, 1);

    s_model_sel = -1;
    if (cfg->current_model) {
        for (int i = 0; i < s_model_count; i++) {
            if (strcmp(s_models[i].name, cfg->current_model) == 0) {
                s_model_sel = i;
                break;
            }
        }
    }
    s_scene_sel = -1;
    if (cfg->current_scene && cfg->current_scene[0] != '\0') {
        for (int i = 0; i < s_scene_count; i++) {
            if (strcmp(s_scenes[i].name, cfg->current_scene) == 0) {
                s_scene_sel = i;
                break;
            }
        }
    }

    __init_gl();

    printf("[settings_overlay] init: %d models, %d scenes, %d anims\n",
           s_model_count, s_scene_count, s_anim_count);
    return 0;
}

void settings_overlay_destroy(void)
{
    if (s_sub_tex)  { glDeleteTextures(1, &s_sub_tex); s_sub_tex = 0; }
    if (s_font_tex) { glDeleteTextures(1, &s_font_tex); s_font_tex = 0; }
    if (s_vbo)      { glDeleteBuffers(1, &s_vbo);       s_vbo = 0; }
    if (s_vao)      { glDeleteVertexArrays(1, &s_vao);  s_vao = 0; }
    if (s_prog)     { glDeleteProgram(s_prog);           s_prog = 0; }
    free(s_sub_pixels); s_sub_pixels = NULL; s_sub_pixels_cap = 0;
    free(s_sub_font_buf); s_sub_font_buf = NULL;
    s_sub_font_ready = 0;
    s_sub_font_tried = 0;
}

int settings_overlay_handle_event(const SDL_Event *ev, int win_w, int win_h)
{
    if (!ev) {
        return 0;
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        if (__icon_hit(ev->button.x, ev->button.y)) {
            s_visible = !s_visible;
            s_scroll  = 0.0f;
            return 1;
        }
    }

    if (!s_visible) {
        return 0;
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        int mx = ev->button.x;
        int my = ev->button.y;
        if (mx >= OVL_PANEL_W) {
            return 0;
        }
        int section = -1, index = -1;
        if (__hit_panel_item(mx, my, win_h, &section, &index)) {
            switch (section) {
            case 0:
                if (index >= 0 && index < s_model_count && index != s_model_sel) {
                    s_model_sel = index;
                    if (s_cb_model) {
                        s_cb_model(s_models[index].path, s_cb_ud);
                    }
                }
                break;
            case 1:
                if (index >= 0 && index < s_anim_count && index != s_anim_sel) {
                    s_anim_sel = index;
                    if (s_cb_anim) {
                        s_cb_anim(index, s_cb_ud);
                    }
                }
                break;
            case 2:
                if (index != s_scene_sel) {
                    s_scene_sel = index;
                    if (s_cb_scene) {
                        s_cb_scene(index < 0 ? "" : s_scenes[index].path, s_cb_ud);
                    }
                }
                break;
            case 3:
                s_cam_locked = !s_cam_locked;
                if (s_cb_cam) {
                    s_cb_cam(s_cam_locked, s_cb_ud);
                }
                break;
            case 4:
                s_sub_enabled = !s_sub_enabled;
                if (s_cb_sub) {
                    s_cb_sub(s_sub_enabled, s_cb_ud);
                }
                break;
            case 5:
                s_fullscreen = !s_fullscreen;
                if (s_cb_fs) {
                    s_cb_fs(s_fullscreen, s_cb_ud);
                }
                break;
            case 6:
                s_spectator = !s_spectator;
                if (s_cb_spec) {
                    s_cb_spec(s_spectator, s_cb_ud);
                }
                break;
            case 7:
                s_visible = 0;
                break;
            }
        }
        return 1;
    }

    if (ev->type == SDL_MOUSEWHEEL && s_visible) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        if (mx < OVL_PANEL_W) {
            s_scroll -= ev->wheel.y * OVL_ITEM_H;
            float max_scroll = __content_height() - (float)win_h;
            if (max_scroll < 0) max_scroll = 0;
            if (s_scroll < 0) s_scroll = 0;
            if (s_scroll > max_scroll) s_scroll = max_scroll;
            return 1;
        }
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEMOTION) {
        int mx = (ev->type == SDL_MOUSEBUTTONDOWN) ? ev->button.x : ev->motion.x;
        if (mx < OVL_PANEL_W) {
            return 1;
        }
    }

    return 0;
}

void settings_overlay_render(int win_w, int win_h)
{
    int has_subtitle = (s_sub_enabled && s_subtitle[0] != '\0');

    gl_saved_state_t saved;
    __save_gl(&saved);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(s_prog);

    float proj[16];
    memset(proj, 0, sizeof(proj));
    proj[0]  =  2.0f / (float)win_w;
    proj[5]  = -2.0f / (float)win_h;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] =  1.0f;
    proj[15] =  1.0f;
    glUniformMatrix4fv(s_u_proj, 1, GL_FALSE, proj);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_font_tex);
    glUniform1i(s_u_tex, 0);

    s_vert_count = 0;

    /* Pass 0: colored rectangles */
    if (s_visible) {
        __walk_panel(0, win_w, win_h);
    } else {
        __draw_settings_icon();
    }
    __walk_subtitle(0, win_w, win_h);

    s_text_batch_start = s_vert_count;

    /* Pass 1: text quads */
    if (s_visible) {
        __walk_panel(1, win_w, win_h);
    }
    __walk_subtitle(1, win_w, win_h);

    if (s_vert_count == 0) {
        __restore_gl(&saved);
        return;
    }

    /* Upload vertex data */
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)(s_vert_count * sizeof(ovl_vert_t)), s_verts);

    /* Draw colored rects (no texture), then text (with font texture) */
    if (s_text_batch_start > 0) {
        glUniform1i(s_u_use_tex, 0);
        glDrawArrays(GL_TRIANGLES, 0, s_text_batch_start);
    }
    if (s_vert_count > s_text_batch_start) {
        glUniform1i(s_u_use_tex, 1);
        glDrawArrays(GL_TRIANGLES, s_text_batch_start,
                     s_vert_count - s_text_batch_start);
    }

    if (has_subtitle) {
        __render_subtitle_texture(win_w, win_h);
    }

    glBindVertexArray(0);

    __restore_gl(&saved);
}

void settings_overlay_toggle(void)
{
    s_visible = !s_visible;
    s_scroll  = 0.0f;
}

int settings_overlay_is_visible(void)
{
    return s_visible;
}

int settings_overlay_camera_locked(void)
{
    return s_cam_locked;
}

int settings_overlay_subtitle_enabled(void)
{
    return s_sub_enabled;
}

int settings_overlay_fullscreen(void)
{
    return s_fullscreen;
}

int settings_overlay_spectator(void)
{
    return s_spectator;
}

void settings_overlay_set_subtitle(const char *text)
{
    if (!text || text[0] == '\0') {
        s_subtitle[0] = '\0';
        __subtitle_texture_reset();
        return;
    }

    size_t text_len = strlen(text);
    const char *end = text + text_len;
    const char *scan = end;
    int cp_back = 0;

    while (scan > text && cp_back < OVL_SUB_MAX_GLYPHS) {
        scan--;
        if (((unsigned char)*scan & 0xC0) != 0x80) {
            cp_back++;
        }
    }

    const char *start = (cp_back >= OVL_SUB_MAX_GLYPHS) ? scan : text;

    size_t j = 0;
    for (size_t i = 0; start[i] != '\0' && j + 1 < OVL_SUBTITLE_MAX; i++) {
        char ch = start[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        s_subtitle[j++] = ch;
    }
    s_subtitle[j] = '\0';
    __subtitle_texture_reset();
}

void settings_overlay_update_anims(char **names, int count, int active)
{
    s_anim_names = names;
    s_anim_count = count;
    s_anim_sel   = active;
}
