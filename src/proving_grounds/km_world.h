// Kamino: Tipoca City built in code (platforms on stilts, bridges, the cloning facility) and the level tables it implies.
#pragma once
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/typedefs.h"
#include "rendering/renderer.h"
#include "rendering/features/clustered_lights.h"
#include "gr_level.h"

#define KM_OCEAN_Y -38.0f
#define KM_PLATFORM_COUNT 4
#define KM_ROOM_COUNT 3

typedef enum KmSector {
  KM_SECTOR_AUREK = 0,  // A: landing pad, the squad's start
  KM_SECTOR_BESH = 1,   // B: junction
  KM_SECTOR_CRESH = 2,  // C: plaza with the dais
  KM_SECTOR_DORN = 3,   // D: the landing craft's platform
  KM_SECTOR_LOBBY = 4,  // E: facility lobby
  KM_SECTOR_CORRIDOR = 5, // F: security corridor
  KM_SECTOR_HALL = 6,   // G: cloning hall
  KM_SECTOR_COUNT = 7,
} KmSector;

typedef enum KmMaterial {
  KM_MAT_HULL = 0, // white Kaminoan hull
  KM_MAT_DECK,     // wet deck plating
  KM_MAT_METAL,    // steel trim, rails
  KM_MAT_DARK,     // dark structure, stilts, droid hardware
  KM_MAT_LIGHT,    // cyan emissive strips
  KM_MAT_RED,      // red emissive (separatist)
  KM_MAT_WARM,     // warm emissive (interior light panels)
  KM_MAT_COUNT,
} KmMaterial;

// a round platform: deck top at y 0
typedef struct KmPlatform {
  v2 center; // x, z
  f32 radius;
} KmPlatform;

// an interior room: floor at y 0
typedef struct KmRoom {
  v2 min, max; // x, z
  f32 ceiling;
} KmRoom;

// the built city: materials, then one build on the main lane once they are ready
typedef struct KmWorld {
  HzHandleT(MaterialBlobAsset) assets[KM_MAT_COUNT];
  HzHandleT(ModelBlobAsset) gunship, lander;
  b32 built;
  u32 instances; // renderer instances spawned
  u32 colliders;
  u32 lights;
} KmWorld;
ECS_COMPONENT_TYPEDEF(KmWorld);

// the storm: lightning strikes on a random clock, each a few flickers of a light high over the player and the sky
typedef struct KmStorm {
  f32 timer;      // seconds to the next strike
  f32 age;        // seconds since the strike began; < 0 between strikes
  f32 flash;      // 0..1 this frame: the light, the sky and the fill read it
  v3 bolt;        // where it struck, world
  f32 thunder_in; // seconds until its thunder reaches the player; < 0 none pending
  u32 thunder_serial; // bumps when the thunder arrives (audio)
  f32 thunder_gain;   // nearer strikes are louder
  u32 seed;
  RendererLocalLightHandle light;
  b32 light_ready;
} KmStorm;
ECS_COMPONENT_TYPEDEF(KmStorm);

// a facility blast door: a slab that grinds up into the wall when the droids fall back through it
#define KM_DOOR_COUNT 2
typedef struct KmDoor {
  u32 index;      // 0 lobby -> corridor, 1 corridor -> hall
  v3 closed;      // the slab's centre when shut
  v3 half;
  f32 open;       // 0 shut .. 1 up
  EcsEntity body; // its static collider
} KmDoor;
ECS_COMPONENT_TYPEDEF(KmDoor);

// the sector's name for the HUD
CString km_sector_name(u32 sector);
// inside the scope: the storm's clock
void km_storm_schedule(EcsWorld *w);
// main lane, app_init: the KmWorld resource and its material loads
void km_world_init(EcsWorld *w, AssetSystem *assets);
// main lane, outside the scope: builds the city once its materials are ready
void km_world_main(EcsWorld *w);
// the level tables the city implies: areas, zones, portals, spawn, health packs, walkable tops (from `a`)
void km_layout_fill(GrLevel *l, Allocator *a);
