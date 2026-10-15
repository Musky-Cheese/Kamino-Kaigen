// Drones: spawn, sight, routing, steering, swept motion, attacks, damage, consolidation.

#define GR_WARP_TIME 0.55f      // warp-in pop duration, s
#define GR_ORBIT_LEAD 0.5f      // seconds of orbit travel the orbit target leads by
#define GR_PORTAL_LEAD 2.5f     // line-up distance before and past a doorway, m
#define GR_WALL_MARGIN 1.0f     // clearance kept from room walls, m
#define GR_ATTACK_RANGE 45.0f   // no attack beyond this distance, m
#define GR_RESOLVE_ITERS 4      // deepest-first pushes per substep, so every overlapping body is resolved in turn
#define GR_SEPARATION 2.5f      // minimum gap between drones, as a multiple of their summed radii
#define GR_DRONE_SPEED_CAP 1.6f // velocity cap as a multiple of the type's max speed
#define GR_SMOKE_HEALTH 0.35f   // health fraction below which a drone smokes
#define GR_FIRE_RECOIL 1.8f     // backwards kick on firing, m/s
#define GR_AIM_DROP 0.25f       // aim this far below the player's eye, m
#define GR_LEAD_FACTOR 0.55f    // fraction of the bolt flight time the aim leads the player by

hz_internal b32 gr_run_live(u32 state) { return state == GR_STATE_PLAYING || state == GR_STATE_ENDING; }

hz_internal String gr_str32(const String32Bytes *s) { return (String){.value = (char *)s->value, .len = s->len}; }

hz_internal String gr_label(const GrDroneArchetype *a) { return gr_str32(&a->label); }

// the shortest signed angle difference, radians
hz_internal f32 gr_angle_delta(f32 from, f32 to) {
  f32 d = m_fmodf(to - from, M_PI2);
  if (d > M_PI)
    d -= M_PI2;
  if (d < -M_PI)
    d += M_PI2;
  return d;
}

hz_internal f32 gr_rand_range(Random *r, v2 range) { return random_f32_range(r, range.x, range.y); }

hz_internal f32 gr_rand_sign(Random *r) { return random_f32(r) < 0.5f ? -1.0f : 1.0f; }

hz_internal v3 gr_v3_cap(v3 v, f32 max_len) {
  f32 len = v3_length(v);
  return len > max_len ? v3_scale(v, max_len / len) : v;
}

// a direction inside the cone of half-angle `angle` around unit `dir`
hz_internal v3 gr_random_in_cone(Random *r, v3 dir, f32 angle) {
  f32 cos_a = m_cosf(angle);
  f32 z = cos_a + (1.0f - cos_a) * random_f32(r);
  f32 phi = random_f32(r) * M_PI2;
  f32 s = m_sqrtf(m_maxf(0.0f, 1.0f - z * z));
  v3 tangent = m_fabsf(dir.y) < 0.99f ? v3_normalize(v3_cross((v3){0, 1, 0}, dir))
                                      : v3_normalize(v3_cross((v3){1, 0, 0}, dir));
  v3 bitangent = v3_cross(dir, tangent);
  return v3_add(v3_add(v3_scale(dir, z), v3_scale(tangent, s * m_cosf(phi))), v3_scale(bitangent, s * m_sinf(phi)));
}

hz_internal void gr_drone_event(GrDrone *d, u32 frame, u32 ev) {
  if (d->events_frame != frame) {
    d->events = 0;
    d->events_frame = frame;
  }
  d->events |= ev;
}

// model root (YXZ pitch, yaw, bank) times the body's bob and roll
hz_internal quat gr_drone_root_rotation(const GrDrone *d) { return quat_from_euler_yxz((v3){d->pitch, d->yaw, d->bank}); }

hz_internal f32 gr_drone_bob(const GrDrone *d) { return km_droid_walks(d->type) ? 0.0f : m_sinf(d->age * 2.3f + d->phase) * 0.06f; }

hz_internal f32 gr_drone_roll(const GrDrone *d) { return km_droid_walks(d->type) ? 0.0f : m_sinf(d->age * 1.7f + d->phase) * 0.05f; }

// a point given relative to the type's root node, in world space
hz_internal v3 gr_drone_body_point(const GrDrone *d, v3 local) {
  quat roll = quat_axis_angle((v3){0, 0, 1}, gr_drone_roll(d));
  v3 body = v3_add((v3){0, gr_drone_bob(d), 0}, quat_rotate(roll, local));
  return v3_add(d->position, quat_rotate(gr_drone_root_rotation(d), v3_scale(body, d->scale)));
}

