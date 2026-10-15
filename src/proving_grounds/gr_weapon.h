// Weapons, viewmodel and pickups: components, the GrWeaponState/GrPickupState contract, config.
#pragma once
#include "animation/animation.h"
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/random.h"
#include "lib/reflect.h"
#include "lib/string.h"
#include "lib/typedefs.h"
#include "rendering/renderer.h"

#define GR_GUN_COUNT 3
#define GR_TRIGGER_BUFFER 0.18f // a trigger press is buffered this long, s
#define GR_MUZZLE_LIGHT 22.0f   // muzzle flash light intensity
#define GR_MAX_SHOTS 4          // per frame; dt <= 1/20 at 10.5 rps fires at most 2
#define GR_MAX_PELLETS (GR_MAX_SHOTS * 9)
#define GR_MAX_DRONE_HITS 32    // per frame: one entry per (shot, drone)
#define GR_MAX_DRONE_IMPACTS 36 // extra pellets landing on an already-hit drone
#define GR_MAX_SOUNDS 16
#define GR_MAX_EJECTS 4
#define GR_MAX_PIERCE 4
#define GR_PACK_COUNT 2
#define GR_PEDESTAL_COUNT 2

// the weapon's sound requests; the audio package renders them
typedef enum GrWeaponSound {
  GR_SND_RIFLE = 0,
  GR_SND_BREACHER,
  GR_SND_LANCER,
  GR_SND_DRY_FIRE,
  GR_SND_RELOAD_OUT,
  GR_SND_RELOAD_IN,
  GR_SND_RELOAD_CHARGE,
  GR_SND_COIL_CHARGE,
  GR_SND_SHELL_INSERT,
  GR_SND_PUMP,
  GR_SND_WEAPON_PICKUP,
  GR_SND_IMPACT,
  GR_SND_COUNT,
} GrWeaponSound;

typedef enum GrCrosshair {
  GR_CROSSHAIR_CROSS = 0,
  GR_CROSSHAIR_RING = 1,
  GR_CROSSHAIR_DOT = 2,
} GrCrosshair;

typedef enum GrReloadPhase {
  GR_RELOAD_NONE = 0,
  GR_RELOAD_MAGAZINE,
  GR_RELOAD_SHELL_START,
  GR_RELOAD_SHELL_INSERT,
  GR_RELOAD_SHELL_END,
} GrReloadPhase;

typedef enum GrPartKind {
  GR_PART_STATIC = 0,
  GR_PART_MAGAZINE, // Magazine and its MagCell child
  GR_PART_HANDLE,   // ChargingHandle (rifle)
  GR_PART_PUMP,     // Pump (breacher)
  GR_PART_SHELL,    // the loose reload Shell (breacher, first-person only)
} GrPartKind;

// arms clip motions (clips named `<gun>_<motion>`)
typedef enum GrArmsMotion {
  GR_ARMS_HOLD = 0,
  GR_ARMS_MAGAZINE_REACH,
  GR_ARMS_MAGAZINE,
  GR_ARMS_PUMP,
  GR_ARMS_SHELL_REACH,
  GR_ARMS_SHELL,
  GR_ARMS_MOTION_COUNT,
} GrArmsMotion;

#define GR_ARMS_CLIP_COUNT 10

// the gun models' mesh-less sockets, by node name
typedef enum GrSocket {
  GR_SOCKET_MUZZLE = 0,
  GR_SOCKET_EJECTION_PORT = 1,
  GR_SOCKET_SIGHT = 2,
  GR_SOCKET_LOAD_PORT = 3, // breacher only
  GR_SOCKET_COUNT = 4,
} GrSocket;

HZ_REFLECT()
typedef struct GrReloadCue {
  f32 at;    // fraction of the magazine reload
  u32 sound; // GrWeaponSound
} GrReloadCue;

