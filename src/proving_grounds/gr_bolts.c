// Proving Grounds enemy bolts: a fixed pool of entities; fire requests are claimed by free rows and fly the frame they fire.

#define GR_BOLT_TRAIL_INTERVAL 0.012f // seconds between trail glows
#define GR_BOLT_SPIN 4.0f             // rad/s
#define GR_BOLT_SAMPLES 4             // capsule test samples per step
#define GR_SPLASH_FACTOR 0.8f         // splash damage x bolt damage at the centre
#define GR_SPLASH_SHAKE 0.2f          // camera shake from a splash

// the first `fraction` of this step sampled against an upright capsule standing on `feet`
hz_internal b32 gr_bolt_hits_capsule(v3 pos, v3 step, f32 fraction, v3 feet, f32 body_radius, f32 height, f32 radius, v3 *closest) {
  v3 start = v3_add(feet, (v3){0, body_radius, 0});
  v3 end = v3_add(feet, (v3){0, height - body_radius, 0});
  f32 reach = radius + body_radius;
  for (u32 i = 1; i <= GR_BOLT_SAMPLES; i++) {
    v3 q = v3_add(pos, v3_scale(step, ((f32)i / (f32)GR_BOLT_SAMPLES) * fraction));
    f32 t = m_clampf((q.y - start.y) / m_maxf(end.y - start.y, 1e-3f), 0.0f, 1.0f);
    v3 axis = v3_lerp(start, end, t);
    if (v3_distancesq(q, axis) < reach * reach) {
      *closest = q;
      return true;
    }
  }
  return false;
}

// a bolt spawned on a free row
hz_internal void gr_bolt_fire(GrBolt *b, const GrBoltRequest *r, const GrDroneArchetype *a, u32 frame) {
  *b = (GrBolt){.type = r->type,
                .state = GR_BOLT_FLYING,
                .run = r->run,
                .spawn_frame = frame,
                .position = r->origin,
                .velocity = v3_scale(r->direction, a->bolt_speed),
                .radius = a->bolt_radius,
                .damage = a->bolt_damage,
                .splash = a->bolt_splash};
}

// this frame's fire events in drone row order, so the pool serves the same requests on a replay
ecs_system(gr_bolt_request_system, EcsIn(GrDrone), EcsResInOut(GrBoltRequests), EcsResIn(GrClock), EcsResIn(GrWaves)) {
  GrBoltRequests *q = ecs_sys_get_resource_mut(GrBoltRequests);
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  if (q->frame != frame) {
    q->frame = frame;
    q->count = 0;
    q->claimed = 0;
  }
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->events_frame != frame || !(d->events & GR_DRONE_EV_FIRED) || d->run != waves->run || q->count >= GR_MAX_DRONES)
      continue;
    q->items[q->count++] =
        (GrBoltRequest){.type = d->type, .run = waves->bolt_run, .origin = d->eye, .direction = d->fire_dir};
  }
}

