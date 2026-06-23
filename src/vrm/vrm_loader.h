/**
 * @file vrm_loader.h
 * @brief Load VRM (glTF 2.0 binary) models via Assimp with skeletal animation
 *        and BlendShape / morph target support.
 */

#ifndef VRM_LOADER_H
#define VRM_LOADER_H

#include <stdint.h>

#define VRM_MAX_BONES_PER_VERTEX 4
#define VRM_MAX_BONES            512
#define VRM_MAX_MORPH_TARGETS    128
#define VRM_MAX_EXPRESSIONS      64

/** Decoded texture (RGBA pixels). */
typedef struct {
    uint8_t  *pixels;    /**< RGBA pixel data */
    int       width;
    int       height;
    int       channels;  /**< Always 4 (RGBA) after decode */
} vrm_texture_t;

/** A single morph target (blend shape) for a mesh. */
typedef struct {
    char     name[128];          /**< Morph target name (from Assimp or glTF) */
    float   *delta_positions;    /**< Per-vertex position deltas (3 floats each) */
    float   *delta_normals;      /**< Per-vertex normal deltas  (3 floats each), may be NULL */
    uint32_t vertex_count;       /**< Must match parent mesh vertex_count */
} vrm_morph_target_t;

/** Per-mesh data extracted from the model. */
typedef struct {
    float    *vertices;      /**< Interleaved: pos(3)+normal(3)+uv(2)+boneId(4)+boneWt(4) = 16 floats */
    float    *base_vertices; /**< Copy of original vertices for morph target blending */
    uint32_t *indices;       /**< Triangle indices */
    uint32_t  vertex_count;
    uint32_t  index_count;
    float     color[4];      /**< Material base color (RGBA) */
    int       texture_index; /**< Index into vrm_model_t::textures, or -1 */
    int       has_bones;     /**< 1 if this mesh has skinning data */

    /* Morph targets */
    vrm_morph_target_t *morph_targets;
    uint32_t            morph_target_count;
    int                 assimp_mesh_index; /**< Index of this mesh in aiScene::mMeshes */
} vrm_mesh_t;

/** A bone in the skeleton hierarchy. */
typedef struct {
    char     name[128];
    int      parent;                /**< Index into vrm_model_t::bones, or -1 for root */
    float    offset_matrix[16];     /**< Inverse bind matrix (col-major 4x4) */
    float    local_transform[16];   /**< Node's local transform (rest pose, col-major 4x4) */
} vrm_bone_t;

/** Keyframe channel for a single bone property. */
typedef struct {
    float   *times;       /**< Timestamps array */
    float   *values;      /**< Values (3 floats for pos/scale, 4 for rotation quat, 1 for weight) */
    uint32_t count;       /**< Number of keyframes */
    int      path;        /**< 0=translation, 1=rotation(quat), 2=scale, 3=weights(scalar) */
} vrm_anim_channel_t;

/** Animation data targeting a specific bone. */
typedef struct {
    int                  bone_index;    /**< Index into vrm_model_t::bones */
    vrm_anim_channel_t  *channels;
    uint32_t             channel_count;
} vrm_bone_anim_t;

/** Animation channel for expression weights. */
typedef struct {
    int                  expression_index; /**< Index into vrm_model_t::expressions */
    vrm_anim_channel_t  channel;           /**< path=3 (weights), scalar keyframes */
} vrm_expr_anim_t;

/** A complete animation clip. */
typedef struct {
    char              name[128];
    float             duration;        /**< Total duration in seconds */
    vrm_bone_anim_t  *bone_anims;
    uint32_t          bone_anim_count;

    /* Expression animation channels */
    vrm_expr_anim_t  *expr_anims;
    uint32_t          expr_anim_count;
} vrm_animation_t;

/** VRM humanoid bone name mapping entry. */
typedef struct {
    char     humanoid_name[64];   /**< e.g. "hips", "leftUpperArm" */
    char     node_name[128];      /**< Actual node name in the model */
} vrm_humanoid_map_t;

/** A binding from an expression to a specific morph target on a mesh. */
typedef struct {
    uint32_t mesh_index;          /**< Index into vrm_model_t::meshes */
    uint32_t morph_index;         /**< Index into that mesh's morph_targets */
    float    weight;              /**< Multiplier (typically 1.0) */
} vrm_expression_bind_t;

/** VRM expression (BlendShape group). */
typedef struct {
    char                   name[64];    /**< e.g. "happy", "blink", "aa" */
    int                    is_preset;   /**< 1 if this is a VRM preset expression */
    vrm_expression_bind_t *binds;
    uint32_t               bind_count;
} vrm_expression_t;

/* ================================================================== */
/*  Spring Bone (secondary animation)                                  */
/* ================================================================== */

#define VRM_MAX_SPRING_JOINTS  1024
#define VRM_MAX_SPRING_GROUPS  64
#define VRM_MAX_COLLIDERS      64

/** A single joint in a spring bone chain. */
typedef struct {
    int      bone_index;           /**< Index into vrm_model_t::bones */
    float    stiffness;            /**< Spring stiffness (higher = stiffer) */
    float    gravity_power;        /**< Gravity force magnitude */
    float    gravity_dir[3];       /**< Gravity direction (world-space) */
    float    drag_force;           /**< Damping / drag [0..1] */
    float    hit_radius;           /**< Sphere radius for collider interaction */
} vrm_spring_joint_t;