// a gun's config plus its view profile and rig constants
HZ_REFLECT()
typedef struct GrGunConfig {
  String32Bytes id;   // the log's gun id
  String32Bytes name; // HUD and beacon label
  b32 automatic;
  f32 damage;
  u32 pellets;
  b32 falloff;
  f32 falloff_start;
  f32 falloff_end;
  f32 falloff_min;
  u32 pierce;
  f32 fire_rate;
  u32 magazine;
  b32 shell_reload;
  f32 reload_time;
  GrReloadCue cues[3];
  u32 cue_count;
  f32 shell_start;
  f32 shell_per;
  f32 shell_end;
  f32 range;
  f32 spread_hip;
  f32 spread_zoom;
  f32 spread_per_shot;
  f32 spread_max;
  f32 spread_recovery;
  f32 pellet_hip;
  f32 pellet_zoom;
  f32 recoil_pitch;
  f32 recoil_yaw;
  f32 recoil_recover;
  f32 ads_fov;
  b32 scope;
  v3 tracer_color; // HDR, linear
  f32 tracer_speed;
  f32 tracer_width;
  b32 tracer_beam;
  u32 glow;         // sRGB hex
  u32 muzzle_light; // sRGB hex
  u32 low_color;    // ammo screen colour at <= 25 % (sRGB hex)
  f32 knockback;
  u32 fire_sound; // GrWeaponSound
  u32 crosshair;  // GrCrosshair
  // view profile
  v3 hip;
  f32 ads_eye_distance;
  f32 kick;
  // rig
  f32 heat_per_shot;
  v3 vent_cold;
  v3 vent_hot;
  v3 flash_color; // HDR
  f32 flash_scale;
  v3 magazine_dir; // normalised
  f32 magazine_travel;
} GrGunConfig;

// every gun's config (indexed by GrWeaponId), the pickups and the health packs; on the Settings entity
HZ_REFLECT()
ECS_COMPONENT(GrWeaponConfig) {
  GrGunConfig guns[GR_GUN_COUNT];
  f32 hip_yaw;      // viewmodel yaw at the hip
  u32 starting_gun; // the gun a run starts with
  // pickups
  f32 pickup_reach;
  f32 display_scale;
  f32 pedestal_hover;
  f32 drop_hover;
  f32 swap_time;
  // breacher shell path
  v3 shell_below;
  v3 shell_mouth;
  f32 shell_push;
  f32 pump_travel;
  // health packs
  f32 pack_heal;
  f32 pack_respawn;
  f32 pack_reach;
  f32 pack_reach_height;
  f32 pack_hover;
};

// Settings' GrWeaponConfig as the systems read it, copied on change
typedef struct GrWeaponRules {
  GrWeaponConfig weapons;
  b32 ready; // the scene's config has loaded
} GrWeaponRules;
ECS_COMPONENT_TYPEDEF(GrWeaponRules);

// ---- events: what one frame of the weapon did, for the look, effects, audio, HUD, player and drones ----

typedef struct GrShotEvent {
  u32 gun;
  v3 origin;       // the camera
  v3 direction;    // the aim cone sample (pellet 0)
  v3 muzzle;       // the real muzzle in the world: the flash and its light
  v3 tracer_start; // the muzzle where the world camera shows it
  f32 zoom;
  f32 recoil_pitch, recoil_yaw, recoil_recover; // the recoil the player adds
  f32 light_intensity;
  u32 first_pellet;
  u32 pellet_count;
  b32 hit; // at least one drone took damage
} GrShotEvent;

typedef enum GrPelletEnd {
  GR_PELLET_RANGE = 0,
  GR_PELLET_WORLD = 1, // a wall: world impact fx at `end`
  GR_PELLET_DRONE = 2,
} GrPelletEnd;

// one tracer or beam from the shot's tracer_start to `end`
typedef struct GrPelletEvent {
  v3 direction;
  v3 end;
  v3 normal; // GR_PELLET_WORLD: the wall's
  u32 end_kind;
  u32 shot;
} GrPelletEvent;

