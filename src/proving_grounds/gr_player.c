// Proving Grounds player: the engine Character and CameraFirstPerson configured for the game, plus health, jump pads and the intake.

#define GR_PLAYER_PITCH_LIMIT 1.52f // the look and recoil pitch clamp
#define GR_PLAYER_FALL_RESET_Y -20.0f
#define GR_PLAYER_PAD_COOLDOWN 0.6f
#define GR_PLAYER_PAD_HEIGHT 0.45f
#define GR_PLAYER_FIXED_HZ 120.0f
#define GR_PLAYER_MAX_STEPS 8

hz_internal b32 gr_state_in_run(u32 state) {
  return state == GR_STATE_PLAYING || state == GR_STATE_PAUSED || state == GR_STATE_ENDING ||
         state == GR_STATE_RESULTS;
}

// the viewmodel fov: the hip viewmodel fov blended to the gun's zoom fov in tan space
hz_internal f32 gr_player_viewmodel_fov(f32 zoom, f32 zoom_fov) {
  f32 hip = m_tanf(m_radians(GR_PLAYER_VIEWMODEL_FOV) * 0.5f);
  f32 aimed = m_tanf(m_radians(zoom_fov) * 0.5f);
  return m_degrees(2.0f * m_atanf(m_lerpf(hip, aimed, zoom)));
}

// the weapon sets the zoom target (RMB, live, not swapping or reloading) and the zoom fov on equip: last frame's weapon
hz_internal void gr_player_zoom_request(GrMotion *m, const GrWeaponState *ws) {
  m->zoom_target = ws->zoom_target;
  if (ws->zoom_fov > 0.0f)
    m->zoom_fov = ws->zoom_fov;
}

// the player as the engine character: a Quake capsule, 1.3 m jump, snapped to the ground 0.35 m down
hz_internal CharacterTuning gr_character_tuning(void) {
  return (CharacterTuning){
      .move_model = CHARACTER_MOVE_QUAKE,
      .facing = CHARACTER_FACING_HEADING,
      .radius = GR_PLAYER_RADIUS,
      .height = GR_PLAYER_HEIGHT,
      .eye_height = GR_PLAYER_EYE_HEIGHT,
      .walk_speed = GR_PLAYER_WALK_SPEED,
      .run_speed = GR_PLAYER_SPRINT_SPEED,
      .ground_accel = GR_PLAYER_GROUND_ACCEL,
      .air_accel = GR_PLAYER_AIR_ACCEL,
      .friction = GR_PLAYER_FRICTION,
      .gravity = GR_PLAYER_GRAVITY,
      .jump_height = GR_PLAYER_JUMP_VELOCITY * GR_PLAYER_JUMP_VELOCITY / (2.0f * GR_PLAYER_GRAVITY),
      .slope_limit_deg = 60.0f,
  };
}

// the camera juice as the first-person rig: bob, landing dip, punch, recoil, roll, ADS and the death slump
hz_internal CameraFirstPerson gr_camera_tuning(EcsEntity player) {
  CameraFirstPerson fp = {
      .disabled = true,
      .target = player,
      .pitch_limit = GR_PLAYER_PITCH_LIMIT,
      .fov = GR_PLAYER_FOV,
      .zoom_fov = GR_PLAYER_FOV,
      .near_plane = GR_CAMERA_NEAR,
      .far_plane = GR_CAMERA_FAR,
      .bob_speed_ref = GR_PLAYER_SPRINT_SPEED,
      .shake_tuning = {.decay = 1.4f, .exponent = 2.0f, .frequency = 22.0f, .roll = 0.05f, .pitch = 0.035f, .yaw = 0.035f},
  };
  camera_shake_init(&fp.shake, 0x6772u);
  return fp;
}

// the run's placement: at rest on `feet`, full health, every timer cleared, the look set
hz_internal void gr_player_reset(GrMotion *m, CharacterInput *ci, v3 feet, f32 yaw, f32 pitch) {
  ci->teleport = true;
  ci->teleport_pos = feet;
  ci->teleport_yaw = yaw;
  m->health = GR_PLAYER_MAX_HEALTH;
  m->alive = true;
  m->zoom_target = 0.0f;
  m->time_since_damage = 999.0f;
  m->pad_cooldown = 0.0f;
  m->pending_shake = m->pending_fov_kick = m->pending_punch = 0.0f;
  m->pending_look = m->pending_recoil = (v2){0};
  m->look_set = true;
  m->look_yaw = yaw;
  m->look_pitch = pitch;
  m->springs_reset = true;
  m->placed = true;
  m->events.reset = true;
}

