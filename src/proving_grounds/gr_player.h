// Proving Grounds player: input bindings, the kinematic capsule controller and camera rig, health.
#pragma once
#include "ecs/ecs.h"
#include "gameplay/camera_rig/camera_rig.h"
#include "gameplay/character/character.h"
#include "gameplay/touch_controls/touch_controls.h"
#include "input.h"
#include "lib/array.h"
#include "lib/hzmath.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"
#include "physics/physics.h"

// player tuning
#define GR_PLAYER_RADIUS 0.35f
#define GR_PLAYER_HEIGHT 1.8f
#define GR_PLAYER_EYE_HEIGHT 1.65f
#define GR_PLAYER_WALK_SPEED 6.4f
#define GR_PLAYER_SPRINT_SPEED 9.2f
#define GR_PLAYER_GROUND_ACCEL 11.0f
#define GR_PLAYER_AIR_ACCEL 2.2f
#define GR_PLAYER_FRICTION 7.5f
#define GR_PLAYER_JUMP_VELOCITY 7.4f
#define GR_PLAYER_GRAVITY 21.0f
#define GR_PLAYER_MAX_HEALTH 100.0f
#define GR_PLAYER_REGEN_DELAY 4.5f
#define GR_PLAYER_REGEN_RATE 28.0f
#define GR_PLAYER_FOV 66.0f
#define GR_PLAYER_VIEWMODEL_FOV 54.0f
#define GR_PLAYER_MOUSE_SENSITIVITY 0.0021f

// hits one frame can queue; more fold into the last slot
#define GR_PLAYER_MAX_HITS 8

// the frame's bindings on the engine InputSystem, written on the main lane before the scope
typedef struct GrInput {
  f32 forward;        // W - S
  f32 strafe;         // D - A
  v2 look;            // pointer movement this frame, pixels; zero unless locked, and on the lock frame (cursor warp)
  b32 sprint;         // Left Shift held (right shift is not bound)
  b32 jump_pressed;   // Space edge
  b32 fire_held;      // left button, counted only while locked; GrDev.fire is the simulated one
  b32 fire_pressed;
  b32 aim_held;       // right button, only while locked; GrDev.ads is the simulated one
  b32 aim_pressed;
  b32 reload_pressed; // R
  b32 swap_pressed;   // E
  b32 throw_pressed;  // G: a thermal detonator
  InputPointerLock pointer_lock; // the lock and its edges this frame
  b32 touch;          // the last input was a touch: the on-screen controls drive the run, no pointer lock
  b32 captured;       // the run takes input: the pointer is locked, or touch
  b32 pause_pressed;  // the touch pause button (Escape's role with no lock to lose)
  b32 focused;
  b32 focus_lost;     // the window lost focus this frame
  b32 held;           // an open editor holds the run: no input reaches it and nothing advances
} GrInput;
ECS_COMPONENT_TYPEDEF(GrInput);

// the on-screen buttons, indexed into TouchControlsConfig.buttons
typedef enum GrTouchButton {
  GR_TOUCH_FIRE = 0,
  GR_TOUCH_ADS,
  GR_TOUCH_JUMP,
  GR_TOUCH_RELOAD,
  GR_TOUCH_SWAP,
  GR_TOUCH_PAUSE,
  GR_TOUCH_BUTTON_COUNT,
} GrTouchButton;

// main-lane touch state: the stick, look and buttons, and the ADS toggle the aim button flips
typedef struct GrTouch {
  TouchControls tc;
  b32 ads;
  b32 sprint; // the stick is over-pushed: the thumb shows it
} GrTouch;
ECS_COMPONENT_TYPEDEF(GrTouch);

// the player's settings (the flow writes sensitivity)
HZ_REFLECT()
ECS_COMPONENT(GrPlayer) {
  f32 sensitivity; // multiplier on the base mouse sensitivity
};

// one damage hit
HZ_REFLECT()
HZ_STRUCT_TYPEDEF(GrPlayerHit) {
  f32 amount;
  v3 from; // world position the hit came from
};

// the inbox gr_player_intake_system consumes; writers: per-entity systems over the player, scheduled before it
HZ_REFLECT()
ECS_COMPONENT(GrPlayerIntake) {
  FixedArray(GrPlayerHit, GR_PLAYER_MAX_HITS) hits; // damage (drone bolts)
  f32 heal;                                         // heal (health packs): restores up to this much
  f32 shake;                                        // shake sum (drone destroyed, splash)
  f32 recoil_kept_pitch;                            // recoil: (1 - recover) of the kicks, radians
  f32 recoil_kept_yaw;
  f32 recoil_spring_pitch;                          // recover of the kicks, into the offset that springs back
  f32 recoil_spring_yaw;
  u32 punches;                                      // recoil kicks (one punch impulse each)
  f32 debug_damage;                                 // automation (set:): one hit of this amount from ahead of the eye
};

