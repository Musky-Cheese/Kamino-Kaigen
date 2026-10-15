// Weapon logic: flow, trigger, hitscan, reloads, swap, pickup interaction.

hz_internal b32 gr_in_run(u32 state) {
  return state == GR_STATE_PLAYING || state == GR_STATE_PAUSED || state == GR_STATE_ENDING;
}

hz_internal void gr_push_sound(GrWeaponEvents *ev, GrSoundEvent sound) {
  if (ev->sound_count < GR_MAX_SOUNDS)
    ev->sounds[ev->sound_count++] = sound;
}

// a point on the gun (drawn with the viewmodel fov) to where the world camera shows the same pixel
hz_internal v3 gr_on_screen(v3 p, const GrPlayerState *player) {
  f32 scale = m_tanf(m_radians(player->fov) * 0.5f) / m_tanf(m_radians(player->viewmodel_fov) * 0.5f);
  v3 local = quat_rotate(quat_conjugate(player->view_rot), v3_sub(p, player->eye));
  local.x *= scale;
  local.y *= scale;
  return v3_add(player->eye, quat_rotate(player->view_rot, local));
}

force_inline v3 gr_socket(const GrWeaponAssets *wa, u32 gun, GrSocket socket) { return wa->sockets[gun][socket]; }

// a model-space point on the gun in hand, in the world (camera -> gun group -> model)
hz_internal v3 gr_gun_point(const GrViewmodel *vm, const GrPlayerState *p, v3 model_point) {
  v3 in_camera = v3_add(vm->local_pos, quat_rotate(vm->local_rot, model_point));
  return v3_add(p->eye, quat_rotate(p->view_rot, in_camera));
}

// ---- reload ----

hz_internal void gr_cancel_reload(GrWeapon *w) {
  w->reload_phase = GR_RELOAD_NONE;
  w->reload_t = 0.0f;
  w->events.rest_mask |= 1u << w->gun;
}

hz_internal void gr_finish_reload(GrWeapon *w) {
  w->reload_phase = GR_RELOAD_NONE;
  w->reload_t = 0.0f;
  w->dry_fired = false; // holding the trigger through a reload must be able to dry-fire again
  w->events.rest_mask |= 1u << w->gun;
  w->events.reload_end = true;
}

hz_internal void gr_start_reload(GrWeapon *w, const GrGunConfig *g) {
  if (w->reload_phase != GR_RELOAD_NONE || w->pending || w->ammo[w->gun] >= g->magazine)
    return;
  w->reload_time = 0.0f;
  w->reload_t = 0.0f;
  w->reload_cue = 0;
  w->reload_interrupt = false;
  f32 total;
  if (!g->shell_reload) {
    w->reload_phase = GR_RELOAD_MAGAZINE;
    w->reload_duration = g->reload_time;
    w->reload_pump = false;
    total = g->reload_time;
  } else {
    w->reload_phase = GR_RELOAD_SHELL_START;
    w->reload_duration = g->shell_start;
    w->reload_pump = w->ammo[w->gun] == 0; // nothing chambered: the reload ends by racking the pump
    total = g->shell_start + g->shell_per * (f32)(g->magazine - w->ammo[w->gun]) + g->shell_end;
  }
  w->events.reload_start = true;
  w->events.reload_duration = total;
}

hz_internal void gr_update_reload(GrWeapon *w, const GrGunConfig *g, f32 dt) {
  if (w->reload_phase == GR_RELOAD_NONE)
    return;
  w->reload_time += dt;
  w->reload_t = m_minf(1.0f, w->reload_time / w->reload_duration);

  if (w->reload_phase == GR_RELOAD_MAGAZINE) {
    while (w->reload_cue < g->cue_count && w->reload_t >= g->cues[w->reload_cue].at) {
      gr_push_sound(&w->events, (GrSoundEvent){.sound = g->cues[w->reload_cue].sound});
      w->reload_cue++;
    }
    if (w->reload_t >= 1.0f) {
      w->ammo[w->gun] = g->magazine;
      gr_finish_reload(w);
    }
    return;
  }

  if (w->reload_phase == GR_RELOAD_SHELL_INSERT && w->reload_cue == 0 && w->reload_t >= 0.6f) {
    // the shell seats: one more round
    w->reload_cue = 1;
    w->ammo[w->gun] = w->ammo[w->gun] + 1 < g->magazine ? w->ammo[w->gun] + 1 : g->magazine;
    gr_push_sound(&w->events, (GrSoundEvent){.sound = GR_SND_SHELL_INSERT});
  }
  if (w->reload_t < 1.0f)
    return;
  if (w->reload_phase == GR_RELOAD_SHELL_END) {
    gr_finish_reload(w);
    return;
  }
  b32 end = w->ammo[w->gun] >= g->magazine || w->reload_interrupt;
  w->reload_phase = end ? GR_RELOAD_SHELL_END : GR_RELOAD_SHELL_INSERT;
  w->reload_time = 0.0f;
  w->reload_t = 0.0f;
  w->reload_cue = 0;
  // without a pump stroke the end is just lifting the gun back up
  w->reload_duration = !end ? g->shell_per : w->reload_pump ? g->shell_end : g->shell_end * 0.5f;
}

