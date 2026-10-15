// Proving Grounds: a first-person arena shooter against three waves of drones; the app entry points.
#include "app/app.h"
#include "assets/asset_loaders.h"
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "ecs/ecs.gen.h"
#include "ecs/camera/ecs_camera.h"
#include "ecs/ecs_assets/ecs_assets.h"
#include "ecs/ecs_physics/ecs_physics.h"
#include "ecs/input/ecs_input.h"
#include "hz_engine.h"
#include "ecs/renderer/ecs_renderer.h"
#include "ecs/renderer/ecs_renderer.gen.h"
#include "ecs/transform/ecs_transform.h"
#include "ecs/transform/ecs_transform.gen.h"
#include "input.h"
#include "lib/color.h"
#include "lib/hzmath.h"
#include "lib/string.h"
#include "lib/thread_context.h"
#include "lib/typedefs.h"
#include "os/os.h"
#include "perf_stats.h"
#include "rendering/camera.h"
#include "rendering/renderer.h"
#include "scene/scene.h"
#include "generated/types_app.gen.h"
#include "gameplay/vfx/vfx.h"
#include "gameplay/hud/hud_map.h"
#include "gameplay/hud/hud_messages.h"
#include "gameplay/world_ui/world_ui.h"
#include "ui/ui.h"
#include "lib/random.h"
#include "procgen/pg_geo.h"
#include "rendering/features/decals.h"
#include "rendering/material.h"
#include "rendering/particle_system.h"
#include "rendering/trail_system.h"
#include "proving_grounds.h"
#include "proving_grounds/proving_grounds.gen.h"
#include "physics/physics.h"
// the editor is a native desktop tool: no wasm or iOS editor
#if !defined(WASM) && !defined(IOS)
#define GR_EDITOR 1
#include "ecs/ecs_editor/ecs_editor.h"
#include "hz_command_registry.h"
#include "rendering/gizmos.h"
#else
#define GR_EDITOR 0
#endif
// level
#include "gr_level.h"
#include "proving_grounds/gr_level.gen.h"
#include "km_world.h"
#include "km_droids.h"
#include "proving_grounds/km_droids.gen.h"
#include "km_air.h"
// player, camera, input
#include "gr_player.h"
#include "proving_grounds/gr_player.gen.h"
// drones, bolts, waves
#include "gr_drones.h"
#include "proving_grounds/gr_drones.gen.h"
// weapons, viewmodel, pickups
#include "gr_weapon.h"
// audio
#include "gr_audio.h"
#include "proving_grounds/gr_audio.gen.h"
// flow, HUD, menus
#include "gr_flow.h"
#include "gr_ui.h"
// look
#include "gr_look.h"
#include "proving_grounds/gr_look.gen.h"
// effects
#include "gr_fx.h"

#define GR_SCENE_PATH "proving_grounds/arena.hzt"

ECS_COMPONENT_TYPEDEF(AppMemory);

#include "gr_run.c"
#include "gr_level.c"
#include "km_world.c"
#include "gr_input.c"
#include "gr_player.c"
#include "gr_drones.c"
#include "gr_bolts.c"
#include "gr_waves.c"
#include "km_droids.c"
#include "gr_weapon.c"
#include "gr_viewmodel.c"
#include "gr_pickups.c"
#include "gr_audio.c"
#include "gr_flow.c"
#include "gr_ui.c"
// look
#include "gr_look.c"
// effects
#include "gr_fx.c"
#include "gr_fx_objects.c"
#include "gr_fx_labels.c"
#include "gr_fx_main.c"
#include "km_squad.c"
#include "km_air.c"

#if GR_EDITOR
#include "gr_editor.c"
#endif

typedef struct AppState {
  EcsWorld *world;
  Allocator allocator;
  SceneSystem scene_sys;
  SceneHandle scene;
  EcsQuery *camera_query;
  u32 lock_seen; // the GrFlow pointer-lock request last applied
#if GR_EDITOR
  GrEditorShell editor;
#endif
} AppState;

