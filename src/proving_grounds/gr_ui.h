// Proving Grounds UI: the HUD, menus and loading screen on the engine UI, drawn from GrHud on the main lane.
#pragma once
#include "ecs/ecs.h"
#include "gpu/gpu.h"
#include "lib/hzmath.h"
#include "lib/typedefs.h"
#include "ui/ui.h"

typedef enum GrUiScreen {
  GR_UI_SCREEN_TITLE = 0,
  GR_UI_SCREEN_PAUSE,
  GR_UI_SCREEN_RESULTS,
  GR_UI_SCREEN_COUNT,
} GrUiScreen;

typedef enum GrUiButton {
  GR_UI_BTN_DEPLOY = 0,
  GR_UI_BTN_RESUME,
  GR_UI_BTN_RESTART,
  GR_UI_BTN_PAUSE_QUIT,
  GR_UI_BTN_AGAIN,
  GR_UI_BTN_RESULTS_QUIT,
  GR_UI_BTN_COUNT,
} GrUiButton;

#define GR_UI_MAX_TICKS 36 // the rifle's magazine, the largest

// one settings block (title and pause each have their own, so the cross-fade never shares an element id)
typedef struct GrUiSettings {
  UIInteract_Dropdown quality;
  UIInteract_Slider sensitivity;
  UIInteract_Slider volume;
  u32 serial; // the GrSettings serial the sliders were synced to
  b32 synced;
} GrUiSettings;

// main-lane UI state: the system, the HUD font, the custom-shader pipelines and every transition the screens retain
typedef struct GrUi {
  UISystem *ui;
  HzHandleT(UIFontFamily) hud; // the HUD face: Bahnschrift, DIN Alternate, Roboto Condensed, Arial Narrow, sans-serif
  HzHandleT(UIFontFamily) mono; // the fatal screen's mono face, loaded only if it shows
  GpuPipeline ring_pipeline;
  GpuPipeline arc_pipeline;
  GpuPipeline radar_pipeline;
  OsCursor cursor;

  UITween hud_fade;
  UITween crosshair_fade;
  UITween reload_fade;
  UITween loading_fade;
  UITween loading_fill;
  UITween screen_fade[GR_UI_SCREEN_COUNT];
  UITween button_hover[GR_UI_BTN_COUNT];
  UITween tick_alpha[GR_UI_MAX_TICKS];
  GrUiSettings settings[GR_UI_SCREEN_COUNT];
  f32 reload_age;      // the blink's clock since the hint showed
  b32 reload_shown;
  f32 critical_age;    // the health blink's clock since critical health began
} GrUi;
ECS_COMPONENT_TYPEDEF(GrUi);

// creates the UI system, attaches it to the renderer and builds the custom-shader pipelines; main lane, after ecs_renderer_init
void gr_ui_init(EcsWorld *w, Allocator *a);
// COLLECTIVE: all lanes, every frame, before gr_ui_frame (runtime font glyphs are generated across the lanes)
void gr_ui_font_update(EcsWorld *w);
// draws the HUD and menus from GrHud and queues the menus' intents into GrActions; main lane, after the scope, before renderer_update
void gr_ui_frame(EcsWorld *w);
