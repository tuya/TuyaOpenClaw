/**
 * @file spring_bone.c
 * @brief VRM Spring Bone physics — referencing UnityChan SpringBone algorithm.
 *
 * Core algorithm (from UnityChan SpringBone):
 *   1. Compute equilibrium position: head + baseWorldRot * boneAxis * springLength
 *   2. Hooke's law: force = stiffness * (equilibrium - current)
 *   3. Add gravity + external forces, scale by 0.5 * dt²
 *   4. Verlet: curr += force + (1 - drag) * (curr - prev)
 *   5. Length constraint: project tail onto sphere of springLength around head
 *   6. Collider resolution: push tail out of collider spheres
 *   7. Compute rotation: initialLocalRot * FromToRotation(boneAxis, localBoneVector)
 */

#include "spring_bone.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ================================================================== */
/*  Math helpers (local)                                               */
/* ================================================================== */

static inline float v3_dot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline void v3_sub(float out[3], const float a[3], const float b[3])
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static inline void v3_add(float out[3], const float a[3], const float b[3])
{
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
}

static inline void v3_scale(float out[3], const float v[3], float s)
{
    out[0] = v[0] * s;
    out[1] = v[1] * s;
    out[2] = v[2] * s;
}

static inline float v3_length(const float v[3])
{
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

static inline void v3_normalize(float v[3])
{
    float len = v3_length(v);
    if (len > 1e-8f) {
        float inv = 1.0f / len;
        v[0] *= inv; v[1] *= inv; v[2] *= inv;
    }
}

static inline void v3_copy(float dst[3], const float src[3])
{
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
}

/* ---- Quaternion helpers (x,y,z,w order) ---- */

static inline void q_identity(float q[4])
{
    q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 1;
}

static void q_mul(float out[4], const float a[4], const float b[4])
{
    float ax=a[0], ay=a[1], az=a[2], aw=a[3];
    float bx=b[0], by=b[1], bz=b[2], bw=b[3];
    out[0] = aw*bx + ax*bw + ay*bz - az*by;
    out[1] = aw*by - ax*bz + ay*bw + az*bx;
    out[2] = aw*bz + ax*by - ay*bx + az*bw;
    out[3] = aw*bw - ax*bx - ay*by - az*bz;
}

static inline void q_conjugate(float out[4], const float q[4])
{
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

/** Rotate a vector by a quaternion: out = q * (0,v) * q⁻¹ */
static void q_rotate_vec3(float out[3], const float q[4], const float v[3])
{
    /* q * v: treat v as pure quaternion (v.x, v.y, v.z, 0) */
    float qv[4];
    qv[0] =  q[3]*v[0] + q[1]*v[2] - q[2]*v[1];
    qv[1] =  q[3]*v[1] + q[2]*v[0] - q[0]*v[2];
    qv[2] =  q[3]*v[2] + q[0]*v[1] - q[1]*v[0];
    qv[3] = -q[0]*v[0] - q[1]*v[1] - q[2]*v[2];

    /* result = (qv) * q⁻¹ */
    out[0] = qv[3]*(-q[0]) + qv[0]*q[3] + qv[1]*(-q[2]) - qv[2]*(-q[1]);
    out[1] = qv[3]*(-q[1]) - qv[0]*(-q[2]) + qv[1]*q[3] + qv[2]*(-q[0]);
    out[2] = qv[3]*(-q[2]) + qv[0]*(-q[1]) - qv[1]*(-q[0]) + qv[2]*q[3];
}

/** Compute quaternion that rotates unit vector 'from' to unit vector 'to'. */
static void q_from_to_rotation(float q[4], const float from[3], const float to[3])
{
    float dot = v3_dot(from, to);

    if (dot > 0.999999f) {
        q_identity(q);
        return;
    }

    if (dot < -0.999999f) {
        /* Nearly opposite — pick arbitrary perpendicular axis */
        float axis[3] = {1, 0, 0};
        if (fabsf(from[0]) > 0.9f) { axis[0] = 0; axis[1] = 1; }
        /* cross(from, axis) */
        float c[3];
        c[0] = from[1]*axis[2] - from[2]*axis[1];
        c[1] = from[2]*axis[0] - from[0]*axis[2];
        c[2] = from[0]*axis[1] - from[1]*axis[0];
        v3_normalize(c);
        q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = 0.0f;
        return;
    }

    /* cross(from, to) */
    float cross[3];
    cross[0] = from[1]*to[2] - from[2]*to[1];
    cross[1] = from[2]*to[0] - from[0]*to[2];
    cross[2] = from[0]*to[1] - from[1]*to[0];

    q[0] = cross[0];
    q[1] = cross[1];
    q[2] = cross[2];
    q[3] = 1.0f + dot;

    /* Normalize */
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-8f) {
        float inv = 1.0f / len;
        q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
    }
}

/** Build a 4x4 column-major matrix from quaternion + translation. */
static void q_to_mat4(float m[16], const float q[4], const float t[3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float x2 = x+x, y2 = y+y, z2 = z+z;
    float xx = x*x2, xy = x*y2, xz = x*z2;
    float yy = y*y2, yz = y*z2, zz = z*z2;
    float wx = w*x2, wy = w*y2, wz = w*z2;

    m[ 0] = 1.0f - (yy + zz);  m[ 4] = xy - wz;           m[ 8] = xz + wy;           m[12] = t[0];
    m[ 1] = xy + wz;            m[ 5] = 1.0f - (xx + zz);  m[ 9] = yz - wx;            m[13] = t[1];
    m[ 2] = xz - wy;            m[ 6] = yz + wx;            m[10] = 1.0f - (xx + yy);  m[14] = t[2];
    m[ 3] = 0.0f;               m[ 7] = 0.0f;               m[11] = 0.0f;               m[15] = 1.0f;
}

/* ---- Matrix helpers ---- */

/** Transform a point by a 4x4 column-major matrix. */
static inline void mat4_transform_point(float out[3], const float m[16], const float p[3])
{
    out[0] = m[0]*p[0] + m[4]*p[1] + m[ 8]*p[2] + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[ 9]*p[2] + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

/** Extract position from a 4x4 col-major matrix. */
static inline void mat4_get_position(float out[3], const float m[16])
{
    out[0] = m[12]; out[1] = m[13]; out[2] = m[14];
}

/** Multiply two 4x4 col-major matrices: out = a * b. out must not alias a or b. */
static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k*4+r] * b[c*4+k];
            out[c*4+r] = s;
        }
    }
}