// ---- swap ----

hz_internal void gr_equip(GrWeapon *w, u32 gun) {
  gr_cancel_reload(w);
  w->gun = gun;
  w->events.rest_mask |= 1u << gun;
  w->events.raise = true;
  w->events.view_last = GR_VIEW_RAISE;
  w->fire_timer = 0.0f;
  w->bloom = 0.0f;
  w->dry_fired = false;
  w->buffered = 0.0f;
  w->queued = false;
  w->events.switched = true;
}

hz_internal b32 gr_swap_to(GrWeapon *w, u32 gun) {
  if (w->pending || gun == w->gun)
    return false;
  gr_cancel_reload(w);
  w->pending = true;
  w->pending_gun = gun;
  w->events.view_last = GR_VIEW_LOWER;
  w->events.taken_gun = (i32)gun;
  gr_push_sound(&w->events, (GrSoundEvent){.sound = GR_SND_WEAPON_PICKUP});
  return true;
}

// is the gun lying in the world for this frame's interaction: last frame's pickups with this frame's reset, take, drop
hz_internal b32 gr_gun_lies(const GrWeapon *w, const GrPickupState *ps, u32 gun) {
  const GrWeaponEvents *ev = &w->events;
  if (ev->taken_gun == (i32)gun)
    return false;
  if (ev->dropped_gun == (i32)gun)
    return true;
  if (ev->run_reset)
    return gun != w->gun && ps->home[gun] >= 0; // run reset: every other gun on its own pedestal
  return ps->in_world[gun];
}

// where it lies (gr_gun_lies true)
hz_internal v3 gr_gun_base(const GrWeapon *w, const GrPickupState *ps, const GrLevel *lvl, u32 gun) {
  if (w->events.dropped_gun == (i32)gun)
    return w->events.drop_base;
  if (w->events.run_reset)
    return lvl->pedestals[ps->home[gun]].position;
  return ps->base[gun];
}

// drop: at the feet, clear of pedestals and other guns, never through a wall, on the floor there
hz_internal v3 gr_drop_base(const GrWeapon *w, const GrPlayerState *p, const GrLevel *lvl, const GrPickupState *ps,
                            const EcsCollisionLayer *layer, u32 dropped) {
  const f32 wall_clearance = 0.35f, gun_clearance = 1.3f, reach_height = 1.4f;
  v3 feet = p->feet;
  f32 x = feet.x, z = feet.z;
  // the pedestals, then the other guns lying in the world, in a fixed order
  for (u32 i = 0; i < GR_LEVEL_PEDESTAL_COUNT + GR_GUN_COUNT; i++) {
    v3 o;
    if (i < GR_LEVEL_PEDESTAL_COUNT) {
      o = lvl->pedestals[i].position;
    } else {
      u32 g = i - GR_LEVEL_PEDESTAL_COUNT;
      if (g == dropped || !gr_gun_lies(w, ps, g))
        continue;
      o = gr_gun_base(w, ps, lvl, g);
    }
    if (m_absf(o.y - feet.y) > reach_height)
      continue;
    f32 dx = x - o.x, dz = z - o.z;
    f32 d = m_sqrtf(dx * dx + dz * dz);
    if (d >= gun_clearance)
      continue;
    // straight out from the obstacle, or towards the facing if exactly on top of it
    f32 ux = d > 1e-3f ? dx / d : -m_sinf(p->yaw);
    f32 uz = d > 1e-3f ? dz / d : -m_cosf(p->yaw);
    x = o.x + ux * gun_clearance;
    z = o.z + uz * gun_clearance;
  }
  // pulled back towards the player if the nudge would reach into a wall at waist height
  f32 shift = m_sqrtf((x - feet.x) * (x - feet.x) + (z - feet.z) * (z - feet.z));
  if (shift > 1e-3f) {
    v3 dir = {(x - feet.x) / shift, 0.0f, (z - feet.z) / shift};
    GrRayHit wall;
    f32 reach = shift;
    if (gr_level_raycast(layer, (v3){feet.x, feet.y + 0.8f, feet.z}, dir, shift + wall_clearance, &wall))
      reach = m_maxf(0.0f, wall.distance - wall_clearance);
    x = feet.x + dir.x * reach;
    z = feet.z + dir.z * reach;
  }
  // inside the room the player is in
  aabb zone = lvl->zones[gr_level_zone_at(lvl, feet)].box;
  x = m_clampf(x, zone.min.x + wall_clearance, zone.max.x - wall_clearance);
  z = m_clampf(z, zone.min.z + wall_clearance, zone.max.z - wall_clearance);
  v3 probe = {x, feet.y + 1.0f, z};
  GrRayHit floor;
  b32 hit = gr_level_raycast(layer, probe, (v3){0.0f, -1.0f, 0.0f}, probe.y - zone.min.y + 1.0f, &floor);
  return (v3){x, hit ? floor.point.y : feet.y, z};
}