// jump pads: the launch replaces the velocity, so the add is the difference
hz_internal void gr_player_jump_pads(GrMotion *m, CharacterInput *ci, const Character *ch, const GrLevel *level) {
  if (m->pad_cooldown > 0.0f)
    return;
  for (u32 p = 0; p < GR_LEVEL_JUMP_PAD_COUNT; p++) {
    const GrPadLaunch *pad = &level->pads[p];
    f32 dx = ch->position.x - pad->position.x;
    f32 dz = ch->position.z - pad->position.z;
    if (dx * dx + dz * dz < pad->radius * pad->radius && ch->position.y - pad->position.y < GR_PLAYER_PAD_HEIGHT) {
      ci->add_velocity = v3_sub(pad->launch_velocity, ch->velocity);
      m->pad_cooldown = GR_PLAYER_PAD_COOLDOWN;
      m->pending_fov_kick += 14.0f;
      m->pending_shake += 0.15f;
      m->events.launched = true;
      m->events.launch_pad = pad->position;
      return;
    }
  }
}

// placement (run start, pose, aim), then the frame's intent into the character while the run is live
ecs_system(gr_player_system, EcsIn(GrPlayer), EcsInOut(GrMotion), EcsInOut(CharacterInput), EcsIn(Character),
           EcsResIn(GrInput), EcsResIn(GrDev), EcsResIn(GrClock), EcsResInOut(GrPlayerClock)) {
  const GrInput *in = ecs_sys_get_resource(GrInput);
  const GrDev *d = ecs_sys_get_resource(GrDev);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  GrPlayerClock *pc = ecs_sys_get_resource_mut(GrPlayerClock);
  const GrLevel *level = ecs_resource_get(it->world, GrLevel);
  const GrWeaponState *ws = ecs_resource_get(it->world, GrWeaponState);
  const CameraRigState *rig = ecs_resource_get(it->world, CameraRigState);
  u32 state = d->state;
  pc->live = false;
  pc->step_dt = 0.0f;
  for (i32 i = 0; i < it->count; i++) {
    GrMotion *m = ecs_sys_get_component_mut(GrMotion, i);
    CharacterInput *ci = ecs_sys_get_component_mut(CharacterInput, i);
    const Character *ch = ecs_sys_get_component(Character, i);
    m->events = (GrPlayerEvents){0};
    m->controls = false;
    b32 entering = gr_state_in_run(state) && !gr_state_in_run(m->last_state);
    m->last_state = state;
    if (!gr_state_in_run(state) || !level->tables_ready)
      continue;

    if (entering || !m->placed || m->applied_restart != d->restart_serial || m->applied_pose != d->pose_serial) {
      // run start or the pose switch: the resolved switches already carry the pose and any aim
      gr_player_reset(m, ci, d->feet, d->yaw, d->pitch);
      m->applied_restart = d->restart_serial;
      m->applied_pose = d->pose_serial;
      m->applied_aim = d->aim_serial;
    } else if (m->applied_aim != d->aim_serial) {
      // the aim switch alone: from where the eye is now
      if (d->aim) {
        m->look_set = true;
        gr_aim_at(rig->position, d->aim_target, &m->look_yaw, &m->look_pitch);
      }
      m->applied_aim = d->aim_serial;
    }

    // playing updates only with controls (else the lock policy pauses); the outro runs without them
    b32 controls = state == GR_STATE_PLAYING && (in->captured || d->debug_controls);
    b32 live = controls || state == GR_STATE_ENDING;
    f32 dt = clock->run_dt;
    pc->controls = controls;
    if (!live || dt <= 0.0f)
      continue;
    pc->live = true;
    pc->step_dt = dt;
    m->controls = controls;
    gr_player_zoom_request(m, ws);

    f32 fwd = controls ? in->forward : 0.0f;
    f32 strafe = controls ? in->strafe : 0.0f;
    f32 len = m_sqrtf(fwd * fwd + strafe * strafe);
    f32 scale = len > 1.0f ? 1.0f / len : 1.0f;
    m->sprinting = controls && in->sprint && fwd > 0.0f && m->zoom_target == 0.0f;
    // aiming slows the walk: the input's length scales the character's speed
    scale *= m_lerpf(1.0f, 0.7f, rig->zoom);
    ci->move = (v2){strafe * scale, fwd * scale};
    ci->heading = rig->ready ? rig->yaw : d->yaw;
    ci->run = m->sprinting;
    ci->jump |= controls && in->jump_pressed;

    m->pad_cooldown -= dt;
    gr_player_jump_pads(m, ci, ch, level);
    if (ch->position.y + GR_PLAYER_HEIGHT - GR_PLAYER_RADIUS < GR_PLAYER_FALL_RESET_Y)
      gr_player_reset(m, ci, level->player_spawn, 0.0f, 0.0f);

    m->time_since_damage += dt;
    if (m->alive && m->time_since_damage > GR_PLAYER_REGEN_DELAY)
      m->health = m_minf(GR_PLAYER_MAX_HEALTH, m->health + GR_PLAYER_REGEN_RATE * dt);
  }
}