/** Invert a 4x4 matrix (general). Returns 0 on success, -1 on singular. */
static int mat4_invert(float inv[16], const float m[16])
{
    float t[16];
    t[ 0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    t[ 4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    t[ 8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    t[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    t[ 1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    t[ 5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    t[ 9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    t[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    t[ 2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    t[ 6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    t[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    t[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    t[ 3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    t[ 7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    t[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    t[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];

    float det = m[0]*t[0] + m[1]*t[4] + m[2]*t[8] + m[3]*t[12];
    if (fabsf(det) < 1e-12f) return -1;
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] = t[i] * inv_det;
    return 0;
}

/** Extract rotation quaternion from a 4x4 column-major matrix (assumes no shear).
 *  Result in (x,y,z,w) order. */
static void mat4_to_quat(float q[4], const float m[16])
{
    /* Normalize columns to remove scale */
    float c0[3] = {m[0], m[1], m[2]};
    float c1[3] = {m[4], m[5], m[6]};
    float c2[3] = {m[8], m[9], m[10]};
    float sx = v3_length(c0); if (sx < 1e-8f) sx = 1.0f;
    float sy = v3_length(c1); if (sy < 1e-8f) sy = 1.0f;
    float sz = v3_length(c2); if (sz < 1e-8f) sz = 1.0f;
    float r00 = m[0]/sx, r10 = m[1]/sx, r20 = m[2]/sx;
    float r01 = m[4]/sy, r11 = m[5]/sy, r21 = m[6]/sy;
    float r02 = m[8]/sz, r12 = m[9]/sz, r22 = m[10]/sz;

    float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        float s = 0.5f / sqrtf(trace + 1.0f);
        q[3] = 0.25f / s;
        q[0] = (r21 - r12) * s;
        q[1] = (r02 - r20) * s;
        q[2] = (r10 - r01) * s;
    } else if (r00 > r11 && r00 > r22) {
        float s = 2.0f * sqrtf(1.0f + r00 - r11 - r22);
        q[3] = (r21 - r12) / s;
        q[0] = 0.25f * s;
        q[1] = (r01 + r10) / s;
        q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = 2.0f * sqrtf(1.0f + r11 - r00 - r22);
        q[3] = (r02 - r20) / s;
        q[0] = (r01 + r10) / s;
        q[1] = 0.25f * s;
        q[2] = (r12 + r21) / s;
    } else {
        float s = 2.0f * sqrtf(1.0f + r22 - r00 - r11);
        q[3] = (r10 - r01) / s;
        q[0] = (r02 + r20) / s;
        q[1] = (r12 + r21) / s;
        q[2] = 0.25f * s;
    }

    /* Normalize */
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-8f) {
        float inv = 1.0f / len;
        q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
    }
}

/* ================================================================== */
/*  Global transform recovery                                          */
/* ================================================================== */

/**
 * Recover world-space global transforms from skinning matrices.
 * global[i] = bone_matrices[i] * inverse(offset_matrix[i])
 */
static void recover_globals(const vrm_model_t *model, const float *bone_matrices,
                            float *globals)
{
    uint32_t nb = model->bone_count;
    for (uint32_t i = 0; i < nb; i++) {
        float inv_offset[16];
        if (mat4_invert(inv_offset, model->bones[i].offset_matrix) == 0) {
            mat4_mul(&globals[i*16], &bone_matrices[i*16], inv_offset);
        } else {
            memcpy(&globals[i*16], &bone_matrices[i*16], 16*sizeof(float));
        }
    }
}

/**
 * Get the parent world rotation quaternion for a bone.
 * If bone has no parent, returns identity.
 */
static void get_parent_world_rotation(const vrm_model_t *model, const float *globals,
                                      int bone_index, float parent_rot[4])
{
    int parent = model->bones[bone_index].parent;
    if (parent >= 0) {
        mat4_to_quat(parent_rot, &globals[parent * 16]);
    } else {
        q_identity(parent_rot);
    }
}

/* ================================================================== */
/*  Find child bone                                                    */
/* ================================================================== */

/** Find the first child bone of a given bone. Returns -1 if leaf. */
static int find_first_child(const vrm_model_t *model, int bone_index)
{
    for (uint32_t i = 0; i < model->bone_count; i++) {
        if (model->bones[i].parent == bone_index)
            return (int)i;
    }
    return -1;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void spring_bone_init(spring_bone_ctx_t *ctx, vrm_model_t *model)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->model = model;
    ctx->enabled = 1;
    ctx->initialized = 0;

    if (model->spring_group_count == 0) {
        printf("[spring_bone] no spring bone groups found\n");
        return;
    }

    /* Each spring group becomes one chain. */
    ctx->chain_count = (int)model->spring_group_count;
    ctx->chains = (spring_chain_t *)calloc(ctx->chain_count, sizeof(spring_chain_t));

    for (int g = 0; g < ctx->chain_count; g++) {
        vrm_spring_group_t *sg = &model->spring_groups[g];
        spring_chain_t *chain = &ctx->chains[g];

        chain->joint_count = (int)sg->joint_count;
        chain->joints = (spring_joint_state_t *)calloc(chain->joint_count, sizeof(spring_joint_state_t));
        chain->center_bone = sg->center_bone;

        /* Copy collider group references */
        chain->collider_group_count = (int)sg->collider_group_count;
        if (chain->collider_group_count > 0) {
            chain->collider_group_indices = (int *)malloc(chain->collider_group_count * sizeof(int));
            memcpy(chain->collider_group_indices, sg->collider_group_indices,
                   chain->collider_group_count * sizeof(int));
        }

        for (int j = 0; j < chain->joint_count; j++) {
            spring_joint_state_t *js = &chain->joints[j];
            vrm_spring_joint_t *jd = &sg->joints[j];

            js->bone_index    = jd->bone_index;
            js->stiffness     = jd->stiffness;
            js->gravity_power = jd->gravity_power;
            v3_copy(js->gravity_dir, jd->gravity_dir);
            js->drag_force    = jd->drag_force;
            js->hit_radius    = jd->hit_radius;
            js->spring_length = 0.0f;

            /* Will be computed on first frame */
            q_identity(js->initial_local_rot);
            js->bone_axis[0] = 0; js->bone_axis[1] = 1; js->bone_axis[2] = 0;
            memset(js->current_tail, 0, sizeof(js->current_tail));
            memset(js->prev_tail, 0, sizeof(js->prev_tail));
        }
    }

    printf("[spring_bone] initialized: %d chains\n", ctx->chain_count);
}

/**
 * Initialize joint positions and rest-pose data from current bone world transforms.
 * This mirrors UnityChan's Initialize():
 *   - boneAxis = normalize(localChildPosition)
 *   - initialLocalRotation = bone.localRotation
 *   - springLength = distance(bone.position, childPosition)
 */
static void init_joint_rest_pose(spring_bone_ctx_t *ctx, const float *globals)
{
    for (int c = 0; c < ctx->chain_count; c++) {
        spring_chain_t *chain = &ctx->chains[c];
        for (int j = 0; j < chain->joint_count; j++) {
            spring_joint_state_t *js = &chain->joints[j];
            if (js->bone_index < 0) continue;

            const float *bone_global = &globals[js->bone_index * 16];

            /* Head position (world) */
            float head[3];
            mat4_get_position(head, bone_global);

            /* Find child position (world) */
            float child_world[3];
            int child = find_first_child(ctx->model, js->bone_index);
            if (child >= 0) {
                mat4_get_position(child_world, &globals[child * 16]);
            } else {
                /* No child: extend along bone's local +Y by default 7cm */
                float local_dir[3] = {0.0f, 0.07f, 0.0f};
                mat4_transform_point(child_world, bone_global, local_dir);
            }

            /* springLength = distance(head, child_world) */
            float diff[3];
            v3_sub(diff, child_world, head);
            js->spring_length = v3_length(diff);
            if (js->spring_length < 1e-6f) js->spring_length = 0.07f;

            /* boneAxis: child position in bone's LOCAL space, normalized.
             * localChildPos = inverse(bone_global) * child_world */
            float inv_global[16];
            if (mat4_invert(inv_global, bone_global) == 0) {
                float local_child[3];
                mat4_transform_point(local_child, inv_global, child_world);
                float axis_len = v3_length(local_child);
                if (axis_len > 1e-6f) {
                    v3_scale(js->bone_axis, local_child, 1.0f / axis_len);
                } else {
                    js->bone_axis[0] = 0; js->bone_axis[1] = 1; js->bone_axis[2] = 0;
                }
            } else {
                js->bone_axis[0] = 0; js->bone_axis[1] = 1; js->bone_axis[2] = 0;
            }

            /* initialLocalRotation: extract from bone's rest local_transform */
            mat4_to_quat(js->initial_local_rot, ctx->model->bones[js->bone_index].local_transform);

            /* Initialize tail positions to current child world position */
            v3_copy(js->current_tail, child_world);
            v3_copy(js->prev_tail, child_world);
        }
    }
}

void spring_bone_reset(spring_bone_ctx_t *ctx, const float *bone_matrices)
{
    if (!ctx || !ctx->model) return;

    uint32_t nb = ctx->model->bone_count;
    float *globals = (float *)malloc(nb * 16 * sizeof(float));
    recover_globals(ctx->model, bone_matrices, globals);
    init_joint_rest_pose(ctx, globals);
    free(globals);
    ctx->initialized = 1;
}

void spring_bone_update(spring_bone_ctx_t *ctx, float dt, float *bone_matrices)
{
    if (!ctx || !ctx->model || !ctx->enabled || ctx->chain_count == 0) return;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f; /* clamp */

    uint32_t nb = ctx->model->bone_count;

    /* ---- Recover global transforms from bone_matrices ---- */
    float *globals = (float *)malloc(nb * 16 * sizeof(float));
    recover_globals(ctx->model, bone_matrices, globals);

    /* ---- Initialize on first frame ---- */
    if (!ctx->initialized) {
        init_joint_rest_pose(ctx, globals);
        ctx->initialized = 1;
        free(globals);
        return;
    }

    /* ---- Process each chain ---- */
    for (int c = 0; c < ctx->chain_count; c++) {
        spring_chain_t *chain = &ctx->chains[c];

        for (int j = 0; j < chain->joint_count; j++) {
            spring_joint_state_t *js = &chain->joints[j];
            if (js->bone_index < 0) continue;

            const float *bone_global = &globals[js->bone_index * 16];
            float head[3];
            mat4_get_position(head, bone_global);

            /* ---- 1. Compute baseWorldRotation = parentRotation * initialLocalRotation ---- */
            float parent_rot[4];
            get_parent_world_rotation(ctx->model, globals, js->bone_index, parent_rot);

            float base_world_rot[4];
            q_mul(base_world_rot, parent_rot, js->initial_local_rot);

            /* ---- 2. Compute rest-pose tail direction in world space ---- */
            /* restDirection = baseWorldRot * boneAxis (unit vector) */
            float rotated_axis[3];
            q_rotate_vec3(rotated_axis, base_world_rot, js->bone_axis);

            /* ---- 3. Stiffness force: pull toward rest direction ---- */
            /* VRM standard: force = restDirection * stiffness * dt */
            float stiff_force[3];
            v3_scale(stiff_force, rotated_axis, js->stiffness * dt);

            /* ---- 4. Gravity force ---- */
            /* VRM standard: gravity = gravityDir * gravityPower * dt */
            float gdir[3];
            v3_copy(gdir, js->gravity_dir);
            float glen = v3_length(gdir);
            if (glen < 0.01f) {
                gdir[0] = 0; gdir[1] = -1; gdir[2] = 0;
            } else {
                v3_normalize(gdir);
            }
            float grav[3];
            v3_scale(grav, gdir, js->gravity_power * dt);

            /* ---- 5. Verlet integration (VRM standard) ---- */
            /* nextTail = currentTail
             *          + (currentTail - prevTail) * (1 - drag)   // velocity with damping
             *          + stiffnessForce                          // return to rest direction
             *          + gravityForce                             // external force
             */
            float velocity[3];
            v3_sub(velocity, js->current_tail, js->prev_tail);
            v3_scale(velocity, velocity, 1.0f - js->drag_force);

            /* Save prev before update */
            float temp[3];
            v3_copy(temp, js->current_tail);

            float new_tail[3];
            v3_add(new_tail, js->current_tail, velocity);
            v3_add(new_tail, new_tail, stiff_force);
            v3_add(new_tail, new_tail, grav);

            v3_copy(js->prev_tail, temp);

            /* ---- 6. Length constraint ---- */
            /* Project new_tail onto sphere of springLength around head */
            float head_to_tail[3];
            v3_sub(head_to_tail, new_tail, head);
            float magnitude = v3_length(head_to_tail);
            const float MAGNITUDE_THRESHOLD = 0.001f;
            if (magnitude <= MAGNITUDE_THRESHOLD) {
                /* Degenerate: use rotated boneAxis direction */
                v3_copy(head_to_tail, rotated_axis);
            } else {
                v3_scale(head_to_tail, head_to_tail, 1.0f / magnitude);
            }
            v3_scale(new_tail, head_to_tail, js->spring_length);
            v3_add(new_tail, head, new_tail);

            /* ---- 7. Collider resolution ---- */
            if (js->hit_radius > 0.0f && chain->collider_group_count > 0) {
                for (int cgi = 0; cgi < chain->collider_group_count; cgi++) {
                    int grp_idx = chain->collider_group_indices[cgi];
                    if (grp_idx < 0 || (uint32_t)grp_idx >= ctx->model->collider_group_count)
                        continue;

                    vrm_collider_group_t *cgrp = &ctx->model->collider_groups[grp_idx];
                    if (cgrp->bone_index < 0) continue;

                    const float *col_global = &globals[cgrp->bone_index * 16];

                    for (uint32_t ci = 0; ci < cgrp->collider_count; ci++) {
                        vrm_spring_collider_t *col = &cgrp->colliders[ci];

                        /* Collider world position */
                        float col_world[3];
                        mat4_transform_point(col_world, col_global, col->offset);

                        float total_radius = col->radius + js->hit_radius;

                        /* Push tail out if inside collider sphere */
                        float diff[3];
                        v3_sub(diff, new_tail, col_world);
                        float dist = v3_length(diff);
                        if (dist < total_radius && dist > 1e-8f) {
                            /* Push outward to surface */
                            v3_normalize(diff);
                            v3_scale(diff, diff, total_radius);
                            v3_add(new_tail, col_world, diff);

                            /* Re-constrain length */
                            v3_sub(head_to_tail, new_tail, head);
                            float len = v3_length(head_to_tail);
                            if (len > 1e-8f) {
                                v3_scale(head_to_tail, head_to_tail, js->spring_length / len);
                                v3_add(new_tail, head, head_to_tail);
                            }
                        }
                    }
                }
            }

            /* ---- 8. Store result ---- */
            v3_copy(js->current_tail, new_tail);

            /* ---- 9. NaN guard ---- */
            if (isnan(js->current_tail[0]) || isnan(js->current_tail[1]) || isnan(js->current_tail[2])) {
                /* Reset to rest pose tail */
                float rest_tail[3];
                v3_scale(rest_tail, rotated_axis, js->spring_length);
                v3_add(rest_tail, head, rest_tail);
                v3_copy(js->current_tail, rest_tail);
                v3_copy(js->prev_tail, rest_tail);
            }

            /* ---- 10. Compute rotation (UnityChan style) ---- */
            /* outputRotation = initialLocalRotation * FromToRotation(boneAxis, localBoneVector)
             * where localBoneVector = inverse(baseWorldRotation) * worldBoneVector */
            float world_bone_vec[3];
            v3_sub(world_bone_vec, js->current_tail, head);
            v3_normalize(world_bone_vec);

            /* Transform to local space relative to baseWorldRotation */
            float inv_base_rot[4];
            q_conjugate(inv_base_rot, base_world_rot);
            float local_bone_vec[3];
            q_rotate_vec3(local_bone_vec, inv_base_rot, world_bone_vec);
            v3_normalize(local_bone_vec);

            /* FromToRotation(boneAxis, localBoneVector) */
            float aim_rot[4];
            q_from_to_rotation(aim_rot, js->bone_axis, local_bone_vec);

            /* finalLocalRotation = initialLocalRotation * aimRotation */
            float final_local_rot[4];
            q_mul(final_local_rot, js->initial_local_rot, aim_rot);

            /* Build new global transform for this bone */
            float new_global[16];
            int parent = ctx->model->bones[js->bone_index].parent;
            if (parent >= 0) {
                /* local_transform = T(local_translation) * R(final_local_rot) */
                float local_t[3] = {
                    ctx->model->bones[js->bone_index].local_transform[12],
                    ctx->model->bones[js->bone_index].local_transform[13],
                    ctx->model->bones[js->bone_index].local_transform[14]
                };

                /* Build local matrix from final_local_rot + local_t */
                float local_mat[16];
                q_to_mat4(local_mat, final_local_rot, local_t);

                /* Preserve scale from rest local transform */
                float sc0[3] = {ctx->model->bones[js->bone_index].local_transform[0],
                               ctx->model->bones[js->bone_index].local_transform[1],
                               ctx->model->bones[js->bone_index].local_transform[2]};
                float sc1[3] = {ctx->model->bones[js->bone_index].local_transform[4],
                               ctx->model->bones[js->bone_index].local_transform[5],
                               ctx->model->bones[js->bone_index].local_transform[6]};
                float sc2[3] = {ctx->model->bones[js->bone_index].local_transform[8],
                               ctx->model->bones[js->bone_index].local_transform[9],
                               ctx->model->bones[js->bone_index].local_transform[10]};
                float sx = v3_length(sc0);
                float sy = v3_length(sc1);
                float sz = v3_length(sc2);

                local_mat[0] *= sx; local_mat[1] *= sx; local_mat[ 2] *= sx;
                local_mat[4] *= sy; local_mat[5] *= sy; local_mat[ 6] *= sy;
                local_mat[8] *= sz; local_mat[9] *= sz; local_mat[10] *= sz;

                mat4_mul(new_global, &globals[parent * 16], local_mat);
            } else {
                /* Root bone: just use final rotation + head position */
                q_to_mat4(new_global, final_local_rot, head);
            }

            /* Update globals for this bone */
            memcpy(&globals[js->bone_index * 16], new_global, 16 * sizeof(float));

            /* Update bone_matrices = new_global * offset_matrix */
            mat4_mul(&bone_matrices[js->bone_index * 16],
                     new_global,
                     ctx->model->bones[js->bone_index].offset_matrix);

            /* Propagate to non-spring-bone children:
             * Any child bone that isn't part of this chain needs its global updated
             * so that subsequent chain joints (which ARE children) use correct parent globals. */
        }
    }

    free(globals);
}

void spring_bone_shutdown(spring_bone_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->chains) {
        for (int i = 0; i < ctx->chain_count; i++) {
            free(ctx->chains[i].joints);
            free(ctx->chains[i].collider_group_indices);
        }
        free(ctx->chains);
    }

    printf("[spring_bone] shutdown\n");
    memset(ctx, 0, sizeof(*ctx));
}