// one pooled bolt's frame, every lane: a free row claims a request (atomic cursor, the only shared write), then flies
ecs_system(gr_bolt_update_system, EcsInOut(GrBolt), EcsOut(WorldTransform), EcsResInOut(GrBoltRequests),
           EcsResIn(GrWaves), EcsResIn(GrDroneRules), EcsResIn(GrPlayerState), EcsResIn(EcsPhysicsWorld), EcsResIn(GrDroneField)) {
  const KmSquadView *squad = &ecs_sys_get_resource(GrDroneField)->squad;
  GrBoltRequests *q = ecs_sys_get_resource_mut(GrBoltRequests);
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  f32 dt = waves->dt;
  b32 step = waves->live && dt > 0.0f;
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  b32 requests = step && q->frame == waves->frame && q->count > 0;
  for (i32 i = 0; i < it->count; i++) {
    const GrBolt *cur = ecs_sys_get_component(GrBolt, i);
    b32 flying = cur->state == GR_BOLT_FLYING && cur->run == waves->bolt_run;
    if (!flying) {
      // terminal and cleared rows lived their frame (cleared ones go even while paused); free rows take the next request
      u32 slot = requests ? atomic_u32_inc_eval(&q->claimed) - 1 : q->count;
      if (slot < q->count) {
        const GrBoltRequest *r = &q->items[slot];
        gr_bolt_fire(ecs_sys_get_component_mut(GrBolt, i), r, &rules->drones.types.items[r->type], waves->frame);
      } else {
        if (cur->state != GR_BOLT_FREE)
          ecs_sys_get_component_mut(GrBolt, i)->state = GR_BOLT_FREE;
        continue;
      }
    }
    if (!step)
      continue;

    GrBolt *b = ecs_sys_get_component_mut(GrBolt, i);
    b->trail = false;
    b->age += dt;
    if (b->age > rules->drones.bolt_max_age) {
      b->state = GR_BOLT_EXPIRED;
      b->end_frame = waves->frame;
      continue;
    }
    v3 step = v3_scale(b->velocity, dt);
    f32 step_len = v3_length(step);
    if (step_len < 1e-6f)
      continue;
    v3 dir = v3_scale(step, 1.0f / step_len);

    // walls first: only the part of the step in front of a wall can reach the player
    GrRayHit hit;
    b32 wall = gr_level_raycast(layer, b->position, dir, step_len + b->radius, &hit);
    f32 open = wall ? m_minf(1.0f, hit.distance / step_len) : 1.0f;

    v3 closest;
    if (player->alive &&
        gr_bolt_hits_capsule(b->position, step, open, player->feet, GR_PLAYER_RADIUS, GR_PLAYER_HEIGHT, b->radius, &closest)) {
      b->state = GR_BOLT_HIT_PLAYER;
      b->end_frame = waves->frame;
      b->hit_point = closest;
      b->hit_normal = v3_negate(dir);
      b->player_damage = b->damage;
      b->damage_from = v3_sub(b->position, step);
      continue;
    }
    // a trooper in the way takes it; it shows as a wall impact
    b32 trooper = false;
    for (u32 s = 0; s < KM_SQUAD_MAX && !trooper; s++) {
      if (squad->alive[s] && gr_bolt_hits_capsule(b->position, step, open, squad->feet[s], 0.35f, 1.8f, b->radius, &closest)) {
        b->state = GR_BOLT_HIT_WALL;
        b->end_frame = waves->frame;
        b->hit_point = closest;
        b->hit_normal = v3_negate(dir);
        b->clone_hit = s + 1;
        trooper = true;
      }
    }
    if (trooper)
      continue;
    if (wall) {
      b->state = GR_BOLT_HIT_WALL;
      b->end_frame = waves->frame;
      b->hit_point = hit.point;
      b->hit_normal = hit.normal;
      // splash: falloff to the capsule centre; the player's intake ignores it when dead
      if (b->splash > 0.0f) {
        v3 center = v3_add(player->feet, (v3){0, GR_PLAYER_HEIGHT * 0.5f, 0});
        f32 d = v3_distance(center, hit.point);
        if (d < b->splash) {
          b->player_damage = b->damage * (1.0f - d / b->splash) * GR_SPLASH_FACTOR;
          b->damage_from = hit.point;
        }
      }
      continue;
    }

    b->position = v3_add(b->position, step);
    b->spin += dt * GR_BOLT_SPIN;
    b->trail_timer -= dt;
    if (b->trail_timer <= 0.0f) {
      b->trail_timer = GR_BOLT_TRAIL_INTERVAL;
      b->trail = true;
    }
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){
        .position = b->position, .rotation = quat_identity(), .scale = V3_ONE};
  }
}

// this frame's bolt damage to the player as one ordered list, splash shake, the live and dropped counts
ecs_system(gr_bolt_consolidate_system, EcsIn(GrBolt), EcsResInOut(GrBoltHits), EcsResInOut(GrBoltRequests),
           EcsResIn(GrClock)) {
  GrBoltHits *h = ecs_sys_get_resource_mut(GrBoltHits);
  GrBoltRequests *q = ecs_sys_get_resource_mut(GrBoltRequests);
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  if (h->frame != frame) {
    h->frame = frame;
    h->count = 0;
    h->shake = 0.0f;
    for (u32 s = 0; s < KM_SQUAD_MAX; s++)
      h->clone_damage[s] = 0.0f;
    q->live = 0;
    if (q->frame == frame && q->claimed < q->count) {
      q->dropped += q->count - q->claimed;
      log_warn("bolts: pool full, % dropped (run total %)", fmt_u32(q->count - q->claimed), fmt_u32(q->dropped));
    }
  }
  for (i32 i = 0; i < it->count; i++) {
    const GrBolt *b = ecs_sys_get_component(GrBolt, i);
    if (b->state == GR_BOLT_FLYING) {
      q->live++;
      continue;
    }
    if (b->end_frame != frame || b->state == GR_BOLT_FREE)
      continue;
    if (b->clone_hit > 0 && b->clone_hit <= KM_SQUAD_MAX)
      h->clone_damage[b->clone_hit - 1] += b->damage;
    if (b->player_damage > 0.0f && h->count < GR_MAX_PLAYER_HITS) {
      h->hits[h->count++] = (GrPlayerHit){.amount = b->player_damage, .from = b->damage_from};
      h->total++;
      log_info("bolts: player hit for % (%)", fmt_f32_p(b->player_damage, 1),
               fmt_cstr(b->state == GR_BOLT_HIT_PLAYER ? "direct" : "splash"));
    }
    if (b->state == GR_BOLT_HIT_WALL && b->splash > 0.0f)
      h->shake += GR_SPLASH_SHAKE;
  }
}

// the pool: `bolt_capacity` hidden rows, made once the config is known
ecs_system(gr_bolt_pool_system, EcsResInOut(GrBoltRequests), EcsResIn(GrDroneRules)) {
  GrBoltRequests *q = ecs_sys_get_resource_mut(GrBoltRequests);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  if (q->pool > 0 || !rules->ready)
    return;
  for (u32 i = 0; i < rules->drones.bolt_capacity; i++) {
    EcsEntity e = ecs_entity_create(it->world);
    ecs_component_set(it->world, e, GrBolt, {.state = GR_BOLT_FREE});
    ecs_component_set(it->world, e, WorldTransform, {.rotation = quat_identity(), .scale = V3_ONE});
  }
  q->pool = rules->drones.bolt_capacity;
  log_info("bolts: pool of %", fmt_u32(q->pool));
}
