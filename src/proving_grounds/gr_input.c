// Proving Grounds input: keyboard, mouse, touch and pointer lock on the engine InputSystem, collected on the main lane before the scope.

// a key released this frame is already up (the release lands before the frame runs)
hz_internal b32 gr_key_held(const InputSystem *in, App_InputButtonType k) {
  return in->buttons[k].is_pressed && !in->buttons[k].released_this_frame;
}

hz_internal f32 gr_key_axis(const InputSystem *in, App_InputButtonType pos, App_InputButtonType neg) {
  return (gr_key_held(in, pos) ? 1.0f : 0.0f) - (gr_key_held(in, neg) ? 1.0f : 0.0f);
}

// touch look in mouse-pixel units: a full landscape swipe (~800 pt) turns ~200 degrees at sensitivity 1
#define GR_TOUCH_LOOK_SCALE 2.1f
// the stick pushed this far past full speed, mostly forward, sprints (Left Shift on a keyboard)
#define GR_TOUCH_SPRINT_TRAVEL 1.6f
#define GR_TOUCH_SPRINT_FORWARD 0.7f

hz_internal TouchControlsLayout gr_touch_layout(TouchOrientation o) {
  TouchControlsLayout l = touch_controls_layout_default(o);
  // the stick floats anywhere on the left, below the radar
  l.region_pos_pct = (v2){0.0f, 0.3f};
  l.region_size_pct = (v2){o == TOUCH_ORIENTATION_PORTRAIT ? 0.5f : 0.42f, 0.7f};
  l.stick_outer_radius_px = 56.0f;
  l.stick_ring_radius_px = 26.0f;
  l.stick_middle_radius_px = 16.0f;
  l.deadzone_radius_px = 4.0f;
  l.max_speed_radius_px = 36.0f;
  l.pinch_grace_s = 0.0f;
  l.buttons[GR_TOUCH_FIRE] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {130.0f, 110.0f}, 84.0f};
  l.buttons[GR_TOUCH_ADS] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {52.0f, 200.0f}, 58.0f};
  l.buttons[GR_TOUCH_JUMP] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {46.0f, 62.0f}, 62.0f};
  l.buttons[GR_TOUCH_PAUSE] = (TouchButtonPlacement){TOUCH_ANCHOR_TOP_RIGHT, {30.0f, 30.0f}, 44.0f};
  if (o == TOUCH_ORIENTATION_PORTRAIT) {
    // the shield bar owns the bottom centre of a narrow screen: the small buttons stack up the right edge
    l.buttons[GR_TOUCH_RELOAD] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {46.0f, 276.0f}, 54.0f};
    l.buttons[GR_TOUCH_SWAP] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {140.0f, 226.0f}, 62.0f};
  } else {
    l.buttons[GR_TOUCH_RELOAD] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {226.0f, 46.0f}, 54.0f};
    l.buttons[GR_TOUCH_SWAP] = (TouchButtonPlacement){TOUCH_ANCHOR_BOTTOM_RIGHT, {200.0f, 190.0f}, 62.0f};
  }
  return l;
}

void gr_touch_init(EcsWorld *w, UISystem *ui, HzHandleT(UIFontFamily) font) {
  TouchControlsConfig cfg = {
      .disable_jump_button = true, // JUMP is an action button: the run wants its press edge
      .ui_scale = 1.0f,
      .ui = ui,
      .disable_pinch = true,
      .label = {.font_family = font, .weight = 700, .fontSize = 12.0f, .letterSpacing = 1.0f,
                .textColor = (HzUIColor){255, 255, 255, 235}},
      .landscape = gr_touch_layout(TOUCH_ORIENTATION_LANDSCAPE),
      .portrait = gr_touch_layout(TOUCH_ORIENTATION_PORTRAIT),
  };
  cfg.buttons[GR_TOUCH_FIRE] = (TouchButtonDesc){.enabled = true, .look_through = true, .label = "FIRE"};
  cfg.buttons[GR_TOUCH_ADS] = (TouchButtonDesc){.enabled = true, .look_through = true, .label = "AIM"};
  cfg.buttons[GR_TOUCH_JUMP] = (TouchButtonDesc){.enabled = true, .label = "JUMP"};
  cfg.buttons[GR_TOUCH_RELOAD] = (TouchButtonDesc){.enabled = true, .label = "RELOAD"};
  cfg.buttons[GR_TOUCH_SWAP] = (TouchButtonDesc){.enabled = true, .label = "SWAP"};
  cfg.buttons[GR_TOUCH_PAUSE] = (TouchButtonDesc){.enabled = true, .label = "II"};
  ecs_resource_set(w, GrTouch, ((GrTouch){.tc = touch_controls_init(cfg)}));
}