// the gun the E prompt offers
hz_internal i32 gr_nearest_gun(const GrWeapon *w, const GrWeaponConfig *cfg, const GrPlayerState *p,
                               const GrPickupState *ps, const GrLevel *lvl) {
  i32 best = -1;
  f32 best_d = cfg->pickup_reach;
  for (u32 g = 0; g < GR_GUN_COUNT; g++) {
    if (!gr_gun_lies(w, ps, g))
      continue;
    v3 base = gr_gun_base(w, ps, lvl, g);
    if (m_absf(base.y - p->feet.y) > 1.4f)
      continue;
    f32 d = m_sqrtf((base.x - p->feet.x) * (base.x - p->feet.x) + (base.z - p->feet.z) * (base.z - p->feet.z));
    if (d < best_d) {
      best_d = d;
      best = (i32)g;
    }
  }
  return best;
}

// ---- fire ----

hz_internal f32 gr_spread(const GrWeapon *w, const GrGunConfig *g, const GrPlayerState *p) {
  f32 zoom = p->zoom;
  f32 base = g->spread_hip + (g->spread_zoom - g->spread_hip) * zoom;
  f32 moving = p->move_amount * 0.012f * (1.0f - zoom);
  f32 airborne = p->on_ground ? 0.0f : 0.015f;
  return base + w->bloom * (1.0f - zoom * 0.6f) + moving + airborne;
}

hz_internal f32 gr_falloff(const GrGunConfig *g, f32 distance) {
  if (!g->falloff)
    return 1.0f;
  return 1.0f - (1.0f - g->falloff_min) * m_smoothstepf(g->falloff_start, g->falloff_end, distance);
}

hz_internal void gr_eject(GrWeaponEvents *ev, Random *rng, u32 kind, v3 on_screen, const GrPlayerState *p) {
  if (ev->eject_count >= GR_MAX_EJECTS)
    return;
  f32 speed = kind == GR_EJECT_SHELL ? 0.8f : 1.0f;
  v3 v = {random_f32_range(rng, 2.2f, 3.2f) * speed, random_f32_range(rng, 0.6f, 1.3f),
          random_f32_range(rng, 0.2f, 0.8f)};
  v = v3_add(quat_rotate(p->view_rot, v), p->velocity);
  ev->ejects[ev->eject_count++] = (GrEjectEvent){.kind = kind, .position = on_screen, .velocity = v};
}

typedef struct GrPelletHit {
  EcsEntity drone;
  f32 damage;
  u32 pellets;
  v3 point, normal;
} GrPelletHit;

