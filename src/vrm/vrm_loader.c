/**
 * @file vrm_loader.c
 * @brief Assimp-based VRM / glTF model loader with skeleton, skinning and
 *        VRMA animation support.
 */

#include "vrm_loader.h"
#include "mat4_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <float.h>
#include <libgen.h>
#include <math.h>

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <stb/stb_image.h>

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static void __normalize_path(char *p)
{
    for (char *c = p; *c; c++)
        if (*c == '\\') *c = '/';
}

static void __ai_mat4_to_float16(const struct aiMatrix4x4 *ai, float *out)
{
    /* Assimp is row-major, OpenGL/our convention is column-major → transpose */
    out[ 0] = ai->a1; out[ 1] = ai->b1; out[ 2] = ai->c1; out[ 3] = ai->d1;
    out[ 4] = ai->a2; out[ 5] = ai->b2; out[ 6] = ai->c2; out[ 7] = ai->d2;
    out[ 8] = ai->a3; out[ 9] = ai->b3; out[10] = ai->c3; out[11] = ai->d3;
    out[12] = ai->a4; out[13] = ai->b4; out[14] = ai->c4; out[15] = ai->d4;
}

/* ---- quaternion helpers ---- */

static void __quat_identity(float q[4])
{
    q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f;
}

static void __quat_normalize(float q[4])
{
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-8f) { q[0]/=len; q[1]/=len; q[2]/=len; q[3]/=len; }
}

static void __quat_slerp(float out[4], const float a[4], const float b[4], float t)
{
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float nb[4] = { b[0], b[1], b[2], b[3] };
    if (dot < 0.0f) {
        dot = -dot;
        nb[0] = -nb[0]; nb[1] = -nb[1]; nb[2] = -nb[2]; nb[3] = -nb[3];
    }
    if (dot > 0.9995f) {
        /* Linear interpolation for very close quaternions */
        for (int i = 0; i < 4; i++) out[i] = a[i] + t * (nb[i] - a[i]);
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / sin_theta;
        float wb = sinf(t * theta) / sin_theta;
        for (int i = 0; i < 4; i++) out[i] = wa * a[i] + wb * nb[i];
    }
    __quat_normalize(out);
}

static void __quat_to_mat4(const float q[4], float m[16])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float x2 = x+x, y2 = y+y, z2 = z+z;
    float xx = x*x2, xy = x*y2, xz = x*z2;
    float yy = y*y2, yz = y*z2, zz = z*z2;
    float wx = w*x2, wy = w*y2, wz = w*z2;

    memset(m, 0, 16 * sizeof(float));
    m[ 0] = 1.0f - (yy + zz);
    m[ 1] = xy + wz;
    m[ 2] = xz - wy;
    m[ 4] = xy - wz;
    m[ 5] = 1.0f - (xx + zz);
    m[ 6] = yz + wx;
    m[ 8] = xz + wy;
    m[ 9] = yz - wx;
    m[10] = 1.0f - (xx + yy);
    m[15] = 1.0f;
}

/** Quaternion conjugate (inverse for unit quaternions): q* = (-x,-y,-z,w) */
static void __quat_conjugate(float out[4], const float q[4])
{
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

/** Quaternion multiplication: out = a * b  (Hamilton product, x,y,z,w order) */
static void __quat_multiply(float out[4], const float a[4], const float b[4])
{
    float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw*bx + ax*bw + ay*bz - az*by;
    out[1] = aw*by - ax*bz + ay*bw + az*bx;
    out[2] = aw*bz + ax*by - ay*bx + az*bw;
    out[3] = aw*bw - ax*bx - ay*by - az*bz;
}

static void __mat4_compose(float out[16], const float t[3], const float q[4], const float s[3])
{
    __quat_to_mat4(q, out);
    /* Apply scale */
    out[0] *= s[0]; out[1] *= s[0]; out[2]  *= s[0];
    out[4] *= s[1]; out[5] *= s[1]; out[6]  *= s[1];
    out[8] *= s[2]; out[9] *= s[2]; out[10] *= s[2];
    /* Set translation */
    out[12] = t[0]; out[13] = t[1]; out[14] = t[2];
}

/* Decompose a 4x4 col-major matrix into T, R(quat), S */
static void __mat4_decompose(const float m[16], float t[3], float q[4], float s[3])
{
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];

    /* Extract scale from column lengths */
    s[0] = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    s[1] = sqrtf(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    s[2] = sqrtf(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

    if (s[0] < 1e-6f) s[0] = 1.0f;
    if (s[1] < 1e-6f) s[1] = 1.0f;
    if (s[2] < 1e-6f) s[2] = 1.0f;

    /* Rotation matrix (remove scale) */
    float r[9];
    r[0] = m[0]/s[0]; r[1] = m[1]/s[0]; r[2] = m[2]/s[0];
    r[3] = m[4]/s[1]; r[4] = m[5]/s[1]; r[5] = m[6]/s[1];
    r[6] = m[8]/s[2]; r[7] = m[9]/s[2]; r[8] = m[10]/s[2];

    /* Rotation matrix to quaternion */
    float trace = r[0] + r[4] + r[8];
    if (trace > 0.0f) {
        float ss = sqrtf(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * ss;
        q[0] = (r[5] - r[7]) / ss;
        q[1] = (r[6] - r[2]) / ss;
        q[2] = (r[1] - r[3]) / ss;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float ss = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
        q[3] = (r[5] - r[7]) / ss;
        q[0] = 0.25f * ss;
        q[1] = (r[1] + r[3]) / ss;
        q[2] = (r[6] + r[2]) / ss;
    } else if (r[4] > r[8]) {
        float ss = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
        q[3] = (r[6] - r[2]) / ss;
        q[0] = (r[1] + r[3]) / ss;
        q[1] = 0.25f * ss;
        q[2] = (r[5] + r[7]) / ss;
    } else {
        float ss = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
        q[3] = (r[1] - r[3]) / ss;
        q[0] = (r[6] + r[2]) / ss;
        q[1] = (r[5] + r[7]) / ss;
        q[2] = 0.25f * ss;
    }
    __quat_normalize(q);
}

/* ================================================================== */
/*  Texture loading helpers                                            */
/* ================================================================== */

static int __decode_embedded_texture(const struct aiTexture *ai_tex, vrm_texture_t *out)
{
    if (!ai_tex || !out) return -1;

    if (ai_tex->mHeight == 0) {
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        uint8_t *pixels = stbi_load_from_memory(
            (const unsigned char *)ai_tex->pcData,
            (int)ai_tex->mWidth, &w, &h, &ch, 4);
        if (!pixels) return -1;
        out->pixels = pixels; out->width = w; out->height = h; out->channels = 4;
        return 0;
    } else {
        int w = (int)ai_tex->mWidth, h = (int)ai_tex->mHeight;
        uint8_t *pixels = (uint8_t *)malloc((size_t)(w * h * 4));
        if (!pixels) return -1;
        const struct aiTexel *src = ai_tex->pcData;
        for (int i = 0; i < w * h; i++) {
            pixels[i*4+0] = src[i].r; pixels[i*4+1] = src[i].g;
            pixels[i*4+2] = src[i].b; pixels[i*4+3] = src[i].a;
        }
        out->pixels = pixels; out->width = w; out->height = h; out->channels = 4;
        return 0;
    }
}

static int __resolve_texture_index(const struct aiMaterial *mat,
                                   const struct aiScene *scene,
                                   char *out_path, int path_size)
{
    struct aiString tex_path;
    out_path[0] = '\0';
    enum aiTextureType types[] = { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE };
    for (int t = 0; t < 2; t++) {
        if (aiGetMaterialTexture(mat, types[t], 0, &tex_path,
                                 NULL, NULL, NULL, NULL, NULL, NULL) == aiReturn_SUCCESS) {
            if (tex_path.length > 0) {
                if (tex_path.data[0] == '*') {
                    int idx = atoi(tex_path.data + 1);
                    if (idx >= 0 && (unsigned)idx < scene->mNumTextures) return idx;
                }
                for (unsigned i = 0; i < scene->mNumTextures; i++) {
                    if (scene->mTextures[i]->mFilename.length > 0 &&
                        strcmp(scene->mTextures[i]->mFilename.data, tex_path.data) == 0)
                        return (int)i;
                }
                snprintf(out_path, path_size, "%s", tex_path.data);
                return -2;
            }
        }
    }
    return -1;
}

#define MAX_CACHED_TEXTURES 256
static struct { char path[1024]; int index; } s_tex_cache[MAX_CACHED_TEXTURES];
static int s_tex_cache_count = 0;

static int __load_external_texture(vrm_model_t *model, const char *model_dir,
                                   const char *rel_path)
{
    char full_path[1024], norm_rel[512];
    snprintf(norm_rel, sizeof(norm_rel), "%s", rel_path);
    __normalize_path(norm_rel);
    if (norm_rel[0] == '/')
        snprintf(full_path, sizeof(full_path), "%s", norm_rel);
    else
        snprintf(full_path, sizeof(full_path), "%s/%s", model_dir, norm_rel);

    for (int i = 0; i < s_tex_cache_count; i++)
        if (strcmp(s_tex_cache[i].path, full_path) == 0)
            return s_tex_cache[i].index;

    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    uint8_t *pixels = stbi_load(full_path, &w, &h, &ch, 4);
    if (!pixels) return -1;

    uint32_t new_idx = model->texture_count;
    vrm_texture_t *new_arr = (vrm_texture_t *)realloc(
        model->textures, (size_t)(new_idx + 1) * sizeof(vrm_texture_t));
    if (!new_arr) { stbi_image_free(pixels); return -1; }
    model->textures = new_arr;
    model->texture_count = new_idx + 1;
    model->textures[new_idx] = (vrm_texture_t){ pixels, w, h, 4 };

    if (s_tex_cache_count < MAX_CACHED_TEXTURES) {
        snprintf(s_tex_cache[s_tex_cache_count].path, 1024, "%s", full_path);
        s_tex_cache[s_tex_cache_count].index = (int)new_idx;
        s_tex_cache_count++;
    }
    printf("[vrm_loader] external texture[%u]: %dx%d  %s\n", new_idx, w, h, full_path);
    return (int)new_idx;
}

/* ================================================================== */
/*  Skeleton building from Assimp scene                                */
/* ================================================================== */

/** Map from Assimp node name → bone index. Built during skeleton extraction. */
typedef struct {
    char name[128];
    int  bone_index;
} bone_name_map_t;

static bone_name_map_t s_bone_map[VRM_MAX_BONES];
static int s_bone_map_count = 0;

static int __find_bone_by_name(const char *name)
{
    for (int i = 0; i < s_bone_map_count; i++)
        if (strcmp(s_bone_map[i].name, name) == 0)
            return s_bone_map[i].bone_index;
    return -1;
}

/** Recursively register all nodes in the scene as bones.
 *  We need the full node hierarchy for correct animation. */
static void __register_node_bones(const struct aiNode *node, int parent_idx,
                                  vrm_model_t *model)
{
    if (model->bone_count >= VRM_MAX_BONES) return;

    int my_idx = (int)model->bone_count;
    vrm_bone_t *bone = &model->bones[my_idx];

    snprintf(bone->name, sizeof(bone->name), "%s", node->mName.data);
    bone->parent = parent_idx;

    /* Store node's local transform */
    __ai_mat4_to_float16(&node->mTransformation, bone->local_transform);

    /* Inverse bind matrix will be filled from aiBone data later; default = identity */
    mat4_identity(bone->offset_matrix);

    /* Register in name map */
    if (s_bone_map_count < VRM_MAX_BONES) {
        snprintf(s_bone_map[s_bone_map_count].name, 128, "%s", node->mName.data);
        s_bone_map[s_bone_map_count].bone_index = my_idx;
        s_bone_map_count++;
    }

    model->bone_count++;

    for (unsigned i = 0; i < node->mNumChildren; i++)
        __register_node_bones(node->mChildren[i], my_idx, model);
}

/** After registering all nodes, fill in offset matrices from aiBone data. */
static void __fill_offset_matrices(const struct aiScene *scene, vrm_model_t *model)
{
    for (unsigned mi = 0; mi < scene->mNumMeshes; mi++) {
        const struct aiMesh *mesh = scene->mMeshes[mi];
        for (unsigned bi = 0; bi < mesh->mNumBones; bi++) {
            const struct aiBone *ab = mesh->mBones[bi];
            int idx = __find_bone_by_name(ab->mName.data);
            if (idx >= 0) {
                __ai_mat4_to_float16(&ab->mOffsetMatrix, model->bones[idx].offset_matrix);
            } else {
                printf("[vrm_loader] __fill_offset_matrices: aiBone '%s' in mesh '%s' NOT FOUND in bone map!\n",
                       ab->mName.data, mesh->mName.data);
            }
        }
    }
}

/* ================================================================== */
/*  Mesh extraction with bone weights                                  */
/* ================================================================== */

static uint32_t __count_meshes(const struct aiNode *node)
{
    uint32_t count = node->mNumMeshes;
    for (unsigned i = 0; i < node->mNumChildren; i++)
        count += __count_meshes(node->mChildren[i]);
    return count;
}

static void __extract_meshes(const struct aiNode *node,
                             const struct aiScene *scene,
                             vrm_model_t *model,
                             uint32_t *out_idx,
                             const char *model_dir,
                             const float *parent_world)
{
    /* Compute this node's accumulated world transform */
    float node_local[16], node_world[16];
    __ai_mat4_to_float16(&node->mTransformation, node_local);
    mat4_multiply(node_world, parent_world, node_local);

    /* Check if world transform is non-identity (needs vertex pre-transform) */
    int world_is_identity = 1;
    for (int i = 0; i < 16; i++) {
        float expected = (i % 5 == 0) ? 1.0f : 0.0f;
        if (fabsf(node_world[i] - expected) > 1e-5f) {
            world_is_identity = 0;
            break;
        }
    }

    for (unsigned m = 0; m < node->mNumMeshes; m++) {
        const struct aiMesh *ai_mesh = scene->mMeshes[node->mMeshes[m]];
        vrm_mesh_t *mesh = &model->meshes[*out_idx];

        /* ---- Vertex layout: pos(3)+normal(3)+uv(2)+boneId(4)+boneWt(4) = 16 floats ---- */
        uint32_t nv = ai_mesh->mNumVertices;
        mesh->vertex_count = nv;
        mesh->vertices = (float *)calloc((size_t)nv * 16, sizeof(float));
        if (!mesh->vertices) return;

        for (unsigned v = 0; v < nv; v++) {
            float *dst = &mesh->vertices[v * 16];
            dst[0] = ai_mesh->mVertices[v].x;
            dst[1] = ai_mesh->mVertices[v].y;
            dst[2] = ai_mesh->mVertices[v].z;
            if (ai_mesh->mNormals) {
                dst[3] = ai_mesh->mNormals[v].x;
                dst[4] = ai_mesh->mNormals[v].y;
                dst[5] = ai_mesh->mNormals[v].z;
            } else {
                dst[4] = 1.0f;
            }
            if (ai_mesh->mTextureCoords[0]) {
                dst[6] = ai_mesh->mTextureCoords[0][v].x;
                dst[7] = ai_mesh->mTextureCoords[0][v].y;
            }
            /* bone IDs [8..11] and weights [12..15] initialized to 0 */
        }

        /* ---- Indices ---- */
        mesh->index_count = 0;
        for (unsigned f = 0; f < ai_mesh->mNumFaces; f++)
            mesh->index_count += ai_mesh->mFaces[f].mNumIndices;

        mesh->indices = (uint32_t *)calloc(mesh->index_count, sizeof(uint32_t));
        if (!mesh->indices) return;

        uint32_t ii = 0;
        for (unsigned f = 0; f < ai_mesh->mNumFaces; f++) {
            const struct aiFace *face = &ai_mesh->mFaces[f];
            for (unsigned j = 0; j < face->mNumIndices; j++)
                mesh->indices[ii++] = face->mIndices[j];
        }

        /* ---- Bone weights ---- */
        mesh->has_bones = (ai_mesh->mNumBones > 0) ? 1 : 0;
        {
            int resolved = 0, unresolved = 0;
            for (unsigned bi = 0; bi < ai_mesh->mNumBones; bi++) {
                const struct aiBone *ab = ai_mesh->mBones[bi];
                int tmp = __find_bone_by_name(ab->mName.data);
                if (tmp >= 0) resolved++; else { unresolved++; printf("[vrm_loader] mesh[%u] '%s': UNRESOLVED bone '%s'\n", *out_idx, ai_mesh->mName.data, ab->mName.data); }
            }
            if (unresolved > 0)
                printf("[vrm_loader] mesh[%u] '%s': %d/%u bones resolved, %d UNRESOLVED\n",
                       *out_idx, ai_mesh->mName.data, resolved, ai_mesh->mNumBones, unresolved);
        }
        for (unsigned bi = 0; bi < ai_mesh->mNumBones; bi++) {
            const struct aiBone *ab = ai_mesh->mBones[bi];
            int bone_idx = __find_bone_by_name(ab->mName.data);
            if (bone_idx < 0) continue;

            for (unsigned wi = 0; wi < ab->mNumWeights; wi++) {
                unsigned vert_id = ab->mWeights[wi].mVertexId;
                float weight = ab->mWeights[wi].mWeight;
                if (vert_id >= nv) continue;

                float *dst = &mesh->vertices[vert_id * 16];
                /* Find an empty slot in the 4 bone slots */
                for (int s = 0; s < 4; s++) {
                    if (dst[12 + s] == 0.0f) {
                        dst[8 + s] = (float)bone_idx;
                        dst[12 + s] = weight;
                        break;
                    }
                }
            }
        }

        /* Normalize bone weights */
        if (mesh->has_bones) {
            for (unsigned v = 0; v < nv; v++) {
                float *dst = &mesh->vertices[v * 16];
                float sum = dst[12] + dst[13] + dst[14] + dst[15];
                if (sum > 1e-6f) {
                    dst[12] /= sum; dst[13] /= sum;
                    dst[14] /= sum; dst[15] /= sum;
                }
            }
        }

        /* ---- Store Assimp mesh index for morph target / expression binding ---- */
        mesh->assimp_mesh_index = (int)node->mMeshes[m];

        /* ---- Morph targets (blend shapes) ---- */
        mesh->morph_targets = NULL;
        mesh->morph_target_count = 0;
        if (ai_mesh->mNumAnimMeshes > 0) {
            uint32_t mt_count = ai_mesh->mNumAnimMeshes;
            if (mt_count > VRM_MAX_MORPH_TARGETS) mt_count = VRM_MAX_MORPH_TARGETS;
            mesh->morph_targets = (vrm_morph_target_t *)calloc(mt_count, sizeof(vrm_morph_target_t));
            mesh->morph_target_count = mt_count;

            for (unsigned mi = 0; mi < mt_count; mi++) {
                const struct aiAnimMesh *am = ai_mesh->mAnimMeshes[mi];
                vrm_morph_target_t *mt = &mesh->morph_targets[mi];

                /* Name from Assimp */
                if (am->mName.length > 0)
                    snprintf(mt->name, sizeof(mt->name), "%s", am->mName.data);
                else
                    snprintf(mt->name, sizeof(mt->name), "morph_%u", mi);

                mt->vertex_count = nv;

                /* Compute position deltas */
                if (am->mVertices) {
                    mt->delta_positions = (float *)calloc((size_t)nv * 3, sizeof(float));
                    for (unsigned v = 0; v < nv; v++) {
                        mt->delta_positions[v*3+0] = am->mVertices[v].x - ai_mesh->mVertices[v].x;
                        mt->delta_positions[v*3+1] = am->mVertices[v].y - ai_mesh->mVertices[v].y;
                        mt->delta_positions[v*3+2] = am->mVertices[v].z - ai_mesh->mVertices[v].z;
                    }
                }

                /* Compute normal deltas */
                if (am->mNormals && ai_mesh->mNormals) {
                    mt->delta_normals = (float *)calloc((size_t)nv * 3, sizeof(float));
                    for (unsigned v = 0; v < nv; v++) {
                        mt->delta_normals[v*3+0] = am->mNormals[v].x - ai_mesh->mNormals[v].x;
                        mt->delta_normals[v*3+1] = am->mNormals[v].y - ai_mesh->mNormals[v].y;
                        mt->delta_normals[v*3+2] = am->mNormals[v].z - ai_mesh->mNormals[v].z;
                    }
                } else {
                    mt->delta_normals = NULL;
                }
            }
            printf("[vrm_loader] mesh[%u]: %u morph targets\n", *out_idx, mt_count);
        }

        /* ---- Pre-transform vertices to world space ---- */
        /* glTF skinning formula: jointMatrix = inv(meshNodeGlobal) * J * IBM
         * By pre-transforming vertices by meshNodeGlobal, our bone_matrix = J * IBM
         * works correctly for all meshes regardless of mesh node position. */
        if (!world_is_identity) {
            for (unsigned v = 0; v < nv; v++) {
                float *dst = &mesh->vertices[v * 16];
                /* Transform position (full 4x4 with translation) */
                float x = dst[0], y = dst[1], z = dst[2];
                dst[0] = node_world[0]*x + node_world[4]*y + node_world[8]*z  + node_world[12];
                dst[1] = node_world[1]*x + node_world[5]*y + node_world[9]*z  + node_world[13];
                dst[2] = node_world[2]*x + node_world[6]*y + node_world[10]*z + node_world[14];
                /* Transform normal (rotation/scale only, no translation) */
                float nx = dst[3], ny = dst[4], nz = dst[5];
                dst[3] = node_world[0]*nx + node_world[4]*ny + node_world[8]*nz;
                dst[4] = node_world[1]*nx + node_world[5]*ny + node_world[9]*nz;
                dst[5] = node_world[2]*nx + node_world[6]*ny + node_world[10]*nz;
            }
            /* Transform morph target deltas (rotation/scale only) */
            for (uint32_t mi2 = 0; mi2 < mesh->morph_target_count; mi2++) {
                vrm_morph_target_t *mt = &mesh->morph_targets[mi2];
                if (mt->delta_positions) {
                    for (unsigned v = 0; v < nv; v++) {
                        float dx = mt->delta_positions[v*3+0];
                        float dy = mt->delta_positions[v*3+1];
                        float dz = mt->delta_positions[v*3+2];
                        mt->delta_positions[v*3+0] = node_world[0]*dx + node_world[4]*dy + node_world[8]*dz;
                        mt->delta_positions[v*3+1] = node_world[1]*dx + node_world[5]*dy + node_world[9]*dz;
                        mt->delta_positions[v*3+2] = node_world[2]*dx + node_world[6]*dy + node_world[10]*dz;
                    }
                }
                if (mt->delta_normals) {
                    for (unsigned v = 0; v < nv; v++) {
                        float dx = mt->delta_normals[v*3+0];
                        float dy = mt->delta_normals[v*3+1];
                        float dz = mt->delta_normals[v*3+2];
                        mt->delta_normals[v*3+0] = node_world[0]*dx + node_world[4]*dy + node_world[8]*dz;
                        mt->delta_normals[v*3+1] = node_world[1]*dx + node_world[5]*dy + node_world[9]*dz;
                        mt->delta_normals[v*3+2] = node_world[2]*dx + node_world[6]*dy + node_world[10]*dz;
                    }
                }
            }
            printf("[vrm_loader] mesh[%u] '%s': pre-transformed to world space\n",
                   *out_idx, ai_mesh->mName.data);
        }

        /* ---- Create base vertices copy for morph blending ---- */
        mesh->base_vertices = (float *)malloc((size_t)nv * 16 * sizeof(float));
        if (mesh->base_vertices)
            memcpy(mesh->base_vertices, mesh->vertices, (size_t)nv * 16 * sizeof(float));

        /* ---- Material ---- */
        mesh->color[0] = mesh->color[1] = mesh->color[2] = mesh->color[3] = 1.0f;
        mesh->texture_index = -1;

        if (ai_mesh->mMaterialIndex < scene->mNumMaterials) {
            const struct aiMaterial *mat = scene->mMaterials[ai_mesh->mMaterialIndex];
            struct aiColor4D base_color = {1,1,1,1};
            if (aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &base_color) != aiReturn_SUCCESS)
                if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &base_color) != aiReturn_SUCCESS)
                    base_color = (struct aiColor4D){1,1,1,1};
            mesh->color[0] = base_color.r; mesh->color[1] = base_color.g;
            mesh->color[2] = base_color.b; mesh->color[3] = base_color.a;

            char ext_path[512];
            int tex_idx = __resolve_texture_index(mat, scene, ext_path, sizeof(ext_path));
            if (tex_idx >= 0)
                mesh->texture_index = tex_idx;
            else if (tex_idx == -2 && ext_path[0] != '\0' && model_dir)
                mesh->texture_index = __load_external_texture(model, model_dir, ext_path);
        }

        (*out_idx)++;
    }

    for (unsigned i = 0; i < node->mNumChildren; i++)
        __extract_meshes(node->mChildren[i], scene, model, out_idx, model_dir, node_world);
}