// the model entity's pose: the root's placement times the body's bob and roll
hz_internal WorldTransform gr_drone_pose(const GrDrone *d) {
  quat roll = quat_axis_angle((v3){0, 0, 1}, gr_drone_roll(d));
  return (WorldTransform){
      .position = gr_drone_body_point(d, V3_ZERO),
      .rotation = quat_mul(gr_drone_root_rotation(d), roll),
      .scale = v3_fill(d->scale),
  };
}

hz_internal GrDrone gr_drone_make(const GrDroneArchetype *a, u32 type, u32 run, v3 position, u64 seed) {
  GrDrone d = {
      .type = type,
      .state = GR_DRONE_WARPING,
      .run = run,
      .position = position,
      .health = a->health,
      .line_of_sight = true,
      .scale = 0.001f,
      .eye = position,
      .rng = random_create(seed),
  };
  d.orbit_dir = gr_rand_sign(&d.rng);
  d.orbit_speed = random_f32_range(&d.rng, 0.25f, 0.45f);
  d.retarget_timer = random_f32_range(&d.rng, 2.0f, 4.0f);
  d.phase = random_f32(&d.rng) * 100.0f;
  d.preferred_distance = gr_rand_range(&d.rng, a->distance);
  d.preferred_altitude = gr_rand_range(&d.rng, a->altitude);
  d.attack_timer = random_f32_range(&d.rng, 0.8f, 1.6f) + a->telegraph;
  if (km_droid_walks(type))
    d.scale = 1.0f;
  return d;
}

// spawn: the type's model (its EcsModelNode sockets come with it), the drone state and an empty damage slot
hz_internal void gr_drone_spawn(EcsWorld *w, const GrDroneRules *rules, const GrDroneAssets *assets, u32 type, u32 run,
                                v3 position, u64 seed) {
  const GrDroneArchetype *a = &rules->drones.types.items[type];
  GrDrone d = gr_drone_make(a, type, run, position, seed);
  d.muzzle = assets->types[type].muzzle;
  WorldTransform pose = gr_drone_pose(&d);
  EcsEntity e = ecs_renderer_spawn(w, NULL,
                                   &(EcsRendererSpawnDesc){
                                       .model_asset = assets->types[type].model,
                                       .transform = {.position = pose.position, .rotation = pose.rotation, .scale = pose.scale},
                                       .casts_shadow = true,
                                       .name = (CString)a->model_node.value,
                                   });
  ecs_component_set(w, e, GrDrone, d);
  ecs_component_set(w, e, GrPendingDamage, {0});
}

// the static-layer sphere query, deepest body first, until clear; the pushes sum
hz_internal b32 gr_drone_resolve(const EcsCollisionLayer *static_layer, v3 *pos, f32 radius, v3 *normal) {
  if (!static_layer)
    return false;
  CollisionLayer *layer = (CollisionLayer *)&static_layer->layer; // read-only query on a non-const API
  Collider sphere = collider_sphere(V3_ZERO, radius);
  v3 start = *pos;
  for (u32 i = 0; i < GR_RESOLVE_ITERS; i++) {
    ColliderDistanceResult r;
    i32 body = -1;
    if (!physics_distance_between_layer(sphere, *pos, quat_identity(), 1.0f, layer, 0.0f, PHYSICS_QUERY_FILTER_ALL, &r,
                                        &body))
      break;
    if (r.distance >= 0.0f)
      break;
    // push along the closest points (the sphere's normal_a); a box's normal_b is its 45-degree edge normal
    *pos = v3_add(*pos, v3_scale(r.normal_a, r.distance));
  }
  v3 push = v3_sub(*pos, start);
  f32 len = v3_length(push);
  if (len < 1e-6f)
    return false;
  *normal = v3_scale(push, 1.0f / len);
  return true;
}

// orbit target: a point a fixed angle ahead on the orbit round the eye, at the preferred distance and height
hz_internal v3 gr_drone_orbit(const GrDrone *d, v3 eye) {
  f32 boost = d->line_of_sight ? 1.0f : 2.2f;
  f32 angle = m_atan2f(d->position.z - eye.z, d->position.x - eye.x) + d->orbit_speed * d->orbit_dir * boost * GR_ORBIT_LEAD;
  f32 altitude = d->preferred_altitude + m_sinf(d->age * 0.7f + d->phase) * 0.8f + (d->line_of_sight ? 0.0f : 2.0f);
  return (v3){eye.x + m_cosf(angle) * d->preferred_distance, altitude, eye.z + m_sinf(angle) * d->preferred_distance};
}