// the look, the queued recoil and trauma, the ADS request and the death slump into the first-person rig
ecs_system(gr_camera_input_system, EcsInOut(CameraFirstPerson), EcsResIn(GrInput), EcsResIn(GrPlayerClock),
           EcsResInOut(CameraRigInput), EcsResIn(CameraRigState)) {
  const GrInput *in = ecs_sys_get_resource(GrInput);
  const GrPlayerClock *pc = ecs_sys_get_resource(GrPlayerClock);
  const CameraRigState *rig = ecs_sys_get_resource(CameraRigState);
  CameraRigInput *out = ecs_sys_get_resource_mut(CameraRigInput);
  *out = (CameraRigInput){0};
  for (i32 i = 0; i < it->count; i++) {
    CameraFirstPerson *fp = ecs_sys_get_component_mut(CameraFirstPerson, i);
    GrMotion *m = ecs_component_get_mut(it->world, fp->target, GrMotion);
    const GrPlayer *player = ecs_component_get(it->world, fp->target, GrPlayer);
    if (!m || !player)
      continue;
    if (m->look_set) {
      fp->yaw = m->look_yaw;
      fp->pitch = m->look_pitch;
      fp->snap = m->springs_reset;
      m->look_set = false;
      m->springs_reset = false;
    }
    fp->disabled = !pc->live;
    if (!pc->live)
      continue;
    fp->zoom_target = m->zoom_target;
    fp->zoom_fov = m->zoom_fov;
    if (pc->controls) {
      // scaled by the zoom so aiming feels the same on screen at any magnification
      f32 zoomed = m_lerpf(GR_PLAYER_FOV, m->zoom_fov, rig->zoom);
      f32 zoom_scale = m_tanf(m_radians(zoomed) * 0.5f) / m_tanf(m_radians(GR_PLAYER_FOV) * 0.5f);
      f32 sens = GR_PLAYER_MOUSE_SENSITIVITY * player->sensitivity * zoom_scale;
      out->look = (v2){in->look.x * sens, -in->look.y * sens};
    }
    out->look = v2_add(out->look, m->pending_look);
    out->recoil = m->pending_recoil;
    out->punch = m->pending_punch;
    out->shake = m->pending_shake;
    out->fov_kick = m->pending_fov_kick;
    out->dead = !m->alive;
    m->pending_look = m->pending_recoil = (v2){0};
    m->pending_punch = m->pending_shake = m->pending_fov_kick = 0.0f;
  }
}

