// Kamino air battle: vultures and gunships on circuits over the city, bursts between them, a vulture shot down now and then.

#define KM_AIR_FALL_TIME 2.6f
#define KM_AIR_GONE_TIME 9.0f
#define KM_AIR_GONE_Y -300.0f

hz_internal b32 km_flyer_is_vulture(const KmFlyer *f) { return f->kind == KM_FLYER_VULTURE; }

// a fresh circuit for the flyer: vultures wide and high, gunships tighter and lower, each kind circling the other way
hz_internal void km_flyer_reset(KmFlyer *f) {
  b32 v = km_flyer_is_vulture(f);
  f->radius = v ? random_f32_range(&f->rng, 35.0f, 75.0f) : random_f32_range(&f->rng, 24.0f, 50.0f);
  f->altitude = v ? random_f32_range(&f->rng, 22.0f, 42.0f) : random_f32_range(&f->rng, 16.0f, 28.0f);
  f32 speed = v ? random_f32_range(&f->rng, 30.0f, 40.0f) : random_f32_range(&f->rng, 16.0f, 24.0f);
  f->rate = speed / f->radius * (v ? 1.0f : -1.0f);
  f->bob = random_f32_range(&f->rng, 3.0f, 8.0f);
  f->bob_phase = random_f32_range(&f->rng, 0.0f, 2.0f * M_PI);
  f->angle = random_f32_range(&f->rng, 0.0f, 2.0f * M_PI);
  f->fire_timer = random_f32_range(&f->rng, 0.8f, 2.5f);
  f->burst_left = 0;
  f->target = -1;
  f->state = KM_FLYER_FLYING;
  f->state_time = 0.0f;
}

hz_internal void km_air_push(KmAirFx *fx, GrFxRequest r) {
  if (fx->count < KM_AIR_FX_MAX)
    fx->items[fx->count++] = r;
}

// the nearest enemy flyer in the air, or -1
hz_internal i32 km_flyer_pick(const KmFlyer *f, const KmAir *air) {
  i32 best = -1;
  f32 best_d = 1e9f;
  for (u32 i = 0; i < KM_FLYERS; i++) {
    b32 enemy_vulture = i < KM_VULTURES;
    if (enemy_vulture == km_flyer_is_vulture(f) || !((air->alive_mask >> i) & 1u))
      continue;
    f32 d = v3_length(v3_sub(air->positions[i], f->position));
    if (d < best_d) {
      best_d = d;
      best = (i32)i;
    }
  }
  return best;
}