hz_internal void gr_fire(GrWeapon *w, const GrViewmodel *vm, const GrWeaponConfig *cfg, const GrWeaponAssets *wa,
                         const GrPlayerState *p, const EcsCollisionLayer *layer, const GrDroneField *field) {
  GrWeaponEvents *ev = &w->events;
  if (ev->shot_count >= GR_MAX_SHOTS) {
    log_error("weapon: more than % shots in one frame; the rest are dropped", fmt_u32(GR_MAX_SHOTS));
    return;
  }
  const GrGunConfig *g = &cfg->guns[w->gun];
  w->ammo[w->gun]--;
  w->shots_fired++;

  v3 origin = p->eye;
  v3 shot = gr_random_in_cone(&w->rng, p->view_forward, gr_spread(w, g, p));
  // the flash lights the gun from its real muzzle; tracers leave the muzzle as it is seen
  v3 muzzle = gr_gun_point(vm, p, gr_socket(wa, w->gun, GR_SOCKET_MUZZLE));
  v3 tracer_start = gr_on_screen(muzzle, p);

  f32 zoom = p->zoom;
  f32 cone = g->pellet_hip + (g->pellet_zoom - g->pellet_hip) * zoom;
  u32 shot_index = ev->shot_count++;
  GrShotEvent *se = &ev->shots[shot_index];
  *se = (GrShotEvent){.gun = w->gun, .origin = origin, .direction = shot, .muzzle = muzzle,
                      .tracer_start = tracer_start, .zoom = zoom, .first_pellet = ev->pellet_count};

  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  GrPelletHit *hits = ALLOC_ARRAY(&ta.allocator, GrPelletHit, g->pierce * g->pellets);
  GrDroneRayHit *rays = ALLOC_ARRAY(&ta.allocator, GrDroneRayHit, g->pierce);
  u32 hit_count = 0;
  b32 world_sound = false;
  for (u32 i = 0; i < g->pellets; i++) {
    // the first pellet flies true so a well-aimed shot always lands something
    v3 pellet = g->pellets > 1 && i > 0 ? gr_random_in_cone(&w->rng, shot, cone) : shot;
    GrRayHit world;
    b32 world_hit = gr_level_raycast(layer, origin, pellet, g->range, &world);
    f32 reach = world_hit ? world.distance : g->range;
    u32 n = gr_drones_raycast_all(field, origin, pellet, reach, g->pierce, rays);    for (u32 h = 0; h < n; h++) {
      f32 damage = g->damage * gr_falloff(g, rays[h].distance);
      u32 e = 0;
      while (e < hit_count && hits[e].drone != rays[h].entity)
        e++;
      if (e < hit_count) {
        hits[e].damage += damage;
        hits[e].pellets++;
        if (ev->drone_impact_count < GR_MAX_DRONE_IMPACTS)
          ev->drone_impacts[ev->drone_impact_count++] =
              (GrDroneImpactEvent){.point = rays[h].point, .normal = rays[h].normal, .direction = pellet,
                                   .drone_type = field->drones[rays[h].slot].type};
      } else {
        hits[hit_count++] = (GrPelletHit){.drone = rays[h].entity, .damage = damage, .pellets = 1,
                                          .point = rays[h].point, .normal = rays[h].normal};
      }
    }
    // a piercing shot stops at the last drone it can pass through; otherwise at the wall
    GrPelletEvent pe = {.direction = pellet, .shot = shot_index, .end_kind = GR_PELLET_RANGE};
    if (n > 0 && g->pierce == 1) {
      pe.end = rays[0].point;
      pe.end_kind = GR_PELLET_DRONE;
    } else if (n > 0 && n == g->pierce && g->pierce > 1) {
      pe.end = rays[n - 1].point;
      pe.end_kind = GR_PELLET_DRONE;
    } else if (world_hit) {
      pe.end = world.point;
      pe.normal = world.normal;
      pe.end_kind = GR_PELLET_WORLD;
      if (!world_sound) {
        world_sound = true;
        gr_push_sound(ev, (GrSoundEvent){.sound = GR_SND_IMPACT, .positional = true, .position = world.point});
      }
    } else {
      pe.end = v3_add(origin, v3_scale(pellet, g->range));
    }
    if (ev->pellet_count < GR_MAX_PELLETS)
      ev->pellets[ev->pellet_count++] = pe;
  }
  se->pellet_count = ev->pellet_count - se->first_pellet;

  // damage lands once per drone per shot: the pellets' damage summed, knockback by share of pellets
  for (u32 h = 0; h < hit_count && ev->drone_hit_count < GR_MAX_DRONE_HITS; h++) {
    ev->drone_hits[ev->drone_hit_count++] = (GrDroneHitEvent){
        .drone = hits[h].drone, .shot = shot_index, .damage = hits[h].damage,
        .knockback = g->knockback * (f32)hits[h].pellets / (f32)g->pellets, .direction = shot,
        .point = hits[h].point, .normal = hits[h].normal};
  }
  if (hit_count > 0) {
    se->hit = true;
    w->shots_hit++;
  }
  tctx_temp_allocator_end(ta);
  se->light_intensity = GR_MUZZLE_LIGHT * (g->pellets > 1 ? 1.6f : g->pierce > 1 ? 1.4f : 1.0f);

  // rifle brass flies out of the ejection port; the breacher ejects on the pump, the lancer vents
  v3 port_on_screen = gr_on_screen(gr_gun_point(vm, p, gr_socket(wa, w->gun, GR_SOCKET_EJECTION_PORT)), p);
  if (w->gun == GR_WEAPON_RIFLE)
    gr_eject(ev, &w->rng, GR_EJECT_BRASS, port_on_screen, p);
  if (w->gun == GR_WEAPON_LANCER && ev->eject_count < GR_MAX_EJECTS)
    ev->ejects[ev->eject_count++] = (GrEjectEvent){
        .kind = GR_EJECT_VENT, .position = port_on_screen,
        .velocity = v3_add(quat_rotate(p->view_rot, (v3){0.6f, 0.35f, 0.0f}), p->velocity)};

  se->recoil_pitch = g->recoil_pitch * (1.0f - zoom * 0.4f);
  se->recoil_yaw = (random_f32(&w->rng) * 2.0f - 1.0f) * g->recoil_yaw;
  se->recoil_recover = g->recoil_recover;
  w->bloom = m_minf(w->bloom + g->spread_per_shot, g->spread_max);
  gr_push_sound(ev, (GrSoundEvent){.sound = g->fire_sound});
}

