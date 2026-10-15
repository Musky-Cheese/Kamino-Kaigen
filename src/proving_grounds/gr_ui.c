// Proving Grounds UI: HUD, menus, loading and fatal screens. Colours are sRGB bytes; sizes are logical px.
#include "shaders/gr_ui_arc.h"
#include "shaders/gr_ui_radar.h"
#include "shaders/gr_ui_ring.h"

#define GR_Z_HUD 10      // the HUD layer
#define GR_Z_LOADING 19  // the loading screen sits just under the menus
#define GR_Z_MENU 20     // the menus layer
#define GR_Z_MENU_POPUP 25 // a dropdown's popup list
#define GR_Z_FATAL 100   // the fatal screen, over everything

#define GR_CYAN ((HzUIColor){94, 242, 255, 255})
#define GR_HUD_WHITE ((HzUIColor){234, 246, 255, 255})
#define GR_RED ((HzUIColor){255, 59, 92, 255})
#define GR_AMBER ((HzUIColor){255, 181, 71, 255})
#define GR_WHITE ((HzUIColor){255, 255, 255, 255})
#define GR_PASS UI_POINTER_CAPTURE_MODE_PASSTHROUGH

#define GR_HUD_SPACING 0.96f   // the HUD's letter spacing: 0.06 of 16 px, the same px for every child
#define GR_RADAR_CANVAS 176.0f // the radar's drawing size, px
#define GR_TICK_ROW 18         // up to 18 ammo ticks per row

// the frame's viewport and the layout breakpoints that read it
typedef struct GrUiView {
  f32 w, h, vw;
  f32 dt;  // presentation dt (GrClock.dt)
  f32 now; // GrHud.time
  b32 narrow;      // width <= 900 px
  b32 very_narrow; // width <= 700 px
  b32 short_screen; // a landscape phone: the menus split into columns so no column outgrows the height
  b32 touch;       // the on-screen controls own the bottom corners: the compact HUD
  f32 touch_top;   // touch: where the corner panels start (below the top panels when they span the width)
  OsSafeAreaInsets safe;
} GrUiView;

// ---- setup ----

hz_internal GpuPipeline gr_ui_pipeline(UISystem *ui, GpuPipelineDesc d) {
  mem_set(d.blend_attachments, 0, sizeof(d.blend_attachments));
  d.vs_uses_bindless = false;
  d.cull_mode = GPU_CULL_NONE;
  d.depth_test = false;
  d.depth_write = false;
  d.blend = (GpuBlendState){
      .enabled = true,
      .src_factor = GPU_BLEND_SRC_ALPHA,
      .dst_factor = GPU_BLEND_ONE_MINUS_SRC_ALPHA,
      .op = GPU_BLEND_OP_ADD,
      .src_factor_alpha = GPU_BLEND_ONE,
      .dst_factor_alpha = GPU_BLEND_ONE_MINUS_SRC_ALPHA,
      .op_alpha = GPU_BLEND_OP_ADD,
  };
  GpuRenderTarget rt = ui_get_render_target(ui);
  if (handle_is_valid(rt)) {
    d.color_attachment_count = 1;
    d.color_formats[0] = gpu_get_render_target_format(rt, 0);
  }
  return gpu_make_pipeline(&d);
}

void gr_ui_init(EcsWorld *w, Allocator *a) {
  GrUi *u = ALLOC(a, GrUi);
  u->ui = ui_create(&(UIInitDesc){.max_glyphs = 8192, .backdrop_filters = true}, ecs_resource_get_mut(w, AssetSystem));
  ui_system_attach_to_renderer(u->ui, ecs_resource_get_mut(w, RendererState));
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  const char **names = ALLOC_ARRAY(&ta.allocator, const char *, 4);
  names[0] = "Bahnschrift";
  names[1] = "DIN Alternate";
  names[2] = "Roboto Condensed";
  names[3] = "Arial Narrow";
  u->hud = ui_load_family_os_list(u->ui, names, 4, NULL); // names are copied
  tctx_temp_allocator_end(ta);
  u->ring_pipeline = gr_ui_pipeline(u->ui, *shader_gr_ui_ring_pipeline_desc());
  u->arc_pipeline = gr_ui_pipeline(u->ui, *shader_gr_ui_arc_pipeline_desc());
  u->radar_pipeline = gr_ui_pipeline(u->ui, *shader_gr_ui_radar_pipeline_desc());
  ecs_resource_set_ref(w, GrUi, u);
  gr_touch_init(w, u->ui, u->hud);
}

void gr_ui_font_update(EcsWorld *w) { ui_font_update(ecs_resource_get(w, GrUi)->ui); }

// ---- helpers ----

// eases a value toward `target` over `seconds`
hz_internal f32 gr_tween(UITween *t, f32 target, f32 seconds, f32 dt) {
  return ui_tween(t, target, seconds, ui_easing_ease(), dt);
}

// a freshly rendered element holds `value` with no running transition
hz_internal void gr_tween_snap(UITween *t, f32 value) {
  *t = (UITween){0};
  ui_tween(t, value, 0.0f, ui_easing_linear(), 0.0f);
}

hz_internal HzUIString gr_str(String s) { return (HzUIString){.chars = s.value, .length = (i32)s.len}; }

hz_internal HzUIColor gr_alpha(HzUIColor c, f32 a) { return (HzUIColor){c.r, c.g, c.b, c.a * a}; }

hz_internal HzUITextConfig gr_font(const GrUi *u, f32 size, u16 weight, HzUIColor color, f32 spacing) {
  return (HzUITextConfig){
      .font_family = u->hud,
      .weight = weight,
      .fontSize = size,
      .textColor = color,
      .letterSpacing = spacing,
      .wrapMode = UI_TEXT_WRAP_NONE,
  };
}

hz_internal HzUITextConfig gr_glow(HzUITextConfig c, HzUIColor color, f32 blur) {
  c.shadows[0] = (HzUITextShadow){.color = color, .blur = blur};
  return c;
}

// the cyan glow (10 px, 0.35 alpha) every text inside a HUD panel carries
hz_internal HzUITextConfig gr_in_panel(HzUITextConfig c) { return gr_glow(c, (HzUIColor){94, 242, 255, 89}, 10.0f); }

// a number with comma thousands grouping: 1,234
hz_internal String gr_thousands(Allocator *a, u32 v) {
  char *out = ALLOC_ARRAY(a, char, 16);
  u32 digits = 0, n = 0;
  char *rev = ALLOC_ARRAY(a, char, 16);
  do {
    if (digits > 0 && digits % 3 == 0)
      rev[n++] = ',';
    rev[n++] = (char)('0' + v % 10);
    v /= 10;
    digits++;
  } while (v > 0);
  for (u32 i = 0; i < n; i++)
    out[i] = rev[n - 1 - i];
  return (String){.value = out, .len = n};
}

// a box with its top-right and bottom-left corners cut by `c` px
hz_internal HzUIClipPath gr_cut_tr_bl(f32 c) {
  return (HzUIClipPath){.count = 6,
                        .points = {{{0, 0}, {0, 0}},
                                   {{1, 0}, {-c, 0}},
                                   {{1, 0}, {0, c}},
                                   {{1, 1}, {0, 0}},
                                   {{0, 1}, {c, 0}},
                                   {{0, 1}, {0, -c}}}};
}

// a box with its top-left and bottom-right corners cut by `c` px
hz_internal HzUIClipPath gr_cut_tl_br(f32 c) {
  return (HzUIClipPath){.count = 6,
                        .points = {{{0, 0}, {c, 0}},
                                   {{1, 0}, {0, 0}},
                                   {{1, 1}, {0, -c}},
                                   {{1, 1}, {-c, 0}},
                                   {{0, 1}, {0, 0}},
                                   {{0, 0}, {0, c}}}};
}

// floats around the screen centre: the element's attach point at (x, y) from the centre
hz_internal HzUIFloating gr_at_centre(v2 at, HzUIFloatingAttachPointType element) {
  return (HzUIFloating){.attach_to = UI_ATTACH_TO_ROOT,
                        .attach_points = {.element = element, .parent = UI_ATTACH_POINT_CENTER_CENTER},
                        .offset = at,
                        .z_index = GR_Z_HUD,
                        .pointer_capture_mode = GR_PASS};
}

hz_internal HzUIFloating gr_at_root(HzUIFloatingAttachPointType point, v2 offset, i16 z) {
  return (HzUIFloating){.attach_to = UI_ATTACH_TO_ROOT,
                        .attach_points = {.element = point, .parent = point},
                        .offset = offset,
                        .z_index = z,
                        .pointer_capture_mode = GR_PASS};
}

// a HUD panel: gradient, 1 px edge, 14 px cut corners, 3 px blur behind
hz_internal const HzUIElementDesc *gr_panel(HzUIElementId id, HzUILayout layout, HzUIFloating floating) {
  Allocator ta = tctx_temp_allocator(NULL);
  HzUIElementDesc *d = ALLOC(&ta, HzUIElementDesc);
  *d = (HzUIElementDesc){
      .id = id,
      .layout = layout,
      .floating = floating,
      .gradient = {.kind = UI_GRADIENT_LINEAR,
                   .angle = 135.0f,
                   .stop_count = 2,
                   .stops = {{0.0f, {10, 30, 42, 158}}, {1.0f, {6, 14, 22, 89}}}},
      .border = {.color = {94, 242, 255, 71}, .width = ui_border_outside(1)},
      .clip_path = gr_cut_tr_bl(14.0f),
      .backdrop_blur = 3.0f,
      .pointer_capture_mode = GR_PASS,
  };
  return d;
}

// a HUD label: 11 px, 0.22 em spacing, cyan at 0.85
hz_internal void gr_ui_label(const GrUi *u, HzUIString text) {
  ui_element({.opacity = opt_some(f32, 0.85f), .pointer_capture_mode = GR_PASS}) {
    ui_text(text, gr_in_panel(gr_font(u, 11.0f, UI_FONT_REGULAR, GR_CYAN, 11.0f * 0.22f)));
  }
}

// tabular digits: every digit in a cell as wide as the widest digit
hz_internal void gr_tabular(GrUi *u, String s, HzUITextConfig c) {
  HzHandleT(UIFontData) font = ui_family_resolve(u->ui, u->hud, (UIFontStyleTag){.weight = c.weight});
  f32 cell = 0.0f;
  for (char d = '0'; d <= '9'; d++)
    cell = m_maxf(cell, ui_measure_text_width_spaced(u->ui, font, &d, 1, c.fontSize, c.letterSpacing));
  // every run shares one font, so top-aligned runs share one baseline
  ui_element({.pointer_capture_mode = GR_PASS}) {
    for (u32 i = 0; i < s.len; i++) {
      HzUIString ch = {.chars = &s.value[i], .length = 1};
      if (s.value[i] < '0' || s.value[i] > '9') {
        // a run of non-digits stays one text element
        while (i + 1 < s.len && (s.value[i + 1] < '0' || s.value[i + 1] > '9')) {
          ch.length++;
          i++;
        }
        ui_text(ch, c);
        continue;
      }
      ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(cell)}, .child_alignment = {.x = UI_ALIGN_X_CENTER}},
                  .pointer_capture_mode = GR_PASS}) {
        ui_text(ch, c);
      }
    }
  }
}

hz_internal HzUIString gr_cstr(CString s) { return (HzUIString){.chars = s, .length = (i32)cstr_len(s)}; }

