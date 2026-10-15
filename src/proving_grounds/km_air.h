// Kamino air battle: vulture droids and Republic gunships wheeling over Tipoca City, trading fire; set dressing, never a threat.
#pragma once
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/random.h"
#include "lib/typedefs.h"
#include "gr_fx.h"

#define KM_VULTURES 6
#define KM_GUNSHIPS 2
#define KM_FLYERS (KM_VULTURES + KM_GUNSHIPS)
#define KM_AIR_FX_MAX 48

typedef enum KmFlyerKind {
  KM_FLYER_VULTURE = 0,
  KM_FLYER_GUNSHIP = 1,
} KmFlyerKind;

typedef enum KmFlyerState {
  KM_FLYER_FLYING = 0,
  KM_FLYER_FALLING = 1, // hit: spirals down trailing smoke, then bursts
  KM_FLYER_GONE = 2,    // below the sea until it rejoins the fight
} KmFlyerState;

// one aircraft on a circuit around the fight: its orbit, its gun and its fate
typedef struct KmFlyer {
  u32 kind;  // KmFlyerKind
  u32 index; // 0..KM_FLYERS-1
  u32 state; // KmFlyerState
  f32 angle;     // orbit phase, radians
  f32 rate;      // rad/s, signed: the orbit's direction
  f32 radius;    // orbit radius, m
  f32 altitude;  // mean height over the deck, m
  f32 bob;       // altitude swing, m
  f32 bob_phase;
  v3 position;
  v3 velocity;
  f32 roll;
  f32 fire_timer;
  u32 burst_left;
  f32 burst_timer;
  i32 target;    // flyer index it shoots at, -1 none
  f32 state_time;
  f32 smoke_timer;
  Random rng;
} KmFlyer;
ECS_COMPONENT_TYPEDEF(KmFlyer);

// the fight's effects this frame (tracers, smoke, the bursts), for the fx consolidation
typedef struct KmAirFx {
  GrFxRequest items[KM_AIR_FX_MAX];
  u32 count;
  u32 frame;
} KmAirFx;
ECS_COMPONENT_TYPEDEF(KmAirFx);

// the squadron: its models and the flyers' last positions (targets read them without touching the rows)
typedef struct KmAir {
  HzHandleT(ModelBlobAsset) vulture, gunship;
  b32 spawned;
  v3 centre;      // the fight drifts after the player
  f32 kill_timer; // seconds until a vulture is shot down
  v3 positions[KM_FLYERS];
  u32 alive_mask; // flyers in the air this frame
  u32 seed;
} KmAir;
ECS_COMPONENT_TYPEDEF(KmAir);

// main lane, app_init: the KmAir resource and its model loads
void km_air_init(EcsWorld *w, AssetSystem *assets);
// inside the scope before ecs_transform_schedule: the flyers' circuits, fire and falls
void km_air_schedule(EcsWorld *w);
// inside the scope after gr_fx_schedule: the fight's tracers, smoke and explosions
void km_air_schedule_fx(EcsWorld *w);