// damage, heal, shake and recoil for everything queued this frame
ecs_system(gr_player_intake_system, EcsInOut(GrMotion), EcsInOut(GrPlayerIntake), EcsResIn(GrDev)) {
  const GrDev *d = ecs_sys_get_resource(GrDev);
  const CameraRigState *rig = ecs_resource_get(it->world, CameraRigState);
  for (i32 i = 0; i < it->count; i++) {
    const GrPlayerIntake *q = ecs_sys_get_component(GrPlayerIntake, i);
    if (q->hits.len == 0 && q->heal == 0.0f && q->shake == 0.0f && q->punches == 0 && q->debug_damage == 0.0f)
      continue;
    GrPlayerIntake *in = ecs_sys_get_component_mut(GrPlayerIntake, i);
    GrMotion *m = ecs_sys_get_component_mut(GrMotion, i);
    if (in->debug_damage != 0.0f)
      gr_player_intake_damage(in, in->debug_damage, v3_add(rig->position, v3_scale(quat_rotate(rig->rotation, V3_BACK), 10.0f)));

    // the god switch makes damage a no-op: no health, shake or event
    for (u32 h = 0; h < in->hits.len && !d->god; h++) {
      if (!m->alive)
        break;
      GrPlayerHit hit = in->hits.items[h];
      m->health = m_maxf(0.0f, m->health - hit.amount);
      m->time_since_damage = 0.0f;
      m->pending_shake += 0.25f + hit.amount * 0.015f;
      if (m->events.hit_count < GR_PLAYER_MAX_HITS)
        m->events.hits[m->events.hit_count++] = hit;
      if (m->health <= 0.0f) {
        m->alive = false;
        m->events.died = true;
      }
    }
    if (in->heal > 0.0f && m->alive) {
      f32 healed = m_minf(in->heal, GR_PLAYER_MAX_HEALTH - m->health);
      if (healed > 0.0f) {
        m->health += healed;
        m->events.healed += healed;
      }
    }
    m->pending_shake += in->shake;
    if (in->punches > 0) {
      // the kept part turns the view for good (yaw + is left), the spring part settles back
      m->pending_look = v2_add(m->pending_look, (v2){-in->recoil_kept_yaw, in->recoil_kept_pitch});
      m->pending_recoil = v2_add(m->pending_recoil, (v2){-in->recoil_spring_yaw, in->recoil_spring_pitch});
      m->pending_punch += -0.35f * (f32)in->punches;
    }
    *in = (GrPlayerIntake){0};
  }
}

hz_internal void gr_player_state_write(GrPlayerState *out, const GrMotion *m, const Character *ch,
                                       const CameraRigState *rig, EcsEntity e, b32 in_run) {
  out->entity = e;
  out->ready = in_run && m->placed;
  out->controls = m->controls;
  out->feet = ch->position;
  out->eye = rig->position;
  out->velocity = ch->velocity;
  out->yaw = rig->yaw;
  out->pitch = rig->pitch;
  out->view_rot = rig->rotation;
  out->view_forward = quat_rotate(rig->rotation, V3_BACK); // the camera looks down its -Z
  out->fov = rig->fov;
  out->viewmodel_fov = gr_player_viewmodel_fov(rig->zoom, m->zoom_fov);
  out->zoom = rig->zoom;
  out->zoom_fov = m->zoom_fov;
  out->move_amount = rig->move_amount;
  out->bob_phase = rig->bob_phase;
  out->on_ground = character_grounded(ch);
  out->sprinting = m->sprinting;
  out->shake = rig->shake;
  out->health = m->health;
  out->alive = m->alive;
  out->events = m->events;
}

// the character's counters since last frame into the player events, then one player row into the shared GrPlayerState
ecs_system(gr_player_publish_system, EcsInOut(GrMotion), EcsIn(Character), EcsResIn(GrFlow), EcsResIn(CameraRigState),
           EcsResInOut(GrPlayerState)) {
  b32 in_run = gr_state_in_run(ecs_sys_get_resource(GrFlow)->state);
  GrPlayerState *out = ecs_sys_get_resource_mut(GrPlayerState);
  const CameraRigState *rig = ecs_sys_get_resource(CameraRigState);
  for (i32 i = 0; i < it->count; i++) {
    GrMotion *m = ecs_sys_get_component_mut(GrMotion, i);
    const Character *ch = ecs_sys_get_component(Character, i);
    if (ch->jumps != m->jumps_seen)
      m->events.jumped = true;
    if (ch->landings != m->landings_seen && ch->land_speed > 3.0f) {
      m->events.landed = true;
      m->events.land_impact = ch->land_speed;
    }
    if (ch->footsteps != m->footsteps_seen) {
      m->events.footstep = true;
      m->events.footstep_sprinting = ch->footstep_running;
    }
    // a teleport the player asked for is expected once; one past that is the --commands hook
    if (ch->teleports > m->teleports_seen)
      m->events.reset = true;
    m->jumps_seen = ch->jumps;
    m->landings_seen = ch->landings;
    m->footsteps_seen = ch->footsteps;
    m->teleports_seen = ch->teleports;
    gr_player_state_write(out, m, ch, rig, it->entities[i], in_run);
  }
}

