// Proving Grounds effects: the fx request contract, effect state components and the resources the fx consolidations own.
#pragma once
#include "ecs/ecs.h"
#include "lib/color.h"
#include "lib/hzmath.h"
#include "lib/random.h"
#include "lib/string.h"
#include "rendering/renderer.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"
#include "proving_grounds.h"
#include "gameplay/vfx/vfx.h"

// one effect a producer asks for: particle recipes, tracers, casings, flash lights, the muzzle flash, debris
typedef enum GrFxKind {
  GR_FX_NONE = 0,
  GR_FX_IMPACT_WORLD,  // p hit point, n surface normal, d shot direction
  GR_FX_IMPACT_DRONE,  // p, n, d; variant = GrDroneType (energy colour)
  GR_FX_IMPACT_BOLT,   // p, n; variant = GrDroneType (bolt colour); scale
  GR_FX_EXPLOSION,     // p centre; variant = GrDroneType (scale + energy colour)
  GR_FX_WARP_IN,       // p; variant = GrDroneType
  GR_FX_DAMAGE_SMOKE,  // p
  GR_FX_LAUNCH,        // p pad centre
  GR_FX_VENT,          // p, d velocity
  GR_FX_MUZZLE,        // p muzzle (the flash light), variant = GrWeaponId, scale = light intensity multiplier
  GR_FX_TRACER,        // p from, n to; variant = GrWeaponId
  GR_FX_BEAM,          // p from, n to; variant = GrWeaponId
  GR_FX_CASING,        // p position, d velocity; variant = GrCasingKind
  GR_FX_BOLT_TRAIL,    // p position, d velocity; variant = GrDroneType
  GR_FX_DEBRIS_SPARK,  // p position, d velocity
  GR_FX_DEBRIS_SMOKE,  // p position
  GR_FX_DRONE_FIRE,    // p the eye, d bolt direction; variant = GrDroneType (the drone's fire glow)
  GR_FX_KIND_COUNT,
} GrFxKind;

typedef enum GrCasingKind {
  GR_CASING_BRASS = 0,
  GR_CASING_SHELL = 1,
  GR_CASING_KIND_COUNT = 2,
} GrCasingKind;

typedef struct GrFxRequest {
  u32 kind;    // GrFxKind
  u32 variant; // GrDroneType / GrWeaponId / GrCasingKind by kind
  f32 scale;   // 0 = 1
  f32 _pad;
  v3 p;
  v3 n;
  v3 d;
} GrFxRequest;

#define GR_FX_FRAME_MAX 2048
// the requests whose objects are not vfx (the muzzle flash quads, the casings), for their consolidations this frame
typedef struct GrFxFrame {
  GrFxRequest *items;
  u32 count;
  u32 dropped;
} GrFxFrame;
ECS_COMPONENT_TYPEDEF(GrFxFrame);

// the effects the requests throw, registered once in vfx when the fx assets load; systems read it to build vfx requests
typedef struct GrFxAssets {
  b32 ready;
  f32 scene_per_unit; // look HDR unit -> pre-exposed scene value: the tracer ribbons' colour carries it
  HzHandleT(VfxAsset) impact_sparks, impact_flash, impact_dust, impact_decal, light_impact;
  HzHandleT(VfxAsset) drone_sparks, drone_glow[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) bolt_sparks[GR_DRONE_TYPE_COUNT][2], bolt_glow[GR_DRONE_TYPE_COUNT], light_bolt[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) ex_core[GR_DRONE_TYPE_COUNT], ex_fire[GR_DRONE_TYPE_COUNT], ex_energy[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) ex_sparks[GR_DRONE_TYPE_COUNT], ex_smoke[GR_DRONE_TYPE_COUNT], light_explosion[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) warp_sparks[GR_DRONE_TYPE_COUNT], warp_glow[GR_DRONE_TYPE_COUNT], light_warp[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) damage_smoke, damage_spark;
  HzHandleT(VfxAsset) launch_sparks, launch_glow, light_launch;
  HzHandleT(VfxAsset) vent;
  HzHandleT(VfxAsset) bolt_trail[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) debris_spark, debris_smoke;
  HzHandleT(VfxAsset) drone_fire[GR_DRONE_TYPE_COUNT];
  HzHandleT(VfxAsset) tracer[GR_GUN_COUNT], beam[GR_GUN_COUNT], light_muzzle[GR_GUN_COUNT]; // tracer or beam by the gun's tracer_beam
} GrFxAssets;
ECS_COMPONENT_TYPEDEF(GrFxAssets);

// the muzzle flash: one flash on the gun in hand
#define GR_FX_FLASH_TIME 0.06f
typedef struct GrFxMuzzle {
  f32 timer;      // seconds left; > 0 shows the flash
  f32 intensity;  // (timer / 0.06)^2 * 1.4, sampled before ageing
  f32 seed;
  f32 star_angle; // random spin of the star
  f32 star_scale; // 0.8..1.3
  f32 flame_len;  // 0.8..1.4
  f32 flame_width_a;
  f32 flame_width_b;
  u32 serial;     // bumps on every trigger
  u32 gun;        // the gun that last fired: its rig's flash colour and scale
  u32 color_gun;  // the gun the materials' colour was written for (+1; 0 = none)
  f32 scale;      // the gun rig's flash scale
  ColorF32 color; // default (9, 5, 2), linear HDR, until a gun fires
  b32 visible;    // the gun group is shown (GrWeaponState.view_visible)
  v3 socket;      // the Muzzle socket in the world this frame (the gun group times the cooked socket)
  quat socket_rot;
  v3 sight;         // the Sight socket in the world this frame
  b32 sight_visible; // the VK-7 (the rig with the holo sight) is in hand and shown
  HzHandleT(RendererMaterial) material[3]; // GrFxFlashRole; created on the main lane at build
  b32 built;
} GrFxMuzzle;
ECS_COMPONENT_TYPEDEF(GrFxMuzzle);