hz_internal HzUIString gr_wave_name(i32 index) { return gr_cstr(gr_wave_label(index)); }

hz_internal HzUIString gr_weapon_name(u32 weapon) { return gr_cstr(gr_weapon_label(weapon)); }

// ---- HUD: top bar ----

hz_internal void gr_draw_top(GrUi *u, const GrHud *h, const GrUiView *v) {
  Allocator ta = tctx_temp_allocator(NULL);
  ui_element({.id = ui_id("GrHudTop"),
              .layout = {.child_gap = 14},
              .floating = gr_at_root(UI_ATTACH_POINT_CENTER_TOP, (v2){0.0f, (v->touch ? 8.0f : 22.0f) + v->safe.top}, GR_Z_HUD),
              .pointer_capture_mode = GR_PASS}) {
    HzUILayout wave = {.sizing = {.width = ui_sizing_fit(.min = v->narrow ? 0.0f : 250.0f)},
                       .padding = {20, 20, 10, 12},
                       .layout_direction = UI_TOP_TO_BOTTOM,
                       .child_alignment = {.x = UI_ALIGN_X_CENTER}};
    ui_element_of(gr_panel(ui_id("GrWavePanel"), wave, (HzUIFloating){0})) {
      ui_element({.layout = {.child_gap = 8, .child_alignment = {.y = UI_ALIGN_Y_BASELINE}}, .pointer_capture_mode = GR_PASS}) {
        gr_ui_label(u, ui_string("FRONT"));
        HzUITextConfig num = gr_in_panel(gr_font(u, 34.0f, UI_FONT_BOLD, GR_WHITE, GR_HUD_SPACING));
        num.lineHeight = 34.0f;
        ui_text(gr_str(str_format(&ta, "%", fmt_i32(h->wave_index + 1))), num);
        ui_element({.opacity = opt_some(f32, 0.7f), .pointer_capture_mode = GR_PASS}) {
          ui_text(gr_str(str_format(&ta, "/ %", fmt_u32(h->wave_total))),
                  gr_in_panel(gr_font(u, 16.0f, UI_FONT_REGULAR, GR_HUD_WHITE, GR_HUD_SPACING)));
        }
      }
      ui_element({.layout = {.margin = {.top = 2}}, .pointer_capture_mode = GR_PASS}) {
        ui_text(gr_wave_name(h->wave_index), gr_in_panel(gr_font(u, 12.0f, UI_FONT_REGULAR, GR_CYAN, 12.0f * 0.3f)));
      }
      ui_element({.layout = {.child_gap = 8, .margin = {.top = 8}, .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
                  .pointer_capture_mode = GR_PASS}) {
        gr_ui_label(u, ui_string("HOSTILES"));
        HzUITextConfig count = gr_font(u, 16.0f, UI_FONT_BOLD, GR_RED, GR_HUD_SPACING);
        ui_text(gr_str(str_format(&ta, "%", fmt_u32(h->hostiles))), gr_glow(count, (HzUIColor){255, 59, 92, 153}, 10.0f));
        ui_element({.layout = {.child_gap = 3}, .pointer_capture_mode = GR_PASS}) {
          u32 pips = MIN(h->hostiles, 12u);
          for (u32 i = 0; i < pips; i++) {
            ui_element({.layout = {.sizing = {ui_sizing_fixed(7.0f), ui_sizing_fixed(7.0f)}},
                        .background_color = GR_RED,
                        .shadow = {.color = {255, 59, 92, 204}, .blur = 8.0f},
                        .transform = opt_some(HzUITransform, ((HzUITransform){.rotation = 45.0f})),
                        .pointer_capture_mode = GR_PASS}) {}
          }
        }
      }
    }

    HzUILayout score = {.sizing = {.width = ui_sizing_fit(.min = v->narrow ? 0.0f : 150.0f), .height = ui_sizing_grow(0)},
                        .padding = {20, 20, 10, 10},
                        .layout_direction = UI_TOP_TO_BOTTOM,
                        .child_alignment = {UI_ALIGN_X_RIGHT, UI_ALIGN_Y_CENTER}};
    ui_element_of(gr_panel(ui_id("GrScorePanel"), score, (HzUIFloating){0})) {
      gr_ui_label(u, ui_string("SCORE"));
      gr_tabular(u, gr_thousands(&ta, h->score), gr_in_panel(gr_font(u, 30.0f, UI_FONT_BOLD, GR_WHITE, GR_HUD_SPACING)));
      // the score delta pops for 1.1 s, ease-out, and holds its end state
      f32 t = h->score_pop_start >= 0.0f ? (v->now - h->score_pop_start) / 1.1f : 1.0f;
      if (t < 1.0f) {
        UIEasing out = ui_easing_out();
        f32 op = ui_keyframes(t, (UIKeyframe[]){{0.0f, 0.0f, out}, {0.15f, 1.0f, out}, {0.8f, 1.0f, out}, {1.0f, 0.0f, out}}, 4);
        f32 y = ui_keyframes(t, (UIKeyframe[]){{0.0f, 6.0f, out}, {0.15f, 0.0f, out}, {1.0f, -4.0f, out}}, 3);
        // 18 px from the right, 2 px below the padding box, inside the panel's cut corners
        ui_element({.floating = {.attach_to = UI_ATTACH_TO_PARENT,
                                 .attach_points = {.element = UI_ATTACH_POINT_RIGHT_BOTTOM, .parent = UI_ATTACH_POINT_RIGHT_BOTTOM},
                                 .offset = {-19.0f, 1.0f},
                                 .pointer_capture_mode = GR_PASS},
                    .opacity = opt_some(f32, op),
                    .transform = opt_some(HzUITransform, ((HzUITransform){.translate = {0.0f, y}})),
                    .pointer_capture_mode = GR_PASS}) {
          ui_text(gr_str(str_format(&ta, "+%", fmt_u32(h->score_delta))),
                  gr_in_panel(gr_font(u, 13.0f, UI_FONT_REGULAR, GR_AMBER, GR_HUD_SPACING)));
        }
      }
    }
  }
}

// ---- HUD: banner ----

// the banner's 0.5 s entry on cubic-bezier(0.2, 0.9, 0.3, 1.2); each line sets its own letter spacing
hz_internal void gr_draw_banner(GrUi *u, const HudMessages *msgs, const GrUiView *v) {
  f32 size = v->narrow ? 34.0f : 56.0f;
  HudMsgStyle style = {.attach = UI_ATTACH_POINT_CENTER_CENTER,
                       .parent = UI_ATTACH_POINT_CENTER_TOP,
                       .offset = {0.0f, (v->touch ? 0.33f : 0.24f) * v->h}, // touch: the top panels fill a short screen's top quarter
                       .z_index = GR_Z_HUD,
                       .anim = HUD_MSG_ANIM_POP,
                       .pop_scale = 1.25f,
                       .body_gap = 6.0f};
  HzUITextConfig t = gr_font(u, size, UI_FONT_BOLD, GR_WHITE, size * 0.28f);
  t.textAlignment = UI_TEXT_ALIGN_CENTER;
  HzUITextConfig s = gr_font(u, 16.0f, UI_FONT_REGULAR, GR_CYAN, 16.0f * 0.5f);
  s.textAlignment = UI_TEXT_ALIGN_CENTER;
  for (u32 k = 0; k < 3; k++) {
    HzUITextConfig kt = t, ks = s;
    if (k == GR_BANNER_KIND_ALERT) {
      kt.textColor = (HzUIColor){255, 227, 232, 255};
      kt.shadows[0] = (HzUITextShadow){.color = {255, 59, 92, 217}, .blur = 26.0f};
    } else {
      kt.shadows[0] = (HzUITextShadow){.color = k == GR_BANNER_KIND_CLEAR ? (HzUIColor){255, 181, 71, 204} : (HzUIColor){94, 242, 255, 179},
                                       .blur = 24.0f};
      kt.shadows[1] = (HzUITextShadow){.color = GR_WHITE, .blur = 2.0f};
    }
    if (k == GR_BANNER_KIND_CLEAR)
      ks.textColor = GR_AMBER;
    style.kinds[k] = (HudMsgKindStyle){.title = kt, .body = ks};
  }
  style.title = t;
  style.body = s;
  hud_msg_layout(msgs, u->ui, HUD_MSG_BANNER, &style);
}

// ---- HUD: reticle layer ----

hz_internal void gr_draw_crosshair(GrUi *u, const GrHud *h, const GrUiView *v, f32 hud_alpha) {
  f32 a = gr_tween(&u->crosshair_fade, h->crosshair_hidden ? 0.0f : 1.0f, 0.15f, v->dt);
  if (a <= 0.0f)
    return;
  // the spread cone's half-angle in px of the view height, plus 7, rounded to 0.5
  f32 half_fov = h->fov * (M_PI / 180.0f) * 0.5f;
  f32 gap = m_tanf(h->reticle_spread) / m_tanf(half_fov) * (v->h * 0.5f) + 7.0f;
  gap = m_roundf(gap * 2.0f) * 0.5f;
  HzUIColor color = h->on_target ? (HzUIColor){255, 77, 106, 255} : (HzUIColor){234, 250, 255, 235};
  // a 3 px black drop shadow (0.6 alpha) under each painted piece
  HzUIShadow drop = {.color = {0, 0, 0, 153.0f * color.a / 255.0f}, .blur = 3.0f};
  Allocator ta = tctx_temp_allocator(NULL);

  if (h->crosshair_style != GR_CROSSHAIR_DOT) {
    b32 dashed = h->crosshair_style == GR_CROSSHAIR_RING;
    f32 half = gap + 8.0f;
    ShaderGrUiRingRingParams *p = ALLOC(&ta, ShaderGrUiRingRingParams);
    p->ring = (vec4){gap, dashed ? 2.0f : 1.5f, dashed ? 1.0f : 0.0f, half};
    p->color = (vec4){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    p->effect = (vec4){0.55f * a * hud_alpha, 0.6f, 1.5f, 0.0f};
    ui_element({.id = ui_id("GrRing"),
                .layout = {.sizing = {ui_sizing_fixed(2.0f * half), ui_sizing_fixed(2.0f * half)}},
                .floating = gr_at_centre((v2){-half, -half}, UI_ATTACH_POINT_LEFT_TOP),
                .shader = {.pipeline = u->ring_pipeline, .uniforms = p, .uniforms_size = sizeof(*p)},
                .pointer_capture_mode = GR_PASS}) {}
  }
  if (h->crosshair_style != GR_CROSSHAIR_RING) {
    // the four crosshair ticks: 2 x 9 bars 3 px past the gap
    for (u32 i = 0; i < 4; i++) {
      b32 vertical = i < 2;
      f32 along = (i % 2 == 0) ? -gap - 12.0f : gap + 3.0f;
      v4 tick = vertical ? (v4){-1.0f, along, 2.0f, 9.0f} : (v4){along, -1.0f, 9.0f, 2.0f};
      ui_element({.id = ui_idi("GrTick", i),
                  .layout = {.sizing = {ui_sizing_fixed(tick.z), ui_sizing_fixed(tick.w)}},
                  .floating = gr_at_centre((v2){tick.x, tick.y}, UI_ATTACH_POINT_LEFT_TOP),
                  .background_color = color,
                  .shadow = drop,
                  .opacity = opt_some(f32, a),
                  .pointer_capture_mode = GR_PASS}) {}
    }
  }
  ui_element({.id = ui_id("GrDot"),
              .layout = {.sizing = {ui_sizing_fixed(3.0f), ui_sizing_fixed(3.0f)}},
              .floating = gr_at_centre((v2){-1.5f, -1.5f}, UI_ATTACH_POINT_LEFT_TOP),
              .background_color = color,
              .corner_radius = ui_corner_radius(1.5f),
              .shadow = drop,
              .opacity = opt_some(f32, a),
              .pointer_capture_mode = GR_PASS}) {}
}

hz_internal void gr_draw_hitmarker(const GrHud *h, const GrUiView *v) {
  if (h->hitmarker_start < 0.0f)
    return;
  b32 kill = h->hitmarker_kill;
  f32 t = (v->now - h->hitmarker_start) / (kill ? 0.4f : 0.18f);
  if (t >= 1.0f || t < 0.0f)
    return;
  // the hitmarker and kill-marker animation, ease-out, on a 0 x 0 container: scale about the centre
  UIEasing out = ui_easing_out();
  f32 op = ui_keyframes(t, (UIKeyframe[]){{0.0f, 1.0f, out}, {kill ? 0.6f : 0.0f, 1.0f, out}, {1.0f, 0.0f, out}}, 3);
  f32 s = ui_keyframes(t, (UIKeyframe[]){{0.0f, kill ? 1.8f : 1.35f, out}, {1.0f, kill ? 1.1f : 1.0f, out}}, 2);
  f32 bar_w = kill ? 15.0f : 11.0f;
  // a -5.5 px left offset keeps the wider kill bar's origin 2 px right of the centre
  f32 origin = -5.5f + bar_w * 0.5f;
  HzUIColor color = kill ? (HzUIColor){255, 77, 106, 255} : GR_WHITE;
  HzUIShadow glow = kill ? (HzUIShadow){.color = {255, 77, 106, 230}, .blur = 8.0f}
                         : (HzUIShadow){.color = {255, 255, 255, 204}, .blur = 6.0f};
  for (u32 i = 0; i < 4; i++) {
    f32 deg = 45.0f + 90.0f * (f32)i;
    f32 rad = deg * (M_PI / 180.0f);
    // rotate by a, move 9 px out about the bar's own centre, then the container's scale
    v2 c = {s * (origin + 9.0f * m_cosf(rad)), s * (9.0f * m_sinf(rad))};
    ui_element({.id = ui_idi("GrHitBar", i),
                .layout = {.sizing = {ui_sizing_fixed(bar_w), ui_sizing_fixed(2.0f)}},
                .floating = gr_at_centre((v2){c.x - bar_w * 0.5f, c.y - 1.0f}, UI_ATTACH_POINT_LEFT_TOP),
                .background_color = color,
                .shadow = glow,
                .opacity = opt_some(f32, op),
                .transform = opt_some(HzUITransform, ((HzUITransform){.rotation = deg, .scale = {s, s}})),
                .pointer_capture_mode = GR_PASS}) {}
  }
}

hz_internal void gr_draw_damage(GrUi *u, const GrHud *h, const GrUiView *v, f32 hud_alpha) {
  Allocator ta = tctx_temp_allocator(NULL);
  // the 190 px box turns about its centre: the quad holds its diagonal plus the drop-shadow's reach
  f32 half = 95.0f * 1.41421356f + 16.0f;
  u32 n = MIN(h->arc_count, (u32)GR_HUD_MAX_ARCS);
  for (u32 i = 0; i < n; i++) {
    const GrDamageArc *arc = &h->arcs[i];
    f32 t = (v->now - arc->start) / 1.1f;
    if (t >= 1.0f || t < 0.0f)
      continue;
    ShaderGrUiArcArcParams *p = ALLOC(&ta, ShaderGrUiArcArcParams);
    p->arc = (vec4){190.0f, 5.0f, arc->angle, half};
    p->color = (vec4){1.0f, 50.0f / 255.0f, 70.0f / 255.0f, 0.95f};
    p->glow = (vec4){1.0f, 30.0f / 255.0f, 60.0f / 255.0f, 0.9f};
    p->effect = (vec4){ui_keyframes(t, (UIKeyframe[]){{0.0f, 1.0f, ui_easing_out()}, {1.0f, 0.0f, ui_easing_out()}}, 2) * hud_alpha, 5.0f, 0.0f, 0.0f};
    ui_element({.id = ui_idi("GrDamageArc", i),
                .layout = {.sizing = {ui_sizing_fixed(2.0f * half), ui_sizing_fixed(2.0f * half)}},
                .floating = gr_at_centre((v2){-half, -half}, UI_ATTACH_POINT_LEFT_TOP),
                .shader = {.pipeline = u->arc_pipeline, .uniforms = p, .uniforms_size = sizeof(*p)},
                .pointer_capture_mode = GR_PASS}) {}
  }
}

// the kill popups and the swap prompt: WorldUi widgets (world_ui_popup / GrPromptTag), laid out by the block
hz_internal hz_noinline void gr_draw_world_ui(GrUi *u, EcsWorld *w) {
  HzUIColor ink = {234, 250, 255, 230};
  f32 spacing = 13.0f * 0.18f;
  WorldUiStyle style = {.kinds = (1u << WORLD_UI_POPUP) | (1u << WORLD_UI_PROMPT), .z_index = GR_Z_HUD};
  style.popup = (WorldUiPopupStyle){
      .attach = UI_ATTACH_POINT_LEFT_TOP,
      .text = gr_glow(gr_font(u, 18.0f, UI_FONT_BOLD, GR_WHITE, GR_HUD_SPACING), (HzUIColor){255, 181, 71, 230}, 12.0f)};
  style.prompt = (WorldUiPromptStyle){.attach = UI_ATTACH_POINT_CENTER_TOP,
                                      .key = gr_font(u, 13.0f, UI_FONT_BOLD, GR_CYAN, 0.0f),
                                      .text = gr_font(u, 13.0f, UI_FONT_REGULAR, ink, spacing),
                                      .emph = gr_font(u, 13.0f, UI_FONT_BOLD, GR_WHITE, spacing),
                                      .key_size = {24.0f, 24.0f}, // the key cap: 24 x 24 with a 1.5 px cyan border, drawn as an inset ring
                                      .key_ring = GR_CYAN,
                                      .background = {4, 16, 24, 158},
                                      .border = {94, 242, 255, 89},
                                      .padding = {14.0f, 8.0f},
                                      .gap = 10.0f};
  world_ui_layout(w, u->ui, &style);
}

hz_internal void gr_draw_reload(GrUi *u, const GrHud *h, const GrUiView *v) {
  b32 shown = h->reload_show;
  if (shown && !u->reload_shown)
    u->reload_age = 0.0f;
  u->reload_shown = shown;
  f32 a = 0.0f;
  if (shown) {
    // the infinite blink overrides the opacity transition while shown
    u->reload_age += v->dt;
    // blinks between full and 0.45 opacity in two hard steps
    UIEasing blink_ease = ui_easing_steps(2, EASE_STEP_JUMP_NONE);
    a = ui_keyframes(m_fmodf(u->reload_age, 0.9f) / 0.9f, (UIKeyframe[]){{0.0f, 1.0f, blink_ease}, {0.5f, 0.45f, blink_ease}, {1.0f, 1.0f, blink_ease}}, 3);
    gr_tween_snap(&u->reload_fade, a);
  } else {
    a = gr_tween(&u->reload_fade, 0.0f, 0.2f, v->dt);
  }
  if (a <= 0.0f)
    return;
  HzUITextConfig c = gr_glow(gr_font(u, 13.0f, UI_FONT_REGULAR, GR_AMBER, 13.0f * 0.4f), (HzUIColor){255, 181, 71, 204}, 10.0f);
  ui_element({.id = ui_id("GrReload"),
              .floating = gr_at_centre((v2){0.0f, 48.0f}, UI_ATTACH_POINT_CENTER_TOP),
              .opacity = opt_some(f32, a),
              .pointer_capture_mode = GR_PASS}) {
    ui_text(h->reloading ? ui_string("RELOADING") : ui_string("RELOAD"), c);
  }
}

// ---- touch controls: the block's geometry in the HUD's ink, kept faint so the scene reads through ----

#define GR_TOUCH_INK ((HzUIColor){6, 14, 22, 56})

hz_internal void gr_touch_disc(HzUIElementId id, v2 c, f32 d, HzUIColor fill, HzUIColor edge, u16 edge_w) {
  ui_element({.id = id,
              .floating = gr_at_root(UI_ATTACH_POINT_LEFT_TOP, (v2){c.x - d * 0.5f, c.y - d * 0.5f}, GR_Z_HUD + 1),
              .layout = {.sizing = {ui_sizing_fixed(d), ui_sizing_fixed(d)}},
              .background_color = fill,
              .border = {.color = edge, .width = ui_border_outside(edge_w)},
              .corner_radius = ui_corner_radius(d * 0.5f),
              .pointer_capture_mode = GR_PASS}) {}
}

// alert: rim and label in the accent while the fill stays faint (a cue, not a press)
hz_internal void gr_draw_touch_button(GrUi *u, const TouchControls *tc, u32 b, HzUIColor accent, b32 primary, b32 alert) {
  TouchButtonView v = touch_controls_button(tc, b);
  if (!v.visible)
    return;
  b32 on = v.held || v.lit;
  // pressed: the disc sinks a little and fills with the accent
  f32 d = v.diameter_px * (v.held ? 0.94f : 1.0f);
  HzUIColor fill = on ? gr_alpha(accent, v.held ? 0.22f : 0.14f) : GR_TOUCH_INK;
  HzUIColor edge = gr_alpha(accent, on ? 0.85f : alert ? 0.7f : 0.32f);
  ui_element({.id = ui_idi("GrTouchButton", b),
              .floating = gr_at_root(UI_ATTACH_POINT_LEFT_TOP, (v2){v.center_px.x - d * 0.5f, v.center_px.y - d * 0.5f}, GR_Z_HUD + 1),
              .layout = {.sizing = {ui_sizing_fixed(d), ui_sizing_fixed(d)},
                         .child_alignment = {UI_ALIGN_X_CENTER, UI_ALIGN_Y_CENTER}},
              .background_color = fill,
              .border = {.color = edge, .width = ui_border_outside(1)},
              .corner_radius = ui_corner_radius(d * 0.5f),
              .backdrop_blur = 2.0f,
              .pointer_capture_mode = GR_PASS}) {
    HzUIColor ink = on || alert ? accent : gr_alpha(GR_HUD_WHITE, 0.78f);
    f32 size = primary ? 12.0f : 10.0f;
    ui_text(ui_str_c(v.label), gr_glow(gr_font(u, size, UI_FONT_BOLD, ink, size * 0.18f), gr_alpha(accent, 0.35f), 8.0f));
  }
  // the primary button carries an inner sight ring, the HUD's reticle language
  if (primary)
    gr_touch_disc(ui_idi("GrTouchSight", b), v.center_px, d * 0.62f, (HzUIColor){0}, gr_alpha(accent, on ? 0.5f : 0.16f), 1);
}

hz_internal void gr_draw_touch(GrUi *u, const GrTouch *t, const GrHud *h) {
  const TouchControls *tc = &t->tc;
  TouchStickView s = touch_controls_stick(tc);
  if (s.visible) {
    HzUIColor thumb = t->sprint ? GR_AMBER : GR_CYAN;
    gr_touch_disc(ui_id("GrTouchStickBase"), s.center_px, s.outer_radius_px * 2.0f, GR_TOUCH_INK, gr_alpha(GR_CYAN, 0.3f), 1);
    gr_touch_disc(ui_id("GrTouchStickReach"), s.center_px, (s.max_speed_radius_px + s.thumb_radius_px) * 2.0f,
                  (HzUIColor){0}, gr_alpha(GR_CYAN, 0.14f), 1);
    gr_touch_disc(ui_id("GrTouchStickThumb"), s.thumb_px, s.thumb_radius_px * 2.0f, gr_alpha(thumb, 0.35f), gr_alpha(thumb, 0.9f), 1);
  }
  // reload asks for attention in the HUD's warning amber, as the RELOAD cue does
  b32 needs_reload = h->reload_show && !h->reloading;
  gr_draw_touch_button(u, tc, GR_TOUCH_FIRE, GR_CYAN, true, false);
  gr_draw_touch_button(u, tc, GR_TOUCH_ADS, GR_CYAN, false, false);
  gr_draw_touch_button(u, tc, GR_TOUCH_JUMP, GR_CYAN, false, false);
  gr_draw_touch_button(u, tc, GR_TOUCH_RELOAD, needs_reload ? GR_AMBER : GR_CYAN, false, needs_reload);
  gr_draw_touch_button(u, tc, GR_TOUCH_SWAP, GR_CYAN, false, false);
  gr_draw_touch_button(u, tc, GR_TOUCH_PAUSE, GR_CYAN, false, false);
}

// ---- HUD: bottom left ----

hz_internal void gr_draw_radar(GrUi *u, const GrRadar *radar, const GrUiView *v, f32 hud_alpha) {
  f32 size = v->touch ? 92.0f : v->narrow ? 120.0f : 176.0f;
  f32 to_canvas = GR_RADAR_CANVAS / size;
  f32 drop_sigma = 4.0f * to_canvas; // an 8 px drop shadow in logical px, measured in canvas px
  f32 bleed = 3.0f * drop_sigma;
  f32 bleed_px = bleed / to_canvas;
  Allocator ta = tctx_temp_allocator(NULL);
  ShaderGrUiRadarRadarParams *p = ALLOC(&ta, ShaderGrUiRadarRadarParams);
  f32 c = GR_RADAR_CANVAS * 0.5f;
  f32 r = c - 6.0f;
  // paint order: the drones, then the ground guns, then the packs
  u32 total = MIN(radar->count, (u32)GR_RADAR_MAX_CONTACTS);
  u32 n = 0;
  HudMap map = {.px_per_m = r / GR_RADAR_RANGE, .heading = radar->heading, .rotate = true, .half_px = {r, r},
                .clip = HUD_MAP_CLIP_CIRCLE};
  for (u32 pass = GR_RADAR_DOT; pass <= GR_RADAR_CROSS; pass++) {
    for (u32 i = 0; i < total; i++) {
      const GrRadarContact *k = &radar->contacts[i];
      if (k->icon != pass)
        continue;
      HudMapPoint pt = hud_map_project(&map, (v2){k->x, k->z}, true);
      f32 marker = m_absf(k->dy) > 3.0f ? (k->dy > 0.0f ? 1.0f : 2.0f) : 0.0f;
      p->contacts[n++] = (vec4){c + pt.px.x, c + pt.px.y, pt.inside ? 5.5f : 3.0f,
                                (f32)k->icon + 4.0f * marker + 16.0f * (f32)k->color};
    }
  }
  p->params = (vec4){GR_RADAR_CANVAS, radar->sweep, (f32)n, bleed};
  p->effect = (vec4){hud_alpha, drop_sigma, 0.25f, 5.0f};
  p->palette[GR_DRONE_SCOUT] = (vec4){1.0f, 45.0f / 255.0f, 85.0f / 255.0f, 1.0f};      // #ff2d55
  p->palette[GR_DRONE_STRIKER] = (vec4){1.0f, 90.0f / 255.0f, 31.0f / 255.0f, 1.0f};    // #ff5a1f
  p->palette[GR_DRONE_SENTINEL] = (vec4){214.0f / 255.0f, 59.0f / 255.0f, 1.0f, 1.0f};  // #d63bff
  p->palette[GR_RADAR_COLOR_GUN] = (vec4){1.0f, 182.0f / 255.0f, 39.0f / 255.0f, 1.0f}; // #ffb627
  p->palette[GR_RADAR_COLOR_PACK] = (vec4){56.0f / 255.0f, 1.0f, 122.0f / 255.0f, 1.0f}; // #38ff7a
  ui_element({.id = ui_id("GrRadar"),
              .layout = {.sizing = {ui_sizing_fixed(size), ui_sizing_fixed(size)}, .layout_direction = UI_STACK},
              .pointer_capture_mode = GR_PASS}) {
    f32 quad = size + 2.0f * bleed_px;
    ui_element({.id = ui_id("GrRadarCanvas"),
                .layout = {.sizing = {ui_sizing_fixed(quad), ui_sizing_fixed(quad)}},
                .transform = opt_some(HzUITransform, ((HzUITransform){.translate = {-bleed_px, -bleed_px}})),
                .shader = {.pipeline = u->radar_pipeline, .uniforms = p, .uniforms_size = sizeof(*p)},
                .pointer_capture_mode = GR_PASS}) {}
  }
}

hz_internal void gr_draw_health(GrUi *u, const GrHud *h, const GrUiView *v) {
  Allocator ta = tctx_temp_allocator(NULL);
  f32 fraction = h->max_health > 0.0f ? h->health / h->max_health : 0.0f;
  b32 critical = h->critical;
  u->critical_age = critical ? u->critical_age + v->dt : 0.0f;
  UIEasing blink_ease = ui_easing_steps(2, EASE_STEP_JUMP_NONE);
  f32 blink = critical ? ui_keyframes(m_fmodf(u->critical_age, 0.6f) / 0.6f, (UIKeyframe[]){{0.0f, 1.0f, blink_ease}, {0.5f, 0.45f, blink_ease}, {1.0f, 1.0f, blink_ease}}, 3) : 1.0f;
  HzUILayout lay = {.sizing = {.width = ui_sizing_fit(.min = v->touch ? 200.0f : v->narrow ? 0.0f : 250.0f)},
                    .padding = v->touch ? (HzUIPadding){12, 12, 6, 8} : (HzUIPadding){16, 16, 10, 12},
                    .layout_direction = UI_TOP_TO_BOTTOM};
  // touch: bottom centre, between the stick and the buttons
  HzUIFloating fl = v->touch ? gr_at_root(UI_ATTACH_POINT_CENTER_BOTTOM, (v2){0.0f, -(8.0f + v->safe.bottom)}, GR_Z_HUD)
                             : (HzUIFloating){0};
  ui_element_of(gr_panel(ui_id("GrHealthPanel"), lay, fl)) {
    gr_ui_label(u, ui_string("SHIELD INTEGRITY"));
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .child_gap = 4, .margin = {.top = 8}},
                .pointer_capture_mode = GR_PASS}) {
      HzUIColor on = critical ? GR_RED : GR_CYAN;
      HzUIColor off = critical ? (HzUIColor){255, 59, 92, 36} : (HzUIColor){94, 242, 255, 31};
      for (u32 i = 0; i < 10; i++) {
        f32 fill = m_clampf(fraction * 10.0f - (f32)i, 0.0f, 1.0f);
        HzUIShadow glow = {0};
        if (fill > 0.0f)
          glow = critical ? (HzUIShadow){.color = {255, 59, 92, 179}, .blur = fill * 12.0f}
                          : (HzUIShadow){.color = {94, 242, 255, 128}, .blur = fill * 10.0f};
        ui_element({.id = ui_idi("GrHealthSeg", i),
                    .layout = {.sizing = {ui_sizing_grow(0), ui_sizing_fixed(v->touch ? 8.0f : 12.0f)}},
                    .gradient = {.kind = UI_GRADIENT_LINEAR,
                                 .angle = 90.0f,
                                 .stop_count = 4,
                                 .stops = {{0.0f, on}, {fill, on}, {fill, off}, {1.0f, off}}},
                    .shadow = glow,
                    .transform = opt_some(HzUITransform, ((HzUITransform){.skew = {-20.0f, 0.0f}})),
                    .opacity = opt_some(f32, blink),
                    .pointer_capture_mode = GR_PASS}) {}
      }
    }
    // the health value: 16 px from the right, 7 px from the top of the padding box
    ui_element({.floating = {.attach_to = UI_ATTACH_TO_PARENT,
                             .attach_points = {.element = UI_ATTACH_POINT_RIGHT_TOP, .parent = UI_ATTACH_POINT_RIGHT_TOP},
                             .offset = {-17.0f, 8.0f},
                             .pointer_capture_mode = GR_PASS},
                .pointer_capture_mode = GR_PASS}) {
      ui_text(gr_str(str_format(&ta, "%", fmt_i32((i32)m_ceilf(h->health)))),
              gr_in_panel(gr_font(u, 15.0f, UI_FONT_BOLD, GR_HUD_WHITE, GR_HUD_SPACING)));
    }
  }
}

