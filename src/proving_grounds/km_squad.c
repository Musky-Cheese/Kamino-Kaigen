// Kamino clone squad: troopers that move up with the player, pick droids off from the front and take the droids' fire.

#define KM_CLONE_HEALTH 90.0f
#define KM_CLONE_SPEED 3.6f        // walk, m/s
#define KM_CLONE_RUN 6.0f          // catching up
#define KM_CLONE_RADIUS 0.4f       // body sphere at the hips against the level
#define KM_CLONE_RANGE 42.0f       // the farthest droid a trooper engages
#define KM_CLONE_DAMAGE 7.0f       // a hit, before falloff
#define KM_CLONE_EYE 0.65f         // above the hips

// formation places around the player, world XZ: the front runs along -Z
static const v2 km_squad_places[KM_SQUAD_MAX] = {{-3.5f, -2.5f}, {3.5f, -2.0f}, {-6.0f, 1.0f}, {6.0f, 1.5f}, {-2.0f, 3.0f}, {2.0f, 3.5f}};

// a new trooper at `at` (the deck under it), facing the front
hz_internal void km_clone_spawn(EcsWorld *w, const KmSquad *sq, u32 slot, u32 run, v3 at) {
  KmClone c = {.slot = slot, .run = run, .position = {at.x, at.y + KM_WALKER_HEIGHT, at.z}, .yaw = M_PI, .health = KM_CLONE_HEALTH,
               .target = -1, .rng = random_create(0xc7u + slot * 7919u + run * 104729u)};
  c.fire_timer = random_f32_range(&c.rng, 0.5f, 1.5f);
  EcsEntity e = ecs_renderer_spawn(w, NULL, &(EcsRendererSpawnDesc){.model_asset = blob_asset_ref_from_handle(ModelBlobAsset, sq->model),
                                                                .transform = {c.position, quat_rotate_y(c.yaw), V3_ONE},
                                                                .casts_shadow = true, .name = "CloneTrooper"});
  ecs_component_set(w, e, KmClone, c);
}

// the nearest droid in range the trooper can see from its eye, or -1
hz_internal i32 km_clone_pick(const KmClone *c, const GrDroneField *f, const EcsCollisionLayer *layer) {
  v3 eye = v3_add(c->position, (v3){0, KM_CLONE_EYE, 0});
  i32 best = -1;
  f32 best_d = KM_CLONE_RANGE;
  for (u32 i = 0; i < f->count; i++) {
    const GrDroneSlot *s = &f->drones[i];
    f32 d = v3_distance(eye, s->position);
    if (!s->targetable || d >= best_d)
      continue;
    GrRayHit hit;
    v3 dir = v3_scale(v3_sub(s->position, eye), 1.0f / m_maxf(d, 1e-3f));
    if (gr_level_raycast(layer, eye, dir, d - s->radius, &hit))
      continue;
    best = (i32)i;
    best_d = d;
  }
  return best;
}

// one shot at the target: a hit lands on its pending damage, a miss flies past; the tracer either way
hz_internal void km_clone_fire(KmClone *c, EcsWorld *w, const GrDroneSlot *t, const EcsCollisionLayer *layer, KmSquadShots *shots) {
  v3 eye = v3_add(c->position, (v3){0, KM_CLONE_EYE, 0});
  quat q = quat_rotate_y(c->yaw);
  v3 muzzle = v3_add(c->position, quat_rotate(q, (v3){-0.1f, 0.3f, 0.95f}));
  f32 d = v3_distance(eye, t->position);
  // nearer droids are easier: 22% at 10 m down to 6% at the edge of range; the player does most of the work
  f32 chance = m_lerpf(0.22f, 0.06f, m_saturatef((d - 10.0f) / (KM_CLONE_RANGE - 10.0f)));
  b32 hit = random_f32(&c->rng) < chance;
  v3 aim = t->position;
  if (!hit)
    aim = v3_add(aim, (v3){random_f32_range(&c->rng, -1.2f, 1.2f), random_f32_range(&c->rng, -0.4f, 1.0f), random_f32_range(&c->rng, -1.2f, 1.2f)});
  v3 dir = v3_normalize(v3_sub(aim, muzzle));
  KmSquadShot shot = {.from = muzzle, .to = v3_add(muzzle, v3_scale(dir, 60.0f))};
  GrRayHit wall;
  b32 blocked = gr_level_raycast(layer, muzzle, dir, 60.0f, &wall);
  if (hit && (!blocked || wall.distance > d - t->radius)) {
    GrPendingDamage *pd = ecs_component_get_mut(w, t->entity, GrPendingDamage);
    if (pd) {
      pd->amount += KM_CLONE_DAMAGE;
      pd->impulse = v3_add(pd->impulse, v3_scale(dir, 0.6f));
      pd->direction = dir;
      pd->hits++;
    }
    shot.to = v3_sub(t->position, v3_scale(dir, t->radius * 0.5f));
  } else if (blocked) {
    shot.to = wall.point;
    shot.normal = wall.normal;
    shot.world = true;
  }
  if (shots->count < KM_SQUAD_SHOTS)
    shots->items[shots->count++] = shot;
}