HZ_APP_API void app_init(AppMemory *memory) {
  if (!is_main_thread())
    return;
  AppState *s = memory->state;
  s->allocator = make_arena_allocator(arena_create(MB(16), MB(1)));

  EcsWorld *w = ecs_world_create();
  s->world = w;
  ecs_resource_set_ref(w, AppMemory, memory);

  ecs_input_init(w);
  ecs_assets_init(w, NULL);
  // level batches outgrow the default geometry pool: commit 8 MB, grow on demand; decals resolve through the AssetSystem; casings are 2 batches
  RendererInitDesc renderer_desc = {.shadows_enabled = true,
                                    .vertex_buffer_size = MB(8),
                                    .vertex_buffer_reserve = MB(128),
                                    .index_buffer_size = MB(4),
                                    .index_buffer_reserve = MB(64),
                                    .assets = ecs_resource_get_mut(w, AssetSystem),
                                    .max_batch_instances = 2 * GR_FX_CASINGS};
  gr_look_renderer_desc(&renderer_desc); // MSAA, shadows, bloom, height fog
  ecs_renderer_init(w, &renderer_desc);
  ecs_transform_init(w, NULL);
  // the static layer is one bucket (the engine's static-layer model); the level's extent with room to fall to the reset height
  ecs_physics_init(w, (CollisionLayerSettings){.world_aabb = aabb_make((v3){-80.0f, -50.0f, -280.0f}, (v3){80.0f, 50.0f, 100.0f}),
                                               .subdivisions = (iv3){1, 1, 1}});
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);

  scene_system_create(&s->scene_sys, w, assets, &s->allocator);
  // the look's runtime render lists are rebuilt on spawn, never authored
  scene_register_not_saveable(&s->scene_sys, EcsRenderLists);
  // the model resolve adds this marker to every instance's models: it is never a prefab override
  scene_set_component_derived(&s->scene_sys, fnv1a_hash("EcsSubMeshSpawned"));
  s->scene = scene_load_file(&s->scene_sys, GR_SCENE_PATH);
  s->camera_query = ecs_query_cached(w, EcsIn(Camera));
  gr_run_init(w);
  gr_level_init(w);
  km_world_init(w, assets);
  gr_player_init(w);
  gr_drones_init(w, assets);
  km_droids_init(w, assets);
  km_squad_init(w);
  km_air_init(w, assets);
  gr_weapon_init(w);
  gr_audio_init(w);
  // the UI lives in its own arena
  Allocator ui_alloc = make_arena_allocator(arena_create(MB(4), KB(256)));
  gr_ui_init(w, &ui_alloc);
  world_ui_init(w, &(WorldUiDesc){0});
  hud_msg_init(w, &(HudMsgDesc){.channels = {[HUD_MSG_BANNER] = {.policy = HUD_MSG_POLICY_REPLACE, .enter_s = 0.5f, .restart_on_replace = true}}});
  gr_look_init(w, &s->allocator); // after the level (GrLevel's areas)
  gr_fx_init(w);
  gr_fx_labels_init(w, ecs_resource_get(w, GrUi)->ui, ecs_resource_get(w, GrUi)->hud); // labels draw in HUD_FONT
#if GR_EDITOR
  gr_editor_init(&s->editor, w, &s->scene_sys, &s->allocator, memory);
#endif

  log_info("proving grounds initialized");
}

