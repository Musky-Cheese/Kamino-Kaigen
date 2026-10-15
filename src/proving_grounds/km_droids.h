// Kamino droids: walking B1/B2 rigs over the drone AI; rigid limbs posed per frame on their submesh entities.
#pragma once
#include "animation/animation.h"
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/random.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"

typedef enum KmLimbId {
  KM_LIMB_NONE = 0,
  KM_LIMB_PELVIS,
  KM_LIMB_CHEST,
  KM_LIMB_NECK,
  KM_LIMB_HEAD,
  KM_LIMB_LEG_UPPER_L,
  KM_LIMB_LEG_LOWER_L,
  KM_LIMB_FOOT_L,
  KM_LIMB_LEG_UPPER_R,
  KM_LIMB_LEG_LOWER_R,
  KM_LIMB_FOOT_R,
  KM_LIMB_ARM_UPPER_L,
  KM_LIMB_ARM_LOWER_L,
  KM_LIMB_HAND_L,
  KM_LIMB_ARM_UPPER_R,
  KM_LIMB_ARM_LOWER_R,
  KM_LIMB_HAND_R,
  KM_LIMB_GUN,
  KM_LIMB_COUNT,
} KmLimbId;

// a limb node's glTF extras: its debris radius and the child joint, model space
HZ_REFLECT()
typedef struct KmPartExtras {
  f32 radius;
  f32 tip_x;
  f32 tip_y;
  f32 tip_z;
} KmPartExtras;

// one droid type's joints, model space: each limb's pivot (its node) and the child joint
#define KM_RIG_SLOTS 64 // submesh slots a rigged model may have
typedef struct KmRig {
  v3 pivot[KM_LIMB_COUNT];
  v3 tip[KM_LIMB_COUNT];
  u8 slot_limb[KM_RIG_SLOTS]; // KmLimbId by flattened submesh slot (EcsSubMesh.slot): a limb with several materials has several
  u32 present; // bit per KmLimbId
  b32 ready;
} KmRig;

#define KM_RIG_TYPES 4 // the three droid types (GrDroneType), then the clone
#define KM_RIG_CLONE 3
typedef struct KmRigs {
  KmRig types[KM_RIG_TYPES];
} KmRigs;
ECS_COMPONENT_TYPEDEF(KmRigs);

// on a droid's submesh entity: the limb it poses (KM_LIMB_NONE on every other submesh, so each is looked at once)
typedef struct KmLimb {
  u32 limb;
  u32 type; // GrDroneType of its droid
} KmLimb;
ECS_COMPONENT_TYPEDEF(KmLimb);

// the B2's clips (its own, played on its skin)
typedef enum KmB2Clip {
  KM_B2_STAND = 0,
  KM_B2_WALK = 1,
  KM_B2_CLIP_COUNT = 2,
} KmB2Clip;

typedef struct KmB2Clips {
  HzHandleT(AnimationBlobAsset) clips[KM_B2_CLIP_COUNT];
  AnimationBlobAsset *blobs[KM_B2_CLIP_COUNT];
  b32 ready;
} KmB2Clips;
ECS_COMPONENT_TYPEDEF(KmB2Clips);

// on a B2: the clip its layer plays
typedef struct KmB2Anim {
  u32 clip; // KmB2Clip
} KmB2Anim;
ECS_COMPONENT_TYPEDEF(KmB2Anim);

// ---- the clone squad ----

#define KM_SQUAD_MAX 6
#define KM_SQUAD_SIZE 4 // troopers at the start and after each secured sector

// one trooper; the entity is the clone model and WorldTransform its pose
typedef struct KmClone {
  u32 slot;  // squad index: formation place, bolt hits
  u32 run;   // GrWaves.run it belongs to
  b32 dead;
  f32 dead_time;
  v3 position; // hips: the deck + KM_WALKER_HEIGHT
  v3 velocity;
  f32 yaw;   // radians, 0 faces +Z
  f32 aim_pitch;
  f32 gait;
  f32 gait_amount;
  f32 health;
  i32 target;           // GrDroneField slot, -1 none
  EcsEntity target_entity;
  f32 retarget_timer;
  f32 fire_timer;
  u32 burst_left;
  f32 burst_timer;
  f32 hit_flash;
  Random rng;
} KmClone;
ECS_COMPONENT_TYPEDEF(KmClone);

// the squad as the frame left it (on GrDroneField): the droids' targets and the bolts' extra capsules
typedef struct KmSquadView {
  v3 feet[KM_SQUAD_MAX];
  b32 alive[KM_SQUAD_MAX];
  u32 count; // live troopers
} KmSquadView;

// the squad's bookkeeping
typedef struct KmSquad {
  u32 run;       // GrWaves.run the squad was raised for
  u32 cleared_seen; // GrWaves.cleared_serial handled (reinforcements)
  HzHandleT(ModelBlobAsset) model;
  b32 model_ready;
  EcsQuery troopers; // every KmClone: the publish runs with none
} KmSquad;
ECS_COMPONENT_TYPEDEF(KmSquad);

// this frame's squad shots, for the tracers, impacts and their sound
#define KM_SQUAD_SHOTS 16
typedef struct KmSquadShot {
  v3 from;
  v3 to;
  v3 normal;
  b32 world; // ended on the level
} KmSquadShot;
typedef struct KmSquadShots {
  KmSquadShot items[KM_SQUAD_SHOTS];
  u32 count;
  u32 frame;
} KmSquadShots;
ECS_COMPONENT_TYPEDEF(KmSquadShots);

// walkers stand on the deck; the rest fly the drone routes
b32 km_droid_walks(u32 type);
// the hip height a walker's position keeps above the deck
#define KM_WALKER_HEIGHT 1.0f

void km_droids_init(EcsWorld *w, AssetSystem *assets);
// main lane, app_init after km_droids_init: the squad's voice
void km_squad_init(EcsWorld *w);
// inside the scope after the drones (their field and bolt hits), before km_droids_schedule
void km_squad_schedule(EcsWorld *w);
// inside the scope after gr_fx_schedule: the squad's tracers and impacts
void km_squad_schedule_fx(EcsWorld *w);
// inside the scope after the drones' update, before ecs_transform_schedule
void km_droids_schedule(EcsWorld *w);