/** A spring bone group (chain of joints with shared parameters). */
typedef struct {
    vrm_spring_joint_t *joints;
    uint32_t            joint_count;
    int                 center_bone;   /**< Center bone index, or -1 */
    int                *collider_group_indices; /**< Indices into collider_groups */
    uint32_t            collider_group_count;
} vrm_spring_group_t;

/** A sphere collider attached to a bone. */
typedef struct {
    float    offset[3];            /**< Local offset from bone */
    float    radius;               /**< Sphere radius */
} vrm_spring_collider_t;

/** A group of colliders on the same bone. */
typedef struct {
    int                    bone_index;  /**< Index into vrm_model_t::bones */
    vrm_spring_collider_t *colliders;
    uint32_t               collider_count;
} vrm_collider_group_t;

/* ================================================================== */
/*  VRMC_node_constraint (Aim / Roll)                                  */
/* ================================================================== */

#define VRM_MAX_CONSTRAINTS 64

/** Aim axis enumeration. */
enum vrm_aim_axis {
    VRM_AIM_POSITIVE_X = 0,
    VRM_AIM_NEGATIVE_X,
    VRM_AIM_POSITIVE_Y,
    VRM_AIM_NEGATIVE_Y,
    VRM_AIM_POSITIVE_Z,
    VRM_AIM_NEGATIVE_Z,
};

/** Roll axis enumeration. */
enum vrm_roll_axis {
    VRM_ROLL_X = 0,
    VRM_ROLL_Y,
    VRM_ROLL_Z,
};

/** A single node constraint (Aim or Roll). */
typedef struct {
    int   bone_index;     /**< Constrained bone index */
    int   source_index;   /**< Source bone index to track */
    int   type;           /**< 0 = aim, 1 = roll */
    int   axis;           /**< enum vrm_aim_axis or vrm_roll_axis */
    float weight;         /**< Constraint influence [0..1] */
} vrm_node_constraint_t;

/** A complete model with bounding-box metadata and skeleton. */
typedef struct {
    vrm_mesh_t       *meshes;
    uint32_t          mesh_count;
    vrm_texture_t    *textures;
    uint32_t          texture_count;
    float             bbox_min[3];
    float             bbox_max[3];
    float             center[3];   /**< AABB center */
    float             extent;      /**< Max dimension of AABB */

    /* Skeleton */
    vrm_bone_t       *bones;
    uint32_t          bone_count;

    /* Animations */
    vrm_animation_t  *animations;
    uint32_t          animation_count;

    /* VRM humanoid bone mapping */
    vrm_humanoid_map_t *humanoid_map;
    uint32_t            humanoid_map_count;

    /* VRM expressions (BlendShape) */
    vrm_expression_t  *expressions;
    uint32_t           expression_count;
    float              expression_weights[VRM_MAX_EXPRESSIONS]; /**< Current weights [0..1] */

    /* Spring bones (secondary animation / physics) */
    vrm_spring_group_t   *spring_groups;
    uint32_t              spring_group_count;
    vrm_collider_group_t *collider_groups;
    uint32_t              collider_group_count;

    /* Node constraints (VRMC_node_constraint: Aim / Roll) */
    vrm_node_constraint_t *constraints;
    uint32_t               constraint_count;
} vrm_model_t;

/**
 * Load a VRM / glTF-binary model from disk with skeleton and skinning data.
 * @return 0 on success, -1 on failure.
 */
int  vrm_model_load(vrm_model_t *model, const char *path);

/** Free all heap memory held by a loaded model. */
void vrm_model_free(vrm_model_t *model);

/**
 * Load a VRMA animation file and retarget it onto the model's skeleton
 * using the VRM humanoid bone naming convention.
 * The animation is appended to model->animations.
 * @return 0 on success, -1 on failure.
 */
int  vrm_load_vrma(vrm_model_t *model, const char *vrma_path);

/**
 * Evaluate animation at time t and compute final bone matrices.
 * Also updates expression_weights if the animation contains expression channels.
 * @param model        The loaded model.
 * @param anim_index   Which animation to evaluate.
 * @param time_sec     Current time in seconds (will be looped).
 * @param out_matrices Output array of bone_count 4x4 col-major matrices.
 *                     Each matrix = globalTransform * inverseBindMatrix.
 */
void vrm_evaluate_animation(const vrm_model_t *model, uint32_t anim_index,
                             float time_sec, float *out_matrices);

/**
 * Compute rest-pose bone matrices (identity skinning — no deformation).
 */
void vrm_rest_pose_matrices(const vrm_model_t *model, float *out_matrices);

/**
 * Find an expression by name.
 * @return index into model->expressions, or -1 if not found.
 */
int  vrm_find_expression(const vrm_model_t *model, const char *name);

/**
 * Set an expression weight by index.
 * @param weight  Value in [0, 1].
 */
void vrm_set_expression_weight(vrm_model_t *model, int expr_index, float weight);

/**
 * Apply current expression weights to mesh vertices (CPU blending).
 * Call this each frame after updating expression weights.
 * Modifies model->meshes[*].vertices in-place from base_vertices + morph deltas.
 */
void vrm_apply_morph_targets(vrm_model_t *model);

/**
 * Evaluate procedural eye-blink at given time. Updates "blink" expression weight.
 * @param time_sec Current time in seconds.
 * @param interval Seconds between blinks (e.g. 4.0).
 */
void vrm_auto_blink(vrm_model_t *model, float time_sec, float interval);

#endif /* VRM_LOADER_H */