// where the trooper heads: its place beside the player, routed through the bridges like the droids
hz_internal v3 km_clone_goal(const KmClone *c, const GrLevel *lvl, v3 player) {
  v2 off = km_squad_places[c->slot % KM_SQUAD_MAX];
  v3 place = {player.x + off.x, player.y + KM_WALKER_HEIGHT, player.z + off.y};
  u32 zone = gr_level_zone_at(lvl, c->position);
  u32 goal = gr_level_zone_at(lvl, place);
  GrNextPortal route = gr_level_next_portal(lvl, zone, goal);
  if (route.portal >= 0) {
    const GrPortal *p = &lvl->portals[route.portal];
    v3 n = v3_scale(p->normal, (f32)route.dir);
    return v3_add((v3){p->center.x, c->position.y, p->center.z}, v3_scale(n, 1.5f));
  }
  // inside its room: clear of the walls and rails
  aabb b = lvl->zones[zone].box;
  place.x = m_clampf(place.x, b.min.x + 0.8f, b.max.x - 0.8f);
  place.z = m_clampf(place.z, b.min.z + 0.8f, b.max.z - 0.8f);
  return place;
}

// the squad's frame: hits taken, death, movement, targets and fire; few troopers, so one caller writes the droids' damage.
// GrWaves, GrClock and GrLevel are settled before the scope's second flush, read like the run's config
ecs_system(km_squad_system, EcsInOut(KmClone), EcsOut(WorldTransform), EcsResIn(GrPlayerState), EcsResIn(GrDroneField),
           EcsResIn(GrBoltHits), EcsResIn(EcsPhysicsWorld), EcsResInOut(KmSquadShots), EcsExternInOut(GrPendingDamage)) {
  const GrWaves *waves = ecs_resource_get(it->world, GrWaves);
  const GrClock *clock = ecs_resource_get(it->world, GrClock);
  const GrLevel *lvl = ecs_resource_get(it->world, GrLevel);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const GrBoltHits *hits = ecs_sys_get_resource(GrBoltHits);
  KmSquadShots *shots = ecs_sys_get_resource_mut(KmSquadShots);
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  if (shots->frame != clock->frame) {
    shots->frame = clock->frame;
    shots->count = 0;
  }
  f32 dt = waves->dt;
  if (!waves->live || dt <= 0.0f || !lvl->tables_ready)
    return;
  for (i32 i = 0; i < it->count; i++) {
    KmClone *c = ecs_sys_get_component_mut(KmClone, i);
    if (c->run != waves->run) {
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    if (!c->dead && hits->frame == clock->frame && c->slot < KM_SQUAD_MAX && hits->clone_damage[c->slot] > 0.0f) {
      c->health -= hits->clone_damage[c->slot];
      c->hit_flash = 1.0f;
      if (c->health <= 0.0f) {
        c->dead = true;
        log_info("squad: trooper % down", fmt_u32(c->slot + 1));
      }
    }
    if (c->dead) {
      // falls back and stays down
      c->dead_time += dt;
      f32 fall = m_smoothstepf(0.0f, 0.7f, c->dead_time);
      c->gait_amount = 0.0f;
      quat lie = quat_mul(quat_rotate_y(c->yaw), quat_axis_angle((v3){1, 0, 0}, -fall * M_PI * 0.5f));
      v3 down = {c->position.x, c->position.y - fall * (KM_WALKER_HEIGHT - 0.25f), c->position.z};
      *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){down, lie, V3_ONE};
      continue;
    }

    // move: steer to its place, pushed out of the level, kept on the deck
    v3 goal = km_clone_goal(c, lvl, player->feet);
    v3 to = v3_sub(goal, c->position);
    to.y = 0.0f;
    f32 dist = v3_length(to);
    f32 speed = dist > 10.0f ? KM_CLONE_RUN : KM_CLONE_SPEED;
    v3 want = dist > 0.6f ? v3_scale(to, m_minf(speed, dist * 1.5f) / dist) : V3_ZERO;
    c->velocity = v3_add(c->velocity, gr_v3_cap(v3_sub(want, c->velocity), 10.0f * dt));
    c->position = v3_add(c->position, v3_scale(c->velocity, dt));
    v3 n;
    gr_drone_resolve(layer, &c->position, KM_CLONE_RADIUS, &n);
    c->position.y = gr_level_ground_height(lvl, v3_add(c->position, (v3){0, 0.5f, 0})) + KM_WALKER_HEIGHT;

    // targets: the nearest visible droid, rechecked twice a second
    c->retarget_timer -= dt;
    if (c->retarget_timer <= 0.0f) {
      c->retarget_timer = random_f32_range(&c->rng, 0.4f, 0.6f);
      c->target = km_clone_pick(c, field, layer);
      c->target_entity = c->target >= 0 ? field->drones[c->target].entity : ECS_ENTITY_INVALID;
    }
    const GrDroneSlot *t = NULL;
    if (c->target >= 0 && (u32)c->target < field->count && field->drones[c->target].entity == c->target_entity &&
        field->drones[c->target].targetable)
      t = &field->drones[c->target];

    // face the target, else where it walks, else the front
    f32 yaw = c->yaw;
    if (t) {
      v3 d = v3_sub(t->position, c->position);
      yaw = m_atan2f(d.x, d.z);
      c->aim_pitch = m_damp(c->aim_pitch, m_clampf(m_atan2f(d.y - KM_CLONE_EYE * 0.5f, m_sqrtf(d.x * d.x + d.z * d.z)), -0.6f, 0.6f), 6.0f, dt);
    } else {
      if (dist > 1.0f)
        yaw = m_atan2f(to.x, to.z);
      else
        yaw = M_PI;
      c->aim_pitch = m_damp(c->aim_pitch, 0.0f, 4.0f, dt);
    }
    c->yaw += m_wrap_pi(yaw - c->yaw) * m_minf(1.0f, dt * 8.0f);
    f32 ground = m_sqrtf(c->velocity.x * c->velocity.x + c->velocity.z * c->velocity.z);
    c->gait_amount = m_damp(c->gait_amount, m_minf(1.0f, ground / 2.2f), 8.0f, dt);
    c->gait += dt * ground / 1.5f * 2.0f * M_PI;
    c->hit_flash = m_maxf(0.0f, c->hit_flash - dt * 4.0f);

    // fire: bursts of three while it has a target and is roughly facing it
    if (t) {
      if (c->burst_left > 0) {
        c->burst_timer -= dt;
        if (c->burst_timer <= 0.0f) {
          km_clone_fire(c, it->world, t, layer, shots);
          c->burst_left--;
          c->burst_timer = 0.16f;
          if (c->burst_left == 0)
            c->fire_timer = random_f32_range(&c->rng, 1.4f, 2.8f);
        }
      } else {
        c->fire_timer -= dt;
        if (c->fire_timer <= 0.0f && m_fabsf(m_wrap_pi(yaw - c->yaw)) < 0.3f) {
          c->burst_left = 3;
          c->burst_timer = 0.0f;
        }
      }
    }
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){c->position, quat_rotate_y(c->yaw), V3_ONE};
  }
}