// ---- HUD: bottom right ----

hz_internal void gr_draw_ammo(GrUi *u, const GrHud *h, const GrUiView *v) {
  Allocator ta = tctx_temp_allocator(NULL);
  HzUILayout lay = {.sizing = {.width = ui_sizing_fit(.min = v->touch ? 150.0f : v->narrow ? 0.0f : 270.0f)},
                    .padding = v->touch ? (HzUIPadding){14, 12, 6, 8} : (HzUIPadding){22, 18, 10, 12},
                    .layout_direction = UI_TOP_TO_BOTTOM};
  // touch: top right under the pause button, clear of the button cluster
  HzUIFloating fl = v->touch ? gr_at_root(UI_ATTACH_POINT_RIGHT_TOP, (v2){-(12.0f + v->safe.right), v->touch_top + 48.0f}, GR_Z_HUD)
                             : gr_at_root(UI_ATTACH_POINT_RIGHT_BOTTOM, (v2){-28.0f - v->safe.right, -26.0f - v->safe.bottom}, GR_Z_HUD);
  ui_element_of(gr_panel(ui_id("GrAmmoPanel"), lay, fl)) {
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)},
                           .child_gap = 8,
                           .child_alignment = {UI_ALIGN_X_RIGHT, UI_ALIGN_Y_BASELINE}},
                .pointer_capture_mode = GR_PASS}) {
      f32 ammo_size = v->touch ? 30.0f : 54.0f;
      HzUITextConfig ammo = gr_in_panel(gr_font(u, ammo_size, UI_FONT_BOLD, h->ammo_low ? GR_AMBER : GR_WHITE, GR_HUD_SPACING));
      ammo.lineHeight = ammo_size;
      if (h->ammo_low)
        ammo = gr_glow(ammo, (HzUIColor){255, 181, 71, 204}, 14.0f);
      if (h->swapping)
        ui_text(ui_string("--"), ammo);
      else
        gr_tabular(u, h->ammo < 10 ? str_format(&ta, "0%", fmt_u32(h->ammo)) : str_format(&ta, "%", fmt_u32(h->ammo)), ammo);
      ui_element({.opacity = opt_some(f32, 0.7f), .pointer_capture_mode = GR_PASS}) {
        HzUITextConfig reserve = gr_in_panel(gr_font(u, 18.0f, UI_FONT_REGULAR, GR_HUD_WHITE, GR_HUD_SPACING));
        // Bahnschrift has no U+221E: the font family takes it per glyph from the next face that has it
        ui_text(ui_string("/ \xE2\x88\x9E"), reserve);
      }
    }
    // the ammo ticks: min(18, magazine) equal columns, gap 3
    u32 mag = MIN(h->magazine, (u32)GR_UI_MAX_TICKS);
    u32 cols = MIN(mag, (u32)GR_TICK_ROW);
    u32 rows = cols > 0 ? (mag + cols - 1) / cols : 0;
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)},
                           .layout_direction = UI_TOP_TO_BOTTOM,
                           .child_gap = 3,
                           .margin = {.top = 6}},
                .pointer_capture_mode = GR_PASS}) {
      for (u32 row = 0; row < rows; row++) {
        ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .child_gap = 3}, .pointer_capture_mode = GR_PASS}) {
          for (u32 col = 0; col < cols; col++) {
            u32 i = row * cols + col;
            if (i >= mag) {
              ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}}, .pointer_capture_mode = GR_PASS}) {}
              continue;
            }
            b32 spent = h->swapping || i >= h->ammo;
            f32 a = gr_tween(&u->tick_alpha[i], spent ? 0.14f : 1.0f, 0.08f, v->dt);
            ui_element({.id = ui_idi("GrTick", 100 + i),
                        .layout = {.sizing = {ui_sizing_grow(0), ui_sizing_fixed(v->touch ? 5.0f : 9.0f)}},
                        .background_color = GR_CYAN,
                        .shadow = spent ? (HzUIShadow){0} : (HzUIShadow){.color = {94, 242, 255, 140}, .blur = 6.0f},
                        .transform = opt_some(HzUITransform, ((HzUITransform){.skew = {-20.0f, 0.0f}})),
                        .opacity = opt_some(f32, a),
                        .pointer_capture_mode = GR_PASS}) {}
          }
        }
      }
    }
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .margin = {.top = 8}, .child_alignment = {.x = UI_ALIGN_X_RIGHT}},
                .pointer_capture_mode = GR_PASS}) {
      ui_text(gr_weapon_name(h->weapon), gr_in_panel(gr_font(u, 11.0f, UI_FONT_REGULAR, GR_CYAN, 11.0f * 0.3f)));
    }
  }
}