// Settings' weapon config copied for the systems when it changes
ecs_system(gr_weapon_config_system, EcsIn(GrWeaponConfig), EcsResInOut(GrWeaponRules)) {
  if (!ecs_iter_changed(it))
    return;
  GrWeaponRules *r = ecs_sys_get_resource_mut(GrWeaponRules);
  for (i32 i = 0; i < it->count; i++)
    r->weapons = *ecs_sys_get_component(GrWeaponConfig, i);
  if (!r->ready)
    log_info("weapons: config ready (starting gun %)", fmt_u32(r->weapons.starting_gun));
  r->ready = it->count > 0;
}

// ---- the run reset ----

hz_internal void gr_weapon_reset(GrWeapon *w, const GrWeaponConfig *cfg) {
  for (u32 g = 0; g < GR_GUN_COUNT; g++)
    w->ammo[g] = cfg->guns[g].magazine;
  w->pending = false;
  gr_equip(w, cfg->starting_gun);
  w->fire_timer = 0.0f;
  w->bloom = 0.0f;
  w->buffered = 0.0f;
  w->queued = false;
  w->shots_fired = 0;
  w->shots_hit = 0;
  w->zoom_target = 0.0f;
  w->events.run_reset = true;
}

// the frame's flow: run start, the step and bindings, the weapon switch, and a lowered swap finishing
ecs_system(gr_weapon_flow_system, EcsInOut(GrWeapon), EcsIn(GrViewmodel), EcsResIn(GrDev), EcsResIn(GrClock),
           EcsResIn(GrInput), EcsResIn(GrWeaponRules), EcsResIn(GrPickupState), EcsResIn(GrPlayerState)) {
  const GrDev *dev = ecs_sys_get_resource(GrDev);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrInput *in = ecs_sys_get_resource(GrInput);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  for (i32 i = 0; i < it->count; i++) {
    GrWeapon *w = ecs_sys_get_component_mut(GrWeapon, i);
    const GrViewmodel *vm = ecs_sys_get_component(GrViewmodel, i);
    GrWeaponEvents *ev = &w->events;
    *ev = (GrWeaponEvents){.taken_gun = -1, .dropped_gun = -1};

    b32 was_in_run = w->in_run;
    w->in_run = gr_in_run(dev->state);
    // run start: from the title (deploy) or any run restart
    if ((w->in_run && !was_in_run) || (w->in_run && dev->restart_serial != w->applied_serial)) {
      w->applied_serial = dev->restart_serial;
      gr_weapon_reset(w, cfg);
      w->granted_weapon = (i32)cfg->starting_gun;
    }

    // the real buttons (GrInput, gated by the pointer lock) or the simulated ones (GrDev, with their edges)
    w->fire_down = in->fire_held || dev->fire;
    w->fire_pressed = in->fire_pressed || dev->fire_pressed;
    w->ads_down = in->aim_held || dev->ads;
    w->reload_pressed = in->reload_pressed;
    w->swap_pressed = in->swap_pressed;

    // the run updates while playing (with or without controls) or in the ending outro; a frozen clock runs nothing
    b32 update = clock->run_dt > 0.0f && (dev->state == GR_STATE_PLAYING || dev->state == GR_STATE_ENDING);
    w->dt = update ? clock->run_dt : 0.0f;
    w->controls = update && dev->state == GR_STATE_PLAYING && p->controls;
    w->live = w->controls && p->alive;
    w->look = w->controls ? in->look : (v2){0.0f, 0.0f};
    w->held = false;
    if (!update) {
      w->prompt_gun = -1;
      continue;
    }

    // the weapon switch: a normal swap to the gun wherever it lies, issued once per request
    if (w->in_run && (i32)dev->weapon != w->granted_weapon) {
      w->granted_weapon = (i32)dev->weapon;
      if (gr_gun_lies(w, ps, dev->weapon))
        gr_swap_to(w, dev->weapon);
    }

    // a raise then a lower in one frame leave the gun fully down
    b32 lowered = vm->lowered || (ev->raise && ev->view_last == GR_VIEW_LOWER);
    if (w->pending && lowered) {
      u32 dropped = w->gun;
      w->pending = false;
      gr_equip(w, w->pending_gun);
      ev->dropped_gun = (i32)dropped; // gr_weapon_interact_system places it
    }
  }
}