/* ================================================================== */
/*  Bounding box                                                       */
/* ================================================================== */

static void __compute_bbox(vrm_model_t *model)
{
    model->bbox_min[0] = model->bbox_min[1] = model->bbox_min[2] =  FLT_MAX;
    model->bbox_max[0] = model->bbox_max[1] = model->bbox_max[2] = -FLT_MAX;

    for (uint32_t m = 0; m < model->mesh_count; m++) {
        const vrm_mesh_t *mesh = &model->meshes[m];
        for (uint32_t i = 0; i < mesh->vertex_count; i++) {
            const float *v = &mesh->vertices[i * 16]; /* pos at offset 0 */
            for (int j = 0; j < 3; j++) {
                if (v[j] < model->bbox_min[j]) model->bbox_min[j] = v[j];
                if (v[j] > model->bbox_max[j]) model->bbox_max[j] = v[j];
            }
        }
    }
    for (int j = 0; j < 3; j++)
        model->center[j] = (model->bbox_min[j] + model->bbox_max[j]) * 0.5f;

    float dx = model->bbox_max[0] - model->bbox_min[0];
    float dy = model->bbox_max[1] - model->bbox_min[1];
    float dz = model->bbox_max[2] - model->bbox_min[2];
    model->extent = dx;
    if (dy > model->extent) model->extent = dy;
    if (dz > model->extent) model->extent = dz;
    if (model->extent < 1e-6f) model->extent = 1.0f;
}

/* ================================================================== */
/*  VRM humanoid bone mapping extraction (from glTF JSON via Assimp)   */
/* ================================================================== */

/**
 * Parse VRM humanoid bone mapping from the model's metadata.
 * Assimp doesn't expose VRM extensions directly, so we parse the raw
 * glTF JSON from the file ourselves.
 */
static void __extract_vrm_humanoid(vrm_model_t *model, const char *path)
{
    /* We need to parse the glTF JSON chunk ourselves since Assimp doesn't
       expose VRM extension data. Read the file and extract the JSON. */
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return; }

    /* Validate glTF binary header */
    if (hdr[0] != 'g' || hdr[1] != 'l' || hdr[2] != 'T' || hdr[3] != 'F') {
        fclose(fp); return;
    }

    uint32_t chunk_len, chunk_type;
    if (fread(&chunk_len, 4, 1, fp) != 1) { fclose(fp); return; }
    if (fread(&chunk_type, 4, 1, fp) != 1) { fclose(fp); return; }
    if (chunk_type != 0x4E4F534A) { fclose(fp); return; } /* 'JSON' */

    char *json = (char *)malloc(chunk_len + 1);
    if (!json) { fclose(fp); return; }
    if (fread(json, 1, chunk_len, fp) != chunk_len) { free(json); fclose(fp); return; }
    json[chunk_len] = '\0';
    fclose(fp);

    /* Simple JSON parsing for VRM humanoid bones.
       We look for both VRM 0.x ("VRM"."humanoid"."humanBones":[...])
       and VRM 1.0 ("VRMC_vrm"."humanoid"."humanBones":{...}) */

    /* Helper: find a key in JSON string. Very basic — works for our use case. */
    model->humanoid_map = NULL;
    model->humanoid_map_count = 0;

    /* Allocate generously */
    vrm_humanoid_map_t *map = (vrm_humanoid_map_t *)calloc(128, sizeof(vrm_humanoid_map_t));
    if (!map) { free(json); return; }
    int map_count = 0;

    /* Try VRM 0.x format: "humanBones" is an array of {"bone":"xxx","node":NNN} */
    const char *hb_ptr = strstr(json, "\"humanBones\"");
    while (hb_ptr) {
        /* Find the opening '[' or '{' after "humanBones" */
        const char *p = hb_ptr + 12;
        while (*p && *p != '[' && *p != '{') p++;

        if (*p == '[') {
            /* VRM 0.x array format */
            p++;
            while (*p && *p != ']' && map_count < 128) {
                /* Find "bone" key */
                const char *bone_key = strstr(p, "\"bone\"");
                if (!bone_key || bone_key > strchr(p, ']')) break;
                /* Extract bone name value */
                const char *bv = strchr(bone_key + 6, '"');
                if (!bv) break;
                bv++;
                const char *bv_end = strchr(bv, '"');
                if (!bv_end) break;
                int blen = (int)(bv_end - bv);
                if (blen >= 64) blen = 63;
                memcpy(map[map_count].humanoid_name, bv, blen);
                map[map_count].humanoid_name[blen] = '\0';

                /* Find "node" key and get node index */
                const char *node_key = strstr(bone_key, "\"node\"");
                if (!node_key) break;
                const char *nv = node_key + 6;
                while (*nv && (*nv == ' ' || *nv == ':' || *nv == '\t')) nv++;
                int node_idx = atoi(nv);

                /* Look up node name from Assimp scene — we stored it in our bone map */
                /* We need to match node index to bone name. Since we registered all nodes
                   in order during __register_node_bones, the assimp node index won't match
                   our bone index directly. Instead, we need to look up by the original
                   scene node. We'll store the node index and resolve after. */
                /* Actually, let's just store the node_idx and resolve later from the scene */
                snprintf(map[map_count].node_name, 128, "__node_idx_%d", node_idx);

                map_count++;
                p = bv_end + 1;
                /* Skip to next object or end */
                const char *next_obj = strchr(p, '{');
                const char *arr_end = strchr(p, ']');
                if (!next_obj || (arr_end && next_obj > arr_end)) break;
                p = next_obj;
            }
            break;
        } else if (*p == '{') {
            /* VRM 1.0 / VRMC_vrm dict format: {"hips":{"node":0}, ...} */
            p++;
            int brace_depth = 1;
            while (*p && brace_depth > 0 && map_count < 128) {
                /* Find next key */
                const char *key_start = strchr(p, '"');
                if (!key_start) break;
                key_start++;
                const char *key_end = strchr(key_start, '"');
                if (!key_end) break;

                int klen = (int)(key_end - key_start);
                if (klen >= 64) klen = 63;

                /* Check if this looks like a bone name (not "node") */
                if (strncmp(key_start, "node", 4) == 0 && klen == 4) {
                    p = key_end + 1;
                    continue;
                }

                memcpy(map[map_count].humanoid_name, key_start, klen);
                map[map_count].humanoid_name[klen] = '\0';

                /* Find "node" value */
                const char *node_key = strstr(key_end, "\"node\"");
                if (!node_key) { p = key_end + 1; continue; }
                const char *nv = node_key + 6;
                while (*nv && (*nv == ' ' || *nv == ':' || *nv == '\t')) nv++;
                int node_idx = atoi(nv);
                snprintf(map[map_count].node_name, 128, "__node_idx_%d", node_idx);

                map_count++;

                /* Move past this entry's closing brace */
                const char *cb = strchr(nv, '}');
                if (!cb) break;
                p = cb + 1;

                /* Check for end of outer brace */
                while (*p && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (*p == '}') break;
            }
            break;
        }

        /* Try next occurrence */
        hb_ptr = strstr(hb_ptr + 12, "\"humanBones\"");
    }

    free(json);

    if (map_count > 0) {
        model->humanoid_map = (vrm_humanoid_map_t *)realloc(map, map_count * sizeof(vrm_humanoid_map_t));
        model->humanoid_map_count = map_count;
    } else {
        free(map);
    }
}

/** After scene is loaded and bone map built, resolve "__node_idx_N" entries
 *  to actual node names. */
static void __resolve_humanoid_node_names(vrm_model_t *model,
                                          const struct aiScene *scene)
{
    if (!model->humanoid_map) return;

    /* Build a flat list of all nodes by DFS order — matching glTF node indices */
    /* glTF node indices correspond to the order nodes appear in the JSON nodes array.
       Assimp preserves node names but flattens the hierarchy. We need to match
       by traversing the scene in the correct order. */

    /* Approach: glTF nodes array order = DFS pre-order of the Assimp scene.
       But actually Assimp doesn't guarantee this. Instead, let's just collect
       all nodes and try to match by index via a BFS/DFS that matches glTF order. */

    /* Simpler approach: we already parsed the node index from JSON. The glTF
       nodes array is ordered, and Assimp names nodes by their glTF names.
       So let's just re-read the JSON to get node name by index. */

    /* Actually the simplest: re-open the file and get node names from JSON. */
    /* But we don't have the path here... let's use a different approach:
       iterate Assimp scene nodes and build index. */

    /* Best approach: the node names are already in our bone_map, keyed by name.
       The "__node_idx_N" approach is fragile. Let's parse the JSON node names
       directly when we have the file path. We'll do this in __extract_vrm_humanoid. */

    /* For now, we'll use a brute-force DFS to assign indices. */
    /* Actually, let's just use a static array filled during scene traversal. */
}

/* Collect node names in DFS pre-order (matching glTF node array order). */
static const char **s_node_names_by_gltf_idx = NULL;
static int s_node_names_count = 0;

static void __collect_node_names_dfs(const struct aiNode *node)
{
    /* This doesn't match glTF node order because Assimp reorders nodes.
       Instead we'll re-parse from the JSON in __extract_vrm_humanoid. */
    (void)node;
}

/** Extract VRM humanoid mapping, resolving node indices to names from raw JSON. */
static void __extract_vrm_humanoid_full(vrm_model_t *model, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return; }
    if (hdr[0] != 'g' || hdr[1] != 'l' || hdr[2] != 'T' || hdr[3] != 'F') {
        fclose(fp); return;
    }

    uint32_t chunk_len, chunk_type;
    if (fread(&chunk_len, 4, 1, fp) != 1) { fclose(fp); return; }
    if (fread(&chunk_type, 4, 1, fp) != 1) { fclose(fp); return; }
    if (chunk_type != 0x4E4F534A) { fclose(fp); return; }

    char *json_str = (char *)malloc(chunk_len + 1);
    if (!json_str) { fclose(fp); return; }
    if (fread(json_str, 1, chunk_len, fp) != chunk_len) { free(json_str); fclose(fp); return; }
    json_str[chunk_len] = '\0';
    fclose(fp);

    /* ---- Parse all node names from "nodes" array ---- */
    char node_names[1024][128];
    int total_nodes = 0;

    const char *nodes_key = strstr(json_str, "\"nodes\"");
    if (nodes_key) {
        const char *arr = strchr(nodes_key + 7, '[');
        if (arr) {
            const char *p = arr + 1;
            while (*p && *p != ']' && total_nodes < 1024) {
                /* Find the next { */
                const char *obj = strchr(p, '{');
                if (!obj) break;
                /* Find "name" within this object */
                const char *obj_end = NULL;
                int depth = 1;
                const char *q = obj + 1;
                while (*q && depth > 0) {
                    if (*q == '{') depth++;
                    else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                    q++;
                }
                if (!obj_end) break;

                /* Find "name" key between obj and obj_end */
                const char *name_key = obj;
                char found_name[128] = "";
                while ((name_key = strstr(name_key, "\"name\"")) != NULL && name_key < obj_end) {
                    const char *colon = name_key + 6;
                    while (*colon && (*colon == ' ' || *colon == ':' || *colon == '\t')) colon++;
                    if (*colon == '"') {
                        colon++;
                        const char *ne = strchr(colon, '"');
                        if (ne) {
                            int nlen = (int)(ne - colon);
                            if (nlen >= 128) nlen = 127;
                            memcpy(found_name, colon, nlen);
                            found_name[nlen] = '\0';
                        }
                    }
                    break;
                }
                snprintf(node_names[total_nodes], 128, "%s", found_name);
                total_nodes++;
                p = obj_end + 1;
            }
        }
    }

    printf("[vrm_loader] parsed %d node names from glTF JSON\n", total_nodes);

    /* ---- Parse humanoid bones ---- */
    model->humanoid_map = NULL;
    model->humanoid_map_count = 0;

    vrm_humanoid_map_t *map = (vrm_humanoid_map_t *)calloc(128, sizeof(vrm_humanoid_map_t));
    if (!map) { free(json_str); return; }
    int map_count = 0;

    /* Find humanBones — try all occurrences and pick the one inside VRM/VRMC_vrm */
    const char *hb_ptr = strstr(json_str, "\"humanBones\"");
    while (hb_ptr && map_count == 0) {
        const char *p = hb_ptr + 12;
        while (*p && *p != '[' && *p != '{') p++;

        if (*p == '[') {
            /* VRM 0.x array: [{"bone":"hips","node":27}, ...] */
            p++;
            while (*p && *p != ']' && map_count < 128) {
                const char *bone_key = strstr(p, "\"bone\"");
                const char *arr_end = strchr(p, ']');
                if (!bone_key || (arr_end && bone_key > arr_end)) break;

                const char *bv = strchr(bone_key + 6, '"');
                if (!bv) break;
                bv++;
                const char *bv_end = strchr(bv, '"');
                if (!bv_end) break;
                int blen = (int)(bv_end - bv);
                if (blen >= 64) blen = 63;
                memcpy(map[map_count].humanoid_name, bv, blen);
                map[map_count].humanoid_name[blen] = '\0';

                const char *node_key = strstr(bv_end, "\"node\"");
                if (!node_key) break;
                const char *nv = node_key + 6;
                while (*nv && (*nv == ' ' || *nv == ':' || *nv == '\t')) nv++;
                int node_idx = atoi(nv);

                if (node_idx >= 0 && node_idx < total_nodes)
                    snprintf(map[map_count].node_name, 128, "%s", node_names[node_idx]);
                else
                    snprintf(map[map_count].node_name, 128, "node_%d", node_idx);

                map_count++;

                const char *next_obj = strchr(bv_end, '{');
                arr_end = strchr(bv_end, ']');
                if (!next_obj || (arr_end && next_obj > arr_end)) break;
                p = next_obj;
            }
        } else if (*p == '{') {
            /* VRM 1.0 / VRMC_vrm dict: {"hips":{"node":0}, "spine":{"node":1}, ...} */
            p++;
            while (*p && map_count < 128) {
                while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
                if (*p == '}') break;
                if (*p != '"') { p++; continue; }

                /* Key */
                p++;
                const char *key_end = strchr(p, '"');
                if (!key_end) break;
                int klen = (int)(key_end - p);
                if (klen >= 64) klen = 63;

                char key_buf[64];
                memcpy(key_buf, p, klen);
                key_buf[klen] = '\0';
                p = key_end + 1;

                /* Find colon and opening brace */
                while (*p && *p != '{') p++;
                if (!*p) break;
                p++;

                /* Find "node" : N */
                const char *node_key = strstr(p, "\"node\"");
                if (!node_key) break;
                const char *nv = node_key + 6;
                while (*nv && (*nv == ' ' || *nv == ':' || *nv == '\t')) nv++;
                int node_idx = atoi(nv);

                /* Skip to closing brace */
                const char *cb = strchr(nv, '}');
                if (!cb) break;
                p = cb + 1;

                memcpy(map[map_count].humanoid_name, key_buf, klen + 1);
                if (node_idx >= 0 && node_idx < total_nodes)
                    snprintf(map[map_count].node_name, 128, "%s", node_names[node_idx]);
                else
                    snprintf(map[map_count].node_name, 128, "node_%d", node_idx);

                map_count++;
            }
        }

        if (map_count == 0)
            hb_ptr = strstr(hb_ptr + 12, "\"humanBones\"");
        else
            break;
    }

    free(json_str);

    if (map_count > 0) {
        model->humanoid_map = (vrm_humanoid_map_t *)realloc(map, map_count * sizeof(vrm_humanoid_map_t));
        model->humanoid_map_count = map_count;
        printf("[vrm_loader] humanoid bone map: %d entries\n", map_count);
        for (int i = 0; i < map_count && i < 5; i++)
            printf("[vrm_loader]   %s -> %s\n", model->humanoid_map[i].humanoid_name,
                   model->humanoid_map[i].node_name);
        if (map_count > 5) printf("[vrm_loader]   ... +%d more\n", map_count - 5);
    } else {
        free(map);
    }
}