hz_internal void gr_draw_hud(GrUi *u, EcsWorld *w, const GrHud *h, const GrRadar *radar, const HudMessages *msgs, const GrUiView *v) {
  // the HUD fades out over 0.4 s when hidden
  f32 a = gr_tween(&u->hud_fade, h->hud_visible ? 1.0f : 0.0f, 0.4f, v->dt);
  if (a <= 0.0f)
    return;
  ui_element({.id = ui_id("GrHud"),
              .layout = {.sizing = {ui_sizing_fixed(v->w), ui_sizing_fixed(v->h)}},
              .floating = gr_at_root(UI_ATTACH_POINT_LEFT_TOP, (v2){0}, GR_Z_HUD),
              .opacity = opt_some(f32, a),
              .pointer_capture_mode = GR_PASS}) {
    gr_draw_top(u, h, v);
    gr_draw_banner(u, msgs, v);
    gr_draw_crosshair(u, h, v, a);
    gr_draw_hitmarker(h, v);
    gr_draw_damage(u, h, v, a);
    gr_draw_reload(u, h, v);
    gr_draw_world_ui(u, w);
    if (v->touch) {
      // the radar takes the top left (the stick owns the bottom left); the shield sits bottom centre
      ui_element({.id = ui_id("GrHudTopLeft"),
                  .floating = gr_at_root(UI_ATTACH_POINT_LEFT_TOP, (v2){12.0f + v->safe.left, v->touch_top}, GR_Z_HUD),
                  .pointer_capture_mode = GR_PASS}) {
        gr_draw_radar(u, radar, v, a);
      }
      gr_draw_health(u, h, v);
    } else {
      ui_element({.id = ui_id("GrHudBottomLeft"),
                  .layout = {.child_gap = 14, .child_alignment = {.y = UI_ALIGN_Y_BOTTOM}},
                  .floating = gr_at_root(UI_ATTACH_POINT_LEFT_BOTTOM, (v2){28.0f + v->safe.left, -26.0f - v->safe.bottom}, GR_Z_HUD),
                  .pointer_capture_mode = GR_PASS}) {
        gr_draw_radar(u, radar, v, a);
        gr_draw_health(u, h, v);
      }
    }
    gr_draw_ammo(u, h, v);
  }
}