// clamp inside the room clear of its walls (its middle when narrower), between floor + 2 m and the ceiling
hz_internal v3 gr_drone_clamp_to_room(v3 desired, const GrNavZone *room, f32 radius, b32 passing, b32 walker) {
  f32 m = radius + GR_WALL_MARGIN;
  aabb b = room->box;
  if (!passing) {
    desired.x = b.min.x + m <= b.max.x - m ? m_clampf(desired.x, b.min.x + m, b.max.x - m) : (b.min.x + b.max.x) * 0.5f;
    desired.z = b.min.z + m <= b.max.z - m ? m_clampf(desired.z, b.min.z + m, b.max.z - m) : (b.min.z + b.max.z) * 0.5f;
  }
  desired.y = walker ? room->box.min.y + KM_WALKER_HEIGHT : m_clampf(desired.y, m_minf(2.0f, room->ceiling), room->ceiling);
  return desired;
}

typedef struct GrDroneCtx {
  const GrDroneArchetype *a;
  const GrDroneRules *rules;
  const GrPlayerState *player;
  const GrDroneField *field;
  const GrLevel *lvl;
  const EcsCollisionLayer *layer;
  EcsEntity self;
  u32 player_zone;
  u32 frame;
  u32 hold_zone;    // the sector the droids fight from (GrWaves.hold_zone)
  u32 retreat_zone; // where the survivors fall back to
  v3 retreat_point;
  const KmSquadView *squad; // the clone troopers they also shoot at
} GrDroneCtx;

// movement: retarget, route or orbit, room clamp, arrive steering, separation, swept motion
hz_internal void gr_drone_move(GrDrone *d, const GrDroneCtx *c, v3 to_player, f32 dt, v3 *last_velocity) {
  const GrDroneArchetype *a = c->a;
  v3 eye = c->player->eye;

  d->retarget_timer -= dt;
  if (d->retarget_timer <= 0.0f) {
    d->retarget_timer = random_f32_range(&d->rng, 2.5f, 5.0f);
    d->preferred_distance = gr_rand_range(&d->rng, a->distance);
    d->preferred_altitude = gr_rand_range(&d->rng, a->altitude);
    if (random_f32(&d->rng) < 0.35f)
      d->orbit_dir = -d->orbit_dir;
    // scouts dash sideways now and then
    if (d->type == GR_DRONE_SCOUT && random_f32(&d->rng) < (km_droid_walks(d->type) ? 0.25f : 0.6f)) {
      v3 side = v3_normalize((v3){-to_player.z, 0.0f, to_player.x});
      f32 sign = gr_rand_sign(&d->rng);
      f32 dash = km_droid_walks(d->type) ? random_f32_range(&d->rng, 2.0f, 3.0f) : random_f32_range(&d->rng, 6.0f, 9.0f);
      d->velocity = v3_add(d->velocity, v3_scale(side, sign * dash));
    }
  }

  u32 zone = gr_level_zone_at(c->lvl, d->position);
  const GrNavZone *room = &c->lvl->zones[zone];
  // walkers hold their sector (or fall back from it); fliers hunt the player anywhere
  u32 goal_zone = c->player_zone;
  if (km_droid_walks(d->type))
    goal_zone = d->retreating ? c->retreat_zone : (c->player_zone == c->hold_zone ? c->player_zone : c->hold_zone);
  GrNextPortal route = gr_level_next_portal(c->lvl, zone, goal_zone);
  b32 routed = route.portal >= 0;
  v3 desired;
  if (routed) {
    // the rooms are convex, so the straight line to a doorway is clear: line up behind it, then go through
    const GrPortal *portal = &c->lvl->portals[route.portal];
    v3 n = v3_scale(portal->normal, (f32)route.dir);
    v3 rel = v3_sub(d->position, portal->center);
    f32 along = v3_dot(rel, n);
    f32 aside = v3_length(v3_sub(rel, v3_scale(n, along)));
    f32 target = along < -0.5f && aside > 0.8f ? -GR_PORTAL_LEAD : GR_PORTAL_LEAD;
    desired = v3_add(portal->center, v3_scale(n, target));
  } else if (d->retreating) {
    desired = v3_add(c->retreat_point, (v3){m_sinf(d->phase) * 6.0f, 0.0f, m_cosf(d->phase) * 6.0f});
  } else {
    desired = gr_drone_orbit(d, eye);
  }
  b32 walker = km_droid_walks(d->type);
  desired = gr_drone_clamp_to_room(desired, room, a->radius, routed, walker);

  v3 steer = gr_v3_cap(v3_scale(v3_sub(desired, d->position), 1.1f), a->max_speed);

  // separation from the other drones as the last frame left them
  for (u32 i = 0; i < c->field->count; i++) {
    const GrDroneSlot *o = &c->field->drones[i];
    if (o->entity == c->self)
      continue;
    v3 away = v3_sub(d->position, o->position);
    f32 d2 = v3_lengthsq(away);
    f32 min = (a->radius + o->radius) * GR_SEPARATION;
    if (d2 < min * min && d2 > 1e-4f)
      steer = v3_add(steer, v3_scale(away, (min * min - d2) / (d2 * 1.5f)));
  }

  *last_velocity = d->velocity;
  v3 change = gr_v3_cap(v3_sub(steer, d->velocity), a->acceleration * dt);
  d->velocity = gr_v3_cap(v3_add(d->velocity, change), a->max_speed * GR_DRONE_SPEED_CAP);
  if (walker)
    d->velocity.y = 0.0f;

  // steps of at most half the radius, each pushed back out of the level, so no step can pass through a wall
  f32 travel = v3_length(d->velocity) * dt;
  u32 steps = (u32)m_maxf(1.0f, m_ceilf(travel / (a->radius * 0.5f)));
  f32 sdt = dt / (f32)steps;
  for (u32 s = 0; s < steps; s++) {
    d->position = v3_add(d->position, v3_scale(d->velocity, sdt));
    v3 n;
    if (gr_drone_resolve(c->layer, &d->position, a->radius, &n)) {
      f32 into = v3_dot(d->velocity, n);
      if (into < 0.0f)
        d->velocity = v3_add(d->velocity, v3_scale(n, -into * 1.5f));
    }
  }
  // walkers keep their hips above the deck under them
  if (walker) {
    d->velocity.y = 0.0f;
    d->position.y = gr_level_ground_height(c->lvl, v3_add(d->position, (v3){0.0f, 0.5f, 0.0f})) + KM_WALKER_HEIGHT;
  }
}