// the trigger, reloads and fire
ecs_system(gr_weapon_fire_system, EcsInOut(GrWeapon), EcsIn(GrViewmodel), EcsResIn(GrPlayerState),
           EcsResIn(GrWeaponRules), EcsResIn(GrWeaponAssets), EcsResIn(EcsPhysicsWorld), EcsResIn(GrDroneField)) {
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrWeaponAssets *wa = ecs_sys_get_resource(GrWeaponAssets);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(GrWeapon, i)->dt <= 0.0f)
      continue;
    GrWeapon *w = ecs_sys_get_component_mut(GrWeapon, i);
    const GrViewmodel *vm = ecs_sys_get_component(GrViewmodel, i);
    GrWeaponEvents *ev = &w->events;
    f32 dt = w->dt;
    const b32 ready = !w->pending;
    const GrGunConfig *g = &cfg->guns[w->gun];

    // a press and release inside one frame still counts
    b32 pressed = w->live && w->fire_pressed;
    b32 held = w->live && (w->fire_down || pressed);
    w->held = held;
    w->zoom_target = w->live && ready && w->ads_down && w->reload_phase == GR_RELOAD_NONE ? 1.0f : 0.0f;

    if (w->live && ready && w->reload_pressed)
      gr_start_reload(w, g);
    gr_update_reload(w, g, dt);

    if (pressed)
      w->buffered = GR_TRIGGER_BUFFER;
    w->buffered = m_maxf(0.0f, w->buffered - dt);

    // the auto accumulator: frame rate never changes the rounds per minute
    if (g->automatic)
      w->fire_timer = m_maxf(w->fire_timer - dt, held ? -dt : 0.0f);
    else
      w->fire_timer = m_maxf(0.0f, w->fire_timer - dt);

    b32 shell_phase = w->reload_phase == GR_RELOAD_SHELL_START || w->reload_phase == GR_RELOAD_SHELL_INSERT ||
                      w->reload_phase == GR_RELOAD_SHELL_END;
    if (ready && pressed && shell_phase && w->ammo[w->gun] > 0) {
      // firing cuts a shell reload short once the current shell is in, then shoots
      w->reload_interrupt = true;
      w->queued = true;
    }

    if (ready && w->reload_phase == GR_RELOAD_NONE && (held || w->buffered > 0.0f || w->queued)) {
      if (w->ammo[w->gun] == 0) {
        if (!w->dry_fired && held) {
          w->dry_fired = true;
          gr_push_sound(ev, (GrSoundEvent){.sound = GR_SND_DRY_FIRE});
          ev->dry_fire = true;
          gr_start_reload(w, g);
        }
      } else if (g->automatic) {
        while (w->fire_timer <= 0.0f && w->ammo[w->gun] > 0) {
          gr_fire(w, vm, cfg, wa, p, layer, field);
          w->fire_timer += 1.0f / g->fire_rate;
        }
      } else if (w->fire_timer <= 0.0f && (w->buffered > 0.0f || w->queued)) {
        w->buffered = 0.0f;
        w->queued = false;
        gr_fire(w, vm, cfg, wa, p, layer, field);
        w->fire_timer = 1.0f / g->fire_rate;
      }
    }
    if (!w->fire_down)
      w->dry_fired = false;
    w->bloom = m_maxf(0.0f, w->bloom - g->spread_recovery * dt * (held ? 0.35f : 1.0f));
  }
}

