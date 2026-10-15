// The look: the _GrLook block, sun and sky, fog, bloom, the final pass grade, the quality level, the planar floor mirror.
#pragma once
#include "ecs/ecs.h"
#include "lib/color.h"
#include "lib/hzmath.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"
#include "rendering/planar_reflection.h"
#include "rendering/renderer.h"
#include "ui/ui.h"
#include "gr_flow.h"
// render list bit of everything the floor mirror shows (world, effects and sky)
#define GR_LIST_MIRROR 0u
// render list bit the area probes capture (world, floor and sky as they stand at the light bake)
#define GR_LIST_PROBE 1u
// the look's fixed exposure: one look unit (scene-linear) is one pre-exposed HDR unit
#define GR_LOOK_EV100 RENDERER_EV100_DEFAULT

// cd/m2 (or lux) per look unit
force_inline f32 gr_look_unit(void) { return 1.0f / renderer_pre_exposure_from_ev100(GR_LOOK_EV100); }

// 0xRRGGBB sRGB bytes to linear
force_inline v3 gr_look_hex_linear(u32 hex) {
  return vec3_srgb_to_linear(
      (v3){(f32)((hex >> 16) & 255) / 255.0f, (f32)((hex >> 8) & 255) / 255.0f, (f32)(hex & 255) / 255.0f});
}

// an sRGB colour's rgb to linear
force_inline v3 gr_look_linear(ColorF32 c) { return vec3_srgb_to_linear((v3){c.r, c.g, c.b}); }

// the entity is never drawn into the floor mirror (the viewmodel, the mirrored hub floor itself)
HZ_REFLECT()
ECS_COMPONENT(GrNoMirror) {
  u8 _;
};

// the entity is part of the static world the area probes capture (drones, bolts and effects are not)
HZ_REFLECT()
ECS_COMPONENT(GrProbeCapture) {
  u8 _;
};

// what a part's material instance shows
typedef enum GrLookRole {
  GR_LOOK_ROLE_NONE = 0,     // looked at, left on its shared material
  GR_LOOK_ROLE_HULL,         // hit flash
  GR_LOOK_ROLE_LENS,         // drone Emit_Eye
  GR_LOOK_ROLE_PUPIL,
  GR_LOOK_ROLE_ENERGY,
  GR_LOOK_ROLE_THRUST,
  GR_LOOK_ROLE_VENT,         // gun vents: heat colour and intensity
  GR_LOOK_ROLE_COIL,         // lancer coils
  GR_LOOK_ROLE_CELL,
  GR_LOOK_ROLE_GUN_LENS,
  GR_LOOK_ROLE_SCREEN,       // the gun's ammo counter surface
  GR_LOOK_ROLE_PEDESTAL,     // weapon pickup pedestal glow
  GR_LOOK_ROLE_SIGN,         // a sign's drawing
} GrLookRole;

// a part on its own material instance: the emissive its owner wants this frame
typedef struct GrLookPart {
  EcsEntity owner; // drones: the drone (EcsSubMesh.model)
  u32 role;        // GrLookRole
  u32 index;       // gun (GrWeaponId), pedestal or sign index
  f32 intensity;   // emissive intensity in look units
  v3 color;        // linear emissive (vents only)
} GrLookPart;
ECS_COMPONENT_TYPEDEF(GrLookPart);

// what the renderer has for the part; written by the consolidation only
typedef struct GrLookPartApplied {
  f32 intensity; // -1 before the first write
  v3 color;
  b32 texture;   // the surface is bound to emission_map
} GrLookPartApplied;
ECS_COMPONENT_TYPEDEF(GrLookPartApplied);

// the model's submeshes have been looked at
typedef struct GrLookModel {
  u8 _;
} GrLookModel;
ECS_COMPONENT_TYPEDEF(GrLookModel);

// a UI surface the look draws into a material's emission_map (ammo screens, signs)
#define GR_LOOK_SCREEN_COUNT 3
#define GR_LOOK_SIGN_COUNT 5
typedef struct GrLookSurfaces {
  UISurface *screens[GR_LOOK_SCREEN_COUNT]; // by gun
  UISurface *signs[GR_LOOK_SIGN_COUNT];     // by GrSignKind
  GpuTexture screen_tex[GR_LOOK_SCREEN_COUNT]; // valid once drawn
  GpuTexture sign_tex[GR_LOOK_SIGN_COUNT];
  u32 screen_key[GR_LOOK_SCREEN_COUNT]; // ammo | max << 8 | low << 16 last drawn, ~0 none
  b32 signs_drawn;
  u32 glyph_epoch; // ui_glyph_epoch at the last paint: glyphs that landed since repaint the signs and screens
  u32 font_frames; // frames the HUD weights have been ready
} GrLookSurfaces;
ECS_COMPONENT_TYPEDEF(GrLookSurfaces);

