// Proving Grounds flow and HUD model: settings, UI intents, the run's totals, the HUD read model.
#pragma once
#include "ecs/ecs.h"
#include "lib/hzmath.h"
#include "lib/typedefs.h"
#include "proving_grounds.h"

#define GR_SLOW_MOTION_TIME 0.9f
#define GR_UNLOCKED_PAUSE 0.35f
#define GR_WAVE_TOTAL 3          // waves in a run
#define GR_HUD_MAX_ARCS 8
#define GR_RADAR_MAX_CONTACTS 48

typedef enum GrQuality {
  GR_QUALITY_LOW = 0,
  GR_QUALITY_MEDIUM,
  GR_QUALITY_HIGH,
  GR_QUALITY_ULTRA,
  GR_QUALITY_COUNT,
} GrQuality;

// settings: per session, no local save; sensitivity -> player, volume -> master bus, quality -> look
typedef struct GrSettings {
  i32 quality; // GrQuality
  f32 sensitivity;
  f32 volume;
  u32 serial; // bumps on every change
} GrSettings;
ECS_COMPONENT_TYPEDEF(GrSettings);

// menu intents, one bit each, queued by the main-lane UI and consumed by gr_run_system (window edges are GrInput's)
typedef enum GrAction {
  GR_ACTION_DEPLOY = 1u << 0,  // title / results "Deploy"
  GR_ACTION_RESUME = 1u << 1,
  GR_ACTION_RESTART = 1u << 2,
  GR_ACTION_QUIT = 1u << 3,    // "Main menu"
} GrAction;

// written by the main lane: the load gates before the scope, the UI's clicks and settings after it
typedef struct GrActions {
  u32 queue;
  b32 settings_changed;
  GrSettings settings; // the UI's values when settings_changed
  b32 pipelines_ready; // renderer_pipelines_ready this frame
  f32 models;          // the level's meshes, models, props and signs spawned (0 or 1)
  b32 audio;           // the clips are rendered (GrAudio.ready)
  b32 light;           // GrLoadGates.light, copied for the flow
  b32 load_failed;
} GrActions;
ECS_COMPONENT_TYPEDEF(GrActions);

// the "Baking light" stage's gate and a fatal load error; the look sets light when its bakes are done
typedef struct GrLoadGates {
  b32 light;  // the volumetric shadow and the probes are baked
  b32 failed; // an unrecoverable load error: the flow stays in loading and shows the fatal screen
} GrLoadGates;
ECS_COMPONENT_TYPEDEF(GrLoadGates);

// the flow's own sound emitter entity: the run-event sounds no other owner plays
typedef struct GrFlowVoice {
  u32 hitmarkers; // played this run, for the log
} GrFlowVoice;
ECS_COMPONENT_TYPEDEF(GrFlowVoice);

typedef enum GrBannerText {
  GR_BANNER_NONE = 0,
  GR_BANNER_WAVE,           // "Wave N" / wave name
  GR_BANNER_WAVE_CLEARED,   // "Wave cleared" / "+N bonus"
  GR_BANNER_TRIAL_COMPLETE, // "Trial complete" / "Accuracy bonus +N"
  GR_BANNER_SIGNAL_LOST,    // "Signal lost" / "Operator down"
  GR_BANNER_FALLBACK,       // "They're falling back" / "Secure <sector>"
} GrBannerText;

typedef enum GrBannerKind {
  GR_BANNER_KIND_WAVE = 0, // cyan glow
  GR_BANNER_KIND_CLEAR,    // amber
  GR_BANNER_KIND_ALERT,    // red
} GrBannerKind;

// the results screen payload
typedef struct GrResults {
  b32 victory;
  u32 score;
  u32 best;
  b32 new_best;
  u32 kills;
  f32 accuracy;
  u32 waves_cleared;
  u32 total_waves;
  f32 seconds;
} GrResults;

// values for the final pass: damage and heal flashes, low health, scope
typedef struct GrFeedbackValues {
  f32 damage;
  f32 heal;
  f32 low_health;
  f32 scope;
} GrFeedbackValues;