// the dropped gun placed, then interaction (the nearest gun in reach, swapped on the E edge)
ecs_system(gr_weapon_interact_system, EcsInOut(GrWeapon), EcsResIn(GrPlayerState), EcsResIn(GrWeaponRules),
           EcsResIn(GrLevel), EcsResIn(EcsPhysicsWorld), EcsResIn(GrPickupState)) {
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrLevel *lvl = ecs_sys_get_resource(GrLevel);
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  for (i32 i = 0; i < it->count; i++) {
    const GrWeapon *cur = ecs_sys_get_component(GrWeapon, i);
    if (cur->dt <= 0.0f && cur->events.dropped_gun < 0)
      continue;
    GrWeapon *w = ecs_sys_get_component_mut(GrWeapon, i);
    GrWeaponEvents *ev = &w->events;
    if (ev->dropped_gun >= 0)
      ev->drop_base = gr_drop_base(w, p, lvl, ps, layer, (u32)ev->dropped_gun);
    // holding E swaps once: the press is an edge
    w->prompt_gun = w->controls && p->alive && !w->pending ? gr_nearest_gun(w, cfg, p, ps, lvl) : -1;
    if (w->prompt_gun >= 0 && w->swap_pressed) {
      gr_swap_to(w, (u32)w->prompt_gun);
      w->prompt_gun = -1;
    }
  }
}

// recoil per shot and the packs' heal, into the player's inbox (a per-entity system on the player)
ecs_system(gr_weapon_intake_system, EcsInOut(GrPlayerIntake), EcsResIn(GrWeaponState), EcsResIn(GrPickupState)) {
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  if (ev->shot_count == 0 && ps->heal <= 0.0f)
    return;
  for (i32 i = 0; i < it->count; i++) {
    GrPlayerIntake *q = ecs_sys_get_component_mut(GrPlayerIntake, i);
    for (u32 s = 0; s < ev->shot_count; s++)
      gr_player_intake_recoil(q, ev->shots[s].recoil_pitch, ev->shots[s].recoil_yaw, ev->shots[s].recoil_recover);
    if (ps->heal > 0.0f)
      gr_player_intake_heal(q, ps->heal);
  }
}

// every drone sums its own share of this frame's hits (the weapon never writes a drone)
ecs_system(gr_drone_hit_intake_system, EcsInOut(GrPendingDamage), EcsResIn(GrWeaponState)) {
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  if (ev->drone_hit_count == 0)
    return;
  for (i32 i = 0; i < it->count; i++) {
    for (u32 h = 0; h < ev->drone_hit_count; h++) {
      const GrDroneHitEvent *e = &ev->drone_hits[h];
      if (e->drone != it->entities[i])
        continue;
      GrPendingDamage *pd = ecs_sys_get_component_mut(GrPendingDamage, i);
      pd->amount += e->damage;
      pd->impulse = v3_add(pd->impulse, v3_scale(e->direction, e->knockback));
      pd->direction = e->direction;
      pd->hits++;    }
  }
}

