// Proving Grounds pickup labels: world_ui billboards over the beacons, main lane before renderer_update.

#define GR_LABEL_W 256 // label surface size
#define GR_LABEL_H 64
#define GR_LABEL_HEIGHT 0.05f // constant screen size: 0.05 x depth world units tall

typedef struct GrFxLabel {
  BillboardGui bg;
  String64Bytes source; // the beacon name the text was set from
  String64Bytes upper;
  HzHandleT(UIFontData) font; // bold face of the default family
  f32 font_size;              // 30 px shrunk until the name fits 236 px
  u32 kind;
  b32 used;
} GrFxLabel;

typedef struct GrFxLabels {
  UISystem *ui;                     // the game UI (GrUi.ui)
  HzHandleT(UIFontData) label_font; // bound at init: a slot bound mid-frame misses that frame's collective update
  GrFxLabel labels[GR_FX_BEACONS];
} GrFxLabels;
ECS_COMPONENT_TYPEDEF(GrFxLabels);

// after the UI is created; main lane at app_init. Labels draw in the HUD face, bold, 30 px
void gr_fx_labels_init(EcsWorld *w, UISystem *ui, HzHandleT(UIFontFamily) hud) {
  assert(is_main_thread()); // binds a UI font slot: UI state is lane-0 work
  ecs_resource_set_zero(w, GrFxLabels);
  GrFxLabels *u = ecs_resource_get_mut(w, GrFxLabels);
  u->ui = ui;
  u->label_font = ui_family_resolve(ui, hud, (UIFontStyleTag){.weight = UI_FONT_BOLD});
}

hz_internal HzUIColor gr_label_kind_color(u32 kind) {
  if (kind == GR_BEACON_OBJECTIVE)
    return (HzUIColor){0x5e, 0xf2, 0xff, 255};
  return kind == GR_BEACON_HEALTH ? (HzUIColor){0x38, 0xff, 0x7a, 255} : (HzUIColor){0xff, 0xb6, 0x27, 255};
}

hz_internal void gr_label_init(UISystem *ui, RendererState *r, HzHandleT(UIFontData) font, GrFxLabel *l,
                               const GrFxBeaconView *b) {
  l->bg = billboard_create(ui, r, &(BillboardGuiDesc){.canvas_w = GR_LABEL_W,
                                                      .canvas_h = GR_LABEL_H,
                                                      .pixels_per_unit = 256.0f,
                                                      .size_mode = BILLBOARD_SIZE_PIXELS,
                                                      .always_on_top = false,
                                                      .clear_color = {0, 0, 0, 0}});
  renderer_submesh_instance_set_sort_priority(r, l->bg.surface->instance, GR_FX_ORDER_LABEL);
  l->kind = b->kind;
  l->font = font;
  l->used = true;
}

// the label's text from the beacon's name, again whenever the name changes (a config edit)
hz_internal void gr_label_text(UISystem *ui, GrFxLabel *l, const GrFxBeaconView *b) {
  l->source = b->name;
  l->upper = b->name;
  for (u32 i = 0; i < l->upper.len; i++)
    if (l->upper.value[i] >= 'a' && l->upper.value[i] <= 'z')
      l->upper.value[i] = (char)(l->upper.value[i] - 'a' + 'A');
  f32 size = 30.0f;
  while (size > 16.0f && ui_measure_text_width(ui, l->font, l->upper.value, l->upper.len, size) > 236.0f)
    size -= 1.0f;
  l->font_size = size;
}

hz_internal void gr_fx_labels_frame(EcsWorld *w, GrFx *fx, RendererState *r, Camera *cam) {
  UNUSED(fx);
  const GrFxBeacons *beacons = ecs_resource_get(w, GrFxBeacons);
  GrFxLabels *u = ecs_resource_get_mut(w, GrFxLabels);
  if (!cam || beacons->count == 0)
    return;
  u32 screen_w = 0, screen_h = 0;
  renderer_post_size(r, &screen_w, &screen_h);
  // the sprite is 0.05 x depth tall: that fraction of the view height at any distance
  f32 half_tan = m_tanf(RADIANS(cam->fov) * 0.5f);
  f32 frac = GR_LABEL_HEIGHT / (2.0f * half_tan);
  for (u32 i = 0; i < beacons->count; i++) {
    const GrFxBeaconView *b = &beacons->items[i];
    GrFxLabel *l = &u->labels[i];
    if (!l->used)
      gr_label_init(u->ui, r, u->label_font, l, b);
    if (l->source.len != b->name.len || mem_cmp(l->source.value, b->name.value, b->name.len) != 0 || l->font_size == 0.0f)
      gr_label_text(u->ui, l, b);
    l->bg.pixel_height = frac * (f32)screen_h;
    v3 anchor = v3_add(b->position, (v3){0.0f, b->label_height, 0.0f});
    // bottom-centred: lift by half the label's height at this depth
    f32 depth = m_maxf(v3_dot(v3_sub(anchor, cam->pos), camera_forward(cam)), 0.05f);
    anchor = v3_add(anchor, v3_scale(camera_up(cam), GR_LABEL_HEIGHT * depth * 0.5f));
    l->bg.visible = b->visible && billboard_place(&l->bg, cam, screen_h, anchor);
    if (!b->visible)
      ui_surface_set_visible(l->bg.surface, false);
  }
  // after placing: billboard_place writes an identity rotation that this turns to the camera
  ui_surface_update_all(u->ui, cam);
  for (u32 i = 0; i < beacons->count; i++) {
    GrFxLabel *l = &u->labels[i];
    if (!l->bg.visible)
      continue;
    HzUIColor kind = gr_label_kind_color(l->kind);
    ui_surface_scope(u->ui, l->bg.surface) {
      ui_element({.id = ui_idi("GrLabel", i),
                  .layout = {.sizing = {.width = ui_sizing_fixed(GR_LABEL_W), .height = ui_sizing_fixed(GR_LABEL_H)},
                             .padding = {.top = 8, .bottom = 8}}}) {
        ui_element({.id = ui_idi("GrLabelBar", i),
                    .layout = {.sizing = {.width = ui_sizing_grow(0), .height = ui_sizing_grow(0)},
                               .child_alignment = {.x = UI_ALIGN_X_CENTER, .y = UI_ALIGN_Y_CENTER}},
                    .background_color = {4, 10, 14, 158}, // alpha 0.62
                    .border = {.width = {.top = 3, .bottom = 3}, .color = kind}}) {
          ui_text(((HzUIString){.chars = l->upper.value, .length = (i32)l->upper.len}),
                  ui_text_config({.fontId = (u16)l->font.idx,
                                  .fontSize = (u16)l->font_size,
                                  .textColor = kind}));
        }
      }
    }
  }
}