// fire from the eye socket, leading the player, a little below the eye, inside the spread cone; recoil
hz_internal void gr_drone_fire(GrDrone *d, const GrDroneCtx *c, f32 distance) {
  const GrDroneArchetype *a = c->a;
  f32 lead = distance / a->bolt_speed * GR_LEAD_FACTOR;
  v3 target = v3_add(c->player->eye, v3_scale(c->player->velocity, lead));
  target.y -= GR_AIM_DROP;
  // half the time a nearer trooper draws the fire
  if (c->squad && random_f32(&d->rng) < 0.5f) {
    f32 best = m_minf(distance, 30.0f);
    for (u32 s = 0; s < KM_SQUAD_MAX; s++) {
      if (!c->squad->alive[s])
        continue;
      v3 chest = v3_add(c->squad->feet[s], (v3){0.0f, 1.25f, 0.0f});
      f32 dd = v3_distance(d->eye, chest);
      if (dd < best) {
        best = dd;
        target = chest;
      }
    }
  }
  v3 aim = v3_normalize(v3_sub(target, d->eye));
  v3 dir = gr_random_in_cone(&d->rng, aim, a->bolt_spread);
  d->fire_dir = dir;
  gr_drone_event(d, c->frame, GR_DRONE_EV_FIRED);
  if (!km_droid_walks(d->type))
    d->velocity = v3_add(d->velocity, v3_scale(dir, -GR_FIRE_RECOIL));
}

// attack: burst, telegraph, then cooldown; the token itself is granted by gr_drone_token_system
hz_internal void gr_drone_attack(GrDrone *d, const GrDroneCtx *c, f32 distance, f32 dt) {
  if (d->state != GR_DRONE_ACTIVE || !c->player->alive)
    return;
  // falling back: no new attacks, the token goes back
  if (d->retreating) {
    d->telegraph_timer = 0.0f;
    d->charge = 0.0f;
    d->burst_left = 0;
    if (d->holds_token)
      d->release_token = true;
    return;
  }
  const GrDroneArchetype *a = c->a;
  if (d->burst_left > 0) {
    d->burst_timer -= dt;
    if (d->burst_timer <= 0.0f) {
      gr_drone_fire(d, c, distance);
      d->burst_left--;
      d->burst_timer = a->burst_interval;
      if (d->burst_left == 0) {
        d->release_token = true;
        d->attack_timer = gr_rand_range(&d->rng, a->cooldown);
      }
    }
    return;
  }
  if (d->telegraph_timer > 0.0f) {
    d->telegraph_timer -= dt;
    d->charge = 1.0f - d->telegraph_timer / a->telegraph;
    if (d->telegraph_timer <= 0.0f) {
      d->charge = 0.0f;
      d->burst_left = a->burst;
      d->burst_timer = 0.0f;
    }
    return;
  }
  d->attack_timer -= dt;
  if (d->attack_timer <= 0.0f && d->line_of_sight && distance < GR_ATTACK_RANGE)
    d->wants_token = true;
}