/* ================================================================== */
/*  Embedded animation extraction from Assimp                         */
/* ================================================================== */

static void __extract_vrm_expressions(vrm_model_t *model, const char *path);
static void __extract_vrm_spring_bones(vrm_model_t *model, const char *path);
static void __extract_vrm_node_constraints(vrm_model_t *model, const char *path);

static void __extract_animations(const struct aiScene *scene, vrm_model_t *model)
{
    if (scene->mNumAnimations == 0) return;

    model->animations = (vrm_animation_t *)calloc(scene->mNumAnimations, sizeof(vrm_animation_t));
    model->animation_count = scene->mNumAnimations;

    for (unsigned ai = 0; ai < scene->mNumAnimations; ai++) {
        const struct aiAnimation *anim = scene->mAnimations[ai];
        vrm_animation_t *va = &model->animations[ai];

        snprintf(va->name, sizeof(va->name), "%s",
                 anim->mName.length > 0 ? anim->mName.data : "Animation");

        double tps = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 30.0;
        va->duration = (float)(anim->mDuration / tps);

        va->bone_anims = (vrm_bone_anim_t *)calloc(anim->mNumChannels, sizeof(vrm_bone_anim_t));
        va->bone_anim_count = 0;

        for (unsigned ci = 0; ci < anim->mNumChannels; ci++) {
            const struct aiNodeAnim *ch = anim->mChannels[ci];
            int bone_idx = __find_bone_by_name(ch->mNodeName.data);
            if (bone_idx < 0) continue;

            vrm_bone_anim_t *ba = &va->bone_anims[va->bone_anim_count];
            ba->bone_index = bone_idx;

            /* Count channels: up to 3 (T, R, S) */
            int nch = 0;
            if (ch->mNumPositionKeys > 0) nch++;
            if (ch->mNumRotationKeys > 0) nch++;
            if (ch->mNumScalingKeys > 0) nch++;

            ba->channels = (vrm_anim_channel_t *)calloc(nch, sizeof(vrm_anim_channel_t));
            ba->channel_count = 0;

            /* Translation */
            if (ch->mNumPositionKeys > 0) {
                vrm_anim_channel_t *ac = &ba->channels[ba->channel_count++];
                ac->path = 0;
                ac->count = ch->mNumPositionKeys;
                ac->times = (float *)malloc(ac->count * sizeof(float));
                ac->values = (float *)malloc(ac->count * 3 * sizeof(float));
                for (unsigned k = 0; k < ac->count; k++) {
                    ac->times[k] = (float)(ch->mPositionKeys[k].mTime / tps);
                    ac->values[k*3+0] = ch->mPositionKeys[k].mValue.x;
                    ac->values[k*3+1] = ch->mPositionKeys[k].mValue.y;
                    ac->values[k*3+2] = ch->mPositionKeys[k].mValue.z;
                }
            }

            /* Rotation (quaternion) */
            if (ch->mNumRotationKeys > 0) {
                vrm_anim_channel_t *ac = &ba->channels[ba->channel_count++];
                ac->path = 1;
                ac->count = ch->mNumRotationKeys;
                ac->times = (float *)malloc(ac->count * sizeof(float));
                ac->values = (float *)malloc(ac->count * 4 * sizeof(float));
                for (unsigned k = 0; k < ac->count; k++) {
                    ac->times[k] = (float)(ch->mRotationKeys[k].mTime / tps);
                    /* Assimp: w,x,y,z → our storage: x,y,z,w */
                    ac->values[k*4+0] = ch->mRotationKeys[k].mValue.x;
                    ac->values[k*4+1] = ch->mRotationKeys[k].mValue.y;
                    ac->values[k*4+2] = ch->mRotationKeys[k].mValue.z;
                    ac->values[k*4+3] = ch->mRotationKeys[k].mValue.w;
                }
            }

            /* Scale */
            if (ch->mNumScalingKeys > 0) {
                vrm_anim_channel_t *ac = &ba->channels[ba->channel_count++];
                ac->path = 2;
                ac->count = ch->mNumScalingKeys;
                ac->times = (float *)malloc(ac->count * sizeof(float));
                ac->values = (float *)malloc(ac->count * 3 * sizeof(float));
                for (unsigned k = 0; k < ac->count; k++) {
                    ac->times[k] = (float)(ch->mScalingKeys[k].mTime / tps);
                    ac->values[k*3+0] = ch->mScalingKeys[k].mValue.x;
                    ac->values[k*3+1] = ch->mScalingKeys[k].mValue.y;
                    ac->values[k*3+2] = ch->mScalingKeys[k].mValue.z;
                }
            }

            va->bone_anim_count++;
        }
    }

    printf("[vrm_loader] extracted %u animation(s)\n", model->animation_count);
}

/* ================================================================== */
/*  Public: vrm_model_load                                            */
/* ================================================================== */

int vrm_model_load(vrm_model_t *model, const char *path)
{
    memset(model, 0, sizeof(*model));
    s_bone_map_count = 0;

    char path_copy[1024];
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    char *model_dir = dirname(path_copy);
    printf("[vrm_loader] model dir: %s\n", model_dir);

    const char *ext = strrchr(path, '.');
    int use_memory_load = 0;
    int is_pmx = 0;
    if (ext && (strcasecmp(ext, ".vrm") == 0)) use_memory_load = 1;
    if (ext && (strcasecmp(ext, ".pmx") == 0 || strcasecmp(ext, ".pmd") == 0)) is_pmx = 1;

    s_tex_cache_count = 0;

    /* Do NOT use aiProcess_PreTransformVertices — we need the bone hierarchy! */
    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_GenSmoothNormals
                       | aiProcess_JoinIdenticalVertices
                       | aiProcess_SortByPType
                       | aiProcess_LimitBoneWeights;  /* limit to 4 weights per vertex */

    if (is_pmx || use_memory_load)
        flags |= aiProcess_FlipUVs;

    const struct aiScene *scene = NULL;

    if (use_memory_load) {
        FILE *fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "[vrm_loader] cannot open: %s\n", path); return -1; }
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (file_size <= 0) { fclose(fp); return -1; }

        unsigned char *buf = (unsigned char *)malloc((size_t)file_size);
        if (!buf) { fclose(fp); return -1; }
        if ((long)fread(buf, 1, (size_t)file_size, fp) != file_size) {
            free(buf); fclose(fp); return -1;
        }
        fclose(fp);

        const char *hint = "glb";
        if (file_size >= 4 && buf[0] == '{') hint = "gltf";

        scene = aiImportFileFromMemory(
            (const char *)buf, (unsigned int)file_size, flags, hint);
        free(buf);
    } else {
        scene = aiImportFile(path, flags);
    }

    if (!scene || !scene->mRootNode) {
        fprintf(stderr, "[vrm_loader] Assimp error: %s\n", aiGetErrorString());
        return -1;
    }

    printf("[vrm_loader] loaded: meshes=%u  materials=%u  textures=%u  animations=%u\n",
           scene->mNumMeshes, scene->mNumMaterials, scene->mNumTextures,
           scene->mNumAnimations);

    /* ---- Build skeleton from node hierarchy ---- */
    model->bones = (vrm_bone_t *)calloc(VRM_MAX_BONES, sizeof(vrm_bone_t));
    model->bone_count = 0;
    __register_node_bones(scene->mRootNode, -1, model);
    __fill_offset_matrices(scene, model);
    printf("[vrm_loader] skeleton: %u bones\n", model->bone_count);

    /* ---- Decode embedded textures ---- */
    if (scene->mNumTextures > 0) {
        model->texture_count = scene->mNumTextures;
        model->textures = (vrm_texture_t *)calloc(model->texture_count, sizeof(vrm_texture_t));
        for (unsigned i = 0; i < scene->mNumTextures; i++) {
            if (__decode_embedded_texture(scene->mTextures[i], &model->textures[i]) == 0)
                printf("[vrm_loader] texture[%u]: %dx%d OK\n", i, model->textures[i].width, model->textures[i].height);
        }
    }

    /* ---- Extract meshes ---- */
    model->mesh_count = __count_meshes(scene->mRootNode);
    if (model->mesh_count == 0) { aiReleaseImport(scene); return -1; }
    model->meshes = (vrm_mesh_t *)calloc(model->mesh_count, sizeof(vrm_mesh_t));

    uint32_t idx = 0;
    {
        float identity_mat[16];
        mat4_identity(identity_mat);
        __extract_meshes(scene->mRootNode, scene, model, &idx, model_dir, identity_mat);
    }
    model->mesh_count = idx;

    /* ---- Extract embedded animations ---- */
    __extract_animations(scene, model);

    /* ---- Bounding box ---- */
    __compute_bbox(model);
    printf("[vrm_loader] center=(%.2f, %.2f, %.2f)  extent=%.2f\n",
           model->center[0], model->center[1], model->center[2], model->extent);

    /* ---- VRM humanoid mapping ---- */
    __extract_vrm_humanoid_full(model, path);

    /* ---- VRM expressions (BlendShape) ---- */
    __extract_vrm_expressions(model, path);

    /* ---- VRM spring bones (secondary animation) ---- */
    __extract_vrm_spring_bones(model, path);

    /* ---- VRM 1.0 node constraints (Aim / Roll) ---- */
    __extract_vrm_node_constraints(model, path);

    aiReleaseImport(scene);
    return 0;
}

/* ================================================================== */
/*  Public: vrm_load_vrma                                              */
/* ================================================================== */

