/**
 * @file mat4_util.h
 * @brief Minimal column-major 4x4 matrix math utilities for C (no GLM dependency).
 */

#ifndef MAT4_UTIL_H
#define MAT4_UTIL_H

#include <math.h>
#include <string.h>

/* Column-major 4x4 matrix: m[col*4 + row] */
typedef float mat4[16];
typedef float vec3[3];

/* ---- basic helpers ---- */

static inline void mat4_identity(mat4 m)
{
    memset(m, 0, sizeof(mat4));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_copy(mat4 dst, const mat4 src)
{
    memcpy(dst, src, sizeof(mat4));
}

/* out = a * b  (safe when out aliases a or b) */
static inline void mat4_multiply(mat4 out, const mat4 a, const mat4 b)
{
    mat4 tmp;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a[k * 4 + r] * b[c * 4 + k];
            tmp[c * 4 + r] = sum;
        }
    }
    memcpy(out, tmp, sizeof(mat4));
}

/* ---- projection ---- */

static inline void mat4_perspective(mat4 m, float fov_rad, float aspect,
                                    float znear, float zfar)
{
    memset(m, 0, sizeof(mat4));
    float f = 1.0f / tanf(fov_rad * 0.5f);
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

/* ---- vec3 helpers ---- */

static inline void vec3_sub(vec3 out, const vec3 a, const vec3 b)
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static inline float vec3_dot(const vec3 a, const vec3 b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void vec3_cross(vec3 out, const vec3 a, const vec3 b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline void vec3_normalize(vec3 v)
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

/* ---- view matrix ---- */

static inline void mat4_look_at(mat4 m, const vec3 eye, const vec3 center,
                                const vec3 up)
{
    vec3 f, s, u;
    vec3_sub(f, center, eye);
    vec3_normalize(f);
    vec3_cross(s, f, up);
    vec3_normalize(s);
    vec3_cross(u, s, f);

    mat4_identity(m);
    m[0]  =  s[0]; m[4]  =  s[1]; m[8]  =  s[2];
    m[1]  =  u[0]; m[5]  =  u[1]; m[9]  =  u[2];
    m[2]  = -f[0]; m[6]  = -f[1]; m[10] = -f[2];
    m[12] = -vec3_dot(s, eye);
    m[13] = -vec3_dot(u, eye);
    m[14] =  vec3_dot(f, eye);
}

/* ---- transforms ---- */

static inline void mat4_rotate_x(mat4 m, float rad)
{
    mat4_identity(m);
    float c = cosf(rad), s = sinf(rad);
    m[5] = c;  m[6]  = s;
    m[9] = -s; m[10] = c;
}

static inline void mat4_rotate_y(mat4 m, float rad)
{
    mat4_identity(m);
    float c = cosf(rad), s = sinf(rad);
    m[0] = c;  m[2]  = -s;
    m[8] = s;  m[10] =  c;
}

static inline void mat4_translate(mat4 m, float x, float y, float z)
{
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static inline void mat4_scale_uniform(mat4 m, float s)
{
    mat4_identity(m);
    m[0] = s;
    m[5] = s;
    m[10] = s;
}

static inline void mat4_ortho(mat4 m, float left, float right,
                              float bottom, float top,
                              float znear, float zfar)
{
    memset(m, 0, sizeof(mat4));
    m[0]  =  2.0f / (right - left);
    m[5]  =  2.0f / (top - bottom);
    m[10] = -2.0f / (zfar - znear);
    m[12] = -(right + left)   / (right - left);
    m[13] = -(top   + bottom) / (top   - bottom);
    m[14] = -(zfar  + znear)  / (zfar  - znear);
    m[15] = 1.0f;
}

/* Extract 3×3 normal matrix (cofactor of upper-left 3×3 of model matrix).
   For uniform-scale models this equals the upper-left 3×3 transposed inverse. */
static inline void mat4_normal_matrix(float out[9], const mat4 m)
{
    out[0] = m[5] * m[10] - m[6] * m[9];
    out[1] = m[6] * m[8]  - m[4] * m[10];
    out[2] = m[4] * m[9]  - m[5] * m[8];
    out[3] = m[2] * m[9]  - m[1] * m[10];
    out[4] = m[0] * m[10] - m[2] * m[8];
    out[5] = m[1] * m[8]  - m[0] * m[9];
    out[6] = m[1] * m[6]  - m[2] * m[5];
    out[7] = m[2] * m[4]  - m[0] * m[6];
    out[8] = m[0] * m[5]  - m[1] * m[4];
}

#endif /* MAT4_UTIL_H */