// visual: face the player, bank into lateral acceleration, hit flash, thrust
hz_internal void gr_drone_present(GrDrone *d, const GrDroneArchetype *a, v3 muzzle, v3 to_player, v3 last_velocity, f32 dt) {
  f32 target_yaw = m_atan2f(to_player.x, to_player.z);
  f32 target_pitch = -m_atan2f(to_player.y, m_sqrtf(to_player.x * to_player.x + to_player.z * to_player.z));
  d->yaw += gr_angle_delta(d->yaw, target_yaw) * m_minf(1.0f, dt * 6.0f);
  if (km_droid_walks(d->type)) {
    // walkers stay upright: the torso leans onto the aim, the legs stride with the ground speed
    d->aim_pitch = m_damp(d->aim_pitch, m_clampf(-target_pitch, -0.6f, 0.6f), 6.0f, dt);
    f32 speed = m_sqrtf(d->velocity.x * d->velocity.x + d->velocity.z * d->velocity.z);
    d->gait_amount = m_damp(d->gait_amount, m_minf(1.0f, speed / 2.2f), 8.0f, dt);
    d->gait += dt * speed / 1.44f * 2.0f * M_PI;
    d->pitch = 0.0f;
    d->bank = 0.0f;
    d->hit_flash = m_maxf(0.0f, d->hit_flash - dt * 9.0f);
    d->thrust = 0.0f;
    d->eye = gr_drone_body_point(d, muzzle);
    return;
  }
  d->pitch = m_damp(d->pitch, m_clampf(target_pitch, -0.9f, 0.9f), 6.0f, dt);
  v3 accel = v3_scale(v3_sub(d->velocity, last_velocity), 1.0f / m_maxf(dt, 1e-4f));
  f32 lateral = accel.x * m_cosf(d->yaw) - accel.z * m_sinf(d->yaw);
  d->bank = m_damp(d->bank, m_clampf(-lateral * 0.03f, -0.5f, 0.5f), 5.0f, dt);
  d->hit_flash = m_maxf(0.0f, d->hit_flash - dt * 9.0f);
  d->thrust = m_minf(1.0f, v3_length(d->velocity) / a->max_speed);
  d->eye = gr_drone_body_point(d, muzzle);
}

// automation (GrDroneDebug.damage_per_second): a steady hit from the eye on every active drone, the weapon's path
ecs_system(gr_drone_debug_damage_system, EcsIn(GrDrone), EcsInOut(GrPendingDamage), EcsResIn(GrDroneRules),
           EcsResIn(GrWaves), EcsResIn(GrPlayerState)) {
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  f32 amount = rules->debug.damage_per_second * waves->dt;
  if (amount <= 0.0f || !waves->live)
    return;
  v3 eye = ecs_sys_get_resource(GrPlayerState)->eye;
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->state != GR_DRONE_ACTIVE || d->run != waves->run)
      continue;
    GrPendingDamage *pd = ecs_sys_get_component_mut(GrPendingDamage, i);
    pd->amount += amount;
    pd->direction = v3_normalize(v3_sub(d->position, eye));
    pd->hits++;
  }
}

// weapon hits: the weapon's pending damage lands on active drones, then clears
ecs_system(gr_drone_damage_system, EcsInOut(GrDrone), EcsInOut(GrPendingDamage), EcsResIn(GrClock)) {
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  for (i32 i = 0; i < it->count; i++) {
    const GrPendingDamage *pd = ecs_sys_get_component(GrPendingDamage, i);
    if (pd->hits == 0)
      continue;
    GrDrone *d = ecs_sys_get_component_mut(GrDrone, i);
    if (d->state == GR_DRONE_ACTIVE) {
      d->health -= pd->amount;
      d->hit_flash = 1.0f;
      d->velocity = v3_add(d->velocity, pd->impulse);
      d->hit_dir = pd->direction;
      gr_drone_event(d, clock->frame, GR_DRONE_EV_HIT);
      if (d->health <= 0.0f) {
        d->state = GR_DRONE_DEAD;
        gr_drone_event(d, clock->frame, GR_DRONE_EV_DIED);
      }
    }
    *ecs_sys_get_component_mut(GrPendingDamage, i) = (GrPendingDamage){0};
  }
}