int vrm_load_vrma(vrm_model_t *model, const char *vrma_path)
{
    printf("[vrm_loader] loading VRMA: %s\n", vrma_path);

    /* ---- Read glTF JSON from VRMA file ---- */
    FILE *fp = fopen(vrma_path, "rb");
    if (!fp) { fprintf(stderr, "[vrm_loader] cannot open VRMA: %s\n", vrma_path); return -1; }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return -1; }
    if (hdr[0] != 'g' || hdr[1] != 'l' || hdr[2] != 'T' || hdr[3] != 'F') {
        fclose(fp); return -1;
    }

    /* Read total file */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char *file_buf = (unsigned char *)malloc(file_size);
    if (!file_buf) { fclose(fp); return -1; }
    if ((long)fread(file_buf, 1, file_size, fp) != file_size) {
        free(file_buf); fclose(fp); return -1;
    }
    fclose(fp);

    /* Parse glTF header */
    uint32_t json_chunk_len, json_chunk_type;
    memcpy(&json_chunk_len, file_buf + 12, 4);
    memcpy(&json_chunk_type, file_buf + 16, 4);
    if (json_chunk_type != 0x4E4F534A) { free(file_buf); return -1; }

    char *json_str = (char *)malloc(json_chunk_len + 1);
    memcpy(json_str, file_buf + 20, json_chunk_len);
    json_str[json_chunk_len] = '\0';

    /* Binary chunk */
    uint32_t bin_offset = 20 + json_chunk_len;
    /* Align to 4 bytes */
    while (bin_offset % 4 != 0) bin_offset++;

    unsigned char *bin_data = NULL;
    uint32_t bin_len = 0;
    if (bin_offset + 8 <= (uint32_t)file_size) {
        memcpy(&bin_len, file_buf + bin_offset, 4);
        uint32_t bin_type;
        memcpy(&bin_type, file_buf + bin_offset + 4, 4);
        if (bin_type == 0x004E4942) { /* 'BIN\0' */
            bin_data = file_buf + bin_offset + 8;
        }
    }

    if (!bin_data) {
        fprintf(stderr, "[vrm_loader] VRMA: no binary chunk found\n");
        free(json_str); free(file_buf);
        return -1;
    }

    /* ---- Parse VRMA node names and humanoid mapping ---- */
    /* Parse nodes array to get VRMA node names + rest rotation/translation + children */
    char vrma_node_names[512][128];
    float vrma_node_rot[512][4];   /* rest rotation (x,y,z,w) */
    float vrma_node_trans[512][3]; /* rest translation */
    int vrma_node_parent[512];     /* parent index (-1 if root) */
    float vrma_world_rot[512][4];  /* computed world rotation */
    int vrma_node_children[512][64]; /* children indices per node */
    int vrma_node_child_count[512];
    int vrma_node_count = 0;
    for (int _i = 0; _i < 512; _i++) {
        vrma_node_rot[_i][0] = vrma_node_rot[_i][1] = vrma_node_rot[_i][2] = 0.0f;
        vrma_node_rot[_i][3] = 1.0f;
        vrma_node_trans[_i][0] = vrma_node_trans[_i][1] = vrma_node_trans[_i][2] = 0.0f;
        vrma_node_parent[_i] = -1;
        vrma_world_rot[_i][0] = vrma_world_rot[_i][1] = vrma_world_rot[_i][2] = 0.0f;
        vrma_world_rot[_i][3] = 1.0f;
        vrma_node_child_count[_i] = 0;
    }

    const char *nodes_key = strstr(json_str, "\"nodes\"");
    if (nodes_key) {
        const char *arr = strchr(nodes_key + 7, '[');
        if (arr) {
            const char *p = arr + 1;
            while (*p && *p != ']' && vrma_node_count < 512) {
                const char *obj = strchr(p, '{');
                if (!obj) break;
                int depth = 1;
                const char *q = obj + 1;
                const char *obj_end = NULL;
                while (*q && depth > 0) {
                    if (*q == '{') depth++;
                    else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                    q++;
                }
                if (!obj_end) break;

                char found_name[128] = "";
                const char *name_key = strstr(obj, "\"name\"");
                if (name_key && name_key < obj_end) {
                    const char *colon = name_key + 6;
                    while (*colon && (*colon == ' ' || *colon == ':' || *colon == '\t')) colon++;
                    if (*colon == '"') {
                        colon++;
                        const char *ne = strchr(colon, '"');
                        if (ne) {
                            int nlen = (int)(ne - colon);
                            if (nlen >= 128) nlen = 127;
                            memcpy(found_name, colon, nlen);
                            found_name[nlen] = '\0';
                        }
                    }
                }
                snprintf(vrma_node_names[vrma_node_count], 128, "%s", found_name);

                /* Parse "rotation":[x,y,z,w] if present */
                const char *rot_k = strstr(obj, "\"rotation\"");
                if (rot_k && rot_k < obj_end) {
                    const char *ra = strchr(rot_k + 10, '[');
                    if (ra && ra < obj_end) {
                        ra++;
                        vrma_node_rot[vrma_node_count][0] = (float)atof(ra);
                        const char *c1 = strchr(ra, ',');
                        if (c1) { vrma_node_rot[vrma_node_count][1] = (float)atof(c1+1);
                        const char *c2 = strchr(c1+1, ',');
                        if (c2) { vrma_node_rot[vrma_node_count][2] = (float)atof(c2+1);
                        const char *c3 = strchr(c2+1, ',');
                        if (c3) { vrma_node_rot[vrma_node_count][3] = (float)atof(c3+1);
                        }}}
                    }
                }
                /* Parse "translation":[x,y,z] if present */
                const char *tr_k = strstr(obj, "\"translation\"");
                if (tr_k && tr_k < obj_end) {
                    const char *ta = strchr(tr_k + 13, '[');
                    if (ta && ta < obj_end) {
                        ta++;
                        vrma_node_trans[vrma_node_count][0] = (float)atof(ta);
                        const char *c1 = strchr(ta, ',');
                        if (c1) { vrma_node_trans[vrma_node_count][1] = (float)atof(c1+1);
                        const char *c2 = strchr(c1+1, ',');
                        if (c2) { vrma_node_trans[vrma_node_count][2] = (float)atof(c2+1);
                        }}
                    }
                }

                /* Parse "children":[i,j,...] if present */
                const char *ch_k = strstr(obj, "\"children\"");
                if (ch_k && ch_k < obj_end) {
                    const char *ca = strchr(ch_k + 10, '[');
                    if (ca && ca < obj_end) {
                        ca++;
                        while (*ca && *ca != ']' && vrma_node_child_count[vrma_node_count] < 64) {
                            while (*ca == ' ' || *ca == '\t' || *ca == '\n' || *ca == '\r' || *ca == ',') ca++;
                            if (*ca == ']') break;
                            int child_idx = atoi(ca);
                            vrma_node_children[vrma_node_count][vrma_node_child_count[vrma_node_count]++] = child_idx;
                            while (*ca && *ca != ',' && *ca != ']') ca++;
                        }
                    }
                }

                vrma_node_count++;
                p = obj_end + 1;
            }
        }
    }

    /* ---- Build parent map and compute world rotations ---- */
    /* From children lists, derive parent indices */
    for (int i = 0; i < vrma_node_count; i++) {
        for (int c = 0; c < vrma_node_child_count[i]; c++) {
            int ch = vrma_node_children[i][c];
            if (ch >= 0 && ch < vrma_node_count)
                vrma_node_parent[ch] = i;
        }
    }
    /* Compute world rotations via BFS (parent indices can be higher than children in glTF!) */
    {
        int queue[512];
        int q_head = 0, q_tail = 0;
        /* Enqueue root nodes */
        for (int i = 0; i < vrma_node_count; i++) {
            if (vrma_node_parent[i] < 0) {
                memcpy(vrma_world_rot[i], vrma_node_rot[i], 4 * sizeof(float));
                queue[q_tail++] = i;
            }
        }
        /* BFS: process children after parents */
        while (q_head < q_tail) {
            int node = queue[q_head++];
            for (int c = 0; c < vrma_node_child_count[node]; c++) {
                int ch = vrma_node_children[node][c];
                if (ch >= 0 && ch < vrma_node_count) {
                    __quat_multiply(vrma_world_rot[ch], vrma_world_rot[node], vrma_node_rot[ch]);
                    queue[q_tail++] = ch;
                }
            }
        }
    }

    /* ---- Parse VRMC_vrm_animation humanBones mapping ---- */
    /* Maps: humanoid_name -> vrma_node_index */
    typedef struct { char humanoid[64]; int vrma_node; } vrma_bone_map_t;
    vrma_bone_map_t vrma_map[128];
    int vrma_map_count = 0;

    const char *vrm_anim = strstr(json_str, "\"VRMC_vrm_animation\"");
    if (vrm_anim) {
        const char *hb = strstr(vrm_anim, "\"humanBones\"");
        if (hb) {
            const char *p = hb + 12;
            while (*p && *p != '{') p++;
            if (*p == '{') {
                p++;
                while (*p && vrma_map_count < 128) {
                    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
                    if (*p == '}') break;
                    if (*p != '"') { p++; continue; }
                    p++;
                    const char *key_end = strchr(p, '"');
                    if (!key_end) break;
                    int klen = (int)(key_end - p);
                    if (klen >= 64) klen = 63;
                    memcpy(vrma_map[vrma_map_count].humanoid, p, klen);
                    vrma_map[vrma_map_count].humanoid[klen] = '\0';
                    p = key_end + 1;

                    while (*p && *p != '{') p++;
                    if (!*p) break;
                    p++;
                    const char *nk = strstr(p, "\"node\"");
                    if (!nk) break;
                    const char *nv = nk + 6;
                    while (*nv && (*nv == ' ' || *nv == ':' || *nv == '\t')) nv++;
                    vrma_map[vrma_map_count].vrma_node = atoi(nv);

                    const char *cb = strchr(nv, '}');
                    if (!cb) break;
                    p = cb + 1;

                    vrma_map_count++;
                }
            }
        }
    }

    printf("[vrm_loader] VRMA humanoid map: %d entries, %d nodes\n", vrma_map_count, vrma_node_count);

    /* ---- Build retarget map: VRMA node index -> model bone index ---- */
    /* VRMA humanoid name -> model's humanoid map -> model node name -> model bone index */
    int retarget[512];
    memset(retarget, -1, sizeof(retarget));

    for (int i = 0; i < vrma_map_count; i++) {
        const char *hname = vrma_map[i].humanoid;
        int vrma_ni = vrma_map[i].vrma_node;
        if (vrma_ni < 0 || vrma_ni >= vrma_node_count) continue;

        /* Find model's node name for this humanoid bone */
        const char *model_node_name = NULL;
        for (uint32_t j = 0; j < model->humanoid_map_count; j++) {
            if (strcmp(model->humanoid_map[j].humanoid_name, hname) == 0) {
                model_node_name = model->humanoid_map[j].node_name;
                break;
            }
        }
        if (!model_node_name) continue;

        /* Find bone index in model by node name */
        int bone_idx = __find_bone_by_name(model_node_name);
        if (bone_idx >= 0) {
            retarget[vrma_ni] = bone_idx;
        }
    }

    int mapped = 0;
    for (int i = 0; i < vrma_node_count; i++)
        if (retarget[i] >= 0) mapped++;
    printf("[vrm_loader] VRMA retarget: %d/%d bones mapped\n", mapped, vrma_map_count);

    /* ---- Detect coordinate system mismatch (X/Z flip) ---- */
    /* VRM standard: left-side bones should have +X translation.
     * Detect purely from the MODEL's bone structure. */
    int coord_flip_xz = 0;
    {
        float score = 0.0f;
        int checks = 0;
        for (int i = 0; i < vrma_map_count; i++) {
            const char *hname = vrma_map[i].humanoid;
            int vrma_ni = vrma_map[i].vrma_node;
            if (vrma_ni < 0 || vrma_ni >= vrma_node_count) continue;
            int bone_idx = retarget[vrma_ni];
            if (bone_idx < 0) continue;
            float mt[3], mq[4], ms[3];
            __mat4_decompose(model->bones[bone_idx].local_transform, mt, mq, ms);
            if (strncmp(hname, "left", 4) == 0 && fabsf(mt[0]) > 0.01f) {
                score += (mt[0] < 0) ? -1.0f : 1.0f; checks++;
            }
            if (strncmp(hname, "right", 5) == 0 && fabsf(mt[0]) > 0.01f) {
                score += (mt[0] > 0) ? -1.0f : 1.0f; checks++;
            }
        }
        if (checks > 2 && score < 0) {
            coord_flip_xz = 1;
            printf("[vrm_loader] VRMA: model needs 180° Y correction\n");
        }
    }

    /* ---- Compute model bone rest world rotations for VRM 1.0 retarget ---- */
    /* We need the model's world-space rest rotations to properly transform
     * the world-space delta into each bone's parent local space. */
    float (*model_bone_world_rot)[4] = NULL;
    if (!coord_flip_xz) {
        model_bone_world_rot = (float (*)[4])calloc(model->bone_count, 4 * sizeof(float));
        for (uint32_t bi = 0; bi < model->bone_count; bi++) {
            float _t[3], lq[4], _s[3];
            __mat4_decompose(model->bones[bi].local_transform, _t, lq, _s);
            if (model->bones[bi].parent < 0) {
                memcpy(model_bone_world_rot[bi], lq, 4 * sizeof(float));
            } else {
                __quat_multiply(model_bone_world_rot[bi],
                                model_bone_world_rot[model->bones[bi].parent], lq);
            }
        }
    }

    /* ---- Parse accessors and bufferViews from JSON ---- */
    typedef struct {
        int buffer_view;
        int component_type; /* 5126=float, 5123=ushort, etc. */
        int count;
        int type_size;      /* 1=SCALAR, 3=VEC3, 4=VEC4 */
        float min_val, max_val;
    } accessor_t;

    accessor_t accessors[2048];
    int accessor_count = 0;

    const char *acc_key = strstr(json_str, "\"accessors\"");
    if (acc_key) {
        const char *arr = strchr(acc_key + 11, '[');
        if (arr) {
            const char *p = arr + 1;
            while (*p && *p != ']' && accessor_count < 2048) {
                const char *obj = strchr(p, '{');
                if (!obj) break;
                int depth = 1;
                const char *q = obj + 1;
                const char *obj_end = NULL;
                while (*q && depth > 0) {
                    if (*q == '{') depth++;
                    else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                    q++;
                }
                if (!obj_end) break;

                accessor_t *a = &accessors[accessor_count];
                memset(a, 0, sizeof(*a));
                a->buffer_view = -1;

                /* Parse fields */
                const char *fld;
                fld = strstr(obj, "\"bufferView\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 12;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    a->buffer_view = atoi(v);
                }
                fld = strstr(obj, "\"componentType\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 15;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    a->component_type = atoi(v);
                }
                fld = strstr(obj, "\"count\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 7;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    a->count = atoi(v);
                }
                fld = strstr(obj, "\"type\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 6;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t' || *v == '"')) v++;
                    if (strncmp(v, "SCALAR", 6) == 0) a->type_size = 1;
                    else if (strncmp(v, "VEC4", 4) == 0) a->type_size = 4;
                    else if (strncmp(v, "VEC3", 4) == 0) a->type_size = 3;
                    else if (strncmp(v, "VEC2", 4) == 0) a->type_size = 2;
                    else a->type_size = 1;
                }
                /* Parse min/max for time range */
                fld = strstr(obj, "\"max\"");
                if (fld && fld < obj_end) {
                    const char *v = strchr(fld + 4, '[');
                    if (v && v < obj_end) {
                        a->max_val = (float)atof(v + 1);
                    } else {
                        v = fld + 4;
                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                        a->max_val = (float)atof(v);
                    }
                }

                accessor_count++;
                p = obj_end + 1;
            }
        }
    }

    typedef struct {
        int buffer;
        int byte_offset;
        int byte_length;
        int byte_stride;
    } buffer_view_t;

    buffer_view_t buffer_views[2048];
    int bv_count = 0;

    const char *bv_key = strstr(json_str, "\"bufferViews\"");
    if (bv_key) {
        const char *arr = strchr(bv_key + 13, '[');
        if (arr) {
            const char *p = arr + 1;
            while (*p && *p != ']' && bv_count < 2048) {
                const char *obj = strchr(p, '{');
                if (!obj) break;
                int depth = 1;
                const char *q = obj + 1;
                const char *obj_end = NULL;
                while (*q && depth > 0) {
                    if (*q == '{') depth++;
                    else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                    q++;
                }
                if (!obj_end) break;

                buffer_view_t *bv = &buffer_views[bv_count];
                memset(bv, 0, sizeof(*bv));

                const char *fld;
                fld = strstr(obj, "\"buffer\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 8;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    bv->buffer = atoi(v);
                }
                fld = strstr(obj, "\"byteOffset\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 12;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    bv->byte_offset = atoi(v);
                }
                fld = strstr(obj, "\"byteLength\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 12;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    bv->byte_length = atoi(v);
                }
                fld = strstr(obj, "\"byteStride\"");
                if (fld && fld < obj_end) {
                    const char *v = fld + 12;
                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                    bv->byte_stride = atoi(v);
                }

                bv_count++;
                p = obj_end + 1;
            }
        }
    }

    /* Helper: read float data from an accessor */
    #define READ_ACCESSOR_FLOATS(acc_idx, out_ptr) do { \
        if ((acc_idx) >= 0 && (acc_idx) < accessor_count) { \
            accessor_t *_a = &accessors[acc_idx]; \
            if (_a->buffer_view >= 0 && _a->buffer_view < bv_count) { \
                buffer_view_t *_bv = &buffer_views[_a->buffer_view]; \
                int _offset = _bv->byte_offset; \
                if (_offset + _a->count * _a->type_size * (int)sizeof(float) <= (int)bin_len) { \
                    out_ptr = (float *)(bin_data + _offset); \
                } \
            } \
        } \
    } while(0)

    /* ---- Parse animations from JSON ---- */
    const char *anim_key = strstr(json_str, "\"animations\"");
    if (!anim_key) {
        fprintf(stderr, "[vrm_loader] VRMA: no animations found\n");
        free(json_str); free(file_buf);
        return -1;
    }

    /* For VRMA, there's typically one animation. Parse samplers and channels. */
    typedef struct { int input; int output; int interpolation; } sampler_t;
    typedef struct { int sampler; int target_node; int target_path; } channel_t;

    sampler_t samplers[1024];
    int sampler_count = 0;
    channel_t channels[1024];
    int channel_count = 0;

    /* Find the first animation object */
    const char *anim_arr = strchr(anim_key + 12, '[');
    if (anim_arr) {
        const char *anim_obj = strchr(anim_arr, '{');
        if (anim_obj) {
            /* Find the end of this animation object */
            int depth = 1;
            const char *q = anim_obj + 1;
            const char *anim_end = NULL;
            while (*q && depth > 0) {
                if (*q == '{') depth++;
                else if (*q == '}') { depth--; if (depth == 0) { anim_end = q; break; } }
                q++;
            }

            if (anim_end) {
                /* Parse samplers */
                const char *samp_key = strstr(anim_obj, "\"samplers\"");
                if (samp_key && samp_key < anim_end) {
                    const char *sarr = strchr(samp_key + 10, '[');
                    if (sarr) {
                        const char *p = sarr + 1;
                        while (*p && *p != ']' && sampler_count < 1024) {
                            const char *obj = strchr(p, '{');
                            if (!obj || obj > anim_end) break;
                            const char *oe = strchr(obj + 1, '}');
                            if (!oe) break;

                            sampler_t *s = &samplers[sampler_count];
                            s->input = -1; s->output = -1; s->interpolation = 0;

                            const char *fld;
                            fld = strstr(obj, "\"input\"");
                            if (fld && fld < oe) {
                                const char *v = fld + 7;
                                while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                s->input = atoi(v);
                            }
                            fld = strstr(obj, "\"output\"");
                            if (fld && fld < oe) {
                                const char *v = fld + 8;
                                while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                s->output = atoi(v);
                            }

                            sampler_count++;
                            p = oe + 1;
                        }
                    }
                }

                /* Parse channels */
                const char *chan_key = strstr(anim_obj, "\"channels\"");
                if (chan_key && chan_key < anim_end) {
                    const char *carr = strchr(chan_key + 10, '[');
                    if (carr) {
                        const char *p = carr + 1;
                        while (*p && *p != ']' && channel_count < 1024) {
                            const char *obj = strchr(p, '{');
                            if (!obj || obj > anim_end) break;

                            /* Find end of this channel object */
                            int d2 = 1;
                            const char *qq = obj + 1;
                            const char *oe = NULL;
                            while (*qq && d2 > 0) {
                                if (*qq == '{') d2++;
                                else if (*qq == '}') { d2--; if (d2 == 0) { oe = qq; break; } }
                                qq++;
                            }
                            if (!oe) break;

                            channel_t *c = &channels[channel_count];
                            c->sampler = -1; c->target_node = -1; c->target_path = -1;

                            const char *fld;
                            fld = strstr(obj, "\"sampler\"");
                            if (fld && fld < oe) {
                                const char *v = fld + 9;
                                while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                c->sampler = atoi(v);
                            }

                            /* target.node and target.path */
                            const char *tgt = strstr(obj, "\"target\"");
                            if (tgt && tgt < oe) {
                                const char *tn = strstr(tgt, "\"node\"");
                                if (tn && tn < oe) {
                                    const char *v = tn + 6;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    c->target_node = atoi(v);
                                }
                                const char *tp = strstr(tgt, "\"path\"");
                                if (tp && tp < oe) {
                                    const char *v = strchr(tp + 6, '"');
                                    if (v) {
                                        v++;
                                        if (strncmp(v, "translation", 11) == 0) c->target_path = 0;
                                        else if (strncmp(v, "rotation", 8) == 0) c->target_path = 1;
                                        else if (strncmp(v, "scale", 5) == 0) c->target_path = 2;
                                    }
                                }
                            }

                            channel_count++;
                            p = oe + 1;
                        }
                    }
                }
            }
        }
    }

    printf("[vrm_loader] VRMA: %d samplers, %d channels\n", sampler_count, channel_count);

    /* ---- Build animation ---- */
    /* Group channels by target node, then by bone */
    /* First, find duration */
    float duration = 0.0f;
    for (int i = 0; i < sampler_count; i++) {
        int input_acc = samplers[i].input;
        if (input_acc >= 0 && input_acc < accessor_count) {
            if (accessors[input_acc].max_val > duration)
                duration = accessors[input_acc].max_val;
        }
    }

    /* Create the animation */
    uint32_t new_anim_idx = model->animation_count;
    model->animations = (vrm_animation_t *)realloc(
        model->animations, (new_anim_idx + 1) * sizeof(vrm_animation_t));
    vrm_animation_t *va = &model->animations[new_anim_idx];
    memset(va, 0, sizeof(*va));
    model->animation_count = new_anim_idx + 1;

    snprintf(va->name, sizeof(va->name), "VRMA");
    va->duration = duration;

    /* Group channels by target_node -> bone_index */
    /* Temporary: count unique target bones */
    int unique_bones[512];
    int unique_bone_count = 0;

    for (int ci = 0; ci < channel_count; ci++) {
        int tn = channels[ci].target_node;
        if (tn < 0 || tn >= vrma_node_count) continue;
        int bone_idx = retarget[tn];
        if (bone_idx < 0) continue;

        /* Check if already in list */
        int found = 0;
        for (int j = 0; j < unique_bone_count; j++) {
            if (unique_bones[j] == bone_idx) { found = 1; break; }
        }
        if (!found && unique_bone_count < 512)
            unique_bones[unique_bone_count++] = bone_idx;
    }

    va->bone_anims = (vrm_bone_anim_t *)calloc(unique_bone_count, sizeof(vrm_bone_anim_t));
    va->bone_anim_count = unique_bone_count;

    for (int bi = 0; bi < unique_bone_count; bi++) {
        vrm_bone_anim_t *ba = &va->bone_anims[bi];
        ba->bone_index = unique_bones[bi];

        /* Count channels for this bone */
        int nch = 0;
        for (int ci = 0; ci < channel_count; ci++) {
            int tn = channels[ci].target_node;
            if (tn < 0 || tn >= vrma_node_count) continue;
            if (retarget[tn] != unique_bones[bi]) continue;
            nch++;
        }

        ba->channels = (vrm_anim_channel_t *)calloc(nch, sizeof(vrm_anim_channel_t));
        ba->channel_count = 0;

        for (int ci = 0; ci < channel_count; ci++) {
            int tn = channels[ci].target_node;
            if (tn < 0 || tn >= vrma_node_count) continue;
            if (retarget[tn] != unique_bones[bi]) continue;
            if (channels[ci].sampler < 0 || channels[ci].sampler >= sampler_count) continue;

            sampler_t *smp = &samplers[channels[ci].sampler];
            if (smp->input < 0 || smp->output < 0) continue;
            if (smp->input >= accessor_count || smp->output >= accessor_count) continue;

            accessor_t *in_acc = &accessors[smp->input];
            accessor_t *out_acc = &accessors[smp->output];

            /* Read time data */
            float *time_data = NULL;
            READ_ACCESSOR_FLOATS(smp->input, time_data);
            if (!time_data) continue;

            /* Read value data */
            float *val_data = NULL;
            READ_ACCESSOR_FLOATS(smp->output, val_data);
            if (!val_data) continue;

            vrm_anim_channel_t *ac = &ba->channels[ba->channel_count++];
            ac->path = channels[ci].target_path;
            ac->count = in_acc->count;

            /* Copy time data */
            ac->times = (float *)malloc(ac->count * sizeof(float));
            memcpy(ac->times, time_data, ac->count * sizeof(float));

            /* Copy value data */
            int val_size = (ac->path == 1) ? 4 : 3; /* rotation=quat(4), others=vec3(3) */
            ac->values = (float *)malloc(ac->count * val_size * sizeof(float));
            memcpy(ac->values, val_data, ac->count * val_size * sizeof(float));

            /* ---- Apply VRMA retarget correction ---- */
            /* three-vrm algorithm:
             * Q_normalized = Q_parent_world_vrma * Q_anim * inv(Q_bone_world_vrma)
             * This normalization is needed for ALL models (VRM 0.x and 1.0).
             * Then for VRM 0.x model ONLY: negate qx, qz (180° Y conjugation)
             *
             * For translation (hips only in VRMA):
             * t_normalized = hipsParentWorldMatrix * t_anim  (matrix transform)
             */
            if (ac->path == 1) {
                /* Rotation retarget using world rotations */
                float inv_bone_world[4];
                __quat_conjugate(inv_bone_world, vrma_world_rot[tn]);

                /* Parent world rotation */
                float parent_world[4] = {0, 0, 0, 1};
                if (vrma_node_parent[tn] >= 0) {
                    memcpy(parent_world, vrma_world_rot[vrma_node_parent[tn]], 4 * sizeof(float));
                }

                /* Get model bone's rest local rotation for VRM 1.0 composition */
                float model_rest_rot[4], _mrt[3], _mrs[3];
                __mat4_decompose(model->bones[unique_bones[bi]].local_transform,
                                 _mrt, model_rest_rot, _mrs);

                /* For VRM 1.0: precompute parent world rotation and its inverse */
                float model_pw[4] = {0, 0, 0, 1};
                float inv_model_pw[4] = {0, 0, 0, 1};
                if (!coord_flip_xz && model_bone_world_rot) {
                    int mbi = unique_bones[bi];
                    if (model->bones[mbi].parent >= 0) {
                        memcpy(model_pw, model_bone_world_rot[model->bones[mbi].parent],
                               4 * sizeof(float));
                    }
                    __quat_conjugate(inv_model_pw, model_pw);
                }

                for (uint32_t k = 0; k < ac->count; k++) {
                    float *qk = &ac->values[k * 4];
                    float t1[4], t2[4];
                    /* Q_world_delta = parent_world_vrma * Q_anim * inv(bone_world_vrma)
                     * This is a world-space delta (identity = no change from rest). */
                    __quat_multiply(t1, parent_world, qk);
                    __quat_multiply(t2, t1, inv_bone_world);
                    if (coord_flip_xz) {
                        /* 180° Y conjugation for VRM 0.x: negate x and z */
                        qk[0] = -t2[0];
                        qk[1] =  t2[1];
                        qk[2] = -t2[2];
                        qk[3] =  t2[3];
                    } else {
                        /* VRM 1.0: transform world delta into parent local space,
                         * then left-multiply with rest rotation.
                         * Q_local_delta = inv(parent_world) * Q_world_delta * parent_world
                         * Q_animated_local = Q_local_delta * Q_rest_local
                         * This ensures rotation axes stay in world alignment
                         * (e.g. spinning around world Y, not a tilted axis). */
                        float ld1[4], local_delta[4];
                        __quat_multiply(ld1, inv_model_pw, t2);
                        __quat_multiply(local_delta, ld1, model_pw);
                        float final_q[4];
                        __quat_multiply(final_q, local_delta, model_rest_rot);
                        qk[0] = final_q[0];
                        qk[1] = final_q[1];
                        qk[2] = final_q[2];
                        qk[3] = final_q[3];
                    }
                }
            }
            if (ac->path == 0) {
                /* Translation retarget (typically hips only):
                 * three-vrm: t_normalized = hipsParentWorldMatrix * t_anim
                 * Normalization is always needed. VRM 0.x additionally negates x,z. */
                float pw[4] = {0, 0, 0, 1};
                if (vrma_node_parent[tn] >= 0)
                    memcpy(pw, vrma_world_rot[vrma_node_parent[tn]], 4 * sizeof(float));

                /* Get model rest hips height for scale */
                float mt[3], mq[4], ms[3];
                __mat4_decompose(model->bones[unique_bones[bi]].local_transform,
                                 mt, mq, ms);
                /* VRMA rest hips position y-component (in world space) */
                float vrma_hy = vrma_node_trans[tn][1]; /* approximate */
                float scale = (vrma_hy > 0.01f) ? (mt[1] / vrma_hy) : 1.0f;

                for (uint32_t k = 0; k < ac->count; k++) {
                    float *tv = &ac->values[k * 3];
                    /* Rotate by parent world rotation: q * v * q^-1 */
                    float vx = tv[0], vy = tv[1], vz = tv[2];
                    /* Cross product part: t = 2 * (pw.xyz x v) */
                    float tx = 2.0f * (pw[1]*vz - pw[2]*vy);
                    float ty = 2.0f * (pw[2]*vx - pw[0]*vz);
                    float tz = 2.0f * (pw[0]*vy - pw[1]*vx);
                    /* result = v + w*t + (pw.xyz x t) */
                    float ox = vx + pw[3]*tx + (pw[1]*tz - pw[2]*ty);
                    float oy = vy + pw[3]*ty + (pw[2]*tx - pw[0]*tz);
                    float oz = vz + pw[3]*tz + (pw[0]*ty - pw[1]*tx);
                    if (coord_flip_xz) {
                        /* Negate x,z for VRM 0.x coord flip, scale */
                        tv[0] = -ox * scale;
                        tv[1] =  oy * scale;
                        tv[2] = -oz * scale;
                    } else {
                        /* VRM 1.0: apply rotation + scale, no coord flip */
                        tv[0] = ox * scale;
                        tv[1] = oy * scale;
                        tv[2] = oz * scale;
                    }
                }
            }
        }
    }

    printf("[vrm_loader] VRMA animation: duration=%.2fs, %d bone tracks, %d total channels\n",
           va->duration, unique_bone_count, channel_count);

    free(model_bone_world_rot);
    free(json_str);
    free(file_buf);
    return 0;

    #undef READ_ACCESSOR_FLOATS
}