// ---- menus: pieces ----

typedef struct GrUiFrame {
  GrUi *u;
  const GrUiView *v;
  InputSystem *input;
  GrActions *actions;
  const GrSettings *settings;
} GrUiFrame;

// a menu button (or the primary one): chamfered, fires on click (mouse release over it)
hz_internal b32 gr_button(const GrUiFrame *f, GrUiButton b, HzUIString label, b32 primary, b32 big, b32 active) {
  GrUi *u = f->u;
  UITween *hover = &u->button_hover[b];
  f32 k = hover->value;
  f32 size = big ? 18.0f : 15.0f;
  Allocator ta = tctx_temp_allocator(NULL);
  HzUIElementDesc *d = ALLOC(&ta, HzUIElementDesc);
  *d = (HzUIElementDesc){
      .id = ui_idi("GrButton", b),
      .layout = {.padding = big ? (HzUIPadding){54, 54, 16, 16} : (HzUIPadding){26, 26, 12, 12},
                 .child_alignment = {UI_ALIGN_X_CENTER, UI_ALIGN_Y_CENTER}},
      .clip_path = gr_cut_tl_br(12.0f),
      .transform = opt_some(HzUITransform, ((HzUITransform){.translate = {0.0f, -k}})),
  };
  if (primary) {
    // the hover gradient swaps at once (no fade); the 30 px glow is clipped by the cut corners
    b32 hot = hover->to > 0.5f;
    d->gradient = (UIGradient){.kind = UI_GRADIENT_LINEAR,
                               .angle = 135.0f,
                               .stop_count = 2,
                               .stops = {{0.0f, hot ? (HzUIColor){143, 247, 255, 255} : (HzUIColor){94, 242, 255, 255}},
                                         {1.0f, hot ? (HzUIColor){63, 208, 234, 255} : (HzUIColor){34, 184, 214, 255}}}};
  } else {
    d->background_color = ui_color_lerp((HzUIColor){8, 24, 34, 153}, (HzUIColor){94, 242, 255, 51}, k);
    d->border = (UIBorder){.color = {94, 242, 255, 115}, .width = ui_border_outside(1)};
  }
  HzUITextConfig c = gr_font(u, size, primary ? UI_FONT_BOLD : UI_FONT_REGULAR,
                             primary ? (HzUIColor){3, 16, 23, 255} : GR_HUD_WHITE, size * 0.3f);
  b32 hovered = false;
  ui_element_of(d) {
    hovered = active && ui_hovered();
    ui_text(label, c);
  }
  gr_tween(hover, hovered ? 1.0f : 0.0f, 0.15f, f->v->dt);
  if (hovered)
    u->cursor = OS_CURSOR_HAND;
  return hovered && f->input->buttons[MOUSE_LEFT].released_this_frame;
}

hz_internal void gr_eyebrow(const GrUi *u, HzUIString text) {
  ui_text(text, gr_font(u, 12.0f, UI_FONT_REGULAR, GR_CYAN, 12.0f * 0.35f));
}

// the controls list: nine key + action pairs in 4 auto columns (2 at <= 700 px), gap 8 18
hz_internal void gr_controls(const GrUi *u, const GrUiView *v) {
  // the keys mean nothing on a touch screen (the on-screen buttons label themselves); a short screen has no room
  if (v->touch || v->short_screen)
    return;
  u32 cols = v->very_narrow ? 2u : 4u;
  ui_element({.layout = {.child_gap = 18, .margin = {.top = 26}}, .pointer_capture_mode = GR_PASS}) {
    for (u32 c = 0; c < cols; c++) {
      ui_element({.layout = {.layout_direction = UI_TOP_TO_BOTTOM, .child_gap = 8}, .pointer_capture_mode = GR_PASS}) {
        for (u32 i = c; i < 9; i += cols) {
          HzUIString key = {0}, action = {0};
          switch (i) {
          case 0: key = ui_string("WASD"); action = ui_string("MOVE"); break;
          case 1: key = ui_string("MOUSE"); action = ui_string("AIM"); break;
          case 2: key = ui_string("LMB"); action = ui_string("FIRE"); break;
          case 3: key = ui_string("RMB"); action = ui_string("ZOOM"); break;
          case 4: key = ui_string("R"); action = ui_string("RELOAD"); break;
          case 5: key = ui_string("E"); action = ui_string("SWAP GUN"); break;
          case 6: key = ui_string("SPACE"); action = ui_string("JUMP"); break;
          case 7: key = ui_string("SHIFT"); action = ui_string("SPRINT"); break;
          default: key = ui_string("ESC"); action = ui_string("PAUSE"); break;
          }
          // letter spacing 0.12 em of 12 px, the same px on the key cap
          ui_element({.layout = {.child_gap = 8, .child_alignment = {.y = UI_ALIGN_Y_CENTER}}, .pointer_capture_mode = GR_PASS}) {
            ui_element({.layout = {.padding = {7, 7, 3, 3}},
                        .background_color = {94, 242, 255, 20},
                        .border = {.color = {94, 242, 255, 115}, .width = ui_border_outside(1)},
                        .pointer_capture_mode = GR_PASS}) {
              ui_text(key, gr_font(u, 11.0f, UI_FONT_REGULAR, GR_CYAN, 1.44f));
            }
            ui_text(action, gr_font(u, 12.0f, UI_FONT_REGULAR, (HzUIColor){234, 246, 255, 191}, 1.44f));
          }
        }
      }
    }
  }
}

hz_internal GrSettings *gr_settings_edit(const GrUiFrame *f) {
  if (!f->actions->settings_changed) {
    f->actions->settings = *f->settings;
    f->actions->settings_changed = true;
  }
  return &f->actions->settings;
}

hz_internal HzUIString gr_quality_name(i32 q) {
  switch (q) {
  case GR_QUALITY_LOW: return ui_string("Low");
  case GR_QUALITY_MEDIUM: return ui_string("Medium");
  case GR_QUALITY_ULTRA: return ui_string("Ultra");
  default: return ui_string("High");
  }
}

// a settings row: 110 px label, flexible control, 48 px value, gap 12
hz_internal void gr_setting_label(const GrUi *u, HzUIString text) {
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(110.0f)}}, .pointer_capture_mode = GR_PASS}) {
    ui_text(text, gr_font(u, 12.0f, UI_FONT_REGULAR, (HzUIColor){234, 246, 255, 191}, 12.0f * 0.2f));
  }
}