HZ_APP_API void app_update_and_render(AppMemory *memory) {
  AppState *s = memory->state;
  EcsWorld *w = s->world;
  perf_stats_begin_frame();

  gr_ui_font_update(w); // collective, every lane, before the UI frame

  // input is main-lane work; the barrier publishes it
  if (is_main_thread()) {
    InputSystem *input = ecs_resource_get_mut(w, InputSystem);
    input_update(input, &memory->input_events, memory->total_time);
#if GR_EDITOR
    gr_editor_keys(&s->editor, input);
#endif
    const GrEditor *editor = ecs_resource_get(w, GrEditor);
    if (editor->open && !editor->playing)
      gr_input_hold(ecs_resource_get_mut(w, GrInput));
    else
      gr_input_collect(ecs_resource_get_mut(w, GrInput), ecs_resource_get_mut(w, GrTouch), input,
                       ecs_resource_get(w, GrFlow)->state, ecs_resource_get(w, GrHud)->prompt_show,
                       memory->input_events.focus_lost, (f32)memory->total_time);
    gr_flow_collect(w); // the pipeline gate
  }
  lane_sync();

  ecs_scope(w) {
    ecs_assets_schedule(w);
    ecs_schedule_single(w, gr_clock_system);
    ecs_schedule_single(w, gr_run_system); // writes the GrFlow, GrClock and GrDev resources
    ecs_schedule_single(w, gr_dev_system); // writes the GrDev resource
    gr_drones_schedule_spawn(w);           // config, model parts, the wave director's spawns
#if GR_EDITOR
    ecs_editor_default_schedule(w); // the asset browser's thumbnail cache
#endif
    ecs_flush(w, memory->dt);

    // every lane reads the same settled clock
    const GrClock *clock = ecs_resource_get(w, GrClock);
    gr_level_schedule(w);
    gr_flow_schedule_settings(w); // settings onto their owners (sensitivity -> GrPlayer)
    gr_player_schedule_input(w);
    ecs_flush(w, clock->dt);
    gr_player_fixed(w);
    gr_player_schedule(w);
    // weapon, viewmodel, pickups and packs; their intake writers come before the player's intake
    gr_weapon_schedule(w);
    gr_drones_schedule(w); // drones, tokens, bolts, consolidations and the drones' writes to the player's intake
    km_squad_schedule(w);  // the clone squad: after the droids' field and bolt hits
    km_droids_schedule(w); // the walkers' limbs, before the transforms
    km_storm_schedule(w);  // lightning and thunder
    km_air_schedule(w);    // the air battle overhead
    gr_player_schedule_intake(w);
    ecs_schedule(w, gr_view_system);
    gr_flow_schedule_consolidate(w); // run totals, radar, HUD model
    gr_fx_schedule(w);               // after every producer's GrFxOut writes
    km_squad_schedule_fx(w);         // the squad's tracers
    km_air_schedule_fx(w);           // the air battle's tracers and bursts
    ecs_transform_schedule(w);
    gr_level_schedule_gather(w); // after the transforms: the scene's sites at their world poses
    ecs_camera_schedule(w, &(EcsCameraScheduleDesc){.transform_sync_off = true}); // the rig and the view write Camera
    world_ui_schedule(w);            // the kill popups and the swap prompt, after the camera
    hud_msg_schedule(w);             // the wave and run banners
    gr_audio_schedule(w);
    // the look: after the camera (the mirror test and the fog read this frame's view)
    gr_look_schedule(w);
    ecs_renderer_schedule(w);
    ecs_flush(w, clock->dt);
  }

  // null until the scene's camera entity has loaded
  Camera *cam = ecs_query_get_singleton(Camera, s->camera_query);
  // main-lane work the barrier publishes before the renderer reads it: the pointer lock, the effects, the UI, the look's renderer state
  if (is_main_thread()) {
    b32 editing = ecs_resource_get(w, GrEditor)->open && !ecs_resource_get(w, GrEditor)->playing;
    // the run's pointer-lock requests wait while the editor holds the cursor
    if (!editing)
      gr_flow_main(w, &s->lock_seen);
    km_world_main(w);    // Tipoca's geometry and colliders once its materials load
    gr_look_surfaces(w); // world-space UI surfaces draw before the screen UI frame
    gr_fx_frame(w, cam); // its world-space labels draw before the screen UI frame opens (ui_surface_begin contract)
    b32 editor_drew = false;
#if GR_EDITOR
    editor_drew = gr_editor_frame(&s->editor, cam, memory);
#endif
    if (!editor_drew)
      gr_ui_frame(w);
    gr_look_sync(w);
  }
  lane_sync();

  if (cam) {
    renderer_update(ecs_resource_get_mut(w, RendererState),
                    &(RendererBeginFrameDesc){
                        .camera = cam,
                        // the sky covers every pixel; opaque black is a fast clear (a sky blue rewrote the whole 4x MSAA target)
                        .clear_color = (ColorF32){0.0f, 0.0f, 0.0f, 1.0f},
                        .time = ecs_resource_get(w, GrClock)->time,
                    });
  }
  perf_stats_end_frame();
}

HZ_APP_API size_t app_state_size(void) { return sizeof(AppState); }