/* ================================================================== */
/*  Animation evaluation                                               */
/* ================================================================== */

/** Find the two keyframes surrounding time t and return interpolation factor. */
static int __find_keyframe(const float *times, uint32_t count, float t, float *out_t)
{
    if (count == 0) { *out_t = 0; return 0; }
    if (t <= times[0]) { *out_t = 0; return 0; }
    if (t >= times[count - 1]) { *out_t = 0; return (int)count - 1; }

    for (uint32_t i = 0; i < count - 1; i++) {
        if (t >= times[i] && t < times[i + 1]) {
            float dt = times[i + 1] - times[i];
            *out_t = (dt > 1e-8f) ? (t - times[i]) / dt : 0.0f;
            return (int)i;
        }
    }
    *out_t = 0;
    return (int)count - 1;
}

static void __lerp3(float out[3], const float a[3], const float b[3], float t)
{
    out[0] = a[0] + t * (b[0] - a[0]);
    out[1] = a[1] + t * (b[1] - a[1]);
    out[2] = a[2] + t * (b[2] - a[2]);
}

void vrm_evaluate_animation(const vrm_model_t *model, uint32_t anim_index,
                             float time_sec, float *out_matrices)
{
    if (anim_index >= model->animation_count) {
        vrm_rest_pose_matrices(model, out_matrices);
        return;
    }

    const vrm_animation_t *anim = &model->animations[anim_index];

    /* Wrap time */
    float t = time_sec;
    if (anim->duration > 0) {
        t = fmodf(t, anim->duration);
        if (t < 0) t += anim->duration;
    }

    /* Start with rest-pose local transforms for all bones */
    uint32_t nb = model->bone_count;
    float *local_transforms = (float *)malloc(nb * 16 * sizeof(float));
    for (uint32_t i = 0; i < nb; i++)
        memcpy(&local_transforms[i * 16], model->bones[i].local_transform, 16 * sizeof(float));

    /* Apply animation channels — overwrite local transforms for animated bones */
    for (uint32_t ai = 0; ai < anim->bone_anim_count; ai++) {
        const vrm_bone_anim_t *ba = &anim->bone_anims[ai];
        int bone_idx = ba->bone_index;
        if (bone_idx < 0 || (uint32_t)bone_idx >= nb) continue;

        /* Decompose current local transform into T, R, S */
        float pos[3], rot[4], scl[3];
        __mat4_decompose(&local_transforms[bone_idx * 16], pos, rot, scl);

        /* Apply each channel */
        for (uint32_t ci = 0; ci < ba->channel_count; ci++) {
            const vrm_anim_channel_t *ch = &ba->channels[ci];
            if (ch->count == 0) continue;

            float interp;
            int ki = __find_keyframe(ch->times, ch->count, t, &interp);

            switch (ch->path) {
            case 0: /* translation */ {
                if (ki < (int)ch->count - 1) {
                    __lerp3(pos, &ch->values[ki*3], &ch->values[(ki+1)*3], interp);
                } else {
                    memcpy(pos, &ch->values[ki*3], 3*sizeof(float));
                }
                break;
            }
            case 1: /* rotation (quaternion) */ {
                if (ki < (int)ch->count - 1) {
                    __quat_slerp(rot, &ch->values[ki*4], &ch->values[(ki+1)*4], interp);
                } else {
                    memcpy(rot, &ch->values[ki*4], 4*sizeof(float));
                }
                break;
            }
            case 2: /* scale */ {
                if (ki < (int)ch->count - 1) {
                    __lerp3(scl, &ch->values[ki*3], &ch->values[(ki+1)*3], interp);
                } else {
                    memcpy(scl, &ch->values[ki*3], 3*sizeof(float));
                }
                break;
            }
            }
        }

        /* Recompose local transform */
        __mat4_compose(&local_transforms[bone_idx * 16], pos, rot, scl);
    }

    /* ---- Evaluate expression animation channels ---- */
    if (anim->expr_anims && anim->expr_anim_count > 0) {
        /* Cast away const for expression weight update — these are mutable state */
        vrm_model_t *mut_model = (vrm_model_t *)model;
        for (uint32_t ei = 0; ei < anim->expr_anim_count; ei++) {
            const vrm_expr_anim_t *ea = &anim->expr_anims[ei];
            if (ea->expression_index < 0 || (uint32_t)ea->expression_index >= model->expression_count)
                continue;
            const vrm_anim_channel_t *ch = &ea->channel;
            if (ch->count == 0) continue;

            float interp;
            int ki = __find_keyframe(ch->times, ch->count, t, &interp);
            float w;
            if (ki < (int)ch->count - 1) {
                w = ch->values[ki] + interp * (ch->values[ki + 1] - ch->values[ki]);
            } else {
                w = ch->values[ki];
            }
            if (w < 0.0f) w = 0.0f;
            if (w > 1.0f) w = 1.0f;
            mut_model->expression_weights[ea->expression_index] = w;
        }
    }

    /* ---- Compute global transforms via hierarchy ---- */
    float *global_transforms = (float *)malloc(nb * 16 * sizeof(float));

    for (uint32_t i = 0; i < nb; i++) {
        if (model->bones[i].parent < 0) {
            memcpy(&global_transforms[i * 16], &local_transforms[i * 16], 16 * sizeof(float));
        } else {
            int p = model->bones[i].parent;
            mat4_multiply(&global_transforms[i * 16],
                          &global_transforms[p * 16],
                          &local_transforms[i * 16]);
        }
    }

    /* ---- Apply VRMC_node_constraint (Aim / Roll) ---- */
    for (uint32_t ci = 0; ci < model->constraint_count; ci++) {
        const vrm_node_constraint_t *nc = &model->constraints[ci];
        int bi = nc->bone_index;
        int si = nc->source_index;
        if (bi < 0 || (uint32_t)bi >= nb || si < 0 || (uint32_t)si >= nb) continue;
        if (nc->weight < 1e-6f) continue;

        float *dst_global = &global_transforms[bi * 16];
        const float *src_global = &global_transforms[si * 16];

        if (nc->type == 0) {
            /* ---- Aim constraint ---- */
            /* Make the constrained bone point its aim axis toward the source bone's
             * world-space position. */
            float dst_pos[3] = { dst_global[12], dst_global[13], dst_global[14] };
            float src_pos[3] = { src_global[12], src_global[13], src_global[14] };

            /* Direction from constrained to source in world space */
            float dir[3] = { src_pos[0]-dst_pos[0], src_pos[1]-dst_pos[1], src_pos[2]-dst_pos[2] };
            float dir_len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
            if (dir_len < 1e-8f) continue;
            dir[0] /= dir_len; dir[1] /= dir_len; dir[2] /= dir_len;

            /* Transform direction into parent's local space */
            int par = model->bones[bi].parent;
            if (par >= 0) {
                float par_t[3], par_q[4], par_s[3];
                __mat4_decompose(&global_transforms[par * 16], par_t, par_q, par_s);
                float inv_par_q[4];
                __quat_conjugate(inv_par_q, par_q);
                /* Rotate dir by inv_par_q */
                float tx2 = 2.0f*(inv_par_q[1]*dir[2] - inv_par_q[2]*dir[1]);
                float ty2 = 2.0f*(inv_par_q[2]*dir[0] - inv_par_q[0]*dir[2]);
                float tz2 = 2.0f*(inv_par_q[0]*dir[1] - inv_par_q[1]*dir[0]);
                float lx = dir[0] + inv_par_q[3]*tx2 + (inv_par_q[1]*tz2 - inv_par_q[2]*ty2);
                float ly = dir[1] + inv_par_q[3]*ty2 + (inv_par_q[2]*tx2 - inv_par_q[0]*tz2);
                float lz = dir[2] + inv_par_q[3]*tz2 + (inv_par_q[0]*ty2 - inv_par_q[1]*tx2);
                dir[0] = lx; dir[1] = ly; dir[2] = lz;
            }

            /* The aim axis in local rest space */
            float aim[3] = {0,0,0};
            switch (nc->axis) {
                case VRM_AIM_POSITIVE_X: aim[0] =  1; break;
                case VRM_AIM_NEGATIVE_X: aim[0] = -1; break;
                case VRM_AIM_POSITIVE_Y: aim[1] =  1; break;
                case VRM_AIM_NEGATIVE_Y: aim[1] = -1; break;
                case VRM_AIM_POSITIVE_Z: aim[2] =  1; break;
                case VRM_AIM_NEGATIVE_Z: aim[2] = -1; break;
            }

            /* Compute rotation from aim to dir using cross + dot */
            float dot = aim[0]*dir[0] + aim[1]*dir[1] + aim[2]*dir[2];
            float cross[3] = {
                aim[1]*dir[2] - aim[2]*dir[1],
                aim[2]*dir[0] - aim[0]*dir[2],
                aim[0]*dir[1] - aim[1]*dir[0]
            };
            float cross_len = sqrtf(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]);

            float aim_q[4] = {0, 0, 0, 1}; /* identity */
            if (cross_len > 1e-8f) {
                /* axis = cross / |cross|, angle = atan2(|cross|, dot) */
                float half_angle = atan2f(cross_len, dot) * 0.5f;
                float s_ha = sinf(half_angle) / cross_len;
                aim_q[0] = cross[0] * s_ha;
                aim_q[1] = cross[1] * s_ha;
                aim_q[2] = cross[2] * s_ha;
                aim_q[3] = cosf(half_angle);
            } else if (dot < -0.999f) {
                /* 180° rotation: pick a perpendicular axis */
                float perp[3] = {1,0,0};
                if (fabsf(aim[0]) > 0.9f) { perp[0]=0; perp[1]=1; }
                aim_q[0] = perp[0]; aim_q[1] = perp[1]; aim_q[2] = perp[2]; aim_q[3] = 0;
            }

            /* Apply weight via slerp with identity */
            if (nc->weight < 0.999f) {
                float id_q[4] = {0,0,0,1};
                __quat_slerp(aim_q, id_q, aim_q, nc->weight);
            }

            /* New local transform = aim_q (replaces rest rotation) */
            float rest_t[3], rest_q[4], rest_s[3];
            __mat4_decompose(&local_transforms[bi * 16], rest_t, rest_q, rest_s);
            __mat4_compose(&local_transforms[bi * 16], rest_t, aim_q, rest_s);

        } else {
            /* ---- Roll constraint ---- */
            /* Extract source bone's local rotation */
            float src_t[3], src_q[4], src_s[3];
            __mat4_decompose(&local_transforms[si * 16], src_t, src_q, src_s);

            /* Decompose source rotation into roll (twist) around the specified axis */
            float twist_q[4] = {0, 0, 0, 1};
            int roll_axis = nc->axis; /* 0=X, 1=Y, 2=Z */
            float proj = src_q[roll_axis]; /* projection onto twist axis */
            float twist_w = src_q[3];
            float twist_len = sqrtf(proj*proj + twist_w*twist_w);

            if (twist_len > 1e-8f) {
                twist_q[roll_axis] = proj / twist_len;
                twist_q[3] = twist_w / twist_len;
            }

            /* Apply weight */
            if (nc->weight < 0.999f) {
                float id_q[4] = {0,0,0,1};
                __quat_slerp(twist_q, id_q, twist_q, nc->weight);
            }

            /* Compose with rest rotation: roll_applied = twist_q * rest_q */
            float rest_t[3], rest_q[4], rest_s[3];
            __mat4_decompose(&local_transforms[bi * 16], rest_t, rest_q, rest_s);
            float new_q[4];
            __quat_multiply(new_q, twist_q, rest_q);
            __mat4_compose(&local_transforms[bi * 16], rest_t, new_q, rest_s);
        }

        /* Recompute global transform for this bone and its descendants */
        for (uint32_t j = (uint32_t)bi; j < nb; j++) {
            if (j == (uint32_t)bi || model->bones[j].parent >= bi) {
                if (model->bones[j].parent < 0) {
                    memcpy(&global_transforms[j * 16], &local_transforms[j * 16], 16 * sizeof(float));
                } else {
                    int p = model->bones[j].parent;
                    mat4_multiply(&global_transforms[j * 16],
                                  &global_transforms[p * 16],
                                  &local_transforms[j * 16]);
                }
            }
        }
    }

    /* ---- Final bone matrices = global * inverseBindMatrix ---- */
    for (uint32_t i = 0; i < nb; i++) {
        mat4_multiply(&out_matrices[i * 16],
                      &global_transforms[i * 16],
                      model->bones[i].offset_matrix);
    }

    free(local_transforms);
    free(global_transforms);
}

void vrm_rest_pose_matrices(const vrm_model_t *model, float *out_matrices)
{
    uint32_t nb = model->bone_count;

    /* Compute global transforms from rest-pose local transforms */
    float *global_transforms = (float *)malloc(nb * 16 * sizeof(float));

    for (uint32_t i = 0; i < nb; i++) {
        if (model->bones[i].parent < 0) {
            memcpy(&global_transforms[i * 16], model->bones[i].local_transform, 16 * sizeof(float));
        } else {
            int p = model->bones[i].parent;
            mat4_multiply(&global_transforms[i * 16],
                          &global_transforms[p * 16],
                          model->bones[i].local_transform);
        }
    }

    for (uint32_t i = 0; i < nb; i++) {
        mat4_multiply(&out_matrices[i * 16],
                      &global_transforms[i * 16],
                      model->bones[i].offset_matrix);
    }

    free(global_transforms);
}

/* ================================================================== */
/*  VRM Expression / BlendShape extraction from glTF JSON              */
/* ================================================================== */

/** Helper: find vrm_mesh_t index by Assimp mesh index */
static int __find_vrm_mesh_by_assimp(const vrm_model_t *m, int assimp_mi)
{
    for (uint32_t i = 0; i < m->mesh_count; i++) {
        if (m->meshes[i].assimp_mesh_index == assimp_mi) return (int)i;
    }
    return -1;
}

/**
 * Parse VRM expressions (BlendShape groups) from VRM JSON.
 * Supports both VRM 0.x (blendShapeMaster.blendShapeGroups) and
 * VRM 1.0 (VRMC_vrm.expressions.preset / custom).
 */
