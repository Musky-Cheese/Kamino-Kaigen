// Drones, enemy bolts and the wave director: config components on Settings, per-entity state, the shared resources.
#pragma once
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "lib/array.h"
#include "lib/color.h"
#include "lib/hzmath.h"
#include "lib/json_parser.h"
#include "lib/random.h"
#include "lib/reflect.h"
#include "lib/string.h"
#include "lib/typedefs.h"
#include "rendering/mesh.h"
#include "proving_grounds.h"
#include "gr_level.h"
#include "gr_player.h"
#include "km_droids.h"

#define GR_MAX_DRONES 64
#define GR_MAX_WAVES 4
#define GR_MAX_WAVE_GROUPS 4
#define GR_MAX_WAVE_SPAWNS 32
#define GR_MAX_DRONE_PARTS 24 // radius-tagged parts per drone model (the B1 has 17)
#define GR_MAX_PLAYER_HITS 16
#define GR_DRONE_TYPES 3 // GR_DRONE_TYPE_COUNT as a macro: reflection codegen sizes an enum-constant FixedArray as 0
static_assert(GR_DRONE_TYPES == GR_DRONE_TYPE_COUNT, "GR_DRONE_TYPES must match GrDroneType");

// one drone archetype's config
HZ_REFLECT()
typedef struct GrDroneArchetype {
  String32Bytes label;      // HUD popup / radar label
  String32Bytes model_node; // the root node of models/drones/<type>.glb; the eye socket is <model_node>Muzzle
  f32 health;
  u32 score;
  f32 scale; // size class: explosion, warp flash, shake and sound pitch
  f32 radius; // collision and hit radius, meters
  f32 max_speed;
  f32 acceleration;
  v2 distance; // preferred engagement distance range
  v2 altitude; // hover altitude range (world y)
  v2 cooldown; // seconds between attacks
  f32 telegraph;
  u32 burst;
  f32 burst_interval;
  f32 bolt_speed;
  f32 bolt_damage;
  f32 bolt_radius;
  f32 bolt_splash;
  f32 bolt_spread; // cone half-angle, radians
  ColorF32 color;  // sRGB energy colour (eye, thrusters, bolts, radar); 3D consumers convert to linear
} GrDroneArchetype;

// a part's glTF extras: the modelling script tags each loose part with its debris radius
HZ_REFLECT()
typedef struct GrDronePartExtras {
  f32 radius;
} GrDronePartExtras;

// every archetype's config and the drone wave rules; indexed by GrDroneType
HZ_REFLECT()
ECS_COMPONENT(GrDroneConfig) {
  FixedArray(GrDroneArchetype, GR_DRONE_TYPES) types;
  u32 max_attackers; // most drones attacking at once
  u32 bolt_capacity; // bolt pool size: a spawn past it is dropped
  f32 bolt_max_age;  // seconds
};

HZ_REFLECT()
typedef struct GrSpawnGroup {
  u32 type; // GrDroneType
  u32 count;
  f32 delay;    // seconds after the wave starts
  f32 interval; // seconds between spawns in the group
} GrSpawnGroup;

HZ_REFLECT()
typedef struct GrWave {
  String32Bytes name;
  FixedArray(GrSpawnGroup, GR_MAX_WAVE_GROUPS) groups;
} GrWave;

// the wave script and its rules
HZ_REFLECT()
ECS_COMPONENT(GrWaveConfig) {
  FixedArray(GrWave, GR_MAX_WAVES) waves;
  f32 intro_delay;
  f32 intermission;
  u32 clear_bonus_per_wave;
  u32 accuracy_bonus_per_percent; // read by the run's end
  f32 announce_delay;             // later waves are announced this far into the intermission
};

// drone debug readouts, driven by --commands `set:`
HZ_REFLECT()
ECS_COMPONENT(GrDroneDebug) {
  b32 nav_audit;      // count drone moves that cross level geometry or leave every room and doorway
  f32 log_interval;   // seconds between audit / drone log lines; 0 logs only on change
  b32 log_drones;     // log type, health and position of each drone
  f32 damage_per_second; // automation: every active drone takes this through GrPendingDamage, as weapon hits do
  b32 auto_secure;       // automation: a broken sector is secured without walking to its capture point
};

typedef enum GrDroneState {
  GR_DRONE_WARPING = 0,
  GR_DRONE_ACTIVE = 1,
  GR_DRONE_DEAD = 2,
} GrDroneState;