// one drone's frame (update, pin and nav audit), every lane; writes only its own rows
ecs_system(gr_drone_update_system, EcsInOut(GrDrone), EcsOut(WorldTransform), EcsResIn(GrWaves), EcsResIn(GrDroneRules),
           EcsResIn(GrPlayerState), EcsResIn(GrDroneField), EcsResIn(GrLevel), EcsResIn(EcsPhysicsWorld)) {
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  f32 dt = waves->dt;
  if (!waves->live || dt <= 0.0f || !ecs_sys_get_resource(GrLevel)->tables_ready)
    return;
  GrDroneCtx c = {
      .rules = rules,
      .player = ecs_sys_get_resource(GrPlayerState),
      .field = ecs_sys_get_resource(GrDroneField),
      .lvl = ecs_sys_get_resource(GrLevel),
      .layer = gr_level_collision(it->world),
      .frame = waves->frame,
  };
  c.player_zone = gr_level_zone_at(c.lvl, c.player->eye);
  c.hold_zone = waves->hold_zone;
  c.retreat_zone = waves->retreat_zone;
  c.retreat_point = waves->retreat_point;
  c.squad = &c.field->squad;
  b32 audit = rules->debug.nav_audit;

  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *cur = ecs_sys_get_component(GrDrone, i);
    if (cur->state == GR_DRONE_DEAD || cur->run != waves->run)
      continue;
    GrDrone *d = ecs_sys_get_component_mut(GrDrone, i);
    c.a = &rules->drones.types.items[d->type];
    c.self = it->entities[i];
    v3 before = d->position;

    // the spawn's rows land when the frame's scope ends, so the first update is the first frame anyone sees it
    if (d->age == 0.0f)
      gr_drone_event(d, c.frame, GR_DRONE_EV_SPAWNED);
    d->age += dt;
    if (d->state == GR_DRONE_WARPING) {
      // elastic pop-in
      f32 t = m_minf(1.0f, d->age / GR_WARP_TIME);
      f32 s = 1.0f + m_sinf(t * M_PI * 1.5f) * (1.0f - t) * 0.6f;
      d->scale = km_droid_walks(d->type) ? 1.0f : m_maxf(0.001f, t * s);
      if (t >= 1.0f) {
        d->state = GR_DRONE_ACTIVE;
        d->scale = 1.0f;
      }
    }

    // the front broke: the walkers on it fall back to the next sector, then hold there
    if (d->retreat_seen != waves->retreat_serial) {
      d->retreat_seen = waves->retreat_serial;
      d->retreating = km_droid_walks(d->type) && waves->phase == GR_WAVES_SECURE;
    }
    if (d->retreating && gr_level_zone_at(c.lvl, d->position) == c.retreat_zone)
      d->retreating = false;

    v3 to_player = v3_sub(c.player->eye, d->position);
    f32 distance = v3_length(to_player);

    // periodic line of sight on the static layer
    d->los_timer -= dt;
    if (d->los_timer <= 0.0f) {
      d->los_timer = 0.2f + random_f32(&d->rng) * 0.1f;
      v3 dir = v3_scale(to_player, 1.0f / m_maxf(distance, 1e-3f));
      GrRayHit hit;
      d->line_of_sight = !gr_level_raycast(c.layer, d->position, dir, distance - 0.5f, &hit);
    }

    v3 last_velocity = d->velocity;
    gr_drone_move(d, &c, to_player, dt, &last_velocity);
    gr_drone_attack(d, &c, distance, dt);
    gr_drone_present(d, c.a, d->muzzle, to_player, last_velocity, dt);

    if (d->health < c.a->health * GR_SMOKE_HEALTH) {
      d->smoke_timer -= dt;
      if (d->smoke_timer <= 0.0f) {
        d->smoke_timer = 0.1f;
        gr_drone_event(d, c.frame, GR_DRONE_EV_SMOKE);
      }
    }

    *ecs_sys_get_component_mut(WorldTransform, i) = gr_drone_pose(d);

    // nav audit: the segment from last frame's position must not hit the level
    if (audit) {
      v3 delta = v3_sub(d->position, before);
      f32 len = v3_length(delta);
      GrRayHit hit;
      d->audit_frame = c.frame;
      d->audit_crossed = len > 1e-4f && gr_level_raycast(c.layer, before, v3_scale(delta, 1.0f / len), len, &hit);
      d->audit_outside = !gr_level_inside(c.lvl, d->position);
      if (d->audit_crossed)
        log_warn("nav audit: % crossed level geometry from (%, %, %) to (%, %, %)", fmt_str(gr_label(c.a)),
                 fmt_f32_p(before.x, 1), fmt_f32_p(before.y, 1), fmt_f32_p(before.z, 1), fmt_f32_p(d->position.x, 1),
                 fmt_f32_p(d->position.y, 1), fmt_f32_p(d->position.z, 1));
    }

    // pinned: the pose and the audit show this frame's move; the drone itself stays put
    if (waves->pinned) {
      d->position = before;
      d->velocity = V3_ZERO;
    }
  }
}