// the weapon's frame into the GrWeaponState resource (HUD, effects, audio, player, drones)
ecs_system(gr_weapon_consolidate_system, EcsIn(GrWeapon), EcsIn(GrViewmodel), EcsIn(WorldTransform),
           EcsResInOut(GrWeaponState), EcsResIn(GrWeaponRules), EcsResIn(GrPlayerState), EcsResIn(GrClock)) {
  GrWeaponState *s = ecs_sys_get_resource_mut(GrWeaponState);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  for (i32 i = 0; i < it->count; i++) {
    const GrWeapon *w = ecs_sys_get_component(GrWeapon, i);
    const GrViewmodel *vm = ecs_sys_get_component(GrViewmodel, i);
    const WorldTransform *xf = ecs_sys_get_component(WorldTransform, i);
    const GrGunConfig *g = &cfg->guns[w->gun];
    u32 prev_reload = s->reload_phase;
    u32 prev_gun = s->gun;
    b32 prev_swapping = s->swapping;
    i32 prev_prompt = s->prompt_gun;
    s->gun = w->gun;
    s->ammo = w->ammo[w->gun];
    s->magazine = g->magazine;
    for (u32 k = 0; k < GR_GUN_COUNT; k++)
      s->ammo_by_gun[k] = w->ammo[k];
    s->reloading = w->reload_phase != GR_RELOAD_NONE;
    s->swapping_magazine = w->reload_phase == GR_RELOAD_MAGAZINE;
    s->reload_phase = w->reload_phase;
    s->reload_t = w->reload_t;
    s->swapping = w->pending;
    s->held = w->held;
    s->zoom = p->zoom;
    s->zoom_target = w->zoom_target;
    s->zoom_fov = g->ads_fov;
    s->scope = g->scope ? m_smoothstepf(0.75f, 0.98f, p->zoom) : 0.0f;
    s->spread = gr_spread(w, g, p);
    s->reticle_spread = s->spread + g->pellet_hip + (g->pellet_zoom - g->pellet_hip) * p->zoom;
    s->crosshair = g->crosshair;
    s->prompt_gun = w->prompt_gun;
    s->shots_fired = w->shots_fired;
    s->shots_hit = w->shots_hit;
    s->accuracy = w->shots_fired == 0 ? 0.0f : (f32)w->shots_hit / (f32)w->shots_fired;
    s->in_run = w->in_run;
    s->view_visible = vm->visible;
    s->view_pos = xf->position;
    s->view_rot = xf->rotation;
    s->parts = vm->parts;
    s->hands_motion = vm->hands_motion;
    s->hands_u = vm->hands_u;
    for (u32 k = 0; k < GR_GUN_COUNT; k++) {
      GrGunLook *l = &s->look[k];
      const GrGunConfig *gk = &cfg->guns[k];
      l->vent_intensity = 3.0f + vm->heat[k] * 12.0f;
      l->vent_color = v3_lerp(gk->vent_cold, gk->vent_hot, vm->heat[k]);
      l->screen_ammo = w->ammo[k];
      l->screen_max = gk->magazine;
      l->screen_low = (f32)w->ammo[k] <= (f32)gk->magazine * 0.25f;
    }
    s->look[GR_WEAPON_LANCER].coil_intensity = vm->coil;
    s->look[GR_WEAPON_LANCER].cell_intensity = vm->cell;
    s->look[GR_WEAPON_LANCER].lens_intensity = vm->lens;

    // the weapon's events, then the rig's (pump strokes, shells)
    s->events = w->events;
    GrWeaponEvents *ev = &s->events;
    for (u32 k = 0; k < vm->sound_count && ev->sound_count < GR_MAX_SOUNDS; k++)
      ev->sounds[ev->sound_count++] = vm->sounds[k];
    for (u32 k = 0; k < vm->eject_count && ev->eject_count < GR_MAX_EJECTS; k++)
      ev->ejects[ev->eject_count++] = vm->ejects[k];

    if (ev->shot_count || ev->dry_fire || ev->reload_start || ev->reload_end || s->reload_phase != prev_reload ||
        s->gun != prev_gun || s->swapping != prev_swapping || s->prompt_gun != prev_prompt || ev->taken_gun >= 0 ||
        ev->dropped_gun >= 0 || ev->run_reset)
      log_info("weapon: t % % ammo rifle % breacher % lancer % | shots +% total % hits % | reload % t % swap % zoom % "
               "prompt % taken % dropped % "
               "at (%, %, %) dry % reset % interrupt % queued %",
               fmt_f32(clock->time), fmt_str(gr_str32(&g->id)), fmt_u32(s->ammo_by_gun[0]), fmt_u32(s->ammo_by_gun[1]),
               fmt_u32(s->ammo_by_gun[2]), fmt_u32(ev->shot_count), fmt_u32(s->shots_fired), fmt_u32(s->shots_hit),
               fmt_u32(s->reload_phase), fmt_f32(s->reload_t), fmt_b32(s->swapping), fmt_f32(s->zoom),
               fmt_i32(s->prompt_gun), fmt_i32(ev->taken_gun), fmt_i32(ev->dropped_gun), fmt_f32(ev->drop_base.x),
               fmt_f32(ev->drop_base.y), fmt_f32(ev->drop_base.z), fmt_b32(ev->dry_fire), fmt_b32(ev->run_reset),
               fmt_b32(w->reload_interrupt), fmt_b32(w->queued));
  }
}