void gr_input_collect(GrInput *in, GrTouch *touch, InputSystem *input, u32 state, b32 swap_prompt, b32 focus_lost,
                      f32 now) {
  b32 playing = state == GR_STATE_PLAYING;
  in->held = false;
  in->touch = input->last_category == INPUT_CATEGORY_TOUCH;
  // a click while playing unlocked requests the lock; Escape and focus loss release it
  input_pointer_lock_update(&in->pointer_lock, input, playing && !in->touch, focus_lost);
  b32 locked = in->pointer_lock.locked;
  in->captured = locked || in->touch;
  in->focused = hz_is_focused();
  in->focus_lost = focus_lost;

  // pointer input counts only while locked; the lock frame's delta is the warp, its click the lock request
  b32 pointer = locked && !in->pointer_lock.gained_this_frame;
  in->look = input_pointer_lock_look(&in->pointer_lock, input);
  in->fire_held = pointer && gr_key_held(input, MOUSE_LEFT);
  in->fire_pressed = pointer && input->buttons[MOUSE_LEFT].pressed_this_frame;
  in->aim_held = pointer && gr_key_held(input, MOUSE_RIGHT);
  in->aim_pressed = pointer && input->buttons[MOUSE_RIGHT].pressed_this_frame;

  // W/A/S/D (no arrows), Left Shift, Space, R, E
  in->forward = gr_key_axis(input, KEY_W, KEY_S);
  in->strafe = gr_key_axis(input, KEY_D, KEY_A);
  in->sprint = gr_key_held(input, KEY_LEFT_SHIFT);
  in->jump_pressed = input->buttons[KEY_SPACE].pressed_this_frame;
  in->reload_pressed = input->buttons[KEY_R].pressed_this_frame;
  in->swap_pressed = input->buttons[KEY_E].pressed_this_frame;
  in->pause_pressed = false;

  // the on-screen controls exist only in a run; outside it every touch belongs to the menus
  TouchControls *tc = &touch->tc;
  for (u32 b = 0; b < GR_TOUCH_BUTTON_COUNT; b++)
    touch_controls_set_button(tc, b, playing, false);
  touch_controls_set_button(tc, GR_TOUCH_SWAP, playing && swap_prompt, false);
  if (!playing || !in->touch) {
    touch->ads = false;
    touch->sprint = false;
    return;
  }
  TouchControlsFrame tf = touch_controls_update(tc, input, now);
  const TouchButtonState *bt = tf.buttons;
  if (bt[GR_TOUCH_ADS].pressed)
    touch->ads = !touch->ads;
  touch_controls_set_button(tc, GR_TOUCH_ADS, true, touch->ads);

  in->forward = m_clampf(in->forward + tf.move.y, -1.0f, 1.0f);
  in->strafe = m_clampf(in->strafe + tf.move.x, -1.0f, 1.0f);
  touch->sprint = tf.stick_travel >= GR_TOUCH_SPRINT_TRAVEL && tf.move.y >= GR_TOUCH_SPRINT_FORWARD;
  in->sprint |= touch->sprint;
  in->look = v2_add(in->look, v2_scale(tf.camera_pan_px, GR_TOUCH_LOOK_SCALE));
  in->fire_held |= bt[GR_TOUCH_FIRE].held;
  in->fire_pressed |= bt[GR_TOUCH_FIRE].pressed;
  in->aim_held |= touch->ads;
  in->aim_pressed |= bt[GR_TOUCH_ADS].pressed && touch->ads;
  in->jump_pressed |= bt[GR_TOUCH_JUMP].pressed;
  in->reload_pressed |= bt[GR_TOUCH_RELOAD].pressed;
  in->swap_pressed |= bt[GR_TOUCH_SWAP].pressed;
  in->pause_pressed = bt[GR_TOUCH_PAUSE].pressed;
}

void gr_input_hold(GrInput *in) {
  *in = (GrInput){.pointer_lock = {.locked = in->pointer_lock.locked},
                  .touch = in->touch,
                  .captured = in->captured,
                  .focused = in->focused,
                  .held = true};
}