// the run's totals and presentation events (gr_run_consolidate); read by the flow, the HUD, audio and the final pass
typedef struct GrRun {
  u32 run_serial; // GrDev.restart_serial this run started from
  u32 score;
  u32 kills;
  f32 run_time;
  u32 best;     // per session (no local save)
  f32 accuracy; // shots with a hit / shots
  u32 score_serial; // bumps with each score delta (the score pop)
  u32 score_delta;
  i32 wave_index;
  u32 wave_total;
  u32 end_serial; // the run ended: the flow switches to ending
  u32 ended_run;  // the run_serial the end last ran for: once per run
  b32 end_victory;
  u32 slow_motion_serial; // a wave clear started the slow-motion beat
  GrResults results;
  u32 results_seen;
  GrFeedbackValues feedback;
  // event serials consumed
  u32 seen_kills, seen_incoming, seen_started, seen_cleared, seen_over, seen_fallback;
} GrRun;
ECS_COMPONENT_TYPEDEF(GrRun);

typedef struct GrDamageArc {
  f32 start;
  f32 angle; // radians, 0 ahead, + right
} GrDamageArc;

// the HUD and menus read model (gr_hud_consolidate), drawn by the main-lane UI; times are GrClock.time
typedef struct GrHud {
  f32 time;
  u32 state; // GrGameState
  // loading screen
  u32 load_stage;
  f32 load_progress;
  // HUD root
  b32 hud_visible;
  b32 critical;
  i32 wave_index;
  u32 wave_total;
  u32 hostiles;
  u32 score;
  u32 score_delta;
  f32 score_pop_start; // < 0: never popped
  f32 reticle_spread;
  f32 fov; // vertical degrees
  b32 on_target;
  b32 crosshair_hidden;
  u32 crosshair_style; // GrCrosshair
  f32 hitmarker_start; // < 0: never
  b32 hitmarker_kill;
  GrDamageArc arcs[GR_HUD_MAX_ARCS];
  u32 arc_count;
  b32 reload_show;
  b32 reloading;
  b32 prompt_show;
  u32 prompt_weapon;
  f32 health;
  f32 max_health;
  u32 ammo;
  u32 magazine;
  b32 swapping;
  b32 ammo_low;
  u32 weapon;
  // menus
  u32 best;
  GrResults results;
  // seen serials
  u32 seen_kills, seen_score;
  u32 run_serial; // GrRun.run_serial seen: a new run resets the panels
} GrHud;
ECS_COMPONENT_TYPEDEF(GrHud);

#define GR_RADAR_COLOR_GUN 3  // a gun on the ground
#define GR_RADAR_COLOR_PACK 4 // a health pack

typedef enum GrRadarIcon {
  GR_RADAR_DOT = 0,
  GR_RADAR_DIAMOND,
  GR_RADAR_CROSS,
} GrRadarIcon;

// one motion-tracker contact: world metres relative to the player (hud_map rotates by GrRadar.heading)
typedef struct GrRadarContact {
  f32 x;
  f32 z;
  f32 dy;
  u32 icon;  // GrRadarIcon
  u32 color; // GrDroneType for drones, else GR_RADAR_COLOR_GUN / _PACK
} GrRadarContact;

// the radar's contacts in paint order: drones, ground guns, packs (gr_radar_system)
typedef struct GrRadar {
  u32 count;
  GrRadarContact contacts[GR_RADAR_MAX_CONTACTS];
  f32 heading; // hud_map heading: -yaw puts the player's forward up
  f32 sweep; // radians, += 2.2 run dt
  b32 gathering; // the run updated this frame: the contacts were rebuilt
} GrRadar;
ECS_COMPONENT_TYPEDEF(GrRadar);

// the entity that carries the swap prompt's WorldUi widget
ECS_TAG(GrPromptTag);

// main lane, app_init: the flow, settings, run, HUD and radar resources
void gr_flow_init(EcsWorld *w);
// main lane before the scope, after gr_level_frame: the load gates into GrActions
void gr_flow_collect(EcsWorld *w);
// in the scope before the player: settings onto their owners (sensitivity -> GrPlayer)
void gr_flow_schedule_settings(EcsWorld *w);
// in the scope after the gameplay consolidations: run totals, then radar and HUD model
void gr_flow_schedule_consolidate(EcsWorld *w);
// main lane after the scope: applies the pointer lock the flow wants
void gr_flow_main(EcsWorld *w, u32 *lock_seen);
