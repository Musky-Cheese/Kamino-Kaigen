// The flow: staged load, state machine, transitions, frame clock, slow motion, focus and pointer-lock policy, dev switches.

#define GR_MAX_DT (1.0f / 20.0f) // clamp long frames to 50 ms
#define GR_EYE_HEIGHT 1.65f
#define GR_FOV 66.0f        // vertical degrees
#define GR_CAMERA_NEAR 0.05f
#define GR_CAMERA_FAR 400.0f
#define GR_PITCH_LIMIT 1.5f // the look's pitch clamp
#define GR_END_VICTORY 3.4f // ending -> results timers
#define GR_END_DEFEAT 2.6f


// each gun's aim-down-sights fov, vertical degrees
hz_internal f32 gr_ads_fov(u32 weapon) {
  switch (weapon) {
  case GR_WEAPON_BREACHER: return 56.0f;
  case GR_WEAPON_LANCER: return 21.2f;
  default: return 42.0f;
  }
}

// the forward a yaw/pitch describe (yaw about Y, then pitch about X, -Z forward)
hz_internal v3 gr_view_forward(f32 yaw, f32 pitch) {
  f32 cp = m_cosf(pitch);
  return (v3){-m_sinf(yaw) * cp, m_sinf(pitch), -m_cosf(yaw) * cp};
}

// the yaw/pitch that look from `eye` onto `target`
hz_internal void gr_aim_at(v3 eye, v3 target, f32 *yaw, f32 *pitch) {
  v3 rel = v3_sub(target, eye);
  *yaw = m_atan2f(-rel.x, -rel.z);
  *pitch = m_atan2f(rel.y, m_sqrtf(rel.x * rel.x + rel.z * rel.z));
}

// `level` gives the scene's player spawn
hz_internal void gr_dev_resolve(GrDev *d, const GrDevSwitches *sw, const GrLevel *level, u32 state, f32 time, i32 quality) {
  d->state = state;
  d->quality = quality;
  d->resolved = true;
  d->hold_waves = sw->hold_waves;
  d->pin_drones = sw->pin_drones;
  d->god = sw->god;
  d->weapon = sw->weapon >= 0 && sw->weapon < GR_WEAPON_COUNT ? (u32)sw->weapon : GR_WEAPON_RIFLE;
  d->ads_pressed = sw->ads && !d->ads;
  d->fire_pressed = sw->fire && !d->fire;
  d->ads = sw->ads;
  d->fire = sw->fire;

  GrPoseSwitches pose = {sw->pose, {sw->pose_x, sw->pose_y, sw->pose_z}, sw->pose_yaw, sw->pose_pitch};
  if (pose.pose != d->pose_seen.pose || !v3_eqv(pose.feet, d->pose_seen.feet) || pose.yaw != d->pose_seen.yaw ||
      pose.pitch != d->pose_seen.pitch) {
    d->pose_seen = pose;
    d->pose_serial++;
  }
  GrAimSwitches aim = {sw->aim, {sw->aim_x, sw->aim_y, sw->aim_z}};
  if (aim.aim != d->aim_seen.aim || !v3_eqv(aim.target, d->aim_seen.target)) {
    d->aim_seen = aim;
    d->aim_serial++;
  }
  d->aim = sw->aim;
  d->aim_target = aim.target;
  d->drones[GR_DRONE_SCOUT] = (GrDroneSpawn){sw->scout, {sw->scout_x, sw->scout_y, sw->scout_z}};
  d->drones[GR_DRONE_STRIKER] = (GrDroneSpawn){sw->striker, {sw->striker_x, sw->striker_y, sw->striker_z}};
  d->drones[GR_DRONE_SENTINEL] = (GrDroneSpawn){sw->sentinel, {sw->sentinel_x, sw->sentinel_y, sw->sentinel_z}};

  if (state == GR_STATE_TITLE || state == GR_STATE_LOADING) {
    // the title's menu camera: a slow orbit around the core
    f32 t = time * 0.045f + 2.2f;
    d->eye = (v3){m_sinf(t) * 15.5f, 5.2f + m_sinf(t * 1.7f) * 0.6f, m_cosf(t) * 15.5f};
    d->feet = level->player_spawn;
    gr_aim_at(d->eye, (v3){0.0f, 4.2f, 0.0f}, &d->yaw, &d->pitch);
    d->fov = GR_FOV;
    return;
  }

  d->feet = sw->pose ? (v3){sw->pose_x, sw->pose_y, sw->pose_z} : level->player_spawn;
  d->eye = v3_add(d->feet, (v3){0.0f, GR_EYE_HEIGHT, 0.0f}); // the capsule's settled height on the level
  d->yaw = sw->pose ? sw->pose_yaw : level->player_spawn_yaw;
  d->pitch = sw->pose ? m_clampf(sw->pose_pitch, -GR_PITCH_LIMIT, GR_PITCH_LIMIT) : 0.0f;
  if (sw->aim)
    gr_aim_at(d->eye, (v3){sw->aim_x, sw->aim_y, sw->aim_z}, &d->yaw, &d->pitch);
  d->fov = d->ads ? gr_ads_fov(d->weapon) : GR_FOV; // the player's zoom eases toward this over the ADS time
}

