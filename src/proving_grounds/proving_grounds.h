// Proving Grounds: components and resources. Components live in a header because reflection codegen scans headers only.
#pragma once
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"

// dev switches for debugging and scripted runs, set with --commands `set:GrDevSwitches.<field>=<value>`
HZ_REFLECT()
ECS_COMPONENT(GrDevSwitches) {
  i32 quality;     // render preset: 0 low, 1 medium, 2 high, 3 ultra
  b32 start;       // starts a run without the title menu or pointer lock
  i32 screen;      // the state screen over the run: 0 none, 1 pause, 2 results
  b32 hold_waves;  // the wave script stops, no drone spawns
  b32 pin_drones;  // drones hold their position
  b32 god;         // the player takes no damage
  i32 weapon;      // the gun in hand: 0 rifle (the start gun), 1 breacher, 2 lancer
  b32 ads;         // the right mouse held: aim down sights / scope
  b32 fire;        // the left mouse held: firing
  b32 pose;        // places the player at pose_*; 0 is a valid coordinate, so the flag is the request
  f32 pose_x;      // feet, meters
  f32 pose_y;
  f32 pose_z;
  f32 pose_yaw;    // radians, 0 faces -Z
  f32 pose_pitch;  // radians, + looks up
  b32 aim;         // the view snaps onto aim_* (after any pose)
  f32 aim_x;
  f32 aim_y;
  f32 aim_z;
  b32 scout;       // spawns a scout at scout_*
  f32 scout_x;
  f32 scout_y;
  f32 scout_z;
  b32 striker;     // spawns a striker at striker_*
  f32 striker_x;
  f32 striker_y;
  f32 striker_z;
  b32 sentinel;    // spawns a sentinel at sentinel_*
  f32 sentinel_x;
  f32 sentinel_y;
  f32 sentinel_z;
  b32 freeze;      // the simulation pauses, rendering continues
  // turn off the volumetric fog, bloom and planar reflections
  b32 no_fog;
  b32 no_bloom;
  b32 no_planar;
  b32 nan;         // NaN/Inf pixels show in magenta
};

// the first-person view, or the menu orbit on the title: the view the camera entity shows
HZ_REFLECT()
ECS_COMPONENT(GrView) {
  f32 yaw;   // radians, 0 faces -Z
  f32 pitch; // radians
  f32 fov;   // vertical degrees
};

// the game state machine
typedef enum GrGameState {
  GR_STATE_LOADING = 0,
  GR_STATE_TITLE = 1,
  GR_STATE_PLAYING = 2,
  GR_STATE_PAUSED = 3,
  GR_STATE_ENDING = 4,
  GR_STATE_RESULTS = 5,
} GrGameState;

typedef enum GrWeaponId {
  GR_WEAPON_RIFLE = 0,
  GR_WEAPON_BREACHER = 1,
  GR_WEAPON_LANCER = 2,
  GR_WEAPON_COUNT = 3,
} GrWeaponId;

typedef enum GrDroneType {
  GR_DRONE_SCOUT = 0,
  GR_DRONE_STRIKER = 1,
  GR_DRONE_SENTINEL = 2,
  GR_DRONE_TYPE_COUNT = 3,
} GrDroneType;

// the staged load: each stage shows its progress for at least one frame
typedef enum GrLoadStage {
  GR_LOAD_MODELS = 0, // 0.02 + 0.33 f "Loading models"
  GR_LOAD_AUDIO,      // 0.4 "Synthesizing audio"
  GR_LOAD_LIGHT,      // 0.6 "Baking light"
  GR_LOAD_SHADERS,    // 0.75 "Compiling shaders": renderer_pipelines_ready
  GR_LOAD_READY,      // 1 "Ready"
  GR_LOAD_DONE,
} GrLoadStage;