// what happened to a drone this frame (events_frame == GrClock.frame); fx, audio and the look read them
typedef enum GrDroneEvent {
  GR_DRONE_EV_SPAWNED = 1u << 0,   // warp-in fx, warp sound, hum start: the drone's first update
  GR_DRONE_EV_TELEGRAPH = 1u << 1, // charge sound
  GR_DRONE_EV_FIRED = 1u << 2,     // muzzle glow + fire sound at `eye` along fire_dir
  GR_DRONE_EV_HIT = 1u << 3,       // a weapon hit landed
  GR_DRONE_EV_DIED = 1u << 4,      // explosion, debris, sound, shake; the entity is destroyed this frame
  GR_DRONE_EV_SMOKE = 1u << 5,     // a damage smoke puff at the position
} GrDroneEvent;

// one drone; the entity is its type's model (EcsModelNode sockets as children) and WorldTransform is its pose
typedef struct GrDrone {
  u32 type;  // GrDroneType
  u32 state; // GrDroneState
  u32 run;   // GrWaves.run it was spawned in; a newer run destroys it
  v3 position;
  v3 velocity;
  f32 health;
  f32 age;
  f32 phase;
  f32 orbit_dir;
  f32 orbit_speed;
  f32 preferred_distance;
  f32 preferred_altitude;
  f32 retarget_timer;
  f32 attack_timer;
  f32 telegraph_timer;
  u32 burst_left;
  f32 burst_timer;
  f32 los_timer;
  b32 line_of_sight;
  f32 smoke_timer;
  b32 holds_token;
  b32 wants_token;    // asks the token system this frame
  b32 release_token;  // gives it back this frame
  // presentation: the look and fx packages read these, never the renderer
  f32 scale;     // warp-in pop
  f32 yaw;
  f32 pitch;
  f32 bank;
  f32 hit_flash; // armour emissive amount, 1 on a hit, -9/s
  f32 charge;    // 0..1 while telegraphing
  f32 thrust;    // speed / max speed
  f32 gait;        // walkers: the stride phase, radians
  f32 gait_amount; // walkers: 0 standing .. 1 full stride
  f32 aim_pitch;   // walkers: the torso's lean onto the target, + up
  b32 retreating;   // falling back to GrWaves.retreat_point; holds fire
  u32 retreat_seen; // GrWaves.retreat_serial last taken
  v3 muzzle;     // the <model_node>Muzzle socket in model space (GrDroneModel), copied at spawn
  v3 eye;        // the muzzle socket in world space, last presentation
  v3 fire_dir;   // the bolt direction when GR_DRONE_EV_FIRED
  v3 hit_dir;    // the latest weapon hit direction (debris push on death)
  u32 events;
  u32 events_frame;
  // nav audit: this frame's move, valid when audit_frame == GrClock.frame
  u32 audit_frame;
  b32 audit_crossed; // the move's segment hit level geometry
  b32 audit_outside; // it ended outside every room and doorway
  Random rng;
} GrDrone;
ECS_COMPONENT_TYPEDEF(GrDrone);

// damage the weapons add to a drone this frame; the drone damage system applies and clears it
typedef struct GrPendingDamage {
  f32 amount;
  v3 impulse;  // sum of direction * knockback
  v3 direction; // the latest shot direction
  u32 hits;
} GrPendingDamage;
ECS_COMPONENT_TYPEDEF(GrPendingDamage);

typedef enum GrBoltState {
  GR_BOLT_FREE = 0, // in the pool; hidden
  GR_BOLT_FLYING = 1,
  GR_BOLT_HIT_PLAYER = 2, // terminal states: `end_frame` is the frame they happened; the row is free again after it
  GR_BOLT_HIT_WALL = 3,
  GR_BOLT_EXPIRED = 4,
} GrBoltState;

// one pooled enemy bolt: `bolt_capacity` rows made once, reused; the look and fx read them, never the renderer
typedef struct GrBolt {
  u32 type; // GrDroneType that fired it (colour, size)
  u32 state;
  u32 run;         // GrWaves.bolt_run it was fired in; a newer one retires it
  u32 end_frame;   // GrClock.frame of the terminal state
  u32 spawn_frame; // GrClock.frame it was fired (muzzle glow, fire sound)
  v3 position;
  v3 velocity;
  f32 radius;
  f32 damage;
  f32 splash;
  f32 age;
  f32 spin;       // glow sprite rotation, 4 rad/s
  f32 trail_timer;
  b32 trail;      // a trail glow this frame, every 0.012 s
  v3 hit_point;   // terminal: where it struck (impact fx at the capsule sample or the wall)
  v3 hit_normal;
  f32 player_damage; // terminal: damage to the player (direct, or splash)
  v3 damage_from;    // where the damage came from (the damage arc)
  u32 clone_hit;     // terminal on a trooper: its squad slot + 1 (shown as a wall impact)
} GrBolt;
ECS_COMPONENT_TYPEDEF(GrBolt);