// a dropdown: the dark closed control, and its popup as a floating list
hz_internal void gr_select(const GrUiFrame *f, GrUiSettings *st, u32 screen, f32 width, b32 active) {
  GrUi *u = f->u;
  i32 current = f->actions->settings_changed ? f->actions->settings.quality : f->settings->quality;
  ui_element({.id = ui_idi("GrQuality", screen),
              .layout = {.sizing = {.width = ui_sizing_fixed(width)},
                         .padding = {8, 8, 6, 6},
                         .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
              .background_color = {8, 24, 34, 204},
              .border = {.color = {94, 242, 255, 89}, .width = ui_border_outside(1)}}) {
    if (active)
      ui_interact_dropdown(&st->quality, f->input);
    if (active && ui_hovered())
      u->cursor = OS_CURSOR_ARROW;
    ui_text(gr_quality_name(current), gr_font(u, 13.0f, UI_FONT_REGULAR, GR_HUD_WHITE, 1.3f));
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}}, .pointer_capture_mode = GR_PASS}) {}
    // the native chevron: a 6 px corner turned 45 degrees
    ui_element({.layout = {.sizing = {ui_sizing_fixed(6.0f), ui_sizing_fixed(6.0f)}, .margin = {.right = 2, .bottom = 3}},
                .border = {.color = GR_HUD_WHITE, .width = {.right = 2, .bottom = 2}},
                .transform = opt_some(HzUITransform, ((HzUITransform){.rotation = 45.0f})),
                .pointer_capture_mode = GR_PASS}) {}
    if (active && st->quality.open) {
      ui_element({.id = ui_idi("GrQualityPopup", screen),
                  .floating = {.attach_to = UI_ATTACH_TO_PARENT,
                               .attach_points = {.element = UI_ATTACH_POINT_LEFT_TOP, .parent = UI_ATTACH_POINT_LEFT_BOTTOM},
                               .offset = {0.0f, 1.0f},
                               .z_index = GR_Z_MENU_POPUP},
                  .layout = {.sizing = {.width = ui_sizing_fixed(width)}, .layout_direction = UI_TOP_TO_BOTTOM, .padding = {1, 1, 1, 1}},
                  .background_color = {8, 24, 34, 250},
                  .border = {.color = {94, 242, 255, 89}, .width = ui_border_outside(1)}}) {
        ui_interact_dropdown_content(&st->quality);
        for (i32 q = 0; q < GR_QUALITY_COUNT; q++) {
          HzUIElementId id = ui_idi("GrQualityOpt", screen * 8 + (u32)q);
          // same-frame highlight: UI_STACK lays the hover rect under the label
          ui_element({.id = id, .layout = {.sizing = {.width = ui_sizing_grow(0)}, .layout_direction = UI_STACK}}) {
            b32 hovered = ui_hovered();
            if (ui_interact_dropdown_option(&st->quality, f->input, q == current))
              gr_settings_edit(f)->quality = q;
            ui_element({.layout = {.sizing = {ui_sizing_grow(0), ui_sizing_grow(0)}},
                        .background_color = hovered ? (HzUIColor){94, 242, 255, 51} : (HzUIColor){0, 0, 0, 0},
                        .pointer_capture_mode = GR_PASS}) {}
            ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .padding = {8, 8, 4, 4}}, .pointer_capture_mode = GR_PASS}) {
              ui_text(gr_quality_name(q), gr_font(u, 13.0f, UI_FONT_REGULAR, GR_HUD_WHITE, 1.3f));
            }
          }
        }
      }
    }
  }
}

// a slider: 8 px track (1 px rgb(133) edge, rgb(59) fill), accent progress, 16 px thumb
hz_internal b32 gr_range(const GrUiFrame *f, UIInteract_Slider *s, HzUIElementId id, f32 min, f32 max, f32 width, b32 active) {
  f32 thumb = 16.0f;
  f32 travel = width - thumb;
  f32 pos = (s->value - min) / (max - min) * travel;
  b32 changed = false;
  ui_element({.layout = {.sizing = {ui_sizing_fixed(width), ui_sizing_fixed(thumb)}, .margin = {2, 2, 2, 2},
                         .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
              .pointer_capture_mode = GR_PASS}) {
    ui_element({.layout = {.sizing = {ui_sizing_grow(0), ui_sizing_fixed(8.0f)}},
                .background_color = {59, 59, 59, 255},
                .corner_radius = ui_corner_radius(4.0f),
                .border = {.color = {133, 133, 133, 255}, .width = ui_border_outside(1)},
                .clip = {.horizontal = true, .vertical = true},
                .pointer_capture_mode = GR_PASS}) {
      ui_element({.layout = {.sizing = {ui_sizing_fixed(pos + thumb * 0.5f), ui_sizing_grow(0)}},
                  .background_color = GR_CYAN,
                  .pointer_capture_mode = GR_PASS}) {}
    }
    ui_element({.id = id,
                 .floating = {.attach_to = UI_ATTACH_TO_PARENT, .offset = {pos, 0.0f}},
                 .layout = {.sizing = {ui_sizing_fixed(thumb), ui_sizing_fixed(thumb)}},
                 .background_color = GR_CYAN,
                 .corner_radius = ui_corner_radius(thumb * 0.5f)}) {
      if (active) {
        f32 before = s->value;
        ui_interact_slider(s, min, max, travel + thumb, thumb, f->input);
        changed = s->value != before;
        if (ui_hovered() || s->drag.dragging)
          f->u->cursor = OS_CURSOR_ARROW;
      }
    }
  }
  return changed;
}

hz_internal void gr_output(const GrUi *u, String text) {
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(48.0f)}, .child_alignment = {.x = UI_ALIGN_X_RIGHT}},
              .pointer_capture_mode = GR_PASS}) {
    ui_text(gr_str(text), gr_font(u, 12.0f, UI_FONT_REGULAR, GR_CYAN, 12.0f * 0.2f));
  }
}

// the settings: Graphics, Sensitivity (0.2..3 step 0.05), Volume (0..1 step 0.01); every input applies at once (no local save)
hz_internal void gr_settings_block(const GrUiFrame *f, u32 screen, b32 active) {
  GrUi *u = f->u;
  GrUiSettings *st = &u->settings[screen];
  // the controls follow GrSettings, except a slider while it is dragged
  if (!st->synced || st->serial != f->settings->serial) {
    if (!st->sensitivity.drag.dragging)
      st->sensitivity.value = f->settings->sensitivity;
    if (!st->volume.drag.dragging)
      st->volume.value = f->settings->volume;
    st->serial = f->settings->serial;
    st->synced = true;
  }
  Allocator ta = tctx_temp_allocator(NULL);
  f32 fr = 380.0f - 110.0f - 48.0f - 24.0f;
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(380.0f)},
                         .layout_direction = UI_TOP_TO_BOTTOM,
                         .child_gap = 10,
                         .margin = {.top = f->v->short_screen ? 16 : 24}},
              .pointer_capture_mode = GR_PASS}) {
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .child_gap = 12, .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
                .pointer_capture_mode = GR_PASS}) {
      gr_setting_label(u, ui_string("GRAPHICS"));
      gr_select(f, st, screen, fr + 12.0f + 48.0f, active); // spans the value column too
    }
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .child_gap = 12, .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
                .pointer_capture_mode = GR_PASS}) {
      gr_setting_label(u, ui_string("SENSITIVITY"));
      if (gr_range(f, &st->sensitivity, ui_idi("GrSensitivity", screen), 0.2f, 3.0f, fr - 4.0f, active)) {
        st->sensitivity.value = 0.2f + m_roundf((st->sensitivity.value - 0.2f) / 0.05f) * 0.05f;
        gr_settings_edit(f)->sensitivity = st->sensitivity.value;
      }
      gr_output(u, str_format(&ta, "%", fmt_f32_p(st->sensitivity.value, 2)));
    }
    ui_element({.layout = {.sizing = {.width = ui_sizing_grow(0)}, .child_gap = 12, .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
                .pointer_capture_mode = GR_PASS}) {
      gr_setting_label(u, ui_string("VOLUME"));
      if (gr_range(f, &st->volume, ui_idi("GrVolume", screen), 0.0f, 1.0f, fr - 4.0f, active)) {
        st->volume.value = m_roundf(st->volume.value * 100.0f) / 100.0f;
        gr_settings_edit(f)->volume = st->volume.value;
      }
      gr_output(u, str_format(&ta, "%%", fmt_i32((i32)m_roundf(st->volume.value * 100.0f)), fmt_char('%')));
    }
  }
}

// a menu screen: full screen, fades over 0.35 s; the pointer is captured only while it shows
hz_internal const HzUIElementDesc *gr_screen(HzUIElementId id, const GrUiView *v, f32 alpha, b32 active) {
  Allocator ta = tctx_temp_allocator(NULL);
  HzUIElementDesc *d = ALLOC(&ta, HzUIElementDesc);
  HzUIPointerCaptureMode mode = active ? UI_POINTER_CAPTURE_MODE_CAPTURE : GR_PASS;
  *d = (HzUIElementDesc){
      .id = id,
      .layout = {.sizing = {ui_sizing_fixed(v->w + 2.0f), ui_sizing_fixed(v->h + 2.0f)},
                 .child_alignment = {UI_ALIGN_X_CENTER, UI_ALIGN_Y_CENTER}},
      // 1 px past each window edge: no anti-aliased seam
      .floating = {.attach_to = UI_ATTACH_TO_ROOT, .offset = {-1.0f, -1.0f}, .z_index = GR_Z_MENU, .pointer_capture_mode = mode},
      .opacity = opt_some(f32, alpha),
      .pointer_capture_mode = mode,
  };
  return d;
}

// ---- menus: screens ----

hz_internal void gr_credit_link(const GrUiFrame *f, HzUIString text, HzUIElementId id, CString url, HzUITextConfig c, b32 active) {
  ui_element({.id = id, .border = {.color = c.textColor, .width = {.bottom = 1}}}) {
    if (active && ui_hovered()) {
      f->u->cursor = OS_CURSOR_HAND;
      if (f->input->buttons[MOUSE_LEFT].released_this_frame)
        os_shell_open_url(url);
    }
    ui_text(text, c);
  }
}

hz_internal void gr_credits_arms(const GrUiFrame *f, HzUITextConfig c, b32 active) {
  ui_text(ui_string("ARMS: "), c);
  gr_credit_link(f, ui_string("\"FPS ARMS\""), ui_id("GrCreditArms"),
                 "https://sketchfab.com/3d-models/fps-arms-9452ce4cddde4110a4fd73e555a8e412", c, active);
}

hz_internal void gr_credits_license(const GrUiFrame *f, HzUITextConfig c, b32 active) {
  ui_text(ui_string(" BY DJMAESEN, "), c);
  gr_credit_link(f, ui_string("CC BY 4.0"), ui_id("GrCreditLicense"), "https://creativecommons.org/licenses/by/4.0/", c, active);
  ui_text(ui_string(" (MODIFIED)"), c);
}

// the CC BY 4.0 attribution the arms model requires: one floating line, or two right-aligned lines in a column
hz_internal void gr_credits(const GrUiFrame *f, b32 active, HzUIFloating floating) {
  HzUITextConfig c = gr_font(f->u, 11.0f, UI_FONT_REGULAR, GR_HUD_WHITE, 11.0f * 0.25f);
  if (floating.attach_to != UI_ATTACH_TO_NONE) {
    ui_element({.id = ui_id("GrCredits"),
                .layout = {.child_alignment = {.y = UI_ALIGN_Y_BOTTOM}},
                .floating = floating,
                .opacity = opt_some(f32, 0.45f),
                .pointer_capture_mode = GR_PASS}) {
      gr_credits_arms(f, c, active);
      gr_credits_license(f, c, active);
    }
    return;
  }
  ui_element({.id = ui_id("GrCredits"),
              .layout = {.layout_direction = UI_TOP_TO_BOTTOM, .child_gap = 4, .child_alignment = {.x = UI_ALIGN_X_RIGHT},
                         .margin = {.top = 14}},
              .opacity = opt_some(f32, 0.45f),
              .pointer_capture_mode = GR_PASS}) {
    ui_element({.pointer_capture_mode = GR_PASS}) { gr_credits_arms(f, c, active); }
    ui_element({.pointer_capture_mode = GR_PASS}) { gr_credits_license(f, c, active); }
  }
}