static void __extract_vrm_expressions(vrm_model_t *model, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return; }
    if (hdr[0] != 'g' || hdr[1] != 'l' || hdr[2] != 'T' || hdr[3] != 'F') {
        fclose(fp); return;
    }

    uint32_t chunk_len, chunk_type;
    if (fread(&chunk_len, 4, 1, fp) != 1) { fclose(fp); return; }
    if (fread(&chunk_type, 4, 1, fp) != 1) { fclose(fp); return; }
    if (chunk_type != 0x4E4F534A) { fclose(fp); return; }

    char *json_str = (char *)malloc(chunk_len + 1);
    if (!json_str) { fclose(fp); return; }
    if (fread(json_str, 1, chunk_len, fp) != chunk_len) { free(json_str); fclose(fp); return; }
    json_str[chunk_len] = '\0';
    fclose(fp);

    model->expressions = (vrm_expression_t *)calloc(VRM_MAX_EXPRESSIONS, sizeof(vrm_expression_t));
    model->expression_count = 0;
    memset(model->expression_weights, 0, sizeof(model->expression_weights));

    /* ---- Parse glTF mesh primitives to build mesh-index mapping ---- */
    /* In glTF, meshes[i].primitives[j] maps to Assimp scene meshes sequentially.
     * We need: for each expression bind (mesh_index, morph_index in glTF),
     * find the corresponding vrm_mesh_t and morph target. */
    int gltf_mesh_to_assimp[256]; /* gltf mesh index -> first assimp mesh index */
    int gltf_mesh_prim_count[256];  /* gltf mesh index -> number of primitives */
    int gltf_mesh_count = 0;
    memset(gltf_mesh_to_assimp, -1, sizeof(gltf_mesh_to_assimp));
    memset(gltf_mesh_prim_count, 0, sizeof(gltf_mesh_prim_count));

    /* Parse "meshes" array to count primitives per glTF mesh */
    const char *meshes_key = strstr(json_str, "\"meshes\"");
    if (meshes_key) {
        const char *arr = strchr(meshes_key + 8, '[');
        if (arr) {
            int assimp_idx = 0;
            const char *p = arr + 1;
            while (*p && *p != ']' && gltf_mesh_count < 256) {
                const char *obj = strchr(p, '{');
                if (!obj) break;
                int depth = 1;
                const char *q = obj + 1;
                const char *obj_end = NULL;
                while (*q && depth > 0) {
                    if (*q == '{') depth++;
                    else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                    q++;
                }
                if (!obj_end) break;

                gltf_mesh_to_assimp[gltf_mesh_count] = assimp_idx;

                /* Count primitives in this mesh */
                int prim_count = 1;
                const char *prim_key = strstr(obj, "\"primitives\"");
                if (prim_key && prim_key < obj_end) {
                    const char *parr = strchr(prim_key + 12, '[');
                    if (parr && parr < obj_end) {
                        prim_count = 0;
                        const char *pp = parr + 1;
                        int pd = 1;
                        while (*pp && pd > 0) {
                            if (*pp == '{') { pd++; if (pd == 2) prim_count++; }
                            else if (*pp == '}') pd--;
                            else if (*pp == ']' && pd == 1) break;
                            pp++;
                        }
                        if (prim_count < 1) prim_count = 1;
                    }
                }
                gltf_mesh_prim_count[gltf_mesh_count] = prim_count;
                assimp_idx += prim_count;

                gltf_mesh_count++;
                p = obj_end + 1;
            }
        }
    }

    /* Helper: find vrm_mesh_t index by assimp mesh index */
    #define FIND_VRM_MESH(assimp_mi) __find_vrm_mesh_by_assimp(model, (assimp_mi))

    /* ---- Try VRM 1.0: VRMC_vrm.expressions ---- */
    const char *vrmc_vrm = strstr(json_str, "\"VRMC_vrm\"");
    if (vrmc_vrm) {
        const char *expr_key = strstr(vrmc_vrm, "\"expressions\"");
        if (expr_key) {
            /* Find "preset" and "custom" sections */
            const char *sections[] = { "\"preset\"", "\"custom\"" };
            int is_preset[] = { 1, 0 };

            for (int sec = 0; sec < 2; sec++) {
                const char *sec_key = strstr(expr_key, sections[sec]);
                if (!sec_key) continue;

                const char *sp = sec_key + strlen(sections[sec]);
                while (*sp && *sp != '{') sp++;
                if (*sp != '{') continue;
                sp++;

                /* Iterate expression entries: "name":{...} */
                while (*sp && model->expression_count < VRM_MAX_EXPRESSIONS) {
                    while (*sp && (*sp == ' ' || *sp == '\n' || *sp == '\r' || *sp == '\t' || *sp == ',')) sp++;
                    if (*sp == '}') break;
                    if (*sp != '"') { sp++; continue; }

                    /* Expression name */
                    sp++;
                    const char *name_end = strchr(sp, '"');
                    if (!name_end) break;
                    int nlen = (int)(name_end - sp);
                    if (nlen >= 64) nlen = 63;

                    vrm_expression_t *expr = &model->expressions[model->expression_count];
                    memcpy(expr->name, sp, nlen);
                    expr->name[nlen] = '\0';
                    expr->is_preset = is_preset[sec];
                    expr->binds = NULL;
                    expr->bind_count = 0;
                    sp = name_end + 1;

                    /* Find the expression object */
                    while (*sp && *sp != '{') sp++;
                    if (!*sp) break;
                    /* Find end of this expression object */
                    int edepth = 1;
                    const char *eq = sp + 1;
                    const char *expr_end = NULL;
                    while (*eq && edepth > 0) {
                        if (*eq == '{') edepth++;
                        else if (*eq == '}') { edepth--; if (edepth == 0) { expr_end = eq; break; } }
                        eq++;
                    }
                    if (!expr_end) break;

                    /* Parse "morphTargetBinds":[{"node":N,"index":I,"weight":W},...] */
                    const char *mtb_key = strstr(sp, "\"morphTargetBinds\"");
                    if (mtb_key && mtb_key < expr_end) {
                        const char *mtb_arr = strchr(mtb_key + 18, '[');
                        if (mtb_arr && mtb_arr < expr_end) {
                            /* Count binds */
                            int max_binds = 32;
                            expr->binds = (vrm_expression_bind_t *)calloc(max_binds, sizeof(vrm_expression_bind_t));

                            const char *bp = mtb_arr + 1;
                            while (*bp && *bp != ']' && (int)expr->bind_count < max_binds) {
                                const char *bobj = strchr(bp, '{');
                                if (!bobj || bobj > expr_end) break;
                                const char *bobj_end = strchr(bobj + 1, '}');
                                if (!bobj_end) break;

                                int gltf_mesh_idx = -1, morph_idx = -1;
                                float bweight = 1.0f;

                                const char *fld;
                                fld = strstr(bobj, "\"node\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 6;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    gltf_mesh_idx = atoi(v);
                                }
                                fld = strstr(bobj, "\"index\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 7;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    morph_idx = atoi(v);
                                }
                                fld = strstr(bobj, "\"weight\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 8;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    bweight = (float)atof(v);
                                }

                                /* Resolve glTF mesh index -> vrm mesh index.
                                 * A single glTF mesh may have N primitives that Assimp
                                 * splits into N separate meshes. Create a bind for each. */
                                if (gltf_mesh_idx >= 0 && gltf_mesh_idx < gltf_mesh_count && morph_idx >= 0) {
                                    int base_ai = gltf_mesh_to_assimp[gltf_mesh_idx];
                                    int nprims  = gltf_mesh_prim_count[gltf_mesh_idx];
                                    for (int pi = 0; pi < nprims && (int)expr->bind_count < max_binds; pi++) {
                                        int vrm_mi = FIND_VRM_MESH(base_ai + pi);
                                        if (vrm_mi >= 0 && (uint32_t)morph_idx < model->meshes[vrm_mi].morph_target_count) {
                                            vrm_expression_bind_t *bind = &expr->binds[expr->bind_count++];
                                            bind->mesh_index = (uint32_t)vrm_mi;
                                            bind->morph_index = (uint32_t)morph_idx;
                                            bind->weight = bweight;
                                        }
                                    }
                                }

                                bp = bobj_end + 1;
                            }

                            if (expr->bind_count == 0) {
                                free(expr->binds);
                                expr->binds = NULL;
                            }
                        }
                    }

                    if (expr->bind_count > 0 || expr->name[0] != '\0') {
                        model->expression_count++;
                    }

                    sp = expr_end + 1;
                }
            }
        }
    }

    /* ---- Try VRM 0.x: blendShapeMaster.blendShapeGroups ---- */
    if (model->expression_count == 0) {
        const char *bsm = strstr(json_str, "\"blendShapeMaster\"");
        if (!bsm) bsm = strstr(json_str, "\"blendShapeGroups\"");
        if (bsm) {
            const char *bsg = strstr(bsm, "\"blendShapeGroups\"");
            if (!bsg) bsg = bsm; /* might already point to it */
            const char *arr = strchr(bsg + 18, '[');
            if (arr) {
                const char *p = arr + 1;
                while (*p && *p != ']' && model->expression_count < VRM_MAX_EXPRESSIONS) {
                    const char *obj = strchr(p, '{');
                    if (!obj) break;
                    int depth = 1;
                    const char *q = obj + 1;
                    const char *obj_end = NULL;
                    while (*q && depth > 0) {
                        if (*q == '{') depth++;
                        else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
                        q++;
                    }
                    if (!obj_end) break;

                    vrm_expression_t *expr = &model->expressions[model->expression_count];
                    memset(expr, 0, sizeof(*expr));

                    /* Parse "name" (VRM 0.x uses "name" field) */
                    const char *nk = strstr(obj, "\"name\"");
                    if (nk && nk < obj_end) {
                        const char *nv = strchr(nk + 6, '"');
                        if (nv && nv < obj_end) {
                            nv++;
                            const char *ne = strchr(nv, '"');
                            if (ne) {
                                int nlen = (int)(ne - nv);
                                if (nlen >= 64) nlen = 63;
                                memcpy(expr->name, nv, nlen);
                                expr->name[nlen] = '\0';
                            }
                        }
                    }

                    /* Parse "presetName" — use it as the canonical expression name
                     * when available and not "unknown", so the emotion system can
                     * match by standard VRM preset names regardless of display language. */
                    const char *pk = strstr(obj, "\"presetName\"");
                    if (pk && pk < obj_end) {
                        const char *pv = strchr(pk + 12, '"');
                        if (pv && pv < obj_end) {
                            pv++;
                            const char *pve = strchr(pv, '"');
                            if (pve) {
                                int plen = (int)(pve - pv);
                                if (plen < 64 && !(plen == 7 && strncmp(pv, "unknown", 7) == 0)) {
                                    /* Overwrite name with standard presetName */
                                    memcpy(expr->name, pv, plen);
                                    expr->name[plen] = '\0';
                                    expr->is_preset = 1;
                                }
                            }
                        }
                    }

                    /* Parse "binds":[{"mesh":M,"index":I,"weight":W},...] */
                    const char *binds_key = strstr(obj, "\"binds\"");
                    if (binds_key && binds_key < obj_end) {
                        const char *barr = strchr(binds_key + 7, '[');
                        if (barr && barr < obj_end) {
                            int max_binds = 128; /* larger: each glTF bind expands to N primitives */
                            expr->binds = (vrm_expression_bind_t *)calloc(max_binds, sizeof(vrm_expression_bind_t));

                            const char *bp = barr + 1;
                            while (*bp && *bp != ']' && (int)expr->bind_count < max_binds) {
                                const char *bobj = strchr(bp, '{');
                                if (!bobj || bobj > obj_end) break;
                                const char *bobj_end = strchr(bobj + 1, '}');
                                if (!bobj_end) break;

                                int gltf_mesh_idx = -1, morph_idx = -1;
                                float bweight = 100.0f; /* VRM 0.x uses 0-100 */

                                const char *fld;
                                fld = strstr(bobj, "\"mesh\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 6;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    gltf_mesh_idx = atoi(v);
                                }
                                fld = strstr(bobj, "\"index\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 7;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    morph_idx = atoi(v);
                                }
                                fld = strstr(bobj, "\"weight\"");
                                if (fld && fld < bobj_end) {
                                    const char *v = fld + 8;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    bweight = (float)atof(v);
                                }

                                /* VRM 0.x weight is 0-100, normalize to 0-1 */
                                bweight /= 100.0f;

                                /* A single glTF mesh may have N primitives that Assimp
                                 * splits into N separate meshes. Create a bind for each. */
                                if (gltf_mesh_idx >= 0 && gltf_mesh_idx < gltf_mesh_count && morph_idx >= 0) {
                                    int base_ai = gltf_mesh_to_assimp[gltf_mesh_idx];
                                    int nprims  = gltf_mesh_prim_count[gltf_mesh_idx];
                                    for (int pi = 0; pi < nprims && (int)expr->bind_count < max_binds; pi++) {
                                        int vrm_mi = FIND_VRM_MESH(base_ai + pi);
                                        if (vrm_mi >= 0 && (uint32_t)morph_idx < model->meshes[vrm_mi].morph_target_count) {
                                            vrm_expression_bind_t *bind = &expr->binds[expr->bind_count++];
                                            bind->mesh_index = (uint32_t)vrm_mi;
                                            bind->morph_index = (uint32_t)morph_idx;
                                            bind->weight = bweight;
                                        }
                                    }
                                }

                                bp = bobj_end + 1;
                            }

                            if (expr->bind_count == 0) {
                                free(expr->binds);
                                expr->binds = NULL;
                            }
                        }
                    }

                    if (expr->name[0] != '\0') {
                        model->expression_count++;
                    }

                    p = obj_end + 1;
                }
            }
        }
    }

    #undef FIND_VRM_MESH

    free(json_str);

    if (model->expression_count > 0) {
        model->expressions = (vrm_expression_t *)realloc(
            model->expressions, model->expression_count * sizeof(vrm_expression_t));
        printf("[vrm_loader] expressions: %u\n", model->expression_count);
        for (uint32_t i = 0; i < model->expression_count && i < 10; i++)
            printf("[vrm_loader]   [%u] \"%s\" (%u binds, %s)\n",
                   i, model->expressions[i].name, model->expressions[i].bind_count,
                   model->expressions[i].is_preset ? "preset" : "custom");
        if (model->expression_count > 10)
            printf("[vrm_loader]   ... +%u more\n", model->expression_count - 10);
    } else {
        free(model->expressions);
        model->expressions = NULL;
        printf("[vrm_loader] no VRM expressions found\n");
    }
}

/* ================================================================== */
/*  Public: Expression / BlendShape API                                */
/* ================================================================== */