typedef struct GrBoltRequest {
  u32 type;
  u32 run; // GrWaves.bolt_run
  v3 origin;
  v3 direction;
} GrBoltRequest;

// this frame's drone fire events in drone row order; free pool rows claim them through the atomic cursor
typedef struct GrBoltRequests {
  GrBoltRequest items[GR_MAX_DRONES];
  u32 count;
  u32 claimed; // atomic: the next request a free row takes; past `count` means the row stays free
  u32 frame;
  u32 pool;    // bolt rows created
  u32 live;    // flying at the end of the frame
  u32 dropped; // requests that found no free row (a full pool), run total
} GrBoltRequests;
ECS_COMPONENT_TYPEDEF(GrBoltRequests);

typedef enum GrWavePhase {
  GR_WAVES_IDLE = 0,
  GR_WAVES_INCOMING = 1,
  GR_WAVES_ACTIVE = 2,
  GR_WAVES_INTERMISSION = 3,
  GR_WAVES_COMPLETE = 4,
  GR_WAVES_SECURE = 5, // the quota is met: the survivors fall back, the player takes the capture point
} GrWavePhase;

typedef struct GrWaveSpawn {
  f32 time;
  u32 type;
} GrWaveSpawn;

// the wave director; consumers (HUD, audio, run flow) react when a serial moves
typedef struct GrWaves {
  // this frame's run facts for the drone and bolt systems, written first thing every frame
  u32 frame;   // GrClock.frame
  f32 dt;      // GrClock.run_dt
  b32 live;    // drones and bolts update: playing or the end-of-run outro
  b32 pinned;  // drones hold their position
  u32 phase; // GrWavePhase
  i32 wave;  // index of the current wave, -1 before the first
  u32 total_waves;
  f32 timer;
  f32 clock;
  GrWaveSpawn schedule[GR_MAX_WAVE_SPAWNS];
  u32 schedule_len;
  u32 schedule_next;
  i32 last_spawn_area;
  i32 last_spawn_index;
  u32 cleared;   // waves cleared this run
  u32 remaining; // drones alive + still to spawn (the HUD's hostiles)
  u32 run;       // bumps when a run starts or the flow clears the drones; older drones and bolts retire
  u32 bolt_run;  // bumps with run and on the flow's bolt clear; older bolts retire
  u32 restart_seen;
  u32 prev_state;
  u32 clear_seen, drones_clear_seen, bolts_clear_seen; // GrFlow clear serials handled
  // drones spawned on the director's last frame: their rows land after that frame, so the field misses them once more
  u32 spawned_last;
  u32 spawned_last_frame;
  u32 spawned_last_run;
  u32 debug_spawned[GR_DRONE_TYPE_COUNT]; // run the debug spawn request was served in
  // wave announce: at once for wave 0 and announce_delay into the intermission after
  b32 announce_pending;
  f32 announce_timer;
  i32 announce_wave;
  u32 announce_serial;
  u32 started_serial; // a wave started
  i32 cleared_wave;   // the wave just cleared
  u32 cleared_bonus;
  u32 cleared_serial;
  u32 victory_serial; // the run was won
  u32 spawned_total;
  // the Kamino campaign: one phase per enemy sector (KmSector), the front line advancing as each is secured
  u32 sector;          // the sector the droids hold this phase
  u32 hold_zone;       // its nav zone: droids fight from it
  u32 killed_phase;    // kills this phase
  f32 quiet_time;      // seconds since this front's last kill
  u32 spawned_phase;   // droids put into this phase
  u32 b2_spawned;
  f32 spawn_timer;
  u32 kills_seen;      // GrDroneField.kills_total last counted
  v3 objective;        // the capture point while securing
  b32 objective_active;
  u32 retreat_serial;  // bumps when the survivors fall back
  u32 retreat_zone;
  v3 retreat_point;
  u32 fallback_serial; // the fall-back banner
  Random rng;
} GrWaves;
ECS_COMPONENT_TYPEDEF(GrWaves);