// the final pass's gameplay feedback, written by gr_look_feedback_system
typedef struct GrFeedback {
  f32 damage;     // + 0.3 + 0.03 x amount per hit (max 1), -2.2/s
  f32 low_health; // (0.35 - health) / 0.35 in a live run
  f32 heal;       // 1 on a heal, -1.6/s
  f32 scope;      // smoothstep(0.75, 0.98, zoom) on scoped guns
} GrFeedback;
ECS_COMPONENT_TYPEDEF(GrFeedback);

// the look's tunables on the Settings entity (the sun is the Sun entity, the fog ambient its EcsSceneSettings)
HZ_REFLECT()
ECS_COMPONENT(GrLookConfig) {
  ColorF32 sky_zenith;  // sRGB
  ColorF32 sky_horizon; // sRGB
  f32 sky_intensity;
  f32 env_intensity;    // the env cube's radiance, look units
  ColorF32 hemi_sky;    // sRGB: the surfaces' hemisphere fill
  ColorF32 hemi_ground; // sRGB
  f32 hemi_intensity;
  f32 fog_density;      // per metre, times the area's fog_density
  f32 fog_falloff;      // per metre of height
  f32 fog_anisotropy;
  f32 fog_max_distance; // metres
  f32 fog_shaft_intensity;
  f32 bloom_strength;   // summed over the 6 levels
  f32 bloom_scatter;
  f32 bloom_threshold;
  f32 saturation;       // 1 neutral
  f32 contrast;         // 1 neutral
  f32 vignette;
  f32 grain;
  f32 chromatic;
  f32 planar_distortion; // the hub floor mirror's normal distortion
};

typedef struct GrMirrorGpu GrMirrorGpu;

// the look's state: written by its consolidations (in the scope) and the main-lane sync (after it)
typedef struct GrLook {
  GrLookConfig cfg; // Settings' GrLookConfig, copied on change
  b32 cfg_ready;
  v3 sun_toward;     // from the Sun entity: unit, towards the sun
  v3 sun_color;      // linear
  f32 sun_intensity; // look units
  b32 sun_ready;
  EcsEntity post;    // the runtime EcsPostProcessing (untagged: the grade plus gameplay feedback is never saved)
  RendererFeature feature; // owns _GrLook, publishes gr_noise and records the env cube bake
  GpuTexture noise;        // the shared noise texture
  GpuTexture env_cube;     // the sky env cube
  GpuComputePipeline env_pipeline;
  b32 env_baked;
  GrMirrorGpu *mirror;
  HzHandleT(MaterialBlobAsset) final_asset; // gr_final_scope_mat: the default final plus the scope overlay
  HzHandleT(RendererMaterial) final_material;
  i32 final_applied;   // 1 the scope final is set on the renderer, 0 the default, -1 before the first
  f32 aspect;          // the render size's width over height, read by the grade's edge radii
  b32 planar_active;   // the hub floor is in view and the quality level mirrors it (written by gr_look_view_system)
  v3 view_pos;         // the camera this frame
  i32 quality_applied; // -1 before the first
  f32 viewmodel_fov_applied; // the projection override's fov on the renderer, 0 before the first
  EcsEntity probes[GR_AREA_COUNT]; // one EcsReflectionProbe per area
  b32 probes_spawned;              // once the level is gathered and instanced
  b32 probes_baked;                // the second bounce is requested
  f32 applied_fog_density;
  b32 applied_fog_enabled;
  EcsEntity sky;
  // GrDevSwitches look toggles, read by the sync: one frame late for the systems
  b32 no_fog, no_bloom, no_planar, nan;
  EcsQuery *switches;
} GrLook;
ECS_COMPONENT_TYPEDEF(GrLook);

// the RendererInitDesc fields the look owns: MSAA, shadows, bloom, height fog, the depth pre-pass
void gr_look_renderer_desc(RendererInitDesc *desc);
// main lane after ecs_renderer_init: features, the noise texture, the env cube, the sky dome, the final materials
void gr_look_init(EcsWorld *w, Allocator *a);
// inside the frame's scope after the camera schedule: render lists, sun, post, the view and the _GrLook block
void gr_look_schedule(EcsWorld *w);
// main lane before the UI frame (world surfaces must precede ui_begin_frame): ammo screens and signs
void gr_look_surfaces(EcsWorld *w);
// main lane after the scope, before renderer_update: final variant, the quality level on change, fog density
void gr_look_sync(EcsWorld *w);
// a viewmodel renderable: drawn with the viewmodel projection, never mirrored
void gr_look_viewmodel(EcsWorld *w, EcsEntity e);
// a static world renderable the area probes capture
void gr_look_probe_capture(EcsWorld *w, EcsEntity e);
