// Proving Grounds editor: the stock default editor; F1 opens it (run held, flycam camera), P plays a preview.

// the stock editor plus the Play panel; main-lane state in AppState
typedef struct GrEditorShell {
  EcsWorld *world;
  EcsEditorDefault ed;
  UIPanelId game_panel;
  b32 was_editing;
} GrEditorShell;
HZPTR_TYPEDEF(GrEditorShell);

hz_internal b32 gr_editor_editing(const GrEditor *e) { return e->open && !e->playing; }

// opens at the game's current view, or closes (dropping any preview)
hz_internal void gr_editor_cmd_edit(const HzCommandCtx *ctx, void *user) {
  GrEditorShell *sh = (GrEditorShell *)user;
  GrEditor *e = ecs_resource_get_mut(sh->world, GrEditor);
  e->open = !e->open;
  e->playing = false;
  const Camera *cam = ecs_query_get_singleton(Camera, sh->ed.camera_query);
  if (e->open && cam) {
    e->eye = cam->pos;
    e->look = cam->rot;
    sh->ed.flycam_synced = false;
  }
  hz_command_set_summary(ctx, e->open ? "editing" : "editor closed");
}

// the game runs with its own camera and input; again returns to editing where the editor camera was
hz_internal void gr_editor_cmd_play(const HzCommandCtx *ctx, void *user) {
  GrEditorShell *sh = (GrEditorShell *)user;
  GrEditor *e = ecs_resource_get_mut(sh->world, GrEditor);
  if (!e->open) {
    hz_command_fail(ctx, "the editor is closed (F1)");
    return;
  }
  e->playing = !e->playing;
  hz_command_set_summary(ctx, e->playing ? "playing" : "editing");
}

hz_internal void gr_editor_game_panel(HzPtr ctx, HzPtr user_data) {
  UNUSED(ctx);
  GrEditorShell *sh = hzptr_cast(GrEditorShell, user_data);
  EcsEditorCtx *ec = ecs_editor_ctx(sh->world);
  ecs_editor_panel_begin(ec, sh->ed.panels, sh->game_panel);
  if (ecs_editor_button(ec, "Play  (P)"))
    hz_command_run(&(HzCommandRunDesc){.name = "gr.play"});
  editor_panel_end();
}

// main lane, app_init after the scene system and the game UI
void gr_editor_init(GrEditorShell *sh, EcsWorld *w, SceneSystem *scene, Allocator *a, const AppMemory *memory) {
  sh->world = w;
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  RendererState *renderer = ecs_resource_get_mut(w, RendererState);
  // the editor draws through the game's UI system: Clay is one context
  ecs_resource_set_ref(w, UISystem, ecs_resource_get(w, GrUi)->ui);
  ecs_editor_default_init(&sh->ed, &(EcsEditorDefaultDesc){.world = w,
                                                           .assets = assets,
                                                           .scene = scene,
                                                           .ui = ecs_resource_get(w, GrUi)->ui,
                                                           .allocator = a,
                                                           .canvas_width = memory->canvas_width,
                                                           .canvas_height = memory->canvas_height});
  ecs_resource_set_ref(w, GizmoSystem,
                       gizmos_create(&(GizmoInitDesc){
                           .assets = assets, .renderer = renderer, .sample_count = renderer_sample_count(renderer)}));
  gizmos_attach_to_renderer(ecs_resource_get_mut(w, GizmoSystem), renderer);
  sh->game_panel = ui_panels_add_panel(sh->ed.panels, &(UIPanelDesc){.title = "GAME",
                                                                       .placement = UI_PANEL_PLACEMENT_FREE,
                                                                       .anchor = UI_ANCHOR_BOTTOM_LEFT,
                                                                       .pad_x = 12,
                                                                       .pad_y = 12,
                                                                       .w = 200,
                                                                       .h = 96,
                                                                       .content_fn = gr_editor_game_panel,
                                                                       .content_user_data = hzptr_void(GrEditorShell, sh)});
  hz_command_register_desc(&(HzCommandDesc){.name = "gr.edit",
                                            .fn = gr_editor_cmd_edit,
                                            .user = sh,
                                            .description = "open or close the editor (F1); the run is held while editing"});
  hz_command_register_desc(&(HzCommandDesc){.name = "gr.play",
                                            .fn = gr_editor_cmd_play,
                                            .user = sh,
                                            .description = "play the game as a preview, or back to editing (P)"});
}

// main lane after input_update, before the scope: F1 and P (P never while a text field takes keys)
void gr_editor_keys(GrEditorShell *sh, const InputSystem *input) {
  const GrEditor *e = ecs_resource_get(sh->world, GrEditor);
  b32 typing = ui_wants_text_input(ecs_resource_get_mut(sh->world, UISystem)) || editor_prompt_has_focus(&sh->ed.prompt);
  if (input->buttons[KEY_F1].pressed_this_frame)
    hz_command_run(&(HzCommandRunDesc){.name = "gr.edit"});
  else if (e->open && input->buttons[KEY_P].pressed_this_frame && (e->playing || !typing))
    hz_command_run(&(HzCommandRunDesc){.name = "gr.play"});
}

// main lane after the scope: the editor's UI frame while editing (true); otherwise false and the game draws its own
b32 gr_editor_frame(GrEditorShell *sh, Camera *cam, const AppMemory *memory) {
  EcsWorld *w = sh->world;
  GrEditor *e = ecs_resource_get_mut(w, GrEditor);
  b32 editing = gr_editor_editing(e);
  // the cursor is the editor's; leaving clears the selection outline
  if (editing && !sh->was_editing)
    os_lock_mouse(false);
  if (!editing && sh->was_editing)
    editor_selection_clear(&sh->ed.selection_render);
  sh->was_editing = editing;
  if (!editing || !cam)
    return false;

  UISystem *ui = ecs_resource_get_mut(w, UISystem);
  ui_begin_frame(ui, ecs_resource_get(w, InputSystem), memory->dt);
  ecs_editor_default_update(&sh->ed, &(EcsEditorDefaultUpdateDesc){.camera = cam,
                                                                  .canvas_width = memory->canvas_width,
                                                                  .canvas_height = memory->canvas_height});
  gizmos_prepare(ecs_resource_get_mut(w, GizmoSystem), &(GizmoFlushDesc){.camera = cam,
                                                                          .line_width = 2.0f,
                                                                          .screen_width = memory->canvas_width,
                                                                          .screen_height = memory->canvas_height});
  ui_finish_layout(ui);
  // the flycam's pose for next frame's view system
  e->eye = cam->pos;
  e->look = cam->rot;
  return true;
}