// the shared attack budget: releases, then grants in row order, so a replay grants the same drones
ecs_system(gr_drone_token_system, EcsInOut(GrDrone), EcsResInOut(GrDroneTokens), EcsResIn(GrDroneRules),
           EcsResIn(GrWaves), EcsResIn(GrClock)) {
  GrDroneTokens *t = ecs_sys_get_resource_mut(GrDroneTokens);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  if (t->run != waves->run) {
    t->run = waves->run;
    t->attackers = 0;
  }
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *cur = ecs_sys_get_component(GrDrone, i);
    if (cur->run != waves->run || !cur->holds_token)
      continue;
    if (!cur->release_token && cur->state != GR_DRONE_DEAD)
      continue;
    GrDrone *d = ecs_sys_get_component_mut(GrDrone, i);
    d->holds_token = false;
    d->release_token = false;
    t->attackers = t->attackers > 0 ? t->attackers - 1 : 0;
  }
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *cur = ecs_sys_get_component(GrDrone, i);
    if (!cur->wants_token || cur->run != waves->run)
      continue;
    GrDrone *d = ecs_sys_get_component_mut(GrDrone, i);
    d->wants_token = false;
    if (d->state != GR_DRONE_ACTIVE)
      continue;
    if (t->attackers < rules->drones.max_attackers) {
      t->attackers++;
      d->holds_token = true;
      d->telegraph_timer = rules->drones.types.items[d->type].telegraph;
      gr_drone_event(d, clock->frame, GR_DRONE_EV_TELEGRAPH);
    } else {
      d->attack_timer = random_f32_range(&d->rng, 0.3f, 0.8f);
    }
  }
}

// frame-end view of live drones (hit tests, separation, radar) and kills; destroys dead and stale drones after the readers
ecs_system(gr_drone_consolidate_system, EcsIn(GrDrone), EcsResInOut(GrDroneField), EcsResIn(GrWaves), EcsResIn(GrClock),
           EcsResIn(GrDroneRules), EcsResIn(GrPlayerState)) {
  GrDroneField *f = ecs_sys_get_resource_mut(GrDroneField);
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  if (f->kill_frame != clock->frame) {
    f->kill_frame = clock->frame;
    f->count = 0;
    f->alive = 0;
    f->kill_count = 0;
    f->shake = 0.0f;
  }
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->run != waves->run) {
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    const GrDroneArchetype *a = &rules->drones.types.items[d->type];
    if (d->audit_frame == clock->frame) {
      f->audit_steps++;
      f->audit_crossings += d->audit_crossed ? 1u : 0u;
      f->audit_outside += d->audit_outside ? 1u : 0u;
    }
    if (d->state == GR_DRONE_DEAD) {
      // destroy: score event and shake by distance
      if (f->kill_count < GR_MAX_DRONES)
        f->kills[f->kill_count++] = (GrDroneKill){.type = d->type,
                                                  .score = a->score,
                                                  .position = d->position,
                                                  .velocity = d->velocity,
                                                  .direction = d->hit_dir,
                                                  .yaw = d->yaw,
                                                  .pitch = d->pitch,
                                                  .bank = d->bank};
      f->kills_total++;
      f->shake += m_maxf(0.0f, 0.55f - v3_distance(d->position, player->eye) * 0.02f) * a->scale;
      log_info("drones: % destroyed at (%, %, %), +% (kills %)", fmt_str(gr_label(a)), fmt_f32_p(d->position.x, 1),
               fmt_f32_p(d->position.y, 1), fmt_f32_p(d->position.z, 1), fmt_u32(a->score), fmt_u32(f->kills_total));
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    f->alive++;
    if (f->count < GR_MAX_DRONES)
      f->drones[f->count++] = (GrDroneSlot){.entity = it->entities[i],
                                            .position = d->position,
                                            .velocity = d->velocity,
                                            .radius = a->radius,
                                            .type = d->type,
                                            .targetable = d->state == GR_DRONE_ACTIVE};
  }
  if (f->kill_count > 0)
    f->kill_serial++;
}

