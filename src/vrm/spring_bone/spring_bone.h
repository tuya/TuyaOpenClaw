/**
 * @file spring_bone.h
 * @brief VRM Spring Bone physics simulation — Verlet integration for
 *        secondary animation (hair, clothing, accessories).
 *
 * Usage:
 *   1. spring_bone_init(&ctx, &model)        — after model is loaded
 *   2. spring_bone_update(&ctx, dt, bone_matrices)  — each frame after animation
 *   3. spring_bone_shutdown(&ctx)            — cleanup
 *
 * The update modifies bone local transforms and recomputes the affected
 * global transforms and final bone matrices in-place.
 */

#ifndef SPRING_BONE_H
#define SPRING_BONE_H

#include "vrm_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-joint runtime state for Verlet integration.
 *  Algorithm references UnityChan SpringBone implementation. */
typedef struct {
    int     bone_index;              /**< Index into vrm_model_t::bones */
    float   current_tail[3];         /**< Current tail position (world-space) */
    float   prev_tail[3];            /**< Previous tail position (Verlet) */
    float   spring_length;           /**< Distance from this bone head to child (rest pose) */

    /* Rest-pose cache (computed at initialization) */
    float   initial_local_rot[4];    /**< Initial local rotation quaternion (x,y,z,w) */
    float   bone_axis[3];            /**< Unit vector from bone head to child in local space */

    /* VRM spring parameters */
    float   stiffness;
    float   gravity_power;
    float   gravity_dir[3];
    float   drag_force;
    float   hit_radius;
} spring_joint_state_t;

/** A chain of spring joints. */
typedef struct {
    spring_joint_state_t *joints;
    int                   joint_count;
    int                   center_bone;
    int                  *collider_group_indices;
    int                   collider_group_count;
} spring_chain_t;

/** Spring bone simulation context. */
typedef struct {
    vrm_model_t   *model;            /**< Pointer to VRM model (not owned) */
    spring_chain_t *chains;
    int             chain_count;
    int             initialized;     /**< 1 after first frame init */
    int             enabled;         /**< 1 = simulation active */
} spring_bone_ctx_t;

/**
 * Initialize the spring bone context from model data.
 * Must be called after model is fully loaded (bones + spring groups parsed).
 */
void spring_bone_init(spring_bone_ctx_t *ctx, vrm_model_t *model);

/**
 * Update spring bone simulation.
 * Call each frame after animation evaluation but before uploading bone matrices to GPU.
 *
 * This function:
 * 1. Runs Verlet integration on each spring joint
 * 2. Applies gravity, stiffness, and drag forces
 * 3. Resolves collider constraints
 * 4. Updates the bone matrices in-place
 *
 * @param ctx            Spring bone context.
 * @param dt             Delta time in seconds.
 * @param bone_matrices  The bone matrices array (bone_count * 16 floats).
 *                       Will be modified for spring bones.
 */
void spring_bone_update(spring_bone_ctx_t *ctx, float dt, float *bone_matrices);

/**
 * Reset all spring bones to rest pose.
 */
void spring_bone_reset(spring_bone_ctx_t *ctx, const float *bone_matrices);

/**
 * Free all spring bone runtime state.
 */
void spring_bone_shutdown(spring_bone_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPRING_BONE_H */