typedef enum GrFxFlashRole {
  GR_FX_FLASH_STAR = 0,
  GR_FX_FLASH_FLAME_A = 1,
  GR_FX_FLASH_FLAME_B = 2,
  GR_FX_FLASH_PART_COUNT = 3,
} GrFxFlashRole;

// a muzzle flash quad; reads GrFxMuzzle
ECS_COMPONENT_TYPEDEF(GrFxFlashPart);
typedef struct GrFxFlashPart {
  u32 role; // GrFxFlashRole
  u32 seen_serial;
} GrFxFlashPart;

// the VK-7 holo sight quad on the rifle rig; reads GrFxMuzzle
ECS_COMPONENT_TYPEDEF(GrFxSight);
typedef struct GrFxSight {
  u32 unused;
} GrFxSight;

// spent casings: 40 pooled; one entity per slot, drawn by two instance batches
#define GR_FX_CASINGS 40
ECS_COMPONENT_TYPEDEF(GrCasing);
typedef struct GrCasing {
  u32 slot;
  u32 kind; // GrCasingKind
  b32 active;
  b32 resting;
  f32 life;
  v3 position;
  v3 velocity;
  quat rotation;
  v3 spin;
  u32 seen_serial;
} GrCasing;

// the eject the casing consolidation assigned this frame, by slot
typedef struct GrFxCasingSpawn {
  u32 serial; // bumps per eject into the slot
  u32 kind;
  v3 position;
  v3 velocity;
  quat rotation;
  v3 spin;
} GrFxCasingSpawn;
typedef struct GrFxCasings {
  GrFxCasingSpawn spawns[GR_FX_CASINGS];
  u32 next; // the ring cursor: a new casing replaces the oldest
  u32 active; // casings alive last frame: the batches submit only while > 0
  Random rng;
  HzHandleT(RendererInstanceBatch) batch[GR_CASING_KIND_COUNT]; // created on the main lane at build
} GrFxCasings;
ECS_COMPONENT_TYPEDEF(GrFxCasings);

// debris: a detached drone part tumbling about its own node; the cooked vertices are in model space
ECS_COMPONENT_TYPEDEF(GrDebris);
typedef struct GrDebris {
  v3 position;     // the part node's pivot in the world
  quat rotation;   // the part node's world rotation
  v3 node_offset;  // the node's model-space position: the draw transform puts it back at the pivot
  quat node_inverse; // the node's model-space rotation, inverted
  f32 scale;       // the drone's scale at break-up
  v3 velocity;
  v3 spin;
  f32 radius;
  f32 life;
  f32 max_life;
  f32 trail_timer;
  b32 trails;        // the part's first submesh trails sparks and smoke; the rest of its submeshes do not repeat them
  f32 glow;          // the part's emission at break-up; 0 = not a glowing part
  f32 glow_now;      // this frame's sputtered emission, written to its material instance
  u32 rng;
} GrDebris;

// on a bolt row whose look parts are spawned
ECS_COMPONENT_TYPEDEF(GrFxBoltRow);
typedef struct GrFxBoltRow {
  u8 _;
} GrFxBoltRow;

// one of an enemy bolt row's six look parts (core sphere and glow sprite, per drone type)
ECS_COMPONENT_TYPEDEF(GrBoltLookPart);
typedef struct GrBoltLookPart {
  EcsEntity bolt; // the pooled GrBolt row
  u32 type;       // GrDroneType this part draws
  b32 glow;       // the sprite; else the core sphere
} GrBoltLookPart;

// pickup beacon: kind colour, ring + column + label over a pickup
typedef enum GrBeaconKind {
  GR_BEACON_WEAPON = 0,
  GR_BEACON_HEALTH = 1,
  GR_BEACON_OBJECTIVE = 2, // the capture point while the droids fall back
} GrBeaconKind;

// one beacon per gun pickup and per health pack; its WorldTransform is the floor point under the pickup
ECS_COMPONENT_TYPEDEF(GrBeacon);
typedef struct GrBeacon {
  u32 kind;           // GrBeaconKind
  u32 index;          // the gun (GrWeaponId) or the pack it marks (GrPickupState)
  String64Bytes name; // upper-cased on the label
  f32 label_height;   // the label's bottom above the floor
  b32 visible;
  b32 built;          // the ring child is spawned
} GrBeacon;

// the beacons this frame, for the main-lane labels (world_ui is main-lane)
#define GR_FX_BEACONS 16
typedef struct GrFxBeaconView {
  v3 position;
  u32 kind;
  String64Bytes name;
  f32 label_height;
  b32 visible;
} GrFxBeaconView;
typedef struct GrFxBeacons {
  GrFxBeaconView items[GR_FX_BEACONS];
  u32 count;
} GrFxBeacons;
ECS_COMPONENT_TYPEDEF(GrFxBeacons);

// a beacon's floor ring or light beam child, shown with its beacon
ECS_COMPONENT_TYPEDEF(GrBeaconPart);
typedef struct GrBeaconPart {
  EcsEntity beacon;
} GrBeaconPart;