// settings sanitise: quality a preset, sensitivity [0.2, 3], volume [0, 1]
hz_internal GrSettings gr_settings_sanitise(GrSettings s) {
  if (s.quality < 0 || s.quality >= GR_QUALITY_COUNT)
    s.quality = GR_QUALITY_HIGH;
  s.sensitivity = m_clampf(s.sensitivity, 0.2f, 3.0f);
  s.volume = m_clampf(s.volume, 0.0f, 1.0f);
  return s;
}

// a pointer lock/unlock request: the main lane applies each request once, after the scope
hz_internal void gr_flow_lock(GrFlow *f, b32 lock) {
  f->lock_value = lock;
  f->lock_serial++;
}

// run start: clear the world, then every owner resets from the restart serial
hz_internal void gr_flow_start_run(GrFlow *f, GrDev *d) {
  f->clear_serial++;
  d->restart_serial++;
  f->unlocked_time = 0.0f;
  f->slow_motion = 0.0f;
  f->state = GR_STATE_PLAYING;
  f->audio_suspended = false;
  log_info("run: start (serial %, debug controls %)", fmt_u32(d->restart_serial), fmt_b32(f->debug_controls));
}

// the title's or results' deploy: a locked run
hz_internal void gr_flow_deploy(GrFlow *f, GrDev *d) {
  f->debug_controls = false;
  f->audio_unlocked = true;
  gr_flow_start_run(f, d);
  gr_flow_lock(f, true);
}

// pause; the trigger release is the weapons' `controls` going false
hz_internal void gr_flow_pause(GrFlow *f) {
  f->audio_suspended = true;
  gr_flow_lock(f, false);
  f->state = GR_STATE_PAUSED;
  log_info("run: paused");
}

// resume: the lock gained flips paused -> playing; a debug or touch run has no lock to wait for
hz_internal void gr_flow_resume(GrFlow *f, b32 touch) {
  f->unlocked_time = 0.0f;
  f->audio_unlocked = true;
  if (f->debug_controls || touch) {
    f->audio_suspended = false;
    f->state = GR_STATE_PLAYING;
  } else {
    gr_flow_lock(f, true);
  }
}

// quit to the title
hz_internal void gr_flow_quit(GrFlow *f) {
  gr_flow_lock(f, false);
  f->audio_suspended = false;
  f->clear_serial++;
  f->state = GR_STATE_TITLE;
  log_info("run: quit to title");
}

// show the results; the payload is GrRun's (gr_run_consolidate sees results_serial)
hz_internal void gr_flow_show_results(GrFlow *f) {
  f->drones_clear_serial++;
  gr_flow_lock(f, false);
  f->results_serial++;
  f->state = GR_STATE_RESULTS;
  log_info("run: results (victory %)", fmt_b32(f->victory));
}

// the slow-motion time scale: eases from 30% back to 100% over the beat, counted on real dt
hz_internal f32 gr_flow_time_scale(GrFlow *f, f32 real_dt) {
  if (f->slow_motion <= 0.0f)
    return 1.0f;
  f->slow_motion = m_maxf(0.0f, f->slow_motion - real_dt);
  f32 t = 1.0f - f->slow_motion / GR_SLOW_MOTION_TIME;
  return 0.3f + 0.7f * t * t;
}