// the squad snapshot the droids and bolts read next frame; the raising at a run's start and the reinforcements
ecs_system(km_squad_publish_system, EcsResInOut(KmSquad), EcsResInOut(GrDroneField), EcsResIn(GrWaves), EcsResIn(GrPlayerState),
           EcsResIn(AssetSystem)) {
  KmSquad *sq = ecs_sys_get_resource_mut(KmSquad);
  KmSquadView *view = &ecs_sys_get_resource_mut(GrDroneField)->squad;
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  if (!sq->model_ready) {
    sq->model_ready = asset_is_ready(ecs_sys_get_resource(AssetSystem), sq->model);
    if (!sq->model_ready)
      return;
  }
  // the snapshot is rebuilt from the living troopers (a fallen one keeps its slot until a reinforcement takes it)
  for (u32 s = 0; s < KM_SQUAD_MAX; s++)
    view->alive[s] = false;
  u32 count = 0;
  ecs_query_sync(&sq->troopers);
  ecs_query_foreach(&sq->troopers, qit) {
    const KmClone *cs = ecs_iter_get_component(&qit, KmClone, 0);
    for (i32 i = 0; i < qit.count; i++) {
      const KmClone *c = &cs[i];
      if (c->run != waves->run || c->slot >= KM_SQUAD_MAX || c->dead)
        continue;
      view->alive[c->slot] = true;
      view->feet[c->slot] = (v3){c->position.x, c->position.y - KM_WALKER_HEIGHT, c->position.z};
      count++;
    }
  }
  view->count = count;
  if (!waves->live)
    return;
  // a new run: the squad stands by the gunship on Aurek
  if (sq->run != waves->run) {
    sq->run = waves->run;
    sq->cleared_seen = waves->cleared_serial;
    for (u32 s = 0; s < KM_SQUAD_SIZE; s++)
      km_clone_spawn(it->world, sq, s, waves->run, (v3){-3.0f + (f32)s * 2.0f, 0.0f, 70.0f - (f32)(s % 2) * 2.0f});
    log_info("squad: % troopers deployed", fmt_u32((u32)KM_SQUAD_SIZE));
    return;
  }
  // a secured sector: fresh troopers fill the empty places, coming up behind the player
  if (sq->cleared_seen != waves->cleared_serial) {
    sq->cleared_seen = waves->cleared_serial;
    u32 sent = 0;
    for (u32 s = 0; s < KM_SQUAD_SIZE; s++) {
      if (view->alive[s])
        continue;
      v3 at = {player->feet.x + km_squad_places[s].x * 0.5f, player->feet.y, player->feet.z + 6.0f + (f32)s};
      km_clone_spawn(it->world, sq, s, waves->run, at);
      sent++;
    }
    if (sent > 0)
      log_info("squad: % reinforcements", fmt_u32(sent));
  }
}

