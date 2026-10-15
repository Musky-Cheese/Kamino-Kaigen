// Kamino thermal detonators: the pouch refills per front, G lobs one, it bounces off the level and bursts on its fuse.

#define KM_DET_GRAVITY 14.0f
#define KM_DET_THROW_SPEED 15.0f
#define KM_DET_SCALE 0.09f

// G: a detonator leaves the eye along the view with a little loft, inheriting the player's motion
ecs_system(km_detonator_throw_system, EcsResInOut(KmDetonators), EcsResIn(GrInput), EcsResIn(GrPlayerState), EcsResIn(GrClock),
           EcsResIn(GrWaves), EcsResIn(GrFx), EcsResInOut(RendererState)) {
  KmDetonators *d = ecs_sys_get_resource_mut(KmDetonators);
  const GrInput *in = ecs_sys_get_resource(GrInput);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  const GrWaves *wv = ecs_sys_get_resource(GrWaves);
  const GrFx *fx = ecs_sys_get_resource(GrFx);
  // a new front refills the pouch
  if (wv->run != d->run || wv->wave != d->wave) {
    d->run = wv->run;
    d->wave = wv->wave;
    d->count = KM_DET_CARRY;
  }
  d->cooldown = m_maxf(0.0f, d->cooldown - c->dt);
  if (!in->throw_pressed || !c->controls || !wv->live || !p->ready || d->count == 0 || d->cooldown > 0.0f || d->live >= KM_DET_LIVE_MAX || !fx->built)
    return;
  d->count--;
  d->cooldown = 0.6f;
  d->thrown_total++;
  d->throw_serial++;
  v3 fwd = p->view_forward;
  v3 start = v3_add(v3_add(p->eye, v3_scale(fwd, 0.6f)), (v3){0.0f, -0.15f, 0.0f});
  v3 vel = v3_add(v3_add(v3_scale(fwd, KM_DET_THROW_SPEED), (v3){0.0f, 3.5f, 0.0f}), v3_scale(p->velocity, 0.7f));
  KmDetonator det = {.position = start, .velocity = vel, .fuse = KM_DET_FUSE, .beep_timer = 0.0f};
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  EcsEntity e = ecs_renderer_spawn(it->world, r, &(EcsRendererSpawnDesc){.submesh = fx->mesh_sphere, .material = fx->mat_core[GR_DRONE_SENTINEL],
                                                                     .transform = {start, quat_identity(), v3_scale(V3_ONE, KM_DET_SCALE)},
                                                                     .name = "Detonator"});
  ecs_component_set(it->world, e, KmDetonator, det);
  ecs_component_set(it->world, e, GrSfxEmitter, {0});
  log_info("detonator: thrown, % left", fmt_u32(d->count));
}

// the burst: droids inside the radius take damage falling off with distance, the thrower too when close
hz_internal void km_detonator_burst(EcsWorld *w, const KmDetonator *det, const GrDroneField *field, const GrPlayerState *player, KmAirFx *fx) {
  u32 hit = 0;
  for (u32 i = 0; i < field->count; i++) {
    const GrDroneSlot *s = &field->drones[i];
    if (!s->targetable)
      continue;
    f32 dist = m_maxf(v3_distance(s->position, det->position) - s->radius, 0.0f);
    if (dist >= KM_DET_RADIUS)
      continue;
    GrPendingDamage *pd = ecs_component_get_mut(w, s->entity, GrPendingDamage);
    if (!pd)
      continue;
    f32 k = 1.0f - dist / KM_DET_RADIUS;
    v3 dir = gr_safe_normalize(v3_sub(s->position, det->position), V3_UP);
    pd->amount += KM_DET_DAMAGE * k;
    pd->impulse = v3_add(pd->impulse, v3_scale(dir, 3.0f * k));
    pd->direction = dir;
    pd->hits++;
    hit++;
  }
  f32 pd = v3_distance(player->feet, det->position);
  GrPlayerIntake *in = ecs_component_get_mut(w, player->entity, GrPlayerIntake);
  if (in) {
    if (pd < KM_DET_SELF_RADIUS)
      gr_player_intake_damage(in, KM_DET_SELF_DAMAGE * (1.0f - pd / KM_DET_SELF_RADIUS), det->position);
    gr_player_intake_shake(in, m_clampf(1.1f - pd * 0.04f, 0.0f, 1.1f));
  }
  if (fx->count + 2 <= KM_AIR_FX_MAX) {
    fx->items[fx->count++] = (GrFxRequest){.kind = GR_FX_EXPLOSION, .variant = GR_DRONE_SENTINEL, .p = det->position};
    fx->items[fx->count++] = (GrFxRequest){.kind = GR_FX_EXPLOSION, .variant = GR_DRONE_STRIKER, .p = v3_add(det->position, (v3){0.0f, 0.6f, 0.0f})};
  }
  log_info("detonator: burst, % droids hit", fmt_u32(hit));
}