// eyebrow, title, tagline, DEPLOY and the best line
hz_internal void gr_title_lead(const GrUiFrame *f, const GrHud *h, b32 active) {
  GrUi *u = f->u;
  const GrUiView *v = f->v;
  Allocator ta = tctx_temp_allocator(NULL);
  gr_eyebrow(u, ui_string("THE BATTLE OF KAMINO \xC2\xB7 TIPOCA CITY"));
  // a short screen sizes the title by its height too, or the lead alone outgrows it
  f32 fluid = v->short_screen ? m_minf(8.0f * v->vw, 0.15f * v->h) : 8.0f * v->vw;
  f32 size = m_clampf(fluid, v->short_screen ? 40.0f : 54.0f, 104.0f);
  HzUITextConfig h1 = gr_glow(gr_font(u, size, UI_FONT_BOLD, GR_WHITE, size * 0.04f), (HzUIColor){94, 242, 255, 89}, 40.0f);
  h1.lineHeight = size * 0.88f;
  ui_element({.layout = {.layout_direction = UI_TOP_TO_BOTTOM, .margin = {.top = 10, .bottom = 8}}, .pointer_capture_mode = GR_PASS}) {
    ui_text(ui_string("KAMINO"), h1);
  }
  HzUITextConfig tag = gr_font(u, 17.0f, UI_FONT_REGULAR, (HzUIColor){234, 246, 255, 204}, 17.0f * 0.04f);
  tag.wrapMode = UI_TEXT_WRAP_WORDS;
  ui_element({.layout = {.margin = {.bottom = v->short_screen ? 18 : 26}}, .pointer_capture_mode = GR_PASS}) {
    ui_text(ui_string("The droid army has landed. Hold the platforms, push them back into the cloning facility."), tag);
  }
  ui_element({.pointer_capture_mode = GR_PASS}) {
    if (gr_button(f, GR_UI_BTN_DEPLOY, ui_string("DEPLOY"), true, true, active))
      f->actions->queue |= GR_ACTION_DEPLOY;
  }
  // the best line keeps its 18 px height while it is empty
  ui_element({.layout = {.sizing = {.height = ui_sizing_fixed(18.0f)}, .margin = {.top = 14}, .child_alignment = {.y = UI_ALIGN_Y_CENTER}},
              .pointer_capture_mode = GR_PASS}) {
    if (h->best > 0)
      ui_text(gr_str(str_format(&ta, "PERSONAL BEST \xC2\xB7 %", fmt_str(gr_thousands(&ta, h->best)))),
              gr_font(u, 13.0f, UI_FONT_REGULAR, GR_AMBER, 13.0f * 0.25f));
  }
}

hz_internal void gr_draw_title(const GrUiFrame *f, const GrHud *h) {
  GrUi *u = f->u;
  const GrUiView *v = f->v;
  b32 active = h->state == GR_STATE_TITLE;
  f32 a = gr_tween(&u->screen_fade[GR_UI_SCREEN_TITLE], active ? 1.0f : 0.0f, 0.35f, v->dt);
  if (a <= 0.0f)
    return;
  HzUIElementDesc *d = (HzUIElementDesc *)gr_screen(ui_id("GrTitle"), v, a, active);
  d->layout.child_alignment = (HzUIChildAlignment){UI_ALIGN_X_LEFT, UI_ALIGN_Y_CENTER};
  d->layout.padding.left = (u16)m_roundf(v->safe.left + 1.0f + m_maxf(6.0f * v->vw, 32.0f));
  d->gradient = (UIGradient){.kind = UI_GRADIENT_LINEAR,
                             .angle = 90.0f,
                             .stop_count = 3,
                             .stops = {{0.0f, {3, 8, 12, 217}}, {0.38f, {3, 8, 12, 140}}, {0.65f, {3, 8, 12, 0}}}};
  if (v->short_screen) {
    // the lead centred on the left; settings and credits stacked from the bottom right, all in flow
    d->layout.padding.right = (u16)m_roundf(v->safe.right + 24.0f);
    d->layout.padding.top = (u16)m_roundf(v->safe.top + 12.0f);
    d->layout.padding.bottom = (u16)m_roundf(v->safe.bottom + 14.0f);
    d->layout.child_gap = 24;
    ui_element_of(d) {
      ui_element({.layout = {.sizing = {.width = ui_sizing_fit(0.0f, 520.0f)}, .layout_direction = UI_TOP_TO_BOTTOM},
                  .pointer_capture_mode = GR_PASS}) {
        gr_title_lead(f, h, active);
      }
      ui_element({.layout = {.sizing = {ui_sizing_grow(0), ui_sizing_grow(0)},
                             .layout_direction = UI_TOP_TO_BOTTOM,
                             .child_alignment = {UI_ALIGN_X_RIGHT, UI_ALIGN_Y_BOTTOM}},
                  .pointer_capture_mode = GR_PASS}) {
        gr_settings_block(f, GR_UI_SCREEN_TITLE, active);
        gr_credits(f, active, (HzUIFloating){0});
      }
    }
    return;
  }
  ui_element_of(d) {
    ui_element({.layout = {.sizing = {.width = ui_sizing_fit(0.0f, 520.0f)}, .layout_direction = UI_TOP_TO_BOTTOM},
                .pointer_capture_mode = GR_PASS}) {
      gr_title_lead(f, h, active);
      gr_controls(u, v);
      gr_settings_block(f, GR_UI_SCREEN_TITLE, active);
    }
    gr_credits(f, active,
               (HzUIFloating){.attach_to = UI_ATTACH_TO_PARENT,
                              .attach_points = {.element = UI_ATTACH_POINT_RIGHT_BOTTOM, .parent = UI_ATTACH_POINT_RIGHT_BOTTOM},
                              .offset = {-29.0f - v->safe.right, -23.0f - v->safe.bottom}});
  }
}

// a results card: gradient, 1 px edge, 24 px cut corners
hz_internal f32 gr_card_pad_x(const GrUiView *v) { return v->very_narrow ? 24.0f : v->short_screen ? 28.0f : 40.0f; }

hz_internal const HzUIElementDesc *gr_card(HzUIElementId id, const GrUiView *v) {
  Allocator ta = tctx_temp_allocator(NULL);
  HzUIElementDesc *d = ALLOC(&ta, HzUIElementDesc);
  u16 px = (u16)gr_card_pad_x(v);
  u16 py = v->very_narrow ? 24 : v->short_screen ? 18 : 34;
  *d = (HzUIElementDesc){
      .id = id,
      .layout = {.sizing = {.width = ui_sizing_fit(.min = m_minf(560.0f, 0.9f * v->w))},
                 .padding = {px, px, py, py},
                 .layout_direction = UI_TOP_TO_BOTTOM},
      .gradient = {.kind = UI_GRADIENT_LINEAR,
                   .angle = 135.0f,
                   .stop_count = 2,
                   .stops = {{0.0f, {10, 30, 42, 217}}, {1.0f, {6, 14, 22, 204}}}},
      .border = {.color = {94, 242, 255, 77}, .width = ui_border_outside(1)},
      .clip_path = gr_cut_tr_bl(24.0f),
      .pointer_capture_mode = GR_PASS,
  };
  return d;
}

hz_internal void gr_card_title(const GrUi *u, const GrUiView *v, HzUIString text, b32 defeat) {
  f32 size = v->short_screen ? 38.0f : 54.0f;
  HzUITextConfig c = gr_font(u, size, UI_FONT_BOLD, GR_WHITE, size * 0.12f);
  c.lineHeight = size;
  c = gr_glow(c, defeat ? (HzUIColor){255, 59, 92, 140} : (HzUIColor){94, 242, 255, 102}, 30.0f);
  ui_element({.layout = {.margin = {.top = v->short_screen ? 4 : 8, .bottom = v->short_screen ? 14 : 22}}, .pointer_capture_mode = GR_PASS}) {
    ui_text(text, c);
  }
}

hz_internal void gr_draw_pause(const GrUiFrame *f, const GrHud *h) {
  GrUi *u = f->u;
  const GrUiView *v = f->v;
  b32 active = h->state == GR_STATE_PAUSED;
  f32 a = gr_tween(&u->screen_fade[GR_UI_SCREEN_PAUSE], active ? 1.0f : 0.0f, 0.35f, v->dt);
  if (a <= 0.0f)
    return;
  HzUIElementDesc *d = (HzUIElementDesc *)gr_screen(ui_id("GrPause"), v, a, active);
  d->background_color = (HzUIColor){3, 8, 12, 153};
  d->backdrop_blur = 6.0f;
  ui_element_of(d) {
    ui_element_of(gr_card(ui_id("GrPauseCard"), v)) {
      gr_eyebrow(u, ui_string("THE BATTLE OF KAMINO"));
      gr_card_title(u, v, ui_string("PAUSED"), false);
      ui_element({.layout = {.child_gap = 12}, .pointer_capture_mode = GR_PASS}) {
        if (gr_button(f, GR_UI_BTN_RESUME, ui_string("RESUME"), true, false, active))
          f->actions->queue |= GR_ACTION_RESUME;
        if (gr_button(f, GR_UI_BTN_RESTART, ui_string("RESTART"), false, false, active))
          f->actions->queue |= GR_ACTION_RESTART;
        if (gr_button(f, GR_UI_BTN_PAUSE_QUIT, ui_string("MAIN MENU"), false, false, active))
          f->actions->queue |= GR_ACTION_QUIT;
      }
      gr_controls(u, v);
      gr_settings_block(f, GR_UI_SCREEN_PAUSE, active);
    }
  }
}

hz_internal f32 gr_text_width(GrUi *u, HzUIString s, f32 size, u16 weight, f32 spacing) {
  HzHandleT(UIFontData) font = ui_family_resolve(u->ui, u->hud, (UIFontStyleTag){.weight = weight});
  return ui_measure_text_width_spaced(u->ui, font, s.chars, (u32)s.length, size, spacing);
}

hz_internal HzUIString gr_stat_label(u32 i, b32 new_best) {
  switch (i) {
  case 0: return ui_string("SCORE");
  case 1: return new_best ? ui_string("NEW BEST!") : ui_string("BEST");
  case 2: return ui_string("DROIDS DESTROYED");
  case 3: return ui_string("ACCURACY");
  case 4: return ui_string("SECTORS SECURED");
  default: return ui_string("TIME");
  }
}

hz_internal void gr_stat(const GrUi *u, const GrUiView *v, u32 i, HzUIString label, String value, b32 highlight, f32 width) {
  u16 py = v->short_screen ? 8 : 12;
  ui_element({.id = ui_idi("GrStat", i),
              .layout = {.sizing = {.width = ui_sizing_fixed(width)}, .padding = {14, 14, py, py}, .layout_direction = UI_TOP_TO_BOTTOM},
              .background_color = highlight ? (HzUIColor){255, 181, 71, 20} : (HzUIColor){94, 242, 255, 13},
              .border = {.color = highlight ? (HzUIColor){255, 181, 71, 153} : (HzUIColor){94, 242, 255, 51},
                         .width = ui_border_outside(1)},
              .pointer_capture_mode = GR_PASS}) {
    ui_text(label, gr_font(u, 11.0f, UI_FONT_REGULAR, highlight ? GR_AMBER : GR_CYAN, 11.0f * 0.22f));
    ui_element({.layout = {.margin = {.top = 4}}, .pointer_capture_mode = GR_PASS}) {
      gr_tabular((GrUi *)u, value, gr_font(u, v->short_screen ? 22.0f : 26.0f, UI_FONT_BOLD, GR_WHITE, 0.0f));
    }
  }
}