// damage once per drone per shot; the drone sums its own share (GrPendingDamage), the drones report kills
typedef struct GrDroneHitEvent {
  EcsEntity drone;
  u32 shot;
  f32 damage;    // the pellets' damage summed, falloff applied
  f32 knockback; // knockback * pellets / pellet_count
  v3 direction;  // the shot's
  v3 point;      // the first pellet's hit
  v3 normal;
} GrDroneHitEvent;

// a drone impact for every extra pellet on an already-hit drone; the colour is the type's energy colour
typedef struct GrDroneImpactEvent {
  v3 point, normal, direction;
  u32 drone_type; // GrDroneType
} GrDroneImpactEvent;

// a weapon sound this frame; the audio call-site table (gr_sfx_site) owns its volume, jitter and reverb
typedef struct GrSoundEvent {
  u32 sound; // GrWeaponSound
  b32 positional;
  v3 position;
} GrSoundEvent;

typedef enum GrEjectKind {
  GR_EJECT_BRASS = 0,
  GR_EJECT_SHELL = 1,
  GR_EJECT_VENT = 2, // a vent puff, not a casing
} GrEjectKind;

typedef struct GrEjectEvent {
  u32 kind;    // GrEjectKind
  v3 position; // on screen
  v3 velocity;
} GrEjectEvent;

typedef enum GrViewCommand {
  GR_VIEW_NONE = 0,
  GR_VIEW_LOWER = 1,
  GR_VIEW_RAISE = 2,
} GrViewCommand;

typedef struct GrWeaponEvents {
  GrShotEvent shots[GR_MAX_SHOTS];
  GrPelletEvent pellets[GR_MAX_PELLETS];
  GrDroneHitEvent drone_hits[GR_MAX_DRONE_HITS];
  GrDroneImpactEvent drone_impacts[GR_MAX_DRONE_IMPACTS];
  GrSoundEvent sounds[GR_MAX_SOUNDS];
  GrEjectEvent ejects[GR_MAX_EJECTS];
  u32 shot_count, pellet_count, drone_hit_count, drone_impact_count, sound_count, eject_count;
  b32 dry_fire;        // the trigger pulled on an empty gun
  b32 reload_start;    // a reload started
  f32 reload_duration; // its total
  b32 reload_end;      // the reload finished
  b32 switched;        // the gun switched (gun = the new one)
  b32 run_reset;       // run start: the pickups and packs reset too
  b32 raise;           // the viewmodel raise ran this frame (equip back to 0)
  u32 view_last;       // GrViewCommand issued last this frame: it decides `lowering`
  u32 rest_mask;       // bit per gun: its rig returns to rest
  i32 taken_gun;       // a pickup left the world into the hands this frame, else -1
  i32 dropped_gun;     // the old gun hits the ground this frame, else -1
  v3 drop_base;        // where (floor point)
} GrWeaponEvents;

// the rig's moving parts for the gun in hand, in model space (offsets from rest)
typedef struct GrRigParts {
  v3 magazine;
  b32 magazine_hidden; // swapped for a fresh one off screen
  v3 handle;
  v3 pump;
  v3 shell; // position offset from the Shell node's rest origin
  b32 shell_visible;
} GrRigParts;

// per-gun values the look drives the gun materials with
typedef struct GrGunLook {
  f32 vent_intensity;
  v3 vent_color;
  f32 coil_intensity; // lancer Emit_Coil
  f32 cell_intensity; // lancer Emit_Cell
  f32 lens_intensity; // lancer Emit_Lens
  u32 screen_ammo;    // ammo shown on the gun's screen
  u32 screen_max;
  b32 screen_low; // ammo <= 25 %
} GrGunLook;

// ---- per-entity state ----