// the squad's tracers in the player's rifle colour, and the impacts that hit the level
ecs_system(km_squad_fx_system, EcsResIn(KmSquadShots), EcsResIn(VfxRequests), EcsResIn(GrFxAssets), EcsResIn(GrClock),
           EcsResInOut(GrFxFrame)) {
  const KmSquadShots *shots = ecs_sys_get_resource(KmSquadShots);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  if (!as->ready || shots->frame != ecs_sys_get_resource(GrClock)->frame)
    return;
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  GrFxFrame *f = ecs_sys_get_resource_mut(GrFxFrame);
  for (u32 i = 0; i < shots->count; i++) {
    const KmSquadShot *s = &shots->items[i];
    gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_TRACER, .variant = GR_WEAPON_RIFLE, .p = s->from, .n = s->to});
    if (s->world)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_WORLD, .p = s->to, .n = s->normal, .d = v3_normalize(v3_sub(s->to, s->from))});
  }
}

// the squad's blaster fire, one voice for every trooper (an emitter takes GR_SFX_EMIT_MAX a frame)
typedef struct KmSquadVoice {
  u8 _;
} KmSquadVoice;
ECS_COMPONENT_TYPEDEF(KmSquadVoice);

ecs_system(km_squad_voice_system, EcsWith(KmSquadVoice), EcsInOut(GrSfxEmitter), EcsResIn(KmSquadShots), EcsResIn(GrClock)) {
  const KmSquadShots *shots = ecs_sys_get_resource(KmSquadShots);
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  if (shots->frame != frame)
    return;
  for (i32 i = 0; i < it->count; i++)
    for (u32 s = 0; s < shots->count; s++)
      gr_sfx_emit(ecs_sys_get_component_mut(GrSfxEmitter, i), frame,
                  (GrSfxPlay){.sound = GR_SOUND_RIFLE, .positional = true, .position = shots->items[s].from, .volume = 0.55f,
                              .rate = 1.08f, .jitter = 0.06f, .reverb = 0.2f});
}

void km_squad_init(EcsWorld *w) {
  // resources and cached queries are main-lane app_init state
  assert(is_main_thread());
  ecs_query_cached_init(w, &ecs_resource_get_mut(w, KmSquad)->troopers, EcsIn(KmClone));
  EcsEntity voice = ecs_entity_create(w);
  ecs_component_set(w, voice, KmSquadVoice, {0});
  ecs_component_set(w, voice, GrSfxEmitter, {0});
}

void km_squad_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_squad_system);         // writes the droids' GrPendingDamage and the shot list
  ecs_schedule_single(w, km_squad_publish_system); // writes KmSquad; raises the squad and its reinforcements
  ecs_schedule(w, km_squad_voice_system);
}

void km_squad_schedule_fx(EcsWorld *w) { ecs_schedule_single(w, km_squad_fx_system); } // writes GrFxFrame