// the current load stage's progress and whether its owner's gate is open
hz_internal b32 gr_flow_load_stage(GrFlow *f, const GrActions *a) {
  switch (f->load_stage) {
  case GR_LOAD_MODELS:
    // the level build: the merged meshes, models, props and signs spawned
    f->load_progress = 0.02f + m_saturatef(a->models) * 0.33f;
    return a->models >= 1.0f;
  case GR_LOAD_AUDIO:
    f->load_progress = 0.4f;
    return a->audio;
  case GR_LOAD_LIGHT:
    f->load_progress = 0.6f;
    return a->light;
  case GR_LOAD_SHADERS:
    f->load_progress = 0.75f;
    return a->pipelines_ready;
  case GR_LOAD_READY:
    f->load_progress = 1.0f;
    return true;
  default: return false;
  }
}

// staged load: each label shows for at least one frame, then the stage's work gates the next
hz_internal void gr_flow_load(GrFlow *f, const GrActions *a) {
  if (a->load_failed)
    return; // a failed load leaves the fatal screen up
  f->stage_frames++;
  b32 gate = gr_flow_load_stage(f, a);
  if (gate && f->stage_frames > 1) {
    f->load_stage++;
    f->stage_frames = 1;
    if (f->load_stage == GR_LOAD_DONE)
      f->state = GR_STATE_TITLE;
    else
      gr_flow_load_stage(f, a);
  }
}

// the dev switches as one-shot requests: each latches when its level changes, once the load is done
hz_internal void gr_flow_switches(GrFlow *f, GrDev *d, const GrDevSwitches *sw) {
  if (sw->start && !f->sw_start) {
    f->debug_controls = true;
    f->audio_unlocked = true;
    gr_flow_start_run(f, d);
  }
  f->sw_start = sw->start;

  if (sw->screen != f->sw_screen) {
    if (sw->screen == 1 && f->state == GR_STATE_PLAYING) {
      gr_flow_pause(f);
    } else if (sw->screen == 2 && (f->state == GR_STATE_PLAYING || f->state == GR_STATE_PAUSED || f->state == GR_STATE_ENDING)) {
      // a defeat straight to results, without the outro
      f->victory = false;
      gr_flow_show_results(f);
    } else if (sw->screen == 0 && f->state == GR_STATE_PAUSED) {
      gr_flow_resume(f, false);
    }
    f->sw_screen = sw->screen;
  }
}

// the one pause policy: window edges from GrInput, intents from the UI
hz_internal void gr_flow_actions(GrFlow *f, GrDev *d, GrActions *a, GrSettings *settings, const GrInput *in) {
  if (a->settings_changed) {
    GrSettings s = gr_settings_sanitise(a->settings);
    s.serial = settings->serial + 1;
    *settings = s;
    a->settings_changed = false;
    log_info("settings: quality %, sensitivity %, volume %", fmt_i32(s.quality), fmt_f32_p(s.sensitivity, 2),
             fmt_f32_p(s.volume, 2));
  }

  u32 q = a->queue;
  a->queue = 0;
  if ((q & GR_ACTION_DEPLOY) && (f->state == GR_STATE_TITLE || f->state == GR_STATE_RESULTS))
    gr_flow_deploy(f, d);
  if ((q & GR_ACTION_RESTART) && f->state == GR_STATE_PAUSED)
    gr_flow_deploy(f, d);
  if ((q & GR_ACTION_RESUME) && f->state == GR_STATE_PAUSED)
    gr_flow_resume(f, in->touch);
  if ((q & GR_ACTION_QUIT) && (f->state == GR_STATE_PAUSED || f->state == GR_STATE_RESULTS))
    gr_flow_quit(f);
  // pointer lock changes (Esc and the click act on the lock in gr_input_collect)
  if (in->pointer_lock.gained_this_frame && f->state == GR_STATE_PAUSED) {
    f->audio_suspended = false;
    f->state = GR_STATE_PLAYING;
  } else if (f->state == GR_STATE_PLAYING && ((in->pointer_lock.lost_this_frame && !f->debug_controls) || in->pause_pressed)) {
    gr_flow_pause(f);
  }

  // window focus: a dev-switch run keeps running unfocused
  if (in->focus_lost) {
    if (f->state == GR_STATE_PLAYING && !f->debug_controls)
      gr_flow_pause(f);
    else if (f->state != GR_STATE_PLAYING)
      f->audio_suspended = true;
  } else if (in->focused && !f->was_focused && f->state != GR_STATE_PAUSED) {
    f->audio_suspended = false;
  }
  f->was_focused = in->focused;
}