// the circuits: each flyer around the drifting centre, firing at the other side; a vulture is picked off on the kill clock
ecs_system(km_air_system, EcsInOut(KmFlyer), EcsOut(WorldTransform), EcsInOut(GrSfxEmitter), EcsResInOut(KmAir), EcsResInOut(KmAirFx),
           EcsResIn(GrClock), EcsResIn(GrPlayerState)) {
  KmAir *air = ecs_sys_get_resource_mut(KmAir);
  KmAirFx *fx = ecs_sys_get_resource_mut(KmAirFx);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  f32 dt = m_minf(clock->dt, 0.05f);
  if (fx->frame != clock->frame) {
    fx->frame = clock->frame;
    fx->count = 0;
  }
  // the fight leans towards the front, a little ahead of the player
  v3 want = (v3){player->feet.x * 0.5f, 0.0f, player->feet.z - 40.0f};
  air->centre = v3_damp(air->centre, want, 0.4f, dt);
  air->kill_timer -= dt;
  i32 victim = -1;
  if (air->kill_timer <= 0.0f) {
    Random rng = random_create(0xa1bu + air->seed++ * 7919u);
    air->kill_timer = random_f32_range(&rng, 12.0f, 22.0f);
    u32 pick = (u32)random_f32_range(&rng, 0.0f, (f32)KM_VULTURES - 0.01f);
    for (u32 k = 0; k < KM_VULTURES; k++) {
      u32 i = (pick + k) % KM_VULTURES;
      if ((air->alive_mask >> i) & 1u) {
        victim = (i32)i;
        break;
      }
    }
  }
  u32 alive = 0;
  for (i32 i = 0; i < it->count; i++) {
    KmFlyer *f = ecs_sys_get_component_mut(KmFlyer, i);
    GrSfxEmitter *em = ecs_sys_get_component_mut(GrSfxEmitter, i);
    b32 vulture = km_flyer_is_vulture(f);
    f->state_time += dt;
    if (f->state == KM_FLYER_FLYING && victim == (i32)f->index) {
      f->state = KM_FLYER_FALLING;
      f->state_time = 0.0f;
      f->smoke_timer = 0.0f;
      km_air_push(fx, (GrFxRequest){.kind = GR_FX_IMPACT_DRONE, .variant = GR_DRONE_STRIKER, .p = f->position, .n = V3_UP, .d = V3_UP});
    }
    if (f->state == KM_FLYER_GONE) {
      if (f->state_time > KM_AIR_GONE_TIME)
        km_flyer_reset(f);
      f->position = (v3){air->centre.x, KM_AIR_GONE_Y, air->centre.z};
      *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){f->position, quat_identity(), V3_ONE};
      continue;
    }
    if (f->state == KM_FLYER_FALLING) {
      // a tightening spiral down, smoke behind it; the burst when its time is up or it meets the deck
      f->angle += f->rate * 2.2f * dt;
      f->altitude -= (12.0f + f->state_time * 22.0f) * dt;
      f->radius = m_maxf(f->radius - 18.0f * dt, 20.0f);
      f->smoke_timer -= dt;
      if (f->smoke_timer <= 0.0f) {
        f->smoke_timer = 0.11f;
        km_air_push(fx, (GrFxRequest){.kind = GR_FX_DAMAGE_SMOKE, .p = f->position});
        if (random_f32_range(&f->rng, 0.0f, 1.0f) < 0.5f)
          km_air_push(fx, (GrFxRequest){.kind = GR_FX_DEBRIS_SPARK, .p = f->position, .d = v3_scale(f->velocity, -0.2f)});
      }
      if (f->state_time > KM_AIR_FALL_TIME || f->altitude < 6.0f) {
        km_air_push(fx, (GrFxRequest){.kind = GR_FX_EXPLOSION, .variant = GR_DRONE_STRIKER, .p = f->position});
        km_air_push(fx, (GrFxRequest){.kind = GR_FX_EXPLOSION, .variant = GR_DRONE_SCOUT, .p = v3_add(f->position, (v3){1.5f, 0.8f, -1.0f})});
        f32 d = v3_length(v3_sub(f->position, player->eye));
        gr_sfx_emit(em, clock->frame,
                    (GrSfxPlay){.sound = GR_SOUND_EXPLOSION, .positional = true, .position = f->position,
                                .volume = m_clampf(1.6f - d / 220.0f, 0.4f, 1.3f), .rate = 0.55f, .jitter = 0.1f, .reverb = 0.8f});
        f->state = KM_FLYER_GONE;
        f->state_time = 0.0f;
      }
    } else {
      f->angle += f->rate * dt;
      f->bob_phase += dt * 0.6f;
    }
    // the circuit point and the velocity along it
    f32 y = f->altitude + m_sinf(f->bob_phase) * f->bob;
    v3 p = (v3){air->centre.x + m_cosf(f->angle) * f->radius, y, air->centre.z + m_sinf(f->angle) * f->radius};
    v3 vel = v3_scale(v3_sub(p, f->position), dt > 0.0f ? 1.0f / dt : 0.0f);
    if (f->state_time < 0.1f || v3_lengthsq(vel) < 1.0f)
      vel = (v3){-m_sinf(f->angle) * f->rate * f->radius, 0.0f, m_cosf(f->angle) * f->rate * f->radius};
    f->velocity = vel;
    f->position = p;
    v3 fwd = v3_normalize(vel);
    f32 want_roll = (f->rate > 0.0f ? -1.0f : 1.0f) * (vulture ? 0.55f : 0.3f);
    if (f->state == KM_FLYER_FALLING)
      want_roll += f->state_time * 1.4f;
    f->roll = m_damp(f->roll, want_roll, 2.5f, dt);
    quat rot = quat_mul(quat_look(fwd, V3_UP), quat_rotate_z(f->roll));
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){p, rot, V3_ONE};
    air->positions[f->index] = p;
    if (f->state == KM_FLYER_FLYING)
      alive |= 1u << f->index;
    // the gun: bursts at the nearest enemy, loose enough to miss most of the time
    if (f->state != KM_FLYER_FLYING)
      continue;
    if (f->burst_left > 0) {
      f->burst_timer -= dt;
      if (f->burst_timer <= 0.0f && f->target >= 0) {
        f->burst_left--;
        f->burst_timer = vulture ? 0.09f : 0.07f;
        v3 to = air->positions[f->target];
        v3 miss = (v3){random_f32_range(&f->rng, -6.0f, 6.0f), random_f32_range(&f->rng, -4.0f, 4.0f), random_f32_range(&f->rng, -6.0f, 6.0f)};
        v3 dir = v3_normalize(v3_sub(v3_add(to, miss), p));
        v3 from = v3_add(p, v3_scale(fwd, vulture ? 3.5f : 6.0f));
        v3 end = v3_add(from, v3_scale(dir, m_minf(v3_length(v3_sub(to, p)) + 30.0f, 220.0f)));
        km_air_push(fx, (GrFxRequest){.kind = GR_FX_TRACER, .variant = vulture ? GR_WEAPON_LANCER : GR_WEAPON_RIFLE, .p = from, .n = end});
        gr_sfx_emit(em, clock->frame,
                    (GrSfxPlay){.sound = vulture ? GR_SOUND_DRONE_FIRE : GR_SOUND_RIFLE, .positional = true, .position = from,
                                .volume = 0.5f, .rate = vulture ? 0.7f : 0.8f, .jitter = 0.08f, .reverb = 0.6f});
      }
    } else {
      f->fire_timer -= dt;
      if (f->fire_timer <= 0.0f) {
        f->fire_timer = random_f32_range(&f->rng, 1.2f, 3.5f);
        f->target = km_flyer_pick(f, air);
        if (f->target >= 0 && v3_dot(fwd, v3_normalize(v3_sub(air->positions[f->target], p))) > 0.2f) {
          f->burst_left = vulture ? 4 : 6;
          f->burst_timer = 0.0f;
        }
      }
    }
  }
  air->alive_mask = alive;
}

