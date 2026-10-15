// Proving Grounds level: the arena scene's authored sites gathered into tables, walkable tops, routes, the static-layer raycast.
#pragma once
#include "ecs/ecs.h"
#include "ecs/ecs_physics/ecs_physics.h"
#include "lib/hzmath.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"

// the level's areas (every per-area shader array uses this order)
typedef enum GrAreaId {
  GR_AREA_HUB = 0,
  GR_AREA_HANGAR = 1,
  GR_AREA_WING = 2,
  GR_AREA_YARD = 3,
  GR_AREA_COUNT = 4,
} GrAreaId;

// the signs, in placement order
typedef enum GrSignKind {
  GR_SIGN_SECTOR = 0,      // east wall: sector number '07'
  GR_SIGN_BANNER_SOUTH = 1, // 'PROVING GROUNDS  ·  SECTOR 7', emissive #9ff1ff
  GR_SIGN_CAUTION_A = 2,   // north wall a -5: warning
  GR_SIGN_CAUTION_B = 3,   // north wall a 5
  GR_SIGN_BANNER_WEST = 4, // 'SIMULATION ACTIVE', emissive #ffc38a
  GR_SIGN_COUNT = 5,
} GrSignKind;

#define GR_LEVEL_ZONE_COUNT 11 // four platforms, four bridges, three rooms (km_layout_fill)
#define GR_LEVEL_PORTAL_COUNT 10
#define GR_LEVEL_AREA_SPAWN_MAX 8
#define GR_LEVEL_JUMP_PAD_COUNT 4
#define GR_LEVEL_PEDESTAL_COUNT 2
#define GR_LEVEL_HEALTH_PACK_COUNT 2
#define GR_DOOR_WIDTH 6.0f
#define GR_DOOR_HEIGHT 5.0f

// ---- authored in the arena scene: the level's tables are gathered from these ----

// an area: fog box, box-projected reflections and shadow fit; index = GrAreaId
HZ_REFLECT()
ECS_COMPONENT(GrAreaDef) {
  u32 index;
  v3 box_min;
  v3 box_max;
  f32 fog_density; // relative to the look's fog
  v3 probe;        // reflection probe capture point
  u32 neighbour_mask; // bit (1 << GrAreaId) per area one doorway away
  FixedArray(v3, GR_LEVEL_AREA_SPAWN_MAX) spawns; // where drones warp in
};

// a convex room drones fly in, between floor + 2 m and `ceiling`; index orders the zones
HZ_REFLECT()
ECS_COMPONENT(GrNavZoneDef) {
  u32 index;
  u32 area; // GrAreaId
  v3 box_min;
  v3 box_max;
  f32 ceiling;
};

// a doorway between zones a and b: centre at flying height, unit normal from a into b
HZ_REFLECT()
ECS_COMPONENT(GrPortalDef) {
  u32 index;
  u32 a;
  u32 b;
  v3 center;
  v3 normal;
};

// the level's extent (sun shadow and volumetric bakes)
HZ_REFLECT()
ECS_COMPONENT(GrLevelBounds) {
  v3 min;
  v3 max;
};

// where the player starts: the entity's position is the feet
HZ_REFLECT()
ECS_COMPONENT(GrPlayerSpawn) {
  f32 yaw;
};

// a jump pad at the entity's position: the launch is solved ballistically onto `target` over `apex`
HZ_REFLECT()
ECS_COMPONENT(GrJumpPad) {
  u32 index;
  v3 target; // landing point
  f32 apex;  // peak height above the pad
  f32 radius;
};

// a weapon pickup pedestal at the entity's position; its model's "Emit_Pedestal" part glows per gun
HZ_REFLECT()
ECS_COMPONENT(GrPedestal) {
  u32 index;
  u32 weapon; // GrWeaponId
  f32 top;    // dish height above the floor point
};

// a health pack's floor point
HZ_REFLECT()
ECS_COMPONENT(GrHealthPackSite) {
  u32 index;
};

// a sign plane facing the entity's +Z; the look draws `kind` onto it
HZ_REFLECT()
ECS_COMPONENT(GrSignSite) {
  u32 kind; // GrSignKind
  f32 width, height;
  f32 density;   // canvas pixels per metre
  f32 intensity; // emissive multiplier
  v3 emissive;   // linear emissive colour
};

// a static box collider whose top is walkable (unrotated)
HZ_REFLECT()
ECS_COMPONENT(GrWalkable) {
  u8 _;
};

// a static upright cylinder collider centred on the entity; the level builds its trimesh body at runtime
HZ_REFLECT()
ECS_COMPONENT(GrCylinderCollider) {
  f32 radius_top;
  f32 radius_bottom;
  f32 height;
  u32 segments;
};

// runtime: the cylinder's static trimesh body, an unsaved root entity
ECS_COMPONENT_TYPEDEF(GrCylinderBody);
typedef struct GrCylinderBody {
  EcsEntity body;
} GrCylinderBody;

// runtime: on the body, the cylinder entity it was built for
typedef struct GrCylinderBodyOf {
  EcsEntity owner;
} GrCylinderBodyOf;
ECS_COMPONENT_TYPEDEF(GrCylinderBodyOf);