int vrm_find_expression(const vrm_model_t *model, const char *name)
{
    if (!model || !model->expressions || !name) return -1;
    for (uint32_t i = 0; i < model->expression_count; i++) {
        if (strcasecmp(model->expressions[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

void vrm_set_expression_weight(vrm_model_t *model, int expr_index, float weight)
{
    if (!model || expr_index < 0 || (uint32_t)expr_index >= model->expression_count) return;
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 1.0f) weight = 1.0f;
    model->expression_weights[expr_index] = weight;
}

void vrm_apply_morph_targets(vrm_model_t *model)
{
    if (!model) return;

    /* Reset all meshes to base vertices */
    for (uint32_t mi = 0; mi < model->mesh_count; mi++) {
        vrm_mesh_t *mesh = &model->meshes[mi];
        if (mesh->base_vertices && mesh->morph_target_count > 0) {
            memcpy(mesh->vertices, mesh->base_vertices,
                   (size_t)mesh->vertex_count * 16 * sizeof(float));
        }
    }

    /* Accumulate morph target deltas from active expressions */
    for (uint32_t ei = 0; ei < model->expression_count; ei++) {
        float ew = model->expression_weights[ei];
        if (ew < 1e-6f) continue;

        const vrm_expression_t *expr = &model->expressions[ei];
        for (uint32_t bi = 0; bi < expr->bind_count; bi++) {
            const vrm_expression_bind_t *bind = &expr->binds[bi];
            if (bind->mesh_index >= model->mesh_count) continue;

            vrm_mesh_t *mesh = &model->meshes[bind->mesh_index];
            if (bind->morph_index >= mesh->morph_target_count) continue;

            const vrm_morph_target_t *mt = &mesh->morph_targets[bind->morph_index];
            float w = ew * bind->weight;

            /* Apply position deltas */
            if (mt->delta_positions) {
                for (uint32_t v = 0; v < mesh->vertex_count; v++) {
                    float *dst = &mesh->vertices[v * 16];
                    dst[0] += mt->delta_positions[v*3+0] * w;
                    dst[1] += mt->delta_positions[v*3+1] * w;
                    dst[2] += mt->delta_positions[v*3+2] * w;
                }
            }

            /* Apply normal deltas */
            if (mt->delta_normals) {
                for (uint32_t v = 0; v < mesh->vertex_count; v++) {
                    float *dst = &mesh->vertices[v * 16];
                    dst[3] += mt->delta_normals[v*3+0] * w;
                    dst[4] += mt->delta_normals[v*3+1] * w;
                    dst[5] += mt->delta_normals[v*3+2] * w;
                }
            }
        }
    }
}

/* ================================================================== */
/*  VRM Spring Bone extraction from glTF JSON                          */
/* ================================================================== */

/**
 * Find bone index by glTF node index.
 * glTF node indices map to Assimp nodes by name; we search our bone array.
 */
static int __find_bone_by_node_index(const vrm_model_t *model,
                                     const char *json_str, int node_idx)
{
    /* Walk to "nodes" array, find entry at node_idx, get "name" */
    const char *narr = strstr(json_str, "\"nodes\"");
    if (!narr) return -1;
    const char *arr = strchr(narr + 7, '[');
    if (!arr) return -1;

    const char *p = arr + 1;
    int cur = 0;
    while (*p && cur < node_idx) {
        const char *obj = strchr(p, '{');
        if (!obj) return -1;
        int depth = 1;
        const char *q = obj + 1;
        while (*q && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            q++;
        }
        p = q;
        cur++;
    }

    /* Now p points near the node_idx-th object */
    const char *obj = strchr(p, '{');
    if (!obj) return -1;

    const char *nk = strstr(obj, "\"name\"");
    if (!nk) return -1;
    const char *nv = strchr(nk + 6, '"');
    if (!nv) return -1;
    nv++;
    const char *nve = strchr(nv, '"');
    if (!nve) return -1;

    int nlen = (int)(nve - nv);
    char node_name[128];
    if (nlen >= 128) nlen = 127;
    memcpy(node_name, nv, nlen);
    node_name[nlen] = '\0';

    /* Find bone with matching name */
    for (uint32_t i = 0; i < model->bone_count; i++) {
        if (strcmp(model->bones[i].name, node_name) == 0)
            return (int)i;
    }
    return -1;
}

/**
 * Build a chain from a root bone by walking all descendants recursively.
 * For single-child paths, follows the chain linearly.
 * For branches, recursively includes all sub-chains (depth-first).
 * Returns the number of bones added.
 */
static int __build_bone_chain(const vrm_model_t *model, int root_bone,
                              int *out_chain, int max_chain)
{
    if (root_bone < 0 || root_bone >= (int)model->bone_count) return 0;
    if (max_chain <= 0) return 0;

    int count = 0;
     out_chain[count++] = root_bone;

    /* Find all children of this bone */
    int children[32];
    int child_count = 0;
    for (uint32_t i = 0; i < model->bone_count && child_count < 32; i++) {
        if (model->bones[i].parent == root_bone) {
            children[child_count++] = (int)i;
        }
    }

    /* Skip _end bones (leaf terminators with no children of their own) */
    if (child_count == 0) {
        return count; /* leaf */
    }

    /* Recursively add all children */
    for (int c = 0; c < child_count; c++) {
        int remaining = max_chain - count;
        if (remaining <= 0) break;
        int added = __build_bone_chain(model, children[c],
                                       &out_chain[count], remaining);
        count += added;
    }

    return count;
}

/* ================================================================== */
/*  VRMC_node_constraint extraction                                    */
/* ================================================================== */

static void __extract_vrm_node_constraints(vrm_model_t *model, const char *path)
{
    model->constraints = NULL;
    model->constraint_count = 0;

    /* Read glTF JSON chunk */
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return; }

    uint32_t json_len;
    if (fread(&json_len, 4, 1, fp) != 1) { fclose(fp); return; }
    uint32_t chunk_type;
    if (fread(&chunk_type, 4, 1, fp) != 1) { fclose(fp); return; }

    char *json_str = (char *)malloc(json_len + 1);
    if (!json_str) { fclose(fp); return; }
    if (fread(json_str, 1, json_len, fp) != json_len) {
        free(json_str); fclose(fp); return;
    }
    json_str[json_len] = '\0';
    fclose(fp);

    /* Scan through "nodes" array for VRMC_node_constraint extensions */
    vrm_node_constraint_t constraints[VRM_MAX_CONSTRAINTS];
    int count = 0;

    const char *nc_ptr = strstr(json_str, "\"VRMC_node_constraint\"");
    while (nc_ptr && count < VRM_MAX_CONSTRAINTS) {
        /* Find the node index: walk backwards to find which node object this is in.
         * Instead, we re-scan from the node array to correlate. */
        nc_ptr = strstr(nc_ptr, "\"constraint\"");
        if (!nc_ptr) break;

        /* Find the enclosing constraint block end */
        const char *cblock = strchr(nc_ptr, '{');
        if (!cblock) break;
        int depth = 1;
        const char *cblock_end = cblock + 1;
        while (*cblock_end && depth > 0) {
            if (*cblock_end == '{') depth++;
            else if (*cblock_end == '}') depth--;
            cblock_end++;
        }

        /* Determine if this is "aim" or "roll" */
        const char *aim_key = strstr(cblock, "\"aim\"");
        const char *roll_key = strstr(cblock, "\"roll\"");

        int is_aim = (aim_key && aim_key < cblock_end) ? 1 : 0;
        int is_roll = (roll_key && roll_key < cblock_end) ? 1 : 0;

        if (!is_aim && !is_roll) {
            nc_ptr = cblock_end;
            continue;
        }

        vrm_node_constraint_t *nc = &constraints[count];
        memset(nc, 0, sizeof(*nc));
        nc->bone_index = -1;
        nc->source_index = -1;
        nc->weight = 1.0f;

        const char *inner = is_aim ? aim_key : roll_key;
        const char *inner_obj = strchr(inner, '{');
        if (!inner_obj || inner_obj >= cblock_end) { nc_ptr = cblock_end; continue; }
        const char *inner_end = strchr(inner_obj + 1, '}');
        if (!inner_end || inner_end >= cblock_end) inner_end = cblock_end - 1;

        /* "source": N */
        const char *src_key = strstr(inner_obj, "\"source\"");
        int source_node = -1;
        if (src_key && src_key < inner_end) {
            const char *v = src_key + 8;
            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
            source_node = atoi(v);
            nc->source_index = __find_bone_by_node_index(model, json_str, source_node);
        }

        /* "weight": W */
        const char *wk = strstr(inner_obj, "\"weight\"");
        if (wk && wk < inner_end) {
            const char *v = wk + 8;
            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
            nc->weight = (float)atof(v);
        }

        if (is_aim) {
            nc->type = 0;
            /* "aimAxis": "PositiveX" etc */
            const char *ax_key = strstr(inner_obj, "\"aimAxis\"");
            if (ax_key && ax_key < inner_end) {
                const char *v = strchr(ax_key + 9, '"');
                if (v) {
                    v++;
                    if (strncmp(v, "PositiveX", 9) == 0) nc->axis = VRM_AIM_POSITIVE_X;
                    else if (strncmp(v, "NegativeX", 9) == 0) nc->axis = VRM_AIM_NEGATIVE_X;
                    else if (strncmp(v, "PositiveY", 9) == 0) nc->axis = VRM_AIM_POSITIVE_Y;
                    else if (strncmp(v, "NegativeY", 9) == 0) nc->axis = VRM_AIM_NEGATIVE_Y;
                    else if (strncmp(v, "PositiveZ", 9) == 0) nc->axis = VRM_AIM_POSITIVE_Z;
                    else if (strncmp(v, "NegativeZ", 9) == 0) nc->axis = VRM_AIM_NEGATIVE_Z;
                }
            }
        } else {
            nc->type = 1;
            /* "rollAxis": "X" / "Y" / "Z" */
            const char *ax_key = strstr(inner_obj, "\"rollAxis\"");
            if (ax_key && ax_key < inner_end) {
                const char *v = strchr(ax_key + 10, '"');
                if (v) {
                    v++;
                    if (*v == 'X') nc->axis = VRM_ROLL_X;
                    else if (*v == 'Y') nc->axis = VRM_ROLL_Y;
                    else if (*v == 'Z') nc->axis = VRM_ROLL_Z;
                }
            }
        }

        count++;
        nc_ptr = cblock_end;
    }

    /* Now we need to find which node each constraint belongs to.
     * Walk through nodes array, and for each node with VRMC_node_constraint,
     * match it to our parsed constraints in order. */
    if (count > 0) {
        /* Find node indices by re-scanning VRMC_node_constraint occurrences
         * correlated with nodes array position */
        const char *nodes_arr = strstr(json_str, "\"nodes\"");
        if (nodes_arr) {
            const char *arr = strchr(nodes_arr + 7, '[');
            if (arr) {
                const char *p = arr + 1;
                int node_idx = 0;
                int ci = 0; /* constraint index */
                while (*p && *p != ']' && ci < count) {
                    const char *obj = strchr(p, '{');
                    if (!obj) break;
                    /* Find matching end brace at depth 1 */
                    int d = 1;
                    const char *q = obj + 1;
                    const char *obj_end = NULL;
                    while (*q && d > 0) {
                        if (*q == '{') d++;
                        else if (*q == '}') { d--; if (d == 0) { obj_end = q; break; } }
                        q++;
                    }
                    if (!obj_end) break;

                    /* Check if this node has VRMC_node_constraint */
                    const char *nc_check = strstr(obj, "\"VRMC_node_constraint\"");
                    if (nc_check && nc_check < obj_end) {
                        if (ci < count) {
                            constraints[ci].bone_index = __find_bone_by_node_index(
                                model, json_str, node_idx);
                            ci++;
                        }
                    }

                    p = obj_end + 1;
                    node_idx++;
                }
            }
        }

        /* Allocate and copy */
        model->constraints = (vrm_node_constraint_t *)malloc(
            count * sizeof(vrm_node_constraint_t));
        memcpy(model->constraints, constraints, count * sizeof(vrm_node_constraint_t));
        model->constraint_count = count;

        printf("[vrm_loader] node constraints: %d\n", count);
        for (int i = 0; i < count; i++) {
            const char *type_str = constraints[i].type == 0 ? "aim" : "roll";
            const char *bone_name = (constraints[i].bone_index >= 0) ?
                model->bones[constraints[i].bone_index].name : "?";
            const char *src_name = (constraints[i].source_index >= 0) ?
                model->bones[constraints[i].source_index].name : "?";
            printf("[vrm_loader]   [%d] %s: %s -> %s (weight=%.2f)\n",
                   i, type_str, bone_name, src_name, constraints[i].weight);
        }
    }

    free(json_str);
}

static void __extract_vrm_spring_bones(vrm_model_t *model, const char *path)
{
    model->spring_groups = NULL;
    model->spring_group_count = 0;
    model->collider_groups = NULL;
    model->collider_group_count = 0;

    /* Read the glTF JSON chunk */
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return; }

    uint32_t json_len;
    if (fread(&json_len, 4, 1, fp) != 1) { fclose(fp); return; }

    uint32_t chunk_type;
    if (fread(&chunk_type, 4, 1, fp) != 1) { fclose(fp); return; }

    char *json_str = (char *)malloc(json_len + 1);
    if (!json_str) { fclose(fp); return; }
    if (fread(json_str, 1, json_len, fp) != json_len) {
        free(json_str); fclose(fp); return;
    }
    json_str[json_len] = '\0';
    fclose(fp);

    /* ---- Try VRM 0.x: secondaryAnimation ---- */
    const char *sec_anim = strstr(json_str, "\"secondaryAnimation\"");
    if (sec_anim) {
        /* ---- Parse colliderGroups ---- */
        const char *cg_key = strstr(sec_anim, "\"colliderGroups\"");
        if (cg_key) {
            const char *cg_arr = strchr(cg_key + 16, '[');
            if (cg_arr) {
                /* Count collider groups */
                int max_cg = 64;
                model->collider_groups = (vrm_collider_group_t *)calloc(max_cg, sizeof(vrm_collider_group_t));

                const char *cp = cg_arr + 1;
                while (*cp && *cp != ']' && (int)model->collider_group_count < max_cg) {
                    const char *cobj = strchr(cp, '{');
                    if (!cobj) break;

                    /* Find matching end brace at depth 1 */
                    int depth = 1;
                    const char *cq = cobj + 1;
                    const char *cobj_end = NULL;
                    while (*cq && depth > 0) {
                        if (*cq == '{') depth++;
                        else if (*cq == '}') { depth--; if (depth == 0) { cobj_end = cq; break; } }
                        cq++;
                    }
                    if (!cobj_end) break;

                    vrm_collider_group_t *grp = &model->collider_groups[model->collider_group_count];

                    /* "node": N */
                    const char *nk = strstr(cobj, "\"node\"");
                    int node_idx = -1;
                    if (nk && nk < cobj_end) {
                        const char *v = nk + 6;
                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                        node_idx = atoi(v);
                    }
                    grp->bone_index = __find_bone_by_node_index(model, json_str, node_idx);

                    /* "colliders": [{offset:{x,y,z}, radius:R}, ...] */
                    const char *col_key = strstr(cobj, "\"colliders\"");
                    if (col_key && col_key < cobj_end) {
                        const char *col_arr = strchr(col_key + 11, '[');
                        if (col_arr && col_arr < cobj_end) {
                            int max_cols = 16;
                            grp->colliders = (vrm_spring_collider_t *)calloc(max_cols, sizeof(vrm_spring_collider_t));

                            const char *colp = col_arr + 1;
                            while (*colp && *colp != ']' && (int)grp->collider_count < max_cols) {
                                const char *co = strchr(colp, '{');
                                if (!co || co > cobj_end) break;
                                const char *co_end = strchr(co + 1, '}');
                                if (!co_end) break;

                                vrm_spring_collider_t *col = &grp->colliders[grp->collider_count];

                                /* Parse offset */
                                const char *off_key = strstr(co, "\"offset\"");
                                if (off_key && off_key < co_end) {
                                    const char *ox = strstr(off_key, "\"x\"");
                                    const char *oy = strstr(off_key, "\"y\"");
                                    const char *oz = strstr(off_key, "\"z\"");
                                    if (ox && ox < co_end) { const char *v = ox+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; col->offset[0]=(float)atof(v); }
                                    if (oy && oy < co_end) { const char *v = oy+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; col->offset[1]=(float)atof(v); }
                                    if (oz && oz < co_end) { const char *v = oz+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; col->offset[2]=(float)atof(v); }
                                }

                                const char *rk = strstr(co, "\"radius\"");
                                if (rk && rk < co_end) {
                                    const char *v = rk + 8;
                                    while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                    col->radius = (float)atof(v);
                                }

                                grp->collider_count++;
                                colp = co_end + 1;
                            }
                        }
                    }

                    model->collider_group_count++;
                    cp = cobj_end + 1;
                }
            }
        }

        /* ---- Parse boneGroups ---- */
        const char *bg_key = strstr(sec_anim, "\"boneGroups\"");
        if (bg_key) {
            const char *bg_arr = strchr(bg_key + 12, '[');
            if (bg_arr) {
                int max_groups = VRM_MAX_SPRING_GROUPS;
                model->spring_groups = (vrm_spring_group_t *)calloc(max_groups, sizeof(vrm_spring_group_t));

                const char *bp = bg_arr + 1;
                while (*bp && *bp != ']' && (int)model->spring_group_count < max_groups) {
                    const char *bobj = strchr(bp, '{');
                    if (!bobj) break;

                    /* Find matching end brace */
                    int depth = 1;
                    const char *bq = bobj + 1;
                    const char *bobj_end = NULL;
                    while (*bq && depth > 0) {
                        if (*bq == '{') depth++;
                        else if (*bq == '}') { depth--; if (depth == 0) { bobj_end = bq; break; } }
                        bq++;
                    }
                    if (!bobj_end) break;

                    vrm_spring_group_t *sgrp = &model->spring_groups[model->spring_group_count];
                    sgrp->center_bone = -1;

                    /* Parse shared parameters */
                    float stiffness = 1.0f, gravity_power = 0.0f, drag_force = 0.5f, hit_radius = 0.0f;
                    float gravity_dir[3] = {0.0f, -1.0f, 0.0f};

                    const char *fld;
                    /* VRM 0.x uses "stiffiness" (yes, misspelled in spec) */
                    fld = strstr(bobj, "\"stiffiness\"");
                    if (!fld || fld > bobj_end) fld = strstr(bobj, "\"stiffness\"");
                    if (fld && fld < bobj_end) {
                        const char *v = fld + 12; while (*v && (*v==' '||*v==':'||*v=='\t')) v++;
                        stiffness = (float)atof(v);
                    }
                    fld = strstr(bobj, "\"gravityPower\"");
                    if (fld && fld < bobj_end) {
                        const char *v = fld + 14; while (*v && (*v==' '||*v==':'||*v=='\t')) v++;
                        gravity_power = (float)atof(v);
                    }
                    fld = strstr(bobj, "\"dragForce\"");
                    if (fld && fld < bobj_end) {
                        const char *v = fld + 11; while (*v && (*v==' '||*v==':'||*v=='\t')) v++;
                        drag_force = (float)atof(v);
                    }
                    fld = strstr(bobj, "\"hitRadius\"");
                    if (fld && fld < bobj_end) {
                        const char *v = fld + 11; while (*v && (*v==' '||*v==':'||*v=='\t')) v++;
                        hit_radius = (float)atof(v);
                    }
                    fld = strstr(bobj, "\"gravityDir\"");
                    if (fld && fld < bobj_end) {
                        const char *gx = strstr(fld, "\"x\"");
                        const char *gy = strstr(fld, "\"y\"");
                        const char *gz = strstr(fld, "\"z\"");
                        if (gx && gx < bobj_end) { const char *v=gx+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; gravity_dir[0]=(float)atof(v); }
                        if (gy && gy < bobj_end) { const char *v=gy+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; gravity_dir[1]=(float)atof(v); }
                        if (gz && gz < bobj_end) { const char *v=gz+3; while(*v&&(*v==' '||*v==':'||*v=='\t'))v++; gravity_dir[2]=(float)atof(v); }
                    }

                    /* "center": node index or -1 */
                    fld = strstr(bobj, "\"center\"");
                    if (fld && fld < bobj_end) {
                        const char *v = fld + 8; while (*v && (*v==' '||*v==':'||*v=='\t')) v++;
                        int center_node = atoi(v);
                        if (center_node >= 0)
                            sgrp->center_bone = __find_bone_by_node_index(model, json_str, center_node);
                    }

                    /* "colliderGroups": [indices...] */
                    fld = strstr(bobj, "\"colliderGroups\"");
                    if (fld && fld < bobj_end) {
                        const char *cga = strchr(fld + 16, '[');
                        if (cga && cga < bobj_end) {
                            int max_cgi = 32;
                            sgrp->collider_group_indices = (int *)calloc(max_cgi, sizeof(int));
                            const char *cgp = cga + 1;
                            while (*cgp && *cgp != ']' && (int)sgrp->collider_group_count < max_cgi) {
                                while (*cgp && (*cgp == ' ' || *cgp == ',' || *cgp == '\t' || *cgp == '\n')) cgp++;
                                if (*cgp == ']') break;
                                sgrp->collider_group_indices[sgrp->collider_group_count++] = atoi(cgp);
                                while (*cgp && *cgp != ',' && *cgp != ']') cgp++;
                            }
                        }
                    }

                    /* "bones": [node_indices...] — each is a root of a chain */
                    const char *bones_key = strstr(bobj, "\"bones\"");
                    if (bones_key && bones_key < bobj_end) {
                        const char *barr = strchr(bones_key + 7, '[');
                        if (barr && barr < bobj_end) {
                            /* Collect root node indices */
                            int root_nodes[256];
                            int root_count = 0;
                            const char *bnp = barr + 1;
                            while (*bnp && *bnp != ']' && root_count < 256) {
                                while (*bnp && (*bnp==' '||*bnp==','||*bnp=='\t'||*bnp=='\n')) bnp++;
                                if (*bnp == ']') break;
                                root_nodes[root_count++] = atoi(bnp);
                                while (*bnp && *bnp != ',' && *bnp != ']') bnp++;
                            }

                            /* Build chains from each root and collect all joints */
                            int total_joints = 0;
                            int chain_buf[256 * 64]; /* temp storage (large for branching trees) */
                            int chain_lengths[256];

                            for (int r = 0; r < root_count; r++) {
                                int root_bone = __find_bone_by_node_index(model, json_str, root_nodes[r]);
                                int max_remaining = (256 * 64) - total_joints;
                                if (max_remaining < 1) max_remaining = 1;
                                chain_lengths[r] = __build_bone_chain(model, root_bone,
                                    &chain_buf[total_joints], max_remaining);
                                total_joints += chain_lengths[r];
                            }

                            if (total_joints > 0) {
                                sgrp->joints = (vrm_spring_joint_t *)calloc(total_joints, sizeof(vrm_spring_joint_t));
                                sgrp->joint_count = total_joints;

                                for (int j = 0; j < total_joints; j++) {
                                    vrm_spring_joint_t *jnt = &sgrp->joints[j];
                                    jnt->bone_index = chain_buf[j];
                                    jnt->stiffness = stiffness;
                                    jnt->gravity_power = gravity_power;
                                    jnt->gravity_dir[0] = gravity_dir[0];
                                    jnt->gravity_dir[1] = gravity_dir[1];
                                    jnt->gravity_dir[2] = gravity_dir[2];
                                    jnt->drag_force = drag_force;
                                    jnt->hit_radius = hit_radius;
                                }
                            }
                        }
                    }

                    if (sgrp->joint_count > 0) {
                        model->spring_group_count++;
                    } else {
                        free(sgrp->collider_group_indices);
                        sgrp->collider_group_indices = NULL;
                        sgrp->collider_group_count = 0;
                    }

                    bp = bobj_end + 1;
                }
            }
        }
    }

    /* ---- Try VRM 1.0: VRMC_springBone extension ---- */
    if (!model->spring_group_count) {
        const char *vrmc_sb = strstr(json_str, "\"VRMC_springBone\"");
        if (vrmc_sb) {
            /* ---- Parse colliders[] (flat array) ---- */
            /* Each collider: {"node": N, "shape": {"sphere": {offset,radius}} or {"capsule": {offset,radius,tail}}} */
            /* We map each collider to a single-collider collider_group so the existing
             * spring bone solver can reference them by index. */

            typedef struct {
                int node_idx;        /* glTF node index */
                float offset[3];
                float radius;
                int is_capsule;
                float tail[3];       /* capsule tail offset (unused by current solver, stored for future) */
            } vrmc_collider_t;

            vrmc_collider_t parsed_colliders[256];
            int parsed_collider_count = 0;

            const char *col_key = strstr(vrmc_sb, "\"colliders\"");
            if (col_key) {
                const char *col_arr = strchr(col_key + 11, '[');
                if (col_arr) {
                    const char *cp = col_arr + 1;
                    while (*cp && *cp != ']' && parsed_collider_count < 256) {
                        const char *cobj = strchr(cp, '{');
                        if (!cobj) break;
                        int depth = 1;
                        const char *cq = cobj + 1;
                        const char *cobj_end = NULL;
                        while (*cq && depth > 0) {
                            if (*cq == '{') depth++;
                            else if (*cq == '}') { depth--; if (depth == 0) { cobj_end = cq; break; } }
                            cq++;
                        }
                        if (!cobj_end) break;

                        vrmc_collider_t *pc = &parsed_colliders[parsed_collider_count];
                        memset(pc, 0, sizeof(*pc));
                        pc->node_idx = -1;

                        /* "node": N */
                        const char *nk = strstr(cobj, "\"node\"");
                        if (nk && nk < cobj_end) {
                            const char *v = nk + 6;
                            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                            pc->node_idx = atoi(v);
                        }

                        /* "shape": { "sphere": {...} } or { "capsule": {...} } */
                        const char *sphere_key = strstr(cobj, "\"sphere\"");
                        const char *capsule_key = strstr(cobj, "\"capsule\"");

                        const char *shape_obj = NULL;
                        const char *shape_end = NULL;

                        if (sphere_key && sphere_key < cobj_end) {
                            shape_obj = strchr(sphere_key + 8, '{');
                            if (shape_obj && shape_obj < cobj_end) {
                                shape_end = strchr(shape_obj + 1, '}');
                                pc->is_capsule = 0;
                            }
                        } else if (capsule_key && capsule_key < cobj_end) {
                            shape_obj = strchr(capsule_key + 9, '{');
                            if (shape_obj && shape_obj < cobj_end) {
                                /* Find matching brace */
                                int d2 = 1;
                                const char *se = shape_obj + 1;
                                while (*se && d2 > 0) {
                                    if (*se == '{') d2++;
                                    else if (*se == '}') { d2--; if (d2 == 0) { shape_end = se; break; } }
                                    se++;
                                }
                                pc->is_capsule = 1;
                            }
                        }

                        if (shape_obj && shape_end) {
                            /* Parse "offset": [x,y,z] */
                            const char *off = strstr(shape_obj, "\"offset\"");
                            if (off && off < shape_end) {
                                const char *oa = strchr(off + 8, '[');
                                if (oa && oa < shape_end) {
                                    oa++;
                                    pc->offset[0] = (float)atof(oa);
                                    const char *c1 = strchr(oa, ',');
                                    if (c1) { pc->offset[1] = (float)atof(c1+1);
                                    const char *c2 = strchr(c1+1, ',');
                                    if (c2) { pc->offset[2] = (float)atof(c2+1); }}
                                }
                            }
                            /* Parse "radius": R */
                            const char *rk = strstr(shape_obj, "\"radius\"");
                            if (rk && rk < shape_end) {
                                const char *v = rk + 8;
                                while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                pc->radius = (float)atof(v);
                            }
                            /* Parse "tail": [x,y,z] for capsule */
                            if (pc->is_capsule) {
                                const char *tk = strstr(shape_obj, "\"tail\"");
                                if (tk && tk < shape_end) {
                                    const char *ta = strchr(tk + 6, '[');
                                    if (ta && ta < shape_end) {
                                        ta++;
                                        pc->tail[0] = (float)atof(ta);
                                        const char *c1 = strchr(ta, ',');
                                        if (c1) { pc->tail[1] = (float)atof(c1+1);
                                        const char *c2 = strchr(c1+1, ',');
                                        if (c2) { pc->tail[2] = (float)atof(c2+1); }}
                                    }
                                }
                            }
                        }

                        parsed_collider_count++;
                        cp = cobj_end + 1;
                    }
                }
            }

            /* ---- Parse colliderGroups[] ---- */
            /* VRM 1.0: {"name":"...", "colliders":[0,1,...]} — indices into the flat colliders array */
            typedef struct {
                char name[64];
                int collider_indices[32];
                int collider_count;
            } vrmc_collider_group_t;

            vrmc_collider_group_t parsed_cgroups[64];
            int parsed_cgroup_count = 0;

            const char *cg_key = strstr(vrmc_sb, "\"colliderGroups\"");
            if (cg_key) {
                const char *cg_arr = strchr(cg_key + 16, '[');
                if (cg_arr) {
                    const char *cgp = cg_arr + 1;
                    while (*cgp && *cgp != ']' && parsed_cgroup_count < 64) {
                        const char *cgobj = strchr(cgp, '{');
                        if (!cgobj) break;
                        int depth = 1;
                        const char *cq = cgobj + 1;
                        const char *cgobj_end = NULL;
                        while (*cq && depth > 0) {
                            if (*cq == '{') depth++;
                            else if (*cq == '}') { depth--; if (depth == 0) { cgobj_end = cq; break; } }
                            cq++;
                        }
                        if (!cgobj_end) break;

                        vrmc_collider_group_t *pcg = &parsed_cgroups[parsed_cgroup_count];
                        memset(pcg, 0, sizeof(*pcg));

                        /* "colliders": [0, 1, ...] */
                        const char *ci_key = strstr(cgobj, "\"colliders\"");
                        if (ci_key && ci_key < cgobj_end) {
                            const char *cia = strchr(ci_key + 11, '[');
                            if (cia && cia < cgobj_end) {
                                const char *cip = cia + 1;
                                while (*cip && *cip != ']' && pcg->collider_count < 32) {
                                    while (*cip && (*cip == ' ' || *cip == ',' || *cip == '\t' || *cip == '\n' || *cip == '\r')) cip++;
                                    if (*cip == ']') break;
                                    pcg->collider_indices[pcg->collider_count++] = atoi(cip);
                                    while (*cip && *cip != ',' && *cip != ']') cip++;
                                }
                            }
                        }

                        parsed_cgroup_count++;
                        cgp = cgobj_end + 1;
                    }
                }
            }

            /* ---- Build runtime collider_groups from parsed data ---- */
            /* Map each VRM 1.0 colliderGroup to a vrm_collider_group_t.
             * A group may reference multiple colliders on different nodes,
             * but our runtime struct has one bone_index per group.
             * We group by the first collider's node. */
            if (parsed_cgroup_count > 0) {
                model->collider_groups = (vrm_collider_group_t *)calloc(
                    parsed_cgroup_count, sizeof(vrm_collider_group_t));

                for (int gi = 0; gi < parsed_cgroup_count; gi++) {
                    vrmc_collider_group_t *pcg = &parsed_cgroups[gi];
                    vrm_collider_group_t *grp = &model->collider_groups[model->collider_group_count];

                    if (pcg->collider_count <= 0) continue;

                    /* Use the first collider's node as the group's bone */
                    int first_ci = pcg->collider_indices[0];
                    if (first_ci >= 0 && first_ci < parsed_collider_count) {
                        grp->bone_index = __find_bone_by_node_index(
                            model, json_str, parsed_colliders[first_ci].node_idx);
                    } else {
                        grp->bone_index = -1;
                    }

                    /* Allocate colliders for this group */
                    grp->colliders = (vrm_spring_collider_t *)calloc(
                        pcg->collider_count, sizeof(vrm_spring_collider_t));

                    for (int ci = 0; ci < pcg->collider_count; ci++) {
                        int idx = pcg->collider_indices[ci];
                        if (idx < 0 || idx >= parsed_collider_count) continue;
                        vrmc_collider_t *pc = &parsed_colliders[idx];
                        vrm_spring_collider_t *col = &grp->colliders[grp->collider_count];
                        col->offset[0] = pc->offset[0];
                        col->offset[1] = pc->offset[1];
                        col->offset[2] = pc->offset[2];
                        col->radius = pc->radius;
                        grp->collider_count++;
                    }

                    model->collider_group_count++;
                }
            }

            /* ---- Parse springs[] ---- */
            /* VRM 1.0: each spring has its own joints[] with per-joint parameters
             * and optional colliderGroups reference */
            const char *springs_key = strstr(vrmc_sb, "\"springs\"");
            if (springs_key) {
                const char *sp_arr = strchr(springs_key + 9, '[');
                if (sp_arr) {
                    int max_groups = VRM_MAX_SPRING_GROUPS;
                    model->spring_groups = (vrm_spring_group_t *)calloc(
                        max_groups, sizeof(vrm_spring_group_t));

                    const char *sp = sp_arr + 1;
                    while (*sp && *sp != ']' && (int)model->spring_group_count < max_groups) {
                        const char *sobj = strchr(sp, '{');
                        if (!sobj) break;
                        int depth = 1;
                        const char *sq = sobj + 1;
                        const char *sobj_end = NULL;
                        while (*sq && depth > 0) {
                            if (*sq == '{') depth++;
                            else if (*sq == '}') { depth--; if (depth == 0) { sobj_end = sq; break; } }
                            sq++;
                        }
                        if (!sobj_end) break;

                        vrm_spring_group_t *sgrp = &model->spring_groups[model->spring_group_count];
                        memset(sgrp, 0, sizeof(*sgrp));
                        sgrp->center_bone = -1;

                        /* "center": node_index (optional) */
                        const char *center_key = strstr(sobj, "\"center\"");
                        if (center_key && center_key < sobj_end) {
                            const char *v = center_key + 8;
                            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                            if (*v >= '0' && *v <= '9') {
                                int center_node = atoi(v);
                                sgrp->center_bone = __find_bone_by_node_index(
                                    model, json_str, center_node);
                            }
                        }

                        /* "colliderGroups": [indices into parsed_cgroups] */
                        const char *scg_key = strstr(sobj, "\"colliderGroups\"");
                        if (scg_key && scg_key < sobj_end) {
                            const char *scg_arr = strchr(scg_key + 16, '[');
                            if (scg_arr && scg_arr < sobj_end) {
                                int max_cgi = 32;
                                sgrp->collider_group_indices = (int *)calloc(max_cgi, sizeof(int));
                                const char *scgp = scg_arr + 1;
                                while (*scgp && *scgp != ']' && (int)sgrp->collider_group_count < max_cgi) {
                                    while (*scgp && (*scgp == ' ' || *scgp == ',' || *scgp == '\t' || *scgp == '\n' || *scgp == '\r')) scgp++;
                                    if (*scgp == ']') break;
                                    sgrp->collider_group_indices[sgrp->collider_group_count++] = atoi(scgp);
                                    while (*scgp && *scgp != ',' && *scgp != ']') scgp++;
                                }
                            }
                        }

                        /* "joints": [{node, stiffness, dragForce, hitRadius, gravityPower, gravityDir}, ...] */
                        const char *joints_key = strstr(sobj, "\"joints\"");
                        if (joints_key && joints_key < sobj_end) {
                            const char *jt_arr = strchr(joints_key + 8, '[');
                            if (jt_arr && jt_arr < sobj_end) {
                                /* Count joints first */
                                int joint_cap = 128;
                                vrm_spring_joint_t *joints = (vrm_spring_joint_t *)calloc(
                                    joint_cap, sizeof(vrm_spring_joint_t));
                                int jcount = 0;

                                const char *jp = jt_arr + 1;
                                while (*jp && *jp != ']' && jcount < joint_cap) {
                                    const char *jobj = strchr(jp, '{');
                                    if (!jobj || jobj > sobj_end) break;
                                    const char *jobj_end = strchr(jobj + 1, '}');
                                    if (!jobj_end || jobj_end > sobj_end) break;

                                    vrm_spring_joint_t *jnt = &joints[jcount];
                                    /* defaults */
                                    jnt->stiffness = 1.0f;
                                    jnt->drag_force = 0.5f;
                                    jnt->gravity_power = 0.0f;
                                    jnt->gravity_dir[0] = 0.0f;
                                    jnt->gravity_dir[1] = -1.0f;
                                    jnt->gravity_dir[2] = 0.0f;
                                    jnt->hit_radius = 0.0f;
                                    jnt->bone_index = -1;

                                    const char *fld;
                                    fld = strstr(jobj, "\"node\"");
                                    if (fld && fld < jobj_end) {
                                        const char *v = fld + 6;
                                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                        int ni = atoi(v);
                                        jnt->bone_index = __find_bone_by_node_index(
                                            model, json_str, ni);
                                    }
                                    fld = strstr(jobj, "\"stiffness\"");
                                    if (fld && fld < jobj_end) {
                                        const char *v = fld + 11;
                                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                        jnt->stiffness = (float)atof(v);
                                    }
                                    fld = strstr(jobj, "\"dragForce\"");
                                    if (fld && fld < jobj_end) {
                                        const char *v = fld + 10;
                                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                        jnt->drag_force = (float)atof(v);
                                    }
                                    fld = strstr(jobj, "\"hitRadius\"");
                                    if (fld && fld < jobj_end) {
                                        const char *v = fld + 11;
                                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                        jnt->hit_radius = (float)atof(v);
                                    }
                                    fld = strstr(jobj, "\"gravityPower\"");
                                    if (fld && fld < jobj_end) {
                                        const char *v = fld + 14;
                                        while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
                                        jnt->gravity_power = (float)atof(v);
                                    }
                                    fld = strstr(jobj, "\"gravityDir\"");
                                    if (fld && fld < jobj_end) {
                                        /* Parse [x,y,z] */
                                        const char *ga = strchr(fld + 12, '[');
                                        if (ga && ga < jobj_end) {
                                            ga++;
                                            jnt->gravity_dir[0] = (float)atof(ga);
                                            const char *c1 = strchr(ga, ',');
                                            if (c1) { jnt->gravity_dir[1] = (float)atof(c1+1);
                                            const char *c2 = strchr(c1+1, ',');
                                            if (c2) { jnt->gravity_dir[2] = (float)atof(c2+1); }}
                                        }
                                    }

                                    jcount++;
                                    jp = jobj_end + 1;
                                }

                                if (jcount > 0) {
                                    sgrp->joints = (vrm_spring_joint_t *)realloc(
                                        joints, jcount * sizeof(vrm_spring_joint_t));
                                    sgrp->joint_count = jcount;
                                } else {
                                    free(joints);
                                }
                            }
                        }

                        if (sgrp->joint_count > 0) {
                            model->spring_group_count++;
                        } else {
                            free(sgrp->collider_group_indices);
                            sgrp->collider_group_indices = NULL;
                            sgrp->collider_group_count = 0;
                        }

                        sp = sobj_end + 1;
                    }
                }
            }

            printf("[vrm_loader] VRM 1.0 spring bones parsed: %d colliders, %d collider groups\n",
                   parsed_collider_count, parsed_cgroup_count);
        }
    }

    free(json_str);

    /* Log summary */
    uint32_t total_joints = 0;
    for (uint32_t i = 0; i < model->spring_group_count; i++)
        total_joints += model->spring_groups[i].joint_count;

    if (model->spring_group_count > 0) {
        printf("[vrm_loader] spring bones: %u groups, %u joints, %u collider groups\n",
               model->spring_group_count, total_joints, model->collider_group_count);
        for (uint32_t i = 0; i < model->spring_group_count; i++) {
            vrm_spring_group_t *sg = &model->spring_groups[i];
            const char *first_name = (sg->joint_count > 0 && sg->joints[0].bone_index >= 0)
                ? model->bones[sg->joints[0].bone_index].name : "?";
            printf("[vrm_loader]   group[%u]: %u joints (first: \"%s\") stiff=%.2f drag=%.2f grav=%.2f\n",
                   i, sg->joint_count, first_name,
                   sg->joints ? sg->joints[0].stiffness : 0,
                   sg->joints ? sg->joints[0].drag_force : 0,
                   sg->joints ? sg->joints[0].gravity_power : 0);
        }
    }
}

void vrm_auto_blink(vrm_model_t *model, float time_sec, float interval)
{
    if (!model) return;

    /* Try common blink expression names */
    static const char *blink_names[] = { "blink", "Blink", "BLINK", "blinkLeft", NULL };
    int blink_idx = -1;
    for (int i = 0; blink_names[i]; i++) {
        blink_idx = vrm_find_expression(model, blink_names[i]);
        if (blink_idx >= 0) break;
    }
    if (blink_idx < 0) return;

    /* Blink pattern: quick close/open over ~0.15s */
    float phase = fmodf(time_sec, interval);
    float blink_dur = 0.15f;
    float w = 0.0f;
    if (phase < blink_dur) {
        /* Triangle wave: 0 -> 1 -> 0 over blink_dur */
        float t = phase / blink_dur;
        w = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
    }
    vrm_set_expression_weight(model, blink_idx, w);
}

/* ================================================================== */
/*  Public: vrm_model_free                                             */
/* ================================================================== */

void vrm_model_free(vrm_model_t *model)
{
    if (!model) return;

    if (model->meshes) {
        for (uint32_t i = 0; i < model->mesh_count; i++) {
            free(model->meshes[i].vertices);
            free(model->meshes[i].base_vertices);
            free(model->meshes[i].indices);
            if (model->meshes[i].morph_targets) {
                for (uint32_t mt = 0; mt < model->meshes[i].morph_target_count; mt++) {
                    free(model->meshes[i].morph_targets[mt].delta_positions);
                    free(model->meshes[i].morph_targets[mt].delta_normals);
                }
                free(model->meshes[i].morph_targets);
            }
        }
        free(model->meshes);
    }
    if (model->textures) {
        for (uint32_t i = 0; i < model->texture_count; i++)
            if (model->textures[i].pixels)
                stbi_image_free(model->textures[i].pixels);
        free(model->textures);
    }
    if (model->bones) free(model->bones);
    if (model->animations) {
        for (uint32_t ai = 0; ai < model->animation_count; ai++) {
            vrm_animation_t *va = &model->animations[ai];
            for (uint32_t bi = 0; bi < va->bone_anim_count; bi++) {
                vrm_bone_anim_t *ba = &va->bone_anims[bi];
                for (uint32_t ci = 0; ci < ba->channel_count; ci++) {
                    free(ba->channels[ci].times);
                    free(ba->channels[ci].values);
                }
                free(ba->channels);
            }
            free(va->bone_anims);
            if (va->expr_anims) {
                for (uint32_t ei = 0; ei < va->expr_anim_count; ei++) {
                    free(va->expr_anims[ei].channel.times);
                    free(va->expr_anims[ei].channel.values);
                }
                free(va->expr_anims);
            }
        }
        free(model->animations);
    }
    if (model->humanoid_map) free(model->humanoid_map);
    if (model->expressions) {
        for (uint32_t i = 0; i < model->expression_count; i++)
            free(model->expressions[i].binds);
        free(model->expressions);
    }

    /* Free spring bone data */
    if (model->spring_groups) {
        for (uint32_t i = 0; i < model->spring_group_count; i++) {
            free(model->spring_groups[i].joints);
            free(model->spring_groups[i].collider_group_indices);
        }
        free(model->spring_groups);
    }
    if (model->collider_groups) {
        for (uint32_t i = 0; i < model->collider_group_count; i++)
            free(model->collider_groups[i].colliders);
        free(model->collider_groups);
    }

    free(model->constraints);

    memset(model, 0, sizeof(*model));
}
