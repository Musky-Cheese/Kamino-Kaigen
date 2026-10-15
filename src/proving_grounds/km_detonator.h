// Kamino thermal detonators: G throws one on an arc; it bounces, beeps through its fuse and bursts, hurting droids and the thrower alike.
#pragma once
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/typedefs.h"

#define KM_DET_CARRY 3        // per front
#define KM_DET_FUSE 2.6f      // seconds
#define KM_DET_RADIUS 7.5f    // droids inside take damage
#define KM_DET_DAMAGE 170.0f  // at the centre, falling to 0 at the edge
#define KM_DET_SELF_RADIUS 5.0f
#define KM_DET_SELF_DAMAGE 45.0f
#define KM_DET_LIVE_MAX 4

// one thrown detonator in flight or ticking on the deck
typedef struct KmDetonator {
  v3 position;
  v3 velocity;
  f32 fuse;      // seconds left
  f32 spin;
  f32 beep_timer;
  b32 resting;
  u32 bounces;
} KmDetonator;
ECS_COMPONENT_TYPEDEF(KmDetonator);

// the player's pouch and the throw clock
typedef struct KmDetonators {
  u32 count;      // detonators carried
  u32 run;        // GrWaves.run the pouch was filled for
  i32 wave;       // the front it was refilled on
  f32 cooldown;   // seconds until the next throw
  u32 live;       // detonators out this frame
  u32 thrown_total;
  b32 look_ready; // the sphere material is built
  u32 throw_serial; // bumps per throw (HUD flash, audio)
} KmDetonators;
ECS_COMPONENT_TYPEDEF(KmDetonators);

// main lane, app_init: the pouch
void km_detonator_init(EcsWorld *w);
// inside the scope after the drones and before the transforms: throws, flight, bounces and bursts
void km_detonator_schedule(EcsWorld *w);