// load and game state
typedef struct GrFlow {
  u32 state;      // GrGameState
  u32 load_frame; // clock frame the load finished on
  u32 load_stage; // GrLoadStage
  f32 load_progress;
  u32 stage_frames;    // frames the current stage has shown
  b32 debug_controls;  // a dev-switch run: takes input without pointer lock and never pauses on lock or focus loss
  f32 unlocked_time;   // playing without controls: > 0.35 s pauses
  b32 was_focused;     // the window had focus last frame (the focus-regained edge)
  f32 end_timer;       // ending -> results
  b32 victory;
  f32 slow_motion;     // seconds left of the wave-clear beat (counts real dt)
  f32 time_scale;      // this frame's run time scale
  u32 lock_serial;     // a pointer lock/unlock request; the main lane applies os_lock_mouse(lock_value) once per serial
  b32 lock_value;
  b32 audio_suspended; // the master output is suspended (pause, focus lost)
  b32 audio_unlocked;  // a user gesture unlocked audio (web)
  // clear requests to the run's other owners: each bumps once per clear
  u32 clear_serial;        // drones, effects, casings, waves, timers (run start, quit)
  u32 drones_clear_serial; // drones (results)
  u32 bolts_clear_serial;  // drone bolts (victory)
  u32 results_serial;      // the results screen opened
  u32 end_seen;            // GrRun.end_serial handled
  u32 slow_seen;           // GrRun.slow_motion_serial handled
  // the dev switches as applied: levels latch once the load is done
  b32 sw_start;
  i32 sw_screen;
  i32 sw_quality;
} GrFlow;
ECS_COMPONENT_TYPEDEF(GrFlow);

// the frame clock: dt (the frame's dt, long frames clamped) drives the simulation; time is its sum and the renderer's time
typedef struct GrClock {
  f32 dt;            // the frame's step: presentation (arena, effects, render clock) runs on it
  f32 run_dt;        // the run's step: dt x slow motion while playing with controls or ending, else 0 (title, paused, results, freeze)
  b32 controls;      // the player reads input this frame (playing and locked, or a dev-switch run)
  b32 live;          // playing: run time, timers, health packs and the wave script advance (not while ending)
  f32 time;          // the sum of dt
  u32 frame;         // frames since start
} GrClock;
ECS_COMPONENT_TYPEDEF(GrClock);

// the editor session (native builds; zero elsewhere): while editing the run is frozen and the camera is the editor's
typedef struct GrEditor {
  b32 open;    // the editor is up
  b32 playing; // the game runs as a preview, the editor shell hidden
  v3 eye;      // the editor camera, written by the shell after the scope, applied by gr_view_system
  quat look;
} GrEditor;
ECS_COMPONENT_TYPEDEF(GrEditor);

// a dev-switch drone spawn request: one per archetype
typedef struct GrDroneSpawn {
  b32 requested;
  v3 position;
} GrDroneSpawn;

// the pose and aim switches as last seen, to turn a `set:` into a one-shot request
typedef struct GrPoseSwitches {
  b32 pose;
  v3 feet;
  f32 yaw;
  f32 pitch;
} GrPoseSwitches;

typedef struct GrAimSwitches {
  b32 aim;
  v3 target;
} GrAimSwitches;

// the dev switches resolved once per frame for per-entity systems
typedef struct GrDev {
  u32 state;          // GrGameState the flow is in
  i32 quality;        // the renderer level: the settings' (High until one is set)
  b32 resolved;       // gr_dev_resolve ran: quality comes from the loaded settings, not the zeroed resource
  b32 hold_waves;
  b32 pin_drones;
  b32 god;
  u32 weapon;         // GrWeaponId in hand
  b32 ads;            // the zoom target; the view narrows to the gun's ADS fov
  b32 fire;           // the trigger held
  GrDroneSpawn drones[GR_DRONE_TYPE_COUNT]; // GrDroneType-indexed spawn requests
  v3 feet;            // where the player stands, the spawn or the pose
  v3 eye;             // feet + eye height
  f32 yaw;            // the view: the spawn's, the pose's, or the aim's
  f32 pitch;
  f32 fov;            // vertical degrees: the hip fov or the gun's ADS fov
  u32 restart_serial; // bumps on every run start: deploy, restart, the start switch
  b32 debug_controls; // GrFlow.debug_controls: the run takes input without pointer lock
  b32 fire_pressed;   // the simulated buttons' press edges
  b32 ads_pressed;
  b32 aim;            // the aim switch is set, onto aim_target
  v3 aim_target;
  u32 pose_serial;    // bumps when the pose switches change: the player re-places
  u32 aim_serial;     // bumps when the aim switches change: the view re-aims from the player's eye
  GrPoseSwitches pose_seen; // the switches the serials were taken from
  GrAimSwitches aim_seen;
} GrDev;
ECS_COMPONENT_TYPEDEF(GrDev);