// the gun logic on the Weapon entity; the frame's step and bindings are resolved once by the flow system
typedef struct GrWeapon {
  u32 gun;
  u32 ammo[GR_GUN_COUNT];
  u32 reload_phase; // GrReloadPhase
  f32 reload_t, reload_time, reload_duration;
  u32 reload_cue;
  b32 reload_interrupt;
  b32 reload_pump;
  f32 fire_timer;
  b32 dry_fired;
  f32 buffered;
  b32 queued;
  b32 pending; // a swap is lowering the gun
  u32 pending_gun;
  f32 bloom;
  u32 shots_fired, shots_hit;
  f32 zoom_target; // the player damps its zoom towards it
  b32 held;
  b32 in_run;
  i32 prompt_gun;     // the E prompt's gun, -1 none
  u32 applied_serial; // GrDev.restart_serial the run was reset for
  i32 granted_weapon; // the last GrDev.weapon a switch swap was issued for
  // this frame (gr_weapon_flow_system)
  f32 dt;         // the run's step while the run updates, else 0
  b32 controls;   // playing with controls
  b32 live;       // controls && alive
  b32 fire_down;  // LMB held, real or simulated (not gated by controls)
  b32 fire_pressed;
  b32 ads_down;
  b32 reload_pressed;
  b32 swap_pressed;
  v2 look; // mouse delta for the sway, zero without controls
  Random rng;
  GrWeaponEvents events;
} GrWeapon;
ECS_COMPONENT_TYPEDEF(GrWeapon);

typedef struct GrSpring3 {
  v3 value, velocity, target;
  f32 stiffness, damping;
} GrSpring3;

// the viewmodel motion and rigs on the Weapon entity, whose WorldTransform is the gun group
typedef struct GrViewmodel {
  GrSpring3 kick_pos, kick_rot, sway;
  f32 sprint;
  f32 equip; // 0 lowered out of view, 1 up
  b32 lowering;
  b32 lowered;
  v3 local_pos; // the gun group in the camera frame
  quat local_rot;
  b32 visible; // root (in run) and scope hide
  // rigs: per gun heat; lancer coils, cell and lens; breacher pump cycle
  f32 heat[GR_GUN_COUNT];
  f32 charge;
  f32 coil;
  f32 cell;
  f32 lens;
  f32 cycle;
  b32 racked, ejected, end_stroke;
  GrRigParts parts;
  u32 hands_motion; // GrArmsMotion of the gun in hand
  f32 hands_u;
  Random rng;
  GrSoundEvent sounds[2];
  GrEjectEvent ejects[2];
  u32 sound_count, eject_count;
} GrViewmodel;
ECS_COMPONENT_TYPEDEF(GrViewmodel);

// one mesh part of a gun: first-person (child of the Weapon entity) or the world copy (child of its GrPickup)
typedef struct GrGunPart {
  u32 gun;
  u32 kind; // GrPartKind
  b32 world_copy;
  v3 rest; // its LocalTransform position at rest (world copies: centred on the copy's bounds)
} GrGunPart;
ECS_COMPONENT_TYPEDEF(GrGunPart);

// the skinned first-person arms, child of the Weapon entity
typedef struct GrArms {
  i32 clip; // index into GrWeaponAssets.clips playing, -1 none
  const AnimationRemap *remaps[GR_ARMS_CLIP_COUNT]; // clip -> arms joints, NULL when 1:1 (EcsRendererAlloc, app lifetime)
} GrArms;
ECS_COMPONENT_TYPEDEF(GrArms);

// a gun lying in the world: the anchor + holder, parent of its world-copy parts
typedef struct GrPickup {
  u32 gun;
  b32 in_world;
  v3 base;      // floor point it hovers over
  i32 pedestal; // GrLevel pedestal index, -1 dropped
  i32 home;     // the pedestal it waits on at run start, -1 none (the starting gun)
  f32 phase;
  u32 placements;
} GrPickup;
ECS_COMPONENT_TYPEDEF(GrPickup);