// the frame's dt, long frames clamped, onto the clock resource: the flow system declares as many terms as a system may
ecs_system(gr_clock_system, EcsResInOut(GrClock)) {
  GrClock *c = ecs_sys_get_resource_mut(GrClock);
  // the flush's dt is the frame's memory->dt
  c->dt = m_minf(it->delta_time, GR_MAX_DT);
  c->time += c->dt;
}

// the flow: load stages, dev switches, intents, run end, then the frame clock
ecs_system(gr_run_system, EcsIn(GrDevSwitches), EcsResIn(GrInput), EcsResInOut(GrFlow), EcsResInOut(GrClock),
           EcsResInOut(GrDev), EcsResInOut(GrActions), EcsResInOut(GrSettings), EcsResIn(GrRun)) {
  const AppMemory *mem = ecs_resource_get(it->world, AppMemory);
  const GrInput *in = ecs_sys_get_resource(GrInput);
  GrFlow *f = ecs_sys_get_resource_mut(GrFlow);
  GrClock *c = ecs_sys_get_resource_mut(GrClock);
  GrDev *d = ecs_sys_get_resource_mut(GrDev);
  GrActions *a = ecs_sys_get_resource_mut(GrActions);
  GrSettings *settings = ecs_sys_get_resource_mut(GrSettings);
  const GrRun *run = ecs_sys_get_resource(GrRun);
  const GrDevSwitches *sw = ecs_sys_get_component(GrDevSwitches, 0);

  // the quality switch sets the render preset; the level latches like every switch
  if (sw->quality != f->sw_quality) {
    GrSettings s = *settings;
    s.quality = sw->quality;
    s = gr_settings_sanitise(s);
    s.serial = settings->serial + 1;
    *settings = s;
    f->sw_quality = sw->quality;
  }

  if (f->state == GR_STATE_LOADING) {
    gr_flow_load(f, a);
    if (f->state != GR_STATE_LOADING) {
      f->load_frame = c->frame;
      log_info("run: loaded at frame %", fmt_u32(c->frame));
    }
  }

  if (f->state != GR_STATE_LOADING) {
    gr_flow_switches(f, d, sw);
    gr_flow_actions(f, d, a, settings, in);
  }

  // run end: only from playing; the score, banner and sounds are the run consolidation's
  if (run->end_serial != f->end_seen) {
    f->end_seen = run->end_serial;
    if (f->state == GR_STATE_PLAYING) {
      f->victory = run->end_victory;
      f->end_timer = f->victory ? GR_END_VICTORY : GR_END_DEFEAT;
      if (f->victory)
        f->bolts_clear_serial++;
      f->state = GR_STATE_ENDING;
      log_info("run: ending (victory %)", fmt_b32(f->victory));
    }
  }
  if (run->slow_motion_serial != f->slow_seen) {
    f->slow_seen = run->slow_motion_serial;
    f->slow_motion = GR_SLOW_MOTION_TIME;
    log_info("run: slow motion (% s)", fmt_f32_p(GR_SLOW_MOTION_TIME, 2));
  }

  f32 dt = c->dt;
  f32 raw_dt = mem->dt;
  // the freeze switch or the editor's hold stops the run; presentation keeps the frame clock
  b32 frozen = sw->freeze || in->held;

  b32 controls = f->state == GR_STATE_PLAYING && (in->captured || f->debug_controls);
  f32 run_dt = 0.0f;
  f->time_scale = 1.0f;
  if (f->state == GR_STATE_PLAYING) {
    if (!controls) {
      // pointer lock refused or lost without an event: don't let the player die idle
      f->unlocked_time += dt;
      if (f->unlocked_time > GR_UNLOCKED_PAUSE)
        gr_flow_pause(f);
    } else {
      f->unlocked_time = 0.0f;
      if (!frozen) {
        f->time_scale = gr_flow_time_scale(f, raw_dt);
        run_dt = dt * f->time_scale;
      }
    }
  } else if (f->state == GR_STATE_ENDING && !frozen) {
    f->time_scale = gr_flow_time_scale(f, raw_dt);
    run_dt = dt * f->time_scale;
    f->end_timer -= dt;
    if (f->end_timer <= 0.0f)
      gr_flow_show_results(f);
  }

  c->dt = dt;
  c->run_dt = run_dt;
  c->controls = f->state == GR_STATE_PLAYING && controls;
  c->live = c->controls && !frozen;
  c->frame++;
}