// helpers for intake writers: damage, heal, shake and recoil
force_inline void gr_player_intake_damage(GrPlayerIntake *in, f32 amount, v3 from) {
  if (in->hits.len < GR_PLAYER_MAX_HITS)
    in->hits.items[in->hits.len++] = (GrPlayerHit){amount, from};
  else
    in->hits.items[GR_PLAYER_MAX_HITS - 1].amount += amount;
}
force_inline void gr_player_intake_shake(GrPlayerIntake *in, f32 amount) { in->shake += amount; }
force_inline void gr_player_intake_heal(GrPlayerIntake *in, f32 amount) { in->heal += amount; }
force_inline void gr_player_intake_recoil(GrPlayerIntake *in, f32 pitch, f32 yaw, f32 recover) {
  in->recoil_kept_pitch += pitch * (1.0f - recover);
  in->recoil_kept_yaw += yaw * (1.0f - recover);
  in->recoil_spring_pitch += pitch * recover;
  in->recoil_spring_yaw += yaw * recover;
  in->punches++;
}

// the player's events this frame
typedef struct GrPlayerEvents {
  b32 reset;       // placed this frame: run start, pose, teleport or fall reset
  b32 jumped;
  b32 landed;
  f32 land_impact; // fall speed
  b32 launched;
  v3 launch_pad;   // the pad's position
  b32 footstep;
  b32 footstep_sprinting;
  u32 hit_count;   // one per applied hit
  GrPlayerHit hits[GR_PLAYER_MAX_HITS];
  f32 healed;      // amount actually restored; 0 = none
  b32 died;
} GrPlayerEvents;

// player state beyond the engine character and camera rig: health, the ADS request, the frame's events and the queued camera effects; runtime only
typedef struct GrMotion {
  b32 sprinting;
  b32 controls;  // the run took input this frame
  f32 zoom_target; // 0..1 aim-down-sights, the weapon's request
  f32 zoom_fov;    // vertical degrees at full zoom
  f32 pad_cooldown;
  f32 health;
  b32 alive;
  f32 time_since_damage;
  GrPlayerEvents events;
  // what the intake and the pads queued for the rig, handed over next frame (CameraRigInput units)
  f32 pending_shake;
  f32 pending_fov_kick;
  f32 pending_punch;
  v2 pending_look;   // kept recoil: radians right / up
  v2 pending_recoil; // spring recoil offsets: radians right / up
  b32 look_set;      // placement or aim: the rig takes this look
  b32 springs_reset; // and clears its springs
  f32 look_yaw;
  f32 look_pitch;
  u32 jumps_seen; // Character counters last seen
  u32 landings_seen;
  u32 footsteps_seen;
  u32 teleports_seen;
  u32 last_state;      // GrGameState seen last frame
  u32 applied_restart; // GrDev serials applied
  u32 applied_pose;
  u32 applied_aim;
  b32 placed;
} GrMotion;
ECS_COMPONENT_TYPEDEF(GrMotion);

// the player's fixed step and what the frame's intent decided: live runs step the character, controls drive the look
typedef struct GrPlayerClock {
  FixedTimestep ft;
  f32 step_dt; // the run's dt this frame; 0 = paused, in a menu or frozen
  b32 live;
  b32 controls;
} GrPlayerClock;
ECS_COMPONENT_TYPEDEF(GrPlayerClock);

// the player for every other system: written after the controller, again after the intake
typedef struct GrPlayerState {
  EcsEntity entity; // the Player entity: its GrPlayerIntake is the inbox
  b32 ready;        // placed in a run
  b32 controls;
  v3 feet;
  v3 eye; // the camera, bob/dip/death included
  v3 velocity;
  f32 yaw; // the look, the kept recoil included
  f32 pitch;
  quat view_rot;    // the camera rotation: recoil offset, shake, punch, roll, death included
  v3 view_forward;  // the camera's -Z (hitscan direction)
  f32 fov;          // world vertical degrees
  f32 viewmodel_fov;
  f32 zoom;
  f32 zoom_fov;
  f32 move_amount;
  f32 bob_phase;
  b32 on_ground;
  b32 sprinting;
  f32 shake;  // trauma^2
  f32 health;
  b32 alive;
  GrPlayerEvents events;
} GrPlayerState;
ECS_COMPONENT_TYPEDEF(GrPlayerState);

// main lane, after input_update and before the scope: devices into GrInput; the lock requests (os_lock_mouse) live here
void gr_input_collect(GrInput *in, GrTouch *touch, InputSystem *input, u32 state, b32 swap_prompt, b32 focus_lost,
                      f32 now);
// an open editor holds the run: no input and no lock or focus edges; the lock state last seen stays
void gr_input_hold(GrInput *in);
// main lane, gr_ui_init: the touch layout, labels in the HUD face
void gr_touch_init(EcsWorld *w, UISystem *ui, HzHandleT(UIFontFamily) font);

// main lane, app_init: the resources and the Player entity
void gr_player_init(EcsWorld *w);
// inside the scope after gr_run_system: the frame's intent into the character (placement, controls, jump pads)
void gr_player_schedule_input(EcsWorld *w);
// inside the scope after a flush: the character's fixed steps, one flush each; every lane takes the same steps
void gr_player_fixed(EcsWorld *w);
// inside the scope after the fixed steps: pose, the first-person rig, GrPlayerState
void gr_player_schedule(EcsWorld *w);
// inside the scope after every intake writer: damage, heals, shake, recoil; republishes GrPlayerState
void gr_player_schedule_intake(EcsWorld *w);