// the squadron takes off once both models are in
ecs_system(km_air_spawn_system, EcsResInOut(KmAir), EcsResIn(AssetSystem), EcsResIn(GrPlayerState)) {
  KmAir *air = ecs_sys_get_resource_mut(KmAir);
  const AssetSystem *assets = ecs_sys_get_resource(AssetSystem);
  if (air->spawned || !asset_is_ready(assets, air->vulture) || !asset_is_ready(assets, air->gunship))
    return;
  air->spawned = true;
  air->centre = (v3){0.0f, 0.0f, ecs_sys_get_resource(GrPlayerState)->feet.z - 40.0f};
  for (u32 i = 0; i < KM_FLYERS; i++) {
    KmFlyer f = {.kind = i < KM_VULTURES ? KM_FLYER_VULTURE : KM_FLYER_GUNSHIP, .index = i, .target = -1, .rng = random_create(0x4a1u + i * 2654435761u)};
    km_flyer_reset(&f);
    f.angle = (f32)i / (f32)KM_FLYERS * 2.0f * M_PI;
    f.position = (v3){air->centre.x + m_cosf(f.angle) * f.radius, f.altitude, air->centre.z + m_sinf(f.angle) * f.radius};
    HzHandleT(ModelBlobAsset) model = f.kind == KM_FLYER_VULTURE ? air->vulture : air->gunship;
    EcsEntity e = ecs_renderer_spawn(it->world, NULL, &(EcsRendererSpawnDesc){.model_asset = blob_asset_ref_from_handle(ModelBlobAsset, model),
                                                                          .transform = {f.position, quat_identity(), V3_ONE},
                                                                          .casts_shadow = false, .name = f.kind == KM_FLYER_VULTURE ? "Vulture" : "Gunship"});
    ecs_component_set(it->world, e, KmFlyer, f);
    ecs_component_set(it->world, e, GrSfxEmitter, {0});
    air->positions[i] = f.position;
  }
  log_info("air: % vultures and % gunships overhead", fmt_u32(KM_VULTURES), fmt_u32(KM_GUNSHIPS));
}

// the fight's effects, thrown once the fx assets are in
ecs_system(km_air_fx_system, EcsResIn(KmAirFx), EcsResIn(VfxRequests), EcsResIn(GrFxAssets), EcsResIn(GrClock), EcsResInOut(GrFxFrame)) {
  const KmAirFx *fx = ecs_sys_get_resource(KmAirFx);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  if (!as->ready || fx->frame != ecs_sys_get_resource(GrClock)->frame)
    return;
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  GrFxFrame *f = ecs_sys_get_resource_mut(GrFxFrame);
  for (u32 i = 0; i < fx->count; i++)
    gr_fx_throw(rq, as, f, fx->items[i]);
}

void km_air_init(EcsWorld *w, AssetSystem *assets) {
  // resources are main-lane app_init state
  assert(is_main_thread());
  KmAir air = {.vulture = asset_load(ModelBlobAsset, assets, .path = "kamino/models/vehicles/vulture.glb"),
               .gunship = asset_load(ModelBlobAsset, assets, .path = "kamino/models/vehicles/gunship.glb"),
               .kill_timer = 9.0f};
  ecs_resource_set(w, KmAir, air);
  ecs_resource_set(w, KmAirFx, ((KmAirFx){0}));
}

void km_air_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_air_spawn_system); // spawns the squadron's entities
  ecs_schedule_single(w, km_air_system);       // writes the shared positions, kill clock and fx list
}

void km_air_schedule_fx(EcsWorld *w) { ecs_schedule_single(w, km_air_fx_system); } // writes GrFxFrame