hz_internal void gr_draw_results(const GrUiFrame *f, const GrHud *h) {
  GrUi *u = f->u;
  const GrUiView *v = f->v;
  b32 active = h->state == GR_STATE_RESULTS;
  f32 a = gr_tween(&u->screen_fade[GR_UI_SCREEN_RESULTS], active ? 1.0f : 0.0f, 0.35f, v->dt);
  if (a <= 0.0f)
    return;
  const GrResults *r = &h->results;
  Allocator ta = tctx_temp_allocator(NULL);
  HzUIElementDesc *d = (HzUIElementDesc *)gr_screen(ui_id("GrResults"), v, a, active);
  d->background_color = (HzUIColor){3, 8, 12, 153};
  d->backdrop_blur = 6.0f;
  ui_element_of(d) {
    ui_element_of(gr_card(ui_id("GrResultsCard"), v)) {
      gr_eyebrow(u, r->victory ? ui_string("THE DROID ARMY IS IN RETREAT") : ui_string("TROOPER DOWN"));
      gr_card_title(u, v, r->victory ? ui_string("VICTORY") : ui_string("KIA"), !r->victory);
      u32 secs = (u32)r->seconds;
      String *values = ALLOC_ARRAY(&ta, String, 6);
      values[0] = gr_thousands(&ta, r->score);
      values[1] = gr_thousands(&ta, r->best);
      values[2] = str_format(&ta, "%", fmt_u32(r->kills));
      values[3] = str_format(&ta, "%%", fmt_i32((i32)m_roundf(r->accuracy * 100.0f)), fmt_char('%'));
      values[4] = str_format(&ta, "% / %", fmt_u32(r->waves_cleared), fmt_u32(r->total_waves));
      values[5] = str_format(&ta, "%:%%", fmt_u32(secs / 60), fmt_u32((secs % 60) / 10), fmt_u32(secs % 10));
      u32 cols = v->very_narrow ? 2u : 3u;
      // N equal columns in a fit-content card: every column is as wide as the widest stat, and at least an equal share
      f32 pad = gr_card_pad_x(v);
      f32 col = (m_minf(560.0f, 0.9f * v->w) - 2.0f * pad - 2.0f - 12.0f * (f32)(cols - 1)) / (f32)cols;
      for (u32 i = 0; i < 6; i++) {
        f32 label_w = gr_text_width(u, gr_stat_label(i, r->new_best), 11.0f, UI_FONT_REGULAR, 11.0f * 0.22f);
        f32 value_w = gr_text_width(u, gr_str(values[i]), v->short_screen ? 22.0f : 26.0f, UI_FONT_BOLD, 0.0f);
        col = m_maxf(col, m_maxf(label_w, value_w) + 30.0f);
      }
      ui_element({.layout = {.layout_direction = UI_TOP_TO_BOTTOM, .child_gap = 12, .margin = {.bottom = v->short_screen ? 16 : 26}},
                  .pointer_capture_mode = GR_PASS}) {
        for (u32 row = 0; row * cols < 6; row++) {
          ui_element({.layout = {.child_gap = 12}, .pointer_capture_mode = GR_PASS}) {
            for (u32 c = 0; c < cols && row * cols + c < 6; c++) {
              u32 i = row * cols + c;
              gr_stat(u, v, i, gr_stat_label(i, r->new_best), values[i], i == 1 && r->new_best, col);
            }
          }
        }
      }
      ui_element({.layout = {.child_gap = 12}, .pointer_capture_mode = GR_PASS}) {
        if (gr_button(f, GR_UI_BTN_AGAIN, ui_string("DEPLOY AGAIN"), true, false, active))
          f->actions->queue |= GR_ACTION_DEPLOY;
        if (gr_button(f, GR_UI_BTN_RESULTS_QUIT, ui_string("MAIN MENU"), false, false, active))
          f->actions->queue |= GR_ACTION_QUIT;
      }
    }
  }
}

hz_internal void gr_draw_loading(GrUi *u, const GrHud *h, const GrUiView *v, b32 failed) {
  b32 loading = h->state == GR_STATE_LOADING && !failed; // a failed load hides the loader
  f32 a = gr_tween(&u->loading_fade, loading ? 1.0f : 0.0f, 0.35f, v->dt);
  if (a <= 0.0f)
    return;
  // the bar's width is round(progress * 100)%, eased over 0.25 s
  f32 fill = gr_tween(&u->loading_fill, m_roundf(m_saturatef(h->load_progress) * 100.0f) * 0.01f, 0.25f, v->dt);
  HzUIString label = {0};
  switch (h->load_stage) {
  case GR_LOAD_MODELS: label = ui_string("LOADING MODELS"); break;
  case GR_LOAD_AUDIO: label = ui_string("SYNTHESIZING AUDIO"); break;
  case GR_LOAD_LIGHT: label = ui_string("BAKING LIGHT"); break;
  case GR_LOAD_SHADERS: label = ui_string("COMPILING SHADERS"); break;
  default: label = ui_string("READY"); break;
  }
  ui_element({.id = ui_id("GrLoading"),
              .layout = {.sizing = {ui_sizing_fixed(v->w + 2.0f), ui_sizing_fixed(v->h + 2.0f)},
                         .child_alignment = {UI_ALIGN_X_CENTER, UI_ALIGN_Y_CENTER}},
              .floating = {.attach_to = UI_ATTACH_TO_ROOT, .offset = {-1.0f, -1.0f}, .z_index = GR_Z_LOADING},
              // radial gradient from the centre, #0b1a24 to #04080c at 70% of the farthest corner
              .gradient = {.kind = UI_GRADIENT_RADIAL,
                           .size = UI_RADIAL_FARTHEST_CORNER,
                           .stop_count = 2,
                           .stops = {{0.0f, {11, 26, 36, 255}}, {0.7f, {4, 8, 12, 255}}}},
              .opacity = opt_some(f32, a)}) {
    ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(m_minf(420.0f, 0.8f * v->w))},
                           .layout_direction = UI_TOP_TO_BOTTOM,
                           .child_alignment = {.x = UI_ALIGN_X_CENTER}},
                .pointer_capture_mode = GR_PASS}) {
      ui_text(ui_string("DEPLOYING TO TIPOCA CITY"), gr_font(u, 14.0f, UI_FONT_REGULAR, GR_CYAN, 14.0f * 0.45f));
      ui_element({.layout = {.sizing = {ui_sizing_grow(0), ui_sizing_fixed(4.0f)}, .margin = {.top = 18, .bottom = 10}},
                  .background_color = {94, 242, 255, 31},
                  .pointer_capture_mode = GR_PASS}) {
        ui_element({.layout = {.sizing = {ui_sizing_percent(fill), ui_sizing_grow(0)}},
                    .background_color = GR_CYAN,
                    .shadow = {.color = GR_CYAN, .blur = 14.0f},
                    .pointer_capture_mode = GR_PASS}) {}
      }
      ui_element({.opacity = opt_some(f32, 0.6f), .pointer_capture_mode = GR_PASS}) {
        ui_text(label, gr_font(u, 12.0f, UI_FONT_REGULAR, GR_HUD_WHITE, 12.0f * 0.2f));
      }
    }
  }
}

// the fatal screen: inset 24 px, 13 px monospace
hz_internal void gr_draw_fatal(GrUi *u, const GrUiView *v) {
  if (!handle_is_valid(u->mono)) {
    // 13 px monospace, 1.5 line height: ui-monospace, Consolas, monospace
    TempAllocator ta = tctx_temp_allocator_begin(NULL);
    const char **mono = ALLOC_ARRAY(&ta.allocator, const char *, 2);
    mono[0] = "Cascadia Mono";
    mono[1] = "Consolas";
    u->mono = ui_load_family_os_list(u->ui, mono, 2, NULL);
    tctx_temp_allocator_end(ta);
  }
  HzUITextConfig c = {.font_family = u->mono, .fontSize = 13.0f, .lineHeight = 19.5f, .textColor = {255, 214, 220, 255},
                      .wrapMode = UI_TEXT_WRAP_WORDS};
  ui_element({.id = ui_id("GrFatal"),
              .layout = {.sizing = {ui_sizing_fixed(v->w - 48.0f), ui_sizing_fixed(v->h - 48.0f)}, .padding = {16, 16, 16, 16}},
              .floating = {.attach_to = UI_ATTACH_TO_ROOT, .offset = {24.0f, 24.0f}, .z_index = GR_Z_FATAL},
              .background_color = {40, 0, 8, 230}}) {
    ui_text(ui_string("Failed to start. A required asset of the level, its models or its shaders failed to load; the log names it."), c);
  }
}

// ---- frame ----

void gr_ui_frame(EcsWorld *w) {
  GrUi *u = ecs_resource_get_mut(w, GrUi);
  const GrHud *h = ecs_resource_get(w, GrHud);
  const GrRadar *radar = ecs_resource_get(w, GrRadar);
  const GrClock *clock = ecs_resource_get(w, GrClock);
  const GrLoadGates *gates = ecs_resource_get(w, GrLoadGates);
  InputSystem *input = ecs_resource_get_mut(w, InputSystem);
  const AppMemory *mem = ecs_resource_get(w, AppMemory);

  GrUiView v = {0};
  v.w = (f32)u->ui->canvas_width;
  v.h = (f32)u->ui->canvas_height;
  v.vw = v.w * 0.01f;
  v.dt = clock->dt;
  v.now = h->time;
  v.narrow = v.w <= 900.0f;
  v.very_narrow = v.w <= 700.0f;
  v.short_screen = v.h <= 500.0f;
  v.touch = ecs_resource_get(w, GrInput)->touch;
  v.safe = os_get_screen_desc().safe_area;
  v.touch_top = v.safe.top + (v.w < v.h ? 118.0f : 10.0f);

  b32 menus = h->state == GR_STATE_TITLE || h->state == GR_STATE_PAUSED || h->state == GR_STATE_RESULTS;
  // a crosshair cursor in play; the menus show the default arrow, buttons and links a hand
  u->cursor = menus ? OS_CURSOR_ARROW : OS_CURSOR_CROSSHAIR;

  GrUiFrame f = {
      .u = u,
      .v = &v,
      .input = input,
      .actions = ecs_resource_get_mut(w, GrActions),
      .settings = ecs_resource_get(w, GrSettings),
  };

  ui_begin_frame(u->ui, input, mem->dt);
  gr_draw_hud(u, w, h, radar, ecs_resource_get(w, HudMessages), &v);
  if (h->state == GR_STATE_PLAYING && ecs_resource_get(w, GrInput)->touch)
    gr_draw_touch(u, ecs_resource_get(w, GrTouch), h);
  gr_draw_title(&f, h);
  gr_draw_pause(&f, h);
  gr_draw_results(&f, h);
  gr_draw_loading(u, h, &v, gates->failed);
  if (gates->failed)
    gr_draw_fatal(u, &v);
  ui_finish_layout(u->ui);
  os_set_cursor(u->cursor);
}