// flight under gravity, bounces off the level, the fuse's beeps, then the burst
ecs_system(km_detonator_system, EcsInOut(KmDetonator), EcsInOut(WorldTransform), EcsInOut(GrSfxEmitter), EcsResInOut(KmDetonators),
           EcsResIn(GrDroneField), EcsResInOut(KmAirFx), EcsExternInOut(GrPendingDamage), EcsExternInOut(GrPlayerIntake)) {
  KmDetonators *d = ecs_sys_get_resource_mut(KmDetonators);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  KmAirFx *fx = ecs_sys_get_resource_mut(KmAirFx);
  const GrClock *clock = ecs_resource_get(it->world, GrClock);
  const GrWaves *waves = ecs_resource_get(it->world, GrWaves);
  const GrPlayerState *player = ecs_resource_get(it->world, GrPlayerState);
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  if (fx->frame != clock->frame) {
    fx->frame = clock->frame;
    fx->count = 0;
  }
  f32 dt = waves->dt;
  u32 live = 0;
  for (i32 i = 0; i < it->count; i++) {
    KmDetonator *det = ecs_sys_get_component_mut(KmDetonator, i);
    GrSfxEmitter *em = ecs_sys_get_component_mut(GrSfxEmitter, i);
    if (!waves->live) {
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    if (dt > 0.0f) {
      det->fuse -= dt;
      if (!det->resting) {
        det->velocity.y -= KM_DET_GRAVITY * dt;
        v3 step = v3_scale(det->velocity, dt);
        f32 len = v3_length(step);
        GrRayHit hit;
        if (len > 1e-5f && gr_level_raycast(layer, det->position, v3_scale(step, 1.0f / len), len + KM_DET_SCALE, &hit)) {
          // bounce: lose most of the normal speed and some of the slide; a slow one on a floor comes to rest
          det->position = v3_add(hit.point, v3_scale(hit.normal, KM_DET_SCALE * 1.05f));
          f32 vn = v3_dot(det->velocity, hit.normal);
          v3 normal_part = v3_scale(hit.normal, vn);
          v3 tangent = v3_sub(det->velocity, normal_part);
          det->velocity = v3_sub(v3_scale(tangent, 0.72f), v3_scale(normal_part, 0.38f));
          det->bounces++;
          if (hit.normal.y > 0.7f && v3_length(det->velocity) < 1.4f) {
            det->resting = true;
            det->velocity = V3_ZERO;
          }
          gr_sfx_emit(em, clock->frame, (GrSfxPlay){.sound = GR_SOUND_CASING, .positional = true, .position = det->position,
                                                     .volume = 0.9f, .rate = 0.6f, .jitter = 0.1f, .reverb = 0.5f});
        } else {
          det->position = v3_add(det->position, step);
        }
        det->spin += dt * 9.0f;
        if (det->position.y < KM_OCEAN_Y)
          det->fuse = 0.0f;
      }
      // beeps quicken as the fuse runs down
      det->beep_timer -= dt;
      if (det->beep_timer <= 0.0f) {
        det->beep_timer = m_lerpf(0.12f, 0.45f, m_saturatef(det->fuse / KM_DET_FUSE));
        gr_sfx_emit(em, clock->frame, (GrSfxPlay){.sound = GR_SOUND_HITMARKER, .positional = true, .position = det->position,
                                                   .volume = 0.8f, .rate = 1.6f + (1.0f - det->fuse / KM_DET_FUSE) * 0.6f, .reverb = 0.4f});
      }
    }
    if (det->fuse <= 0.0f) {
      km_detonator_burst(it->world, det, field, player, fx);
      gr_sfx_emit(em, clock->frame, (GrSfxPlay){.sound = GR_SOUND_EXPLOSION, .positional = true, .position = det->position,
                                                 .volume = 1.4f, .rate = 0.9f, .jitter = 0.08f, .reverb = 0.9f});
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    live++;
    f32 pulse = 1.0f + 0.25f * m_sinf(det->fuse * 18.0f) * (1.0f - det->fuse / KM_DET_FUSE);
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){det->position, quat_rotate_y(det->spin), v3_scale(V3_ONE, KM_DET_SCALE * pulse)};
  }
  d->live = live;
}

void km_detonator_init(EcsWorld *w) {
  // resources are main-lane app_init state
  assert(is_main_thread());
  ecs_resource_set(w, KmDetonators, ((KmDetonators){.count = KM_DET_CARRY, .wave = -2}));
}

void km_detonator_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_detonator_throw_system); // spawns the detonator entity, writes the pouch
  ecs_schedule_single(w, km_detonator_system);       // writes droid damage, the player's intake and the fx list
}