// a health pack
typedef struct GrHealthPack {
  u32 index;
  v3 base;
  f32 cooldown; // seconds until it is back, 0 while it is there
  f32 phase;
  f32 healed; // healed this frame
} GrHealthPack;
ECS_COMPONENT_TYPEDEF(GrHealthPack);

// ---- resources ----

// the weapon this frame: HUD, effects, audio, player recoil/zoom and drone damage read it
typedef struct GrWeaponState {
  u32 gun;
  u32 ammo;
  u32 magazine;
  u32 ammo_by_gun[GR_GUN_COUNT];
  b32 reloading;
  b32 swapping_magazine; // whole-magazine reload: the HUD shows no count
  u32 reload_phase;
  f32 reload_t;
  b32 swapping;
  b32 held;
  f32 zoom;          // 0..1 aim-down-sights, as the weapon used it
  f32 zoom_target;   // the player damps towards it next frame
  f32 zoom_fov;      // the gun's ADS fov
  f32 scope;         // 0..1 scope overlay, rises with zoom on scoped guns
  f32 spread;        // the current aim cone spread
  f32 reticle_spread;
  u32 crosshair;
  i32 prompt_gun; // -1 none
  u32 shots_fired, shots_hit;
  f32 accuracy;
  b32 in_run;
  b32 view_visible;
  v3 view_pos; // the gun group's world pose (the viewmodel's draw input)
  quat view_rot;
  GrRigParts parts;
  u32 hands_motion;
  f32 hands_u;
  GrGunLook look[GR_GUN_COUNT];
  GrWeaponEvents events;
} GrWeaponState;
ECS_COMPONENT_TYPEDEF(GrWeaponState);

// the guns on the ground and the health packs this frame
typedef struct GrPickupState {
  b32 in_world[GR_GUN_COUNT];
  v3 base[GR_GUN_COUNT];
  i32 pedestal[GR_GUN_COUNT];
  i32 home[GR_GUN_COUNT]; // the pedestal each gun waits on at run start, -1 none
  b32 pedestal_occupied[GR_PEDESTAL_COUNT];
  f32 pedestal_glow[GR_PEDESTAL_COUNT]; // Glow emissive intensity, tinted gold (the look drives the material)
  b32 pack_present[GR_PACK_COUNT];
  v3 pack_base[GR_PACK_COUNT];
  f32 pack_cooldown[GR_PACK_COUNT];
  f32 heal;        // health the packs restore this frame; the intake writer forwards it
  b32 heal_event;  // a pack healed the player this frame
  v3 heal_position;
} GrPickupState;
ECS_COMPONENT_TYPEDEF(GrPickupState);

// the models, arm clips and the spawned entities
typedef struct GrWeaponAssets {
  HzHandleT(ModelBlobAsset) guns[GR_GUN_COUNT];
  HzHandleT(ModelBlobAsset) arms;
  HzHandleT(ModelBlobAsset) healthpack;
  HzHandleT(AnimationBlobAsset) clips[GR_ARMS_CLIP_COUNT];
  AnimationBlobAsset *clip_blobs[GR_ARMS_CLIP_COUNT];
  EcsEntity weapon;
  EcsEntity pickups[GR_GUN_COUNT];
  v3 sockets[GR_GUN_COUNT][GR_SOCKET_COUNT]; // model space, from the cooked node table
  v3 shell_rest;                             // the breacher's Shell node, model space
  b32 built;
  b32 clips_ready;
  b32 packs_spawned; // the health packs, once the level's tables are gathered
} GrWeaponAssets;
ECS_COMPONENT_TYPEDEF(GrWeaponAssets);

// main lane, app_init after gr_level_init: resources, asset loads, the Weapon and pickup entities (packs spawn once the level gathers)
void gr_weapon_init(EcsWorld *w);
// inside the scope after the player publishes GrPlayerState: weapon, viewmodel, pickups, packs, arms, consolidations
void gr_weapon_schedule(EcsWorld *w);