// the drones' writes to the player's inbox: this frame's bolt hits, splash shake and kill shake
ecs_system(gr_drone_intake_system, EcsInOut(GrPlayerIntake), EcsResIn(GrBoltHits), EcsResIn(GrDroneField),
           EcsResIn(GrClock)) {
  const GrBoltHits *h = ecs_sys_get_resource(GrBoltHits);
  const GrDroneField *f = ecs_sys_get_resource(GrDroneField);
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  u32 hits = h->frame == frame ? h->count : 0;
  f32 shake = (h->frame == frame ? h->shake : 0.0f) + (f->kill_frame == frame ? f->shake : 0.0f);
  if (hits == 0 && shake <= 0.0f)
    return;
  for (i32 i = 0; i < it->count; i++) {
    GrPlayerIntake *in = ecs_sys_get_component_mut(GrPlayerIntake, i);
    for (u32 k = 0; k < hits; k++)
      gr_player_intake_damage(in, h->hits[k].amount, h->hits[k].from);
    if (shake > 0.0f)
      gr_player_intake_shake(in, shake);
  }
}

// nav audit and drone readouts, on the log every log_interval seconds of run time
ecs_system(gr_drone_log_system, EcsResInOut(GrDroneField), EcsResIn(GrDroneRules), EcsResIn(GrClock), EcsResIn(GrWaves)) {
  GrDroneField *f = ecs_sys_get_resource_mut(GrDroneField);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  if (!rules->debug.nav_audit && !rules->debug.log_drones)
    return;
  f->log_timer -= clock->dt;
  if (f->log_timer > 0.0f)
    return;
  f->log_timer = m_maxf(rules->debug.log_interval, 0.25f);
  if (rules->debug.nav_audit)
    log_info("nav audit: steps %, crossings %, outside %", fmt_u32(f->audit_steps), fmt_u32(f->audit_crossings),
             fmt_u32(f->audit_outside));
  if (rules->debug.log_drones) {
    log_info("drones: wave %, phase %, alive %, remaining %, kills %", fmt_i32(waves->wave), fmt_u32(waves->phase),
             fmt_u32(f->alive), fmt_u32(waves->remaining), fmt_u32(f->kills_total));
    for (u32 i = 0; i < f->count; i++) {
      const GrDroneSlot *s = &f->drones[i];
      log_info("  % at (%, %, %) speed %", fmt_str(gr_label(&rules->drones.types.items[s->type])),
               fmt_f32_p(s->position.x, 1), fmt_f32_p(s->position.y, 1), fmt_f32_p(s->position.z, 1),
               fmt_f32_p(v3_length(s->velocity), 2));
    }
  }
}

// distance along a unit ray to the drone's sphere, -1 on a miss or from inside
hz_internal f32 gr_drone_ray_distance(const GrDroneSlot *s, v3 origin, v3 dir) {
  v3 oc = v3_sub(origin, s->position);
  f32 b = v3_dot(oc, dir);
  f32 c = v3_lengthsq(oc) - s->radius * s->radius;
  f32 disc = b * b - c;
  if (disc < 0.0f)
    return -1.0f;
  return -b - m_sqrtf(disc);
}

hz_internal GrDroneRayHit gr_drone_ray_hit(const GrDroneField *f, u32 slot, v3 origin, v3 dir, f32 t) {
  v3 point = v3_add(origin, v3_scale(dir, t));
  return (GrDroneRayHit){.entity = f->drones[slot].entity,
                         .slot = slot,
                         .distance = t,
                         .point = point,
                         .normal = v3_normalize(v3_sub(point, f->drones[slot].position))};
}

u32 gr_drones_raycast_all(const GrDroneField *f, v3 origin, v3 dir, f32 max_distance, u32 limit, GrDroneRayHit *out) {
  u32 n = 0;
  for (u32 i = 0; i < f->count; i++) {
    if (!f->drones[i].targetable)
      continue;
    f32 t = gr_drone_ray_distance(&f->drones[i], origin, dir);
    if (t < 0.0f || t > max_distance)
      continue;
    // insertion keeps the list sorted nearest first and cut to `limit`
    u32 at = n < limit ? n : limit;
    while (at > 0 && out[at - 1].distance > t) {
      if (at < limit)
        out[at] = out[at - 1];
      at--;
    }
    if (at < limit) {
      out[at] = gr_drone_ray_hit(f, i, origin, dir, t);
      if (n < limit)
        n++;
    }
  }
  return n;
}

b32 gr_drones_raycast(const GrDroneField *f, v3 origin, v3 dir, f32 max_distance, GrDroneRayHit *out) {
  return gr_drones_raycast_all(f, origin, dir, max_distance, 1, out) > 0;
}