// the dev switches resolved against this frame's flow, clock and the scene's level
ecs_system(gr_dev_system, EcsIn(GrDevSwitches), EcsResIn(GrFlow), EcsResIn(GrClock), EcsResIn(GrSettings),
           EcsResIn(GrLevel), EcsResInOut(GrDev)) {
  const GrFlow *f = ecs_sys_get_resource(GrFlow);
  GrDev *d = ecs_sys_get_resource_mut(GrDev);
  gr_dev_resolve(d, ecs_sys_get_component(GrDevSwitches, 0), ecs_sys_get_resource(GrLevel), f->state,
                 ecs_sys_get_resource(GrClock)->time, ecs_sys_get_resource(GrSettings)->quality);
  d->debug_controls = f->debug_controls;
}

// the view: the menu orbit on the title; in a run the first-person rig writes the camera and this reads it back
ecs_system(gr_view_system, EcsInOut(GrView), EcsInOut(Camera), EcsResIn(GrDev), EcsResIn(GrPlayerState),
           EcsResIn(GrEditor)) {
  const GrDev *d = ecs_sys_get_resource(GrDev);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrEditor *editor = ecs_sys_get_resource(GrEditor);
  b32 editing = editor->open && !editor->playing;
  b32 run = d->state != GR_STATE_TITLE && d->state != GR_STATE_LOADING && p->ready;
  for (i32 i = 0; i < it->count; i++) {
    GrView *view = ecs_sys_get_component_mut(GrView, i);
    Camera *cam = ecs_sys_get_component_mut(Camera, i);
    view->yaw = run ? p->yaw : d->yaw;
    view->pitch = run ? p->pitch : d->pitch;
    view->fov = run ? p->fov : d->fov;
    // while editing the editor's flycam owns the pose; in a run the first-person rig; else the menu orbit
    if (editing) {
      cam->pos = editor->eye;
      cam->rot = editor->look;
    } else if (!run) {
      cam->pos = d->eye;
      // the camera looks down its -Z
      cam->rot = quat_look(v3_negate(gr_view_forward(view->yaw, view->pitch)), V3_UP);
      cam->fov = view->fov;
    }
    cam->near_plane = GR_CAMERA_NEAR;
    cam->far_plane = GR_CAMERA_FAR;
  }
}

void gr_run_init(EcsWorld *w) {
  ecs_resource_set(w, GrFlow, ((GrFlow){.sw_quality = GR_QUALITY_HIGH, .was_focused = true, .time_scale = 1.0f}));
  ecs_resource_set(w, GrClock, ((GrClock){0}));
  ecs_resource_set(w, GrDev, ((GrDev){0}));
  ecs_resource_set(w, GrEditor, ((GrEditor){0}));
  gr_flow_init(w);
  // the camera exists from the first frame, so the loading screen renders before any asset lands
  EcsEntity cam = ecs_entity_create(w);
  ecs_component_set(w, cam, EntityName, {.name = fixedstr64_from_cstr("Camera")});
  ecs_component_set(w, cam, Camera,
                    {.rot = quat_identity(), .fov = GR_FOV, .near_plane = GR_CAMERA_NEAR, .far_plane = GR_CAMERA_FAR,
                     .aspect = 16.0f / 9.0f});
  ecs_component_set(w, cam, GrView, {.fov = GR_FOV});
}