// the token budget, written by one system per frame in row order so a run replays the same
typedef struct GrDroneTokens {
  u32 attackers;
  u32 run; // GrWaves.run the count belongs to; a new run starts from zero
} GrDroneTokens;
ECS_COMPONENT_TYPEDEF(GrDroneTokens);

typedef struct GrDroneSlot {
  EcsEntity entity;
  v3 position;
  v3 velocity;
  f32 radius;
  u32 type;
  b32 targetable; // active: weapons and the crosshair test it
} GrDroneSlot;

// a kill: the score event, and what the explosion, debris and sound read
typedef struct GrDroneKill {
  u32 type;
  u32 score;
  v3 position;
  v3 velocity;  // debris inherit velocity * 0.5 + direction * 3
  v3 direction; // the killing shot
  f32 yaw;      // the pose the debris parts start from
  f32 pitch;
  f32 bank;
} GrDroneKill;

// every live drone as the frame ended: hit tests, separation, radar and the director read it; kills are score events
typedef struct GrDroneField {
  GrDroneSlot drones[GR_MAX_DRONES];
  u32 count;
  u32 alive;
  GrDroneKill kills[GR_MAX_DRONES]; // kills this frame
  u32 kill_count;
  u32 kill_frame;
  u32 kills_total;
  u32 kill_serial; // bumps on a frame with kills
  f32 shake;       // camera shake from this frame's kills, max(0, 0.55 - 0.02 d) * scale each
  u32 audit_steps;
  u32 audit_crossings;
  u32 audit_outside;
  f32 log_timer;
  KmSquadView squad; // written by km_squad_publish_system after the droids' consolidation
} GrDroneField;
ECS_COMPONENT_TYPEDEF(GrDroneField);

// bolt damage to the player this frame, in bolt row order; gr_drone_intake_system hands it to GrPlayerIntake
typedef struct GrBoltHits {
  GrPlayerHit hits[GR_MAX_PLAYER_HITS];
  u32 count;
  u32 frame;
  f32 shake; // 0.2 per splash-bolt wall impact
  u32 total; // run total, for the log
  f32 clone_damage[KM_SQUAD_MAX]; // this frame's bolt damage per squad slot
} GrBoltHits;
ECS_COMPONENT_TYPEDEF(GrBoltHits);

// the config resolved for systems: the Settings components copied when they change
typedef struct GrDroneRules {
  GrDroneConfig drones;
  GrWaveConfig waves;
  GrDroneDebug debug;
  b32 ready;
} GrDroneRules;
ECS_COMPONENT_TYPEDEF(GrDroneRules);

// a loose part that flies apart on death: its node, debris radius and model-space placement
typedef struct GrDronePart {
  u32 node; // index in ModelBlobAsset.nodes; its `mesh` is what the debris draws
  f32 radius;
  v3 position;
  quat rotation;
} GrDronePart;

// one archetype's model, read from its cooked node table once the asset is ready
typedef struct GrDroneModel {
  BlobAssetRef(ModelBlobAsset) model;
  v3 muzzle; // <model_node>Muzzle in model space: where bolts spawn
  GrDronePart parts[GR_MAX_DRONE_PARTS];
  u32 part_count;
  b32 ready;
} GrDroneModel;

// models/drones/{scout,striker,sentinel}.glb, indexed by GrDroneType
typedef struct GrDroneAssets {
  GrDroneModel types[GR_DRONE_TYPE_COUNT];
  b32 ready; // every type's model is ready
} GrDroneAssets;
ECS_COMPONENT_TYPEDEF(GrDroneAssets);

typedef struct GrDroneRayHit {
  EcsEntity entity;
  u32 slot;
  f32 distance;
  v3 point;
  v3 normal;
} GrDroneRayHit;

// targetable drones along a ray within max_distance, nearest first, at most `limit`; returns the count
u32 gr_drones_raycast_all(const GrDroneField *f, v3 origin, v3 dir, f32 max_distance, u32 limit, GrDroneRayHit *out);
// the nearest targetable drone along a ray
b32 gr_drones_raycast(const GrDroneField *f, v3 origin, v3 dir, f32 max_distance, GrDroneRayHit *out);

void gr_drones_init(EcsWorld *w, AssetSystem *assets);
// the first flush: config, model parts and the wave director (its spawns are rows from the next frame: the scope applies them)
void gr_drones_schedule_spawn(EcsWorld *w);
// the second flush: damage, AI, tokens, bolts and the consolidations
void gr_drones_schedule(EcsWorld *w);