// on the core's ring submesh entities: spins about its node origin while it wobbles and bobs
HZ_REFLECT()
ECS_COMPONENT(GrCoreRing) {
  u32 index; // 0 RingOuter (spins +), 1 RingInner (spins -)
  v3 pivot;  // the ring node's origin in model space: the cooked vertices are baked to model space
  quat rest; // the node's model-space rotation baked into those vertices
};

// ---- the gathered tables ----

typedef struct GrArea {
  aabb box;           // interior volume: fog box, box-projected reflections, shadow fit
  f32 fog_density;    // relative to the look's fog
  v3 probe;           // reflection probe capture point
  v3 spawns[GR_LEVEL_AREA_SPAWN_MAX]; // where drones warp in
  u32 spawn_count;
  u32 neighbour_mask; // bit (1 << GrAreaId) per area one doorway away
} GrArea;

// a convex room drones fly anywhere inside, between floor + 2 m and `ceiling`
typedef struct GrNavZone {
  u32 area; // GrAreaId
  aabb box;
  f32 ceiling;
} GrNavZone;

// a doorway between zones a and b, centre at flying height, unit normal from a into b
typedef struct GrPortal {
  u32 a;
  u32 b;
  v3 center;
  v3 normal;
} GrPortal;

// the first doorway on the shortest path, crossed a -> b when dir is +1; portal -1 = same zone
typedef struct GrNextPortal {
  i32 portal;
  i32 dir;
} GrNextPortal;

// the top of an unrotated walkable box
typedef struct GrWalkableTop {
  f32 min_x, max_x;
  f32 min_z, max_z;
  f32 y;
} GrWalkableTop;

// a jump pad: launch solved ballistically onto its target
typedef struct GrPadLaunch {
  v3 position; // floor point at the pad centre
  f32 radius;
  v3 launch_velocity;
} GrPadLaunch;

// a weapon pickup pedestal
typedef struct GrPedestalSite {
  u32 weapon;  // GrWeaponId
  v3 position; // floor point under the centre
  f32 top;     // dish height above position
} GrPedestalSite;

// gathered from the scene once it has spawned; regathered when an authored site changes
typedef struct GrLevel {
  b32 tables_ready; // every table below is valid
  b32 meshes_ready; // the tables are valid and every scene model has its renderer instance
  u32 generation;   // bumped by every gather
  GrArea areas[GR_AREA_COUNT];
  GrNavZone zones[GR_LEVEL_ZONE_COUNT];
  GrPortal portals[GR_LEVEL_PORTAL_COUNT];
  GrNextPortal next[GR_LEVEL_ZONE_COUNT][GR_LEVEL_ZONE_COUNT]; // [from][to]
  aabb bounds;     // the level bounds: sun shadow and volumetric bakes
  aabb hub_bounds; // the hub interior (dust motes)
  v3 player_spawn; // feet
  f32 player_spawn_yaw;
  f32 gravity; // the player's gravity, the pad solve's g
  GrPadLaunch pads[GR_LEVEL_JUMP_PAD_COUNT];
  GrPedestalSite pedestals[GR_LEVEL_PEDESTAL_COUNT];
  v3 health_packs[GR_LEVEL_HEALTH_PACK_COUNT]; // floor points
  GrSignSite signs[GR_SIGN_COUNT];             // by kind
  GrWalkableTop *tops;
  u32 top_count;
  // uniform XZ grid over the tops' extent listing the tops overlapping each cell (CSR)
  u32 *grid_start; // grid_w * grid_d + 1 offsets into grid_items
  u32 *grid_items;
  u32 grid_w, grid_d;
  f32 grid_cell;
  v2 grid_origin; // min x, min z
  v2 grid_max;    // max x, max z
} GrLevel;
ECS_COMPONENT_TYPEDEF(GrLevel);

typedef struct GrRayHit {
  v3 point;
  v3 normal; // faces the ray
  f32 distance;
  EcsEntity entity; // the collider entity hit
} GrRayHit;

// the highest walkable top <= p.y + 0.05 containing (p.x, p.z), else 0
f32 gr_level_ground_height(const GrLevel *l, v3 p);
// the nav zone containing p, or the nearest one
u32 gr_level_zone_at(const GrLevel *l, v3 p);
// the area containing p
u32 gr_level_area_at(const GrLevel *l, v3 p);
// inside a zone, or within a doorway's wall thickness
b32 gr_level_inside(const GrLevel *l, v3 p);
GrNextPortal gr_level_next_portal(const GrLevel *l, u32 from_zone, u32 to_zone);

// the level's static collision layer; NULL before its first build. Systems calling it declare EcsResIn(EcsPhysicsWorld)
const EcsCollisionLayer *gr_level_collision(EcsWorld *w);
// the nearest hit within max_distance along the unit `dir`
b32 gr_level_raycast(const EcsCollisionLayer *layer, v3 origin, v3 dir, f32 max_distance, GrRayHit *hit);

// main lane, app_init after ecs_physics_init: the GrLevel resource and the gather's queries
void gr_level_init(EcsWorld *w);
// inside the frame's scope, early: the collision layer build
void gr_level_schedule(EcsWorld *w);
// inside the frame's scope after ecs_transform_schedule: the core rings, collider sync, cylinder bodies, the gather and the mesh gate
void gr_level_schedule_gather(EcsWorld *w);