// the same after the intake: health, hits, heal, death and the shake it added
ecs_system(gr_player_consolidate, EcsIn(GrMotion), EcsIn(Character), EcsResIn(GrFlow), EcsResIn(CameraRigState),
           EcsResInOut(GrPlayerState)) {
  b32 in_run = gr_state_in_run(ecs_sys_get_resource(GrFlow)->state);
  GrPlayerState *out = ecs_sys_get_resource_mut(GrPlayerState);
  for (i32 i = 0; i < it->count; i++)
    gr_player_state_write(out, ecs_sys_get_component(GrMotion, i), ecs_sys_get_component(Character, i),
                          ecs_sys_get_resource(CameraRigState), it->entities[i], in_run);
}

// the remainder of the fixed loop into the alpha the character pose blends by
ecs_system(gr_player_alpha_system, EcsResIn(GrPlayerClock), EcsResInOut(CharacterFrame)) {
  ecs_sys_get_resource_mut(CharacterFrame)->alpha = fixed_timestep_alpha(&ecs_sys_get_resource(GrPlayerClock)->ft);
}

void gr_player_init(EcsWorld *w) {
  ecs_resource_set(w, GrInput, ((GrInput){0}));
  ecs_resource_set(w, GrPlayerState, ((GrPlayerState){0}));
  ecs_resource_set(w, GrPlayerClock,
                   ((GrPlayerClock){.ft = fixed_timestep_make(GR_PLAYER_FIXED_HZ, GR_PLAYER_MAX_STEPS)}));
  character_controller_init(w);
  camera_rig_init(w);

  EcsEntity e = ecs_entity_create(w);
  ecs_component_set(w, e, EntityName, {.name = fixedstr64_from_cstr("Player")});
  ecs_component_set(w, e, GrPlayer, {.sensitivity = 1.0f});
  ecs_component_set(w, e, GrPlayerIntake, {0});
  ecs_component_set(w, e, GrMotion,
                    {
                        .alive = true,
                        .health = GR_PLAYER_MAX_HEALTH,
                        .zoom_fov = GR_PLAYER_FOV,
                        .time_since_damage = 999.0f,
                    });
  ecs_component_set(w, e, WorldTransform, {.position = V3_ZERO, .rotation = quat_identity(), .scale = V3_ONE});
  ecs_component_set(w, e, LocalTransform, {.position = V3_ZERO, .rotation = quat_identity(), .scale = V3_ONE});
  ecs_component_set(w, e, CharacterTuning, gr_character_tuning());
  ecs_component_set(w, e, CharacterInput, {0});

  // the camera exists from the first frame (gr_run_init); the rig takes it over once a run starts
  EcsQuery *cams = ecs_query_cached(w, EcsIn(Camera));
  ecs_query_foreach(cams, qit) {
    if (qit.count > 0)
      ecs_component_set(w, qit.entities[0], CameraFirstPerson, gr_camera_tuning(e));
  }
}

void gr_player_schedule_input(EcsWorld *w) {
  ecs_schedule_single(w, gr_player_system); // writes the GrPlayerClock resource
}

void gr_player_fixed(EcsWorld *w) {
  GrPlayerClock *clock = ecs_resource_get_mut(w, GrPlayerClock);
  // every lane takes the same steps from the same dt
  i32 steps = fixed_timestep_begin(&clock->ft, clock->step_dt);
  for (i32 k = 0; k < steps; k++) {
    character_schedule_fixed(w);
    ecs_flush_ex(w, &(EcsFlushDesc){.delta_time = 1.0f / GR_PLAYER_FIXED_HZ, .steps = 1});
  }
}

void gr_player_schedule(EcsWorld *w) {
  ecs_schedule_single(w, gr_player_alpha_system); // writes the CharacterFrame resource
  character_schedule_frame(w);
  ecs_schedule_single(w, gr_camera_input_system); // writes the CameraRigInput resource
  camera_rig_schedule(w);
  ecs_schedule_single(w, gr_player_publish_system); // writes the GrPlayerState resource (one row)
}

void gr_player_schedule_intake(EcsWorld *w) {
  ecs_schedule(w, gr_player_intake_system);
  ecs_schedule_single(w, gr_player_consolidate); // writes the GrPlayerState resource (one row)
}
