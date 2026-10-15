// The wave director: wave script, spawn points, clears and victory, plus the drone package's init and schedules.

#define GR_WAVES_SEED 0x5eedf00dull // every run replays the same spawn points and drone rolls
#define GR_SPAWN_MIN_DISTANCE 10.0f // no spawn point closer to the player than this, m
#define KM_PHASE_COUNT 6 // the Kamino campaign's fronts (km_phases)

hz_internal void gr_waves_reset(GrWaves *wv) {
  wv->phase = GR_WAVES_IDLE;
  wv->wave = -1;
  wv->cleared = 0;
  wv->schedule_len = 0;
  wv->schedule_next = 0;
}

// queue the next wave: wave 0 is announced at once, later ones announce_delay into the intermission
hz_internal void gr_waves_queue_next(GrWaves *wv, const GrWaveConfig *cfg, f32 delay) {
  i32 next = wv->wave + 1;
  wv->phase = next == 0 ? GR_WAVES_INCOMING : GR_WAVES_INTERMISSION;
  wv->timer = delay;
  wv->announce_wave = next;
  if (next == 0) {
    wv->announce_pending = false;
    wv->announce_serial++;
    log_info("waves: incoming % \"%\" in % s", fmt_i32(next + 1),
             fmt_str(gr_str32(&cfg->waves.items[next].name)), fmt_f32_p(delay, 2));
  } else {
    wv->announce_pending = true;
    wv->announce_timer = cfg->announce_delay;
  }
}

// begin a wave: the groups flattened to (delay + i * interval, type), stably sorted by time
hz_internal void gr_waves_begin(GrWaves *wv, const GrWaveConfig *cfg) {
  wv->wave++;
  const GrWave *wave = &cfg->waves.items[wv->wave];
  wv->schedule_len = 0;
  for (u32 g = 0; g < wave->groups.len; g++) {
    const GrSpawnGroup *grp = &wave->groups.items[g];
    for (u32 k = 0; k < grp->count && wv->schedule_len < GR_MAX_WAVE_SPAWNS; k++) {
      GrWaveSpawn s = {.time = grp->delay + (f32)k * grp->interval, .type = grp->type};
      u32 at = wv->schedule_len++;
      while (at > 0 && wv->schedule[at - 1].time > s.time) {
        wv->schedule[at] = wv->schedule[at - 1];
        at--;
      }
      wv->schedule[at] = s;
    }
  }
  wv->schedule_next = 0;
  wv->clock = 0.0f;
  wv->phase = GR_WAVES_ACTIVE;
  wv->started_serial++;
  log_info("waves: wave % \"%\" started, % drones", fmt_i32(wv->wave + 1), fmt_str(gr_str32(&wave->name)),
           fmt_u32(wv->schedule_len));
}

// finish a wave: the clear bonus, then the next intermission or victory
hz_internal void gr_waves_finish(GrWaves *wv, const GrWaveConfig *cfg) {
  u32 bonus = cfg->clear_bonus_per_wave * (u32)(wv->wave + 1);
  wv->cleared++;
  wv->cleared_wave = wv->wave;
  wv->cleared_bonus = bonus;
  wv->cleared_serial++;
  log_info("waves: wave % cleared, bonus %", fmt_i32(wv->wave + 1), fmt_u32(bonus));
  if (wv->wave >= (i32)cfg->waves.len - 1) {
    wv->phase = GR_WAVES_COMPLETE;
    wv->victory_serial++;
    log_info("waves: complete (victory)");
  } else {
    gr_waves_queue_next(wv, cfg, cfg->intermission);
  }
}

// pick a spawn point: the player's area, else its neighbours; never the last point, never within 10 m
hz_internal v3 gr_waves_pick_spawn(GrWaves *wv, const GrLevel *lvl, v3 player) {
  u32 area = gr_level_area_at(lvl, player);
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 cap = GR_AREA_COUNT * GR_LEVEL_AREA_SPAWN_MAX;
  u32 *cand_area = ALLOC_ARRAY(&ta.allocator, u32, cap);
  u32 *cand_index = ALLOC_ARRAY(&ta.allocator, u32, cap);
  u32 n = 0;
  for (u32 pass = 0; pass < 2 && n == 0; pass++) {
    u32 mask = pass == 0 ? 1u << area : lvl->areas[area].neighbour_mask;
    for (u32 a = 0; a < GR_AREA_COUNT; a++) {
      if (!(mask & (1u << a)))
        continue;
      for (u32 s = 0; s < lvl->areas[a].spawn_count; s++) {
        b32 last = (i32)a == wv->last_spawn_area && (i32)s == wv->last_spawn_index;
        if (last || v3_distance(lvl->areas[a].spawns[s], player) <= GR_SPAWN_MIN_DISTANCE)
          continue;
        cand_area[n] = a;
        cand_index[n] = s;
        n++;
      }
    }
  }
  u32 pick_area = area, pick_index = 0;
  if (n > 0) {
    u32 k = random_u32_range(&wv->rng, 0, n - 1);
    pick_area = cand_area[k];
    pick_index = cand_index[k];
  }
  tctx_temp_allocator_end(ta);
  wv->last_spawn_area = (i32)pick_area;
  wv->last_spawn_index = (i32)pick_index;
  return lvl->areas[pick_area].spawns[pick_index];
}

// a run start's wave half: a new run id retires every drone and bolt of the last one
hz_internal void gr_waves_start_run(GrWaves *wv, const GrWaveConfig *cfg, b32 hold) {
  wv->run++;
  wv->bolt_run++;
  gr_waves_reset(wv);
  wv->rng = random_create(GR_WAVES_SEED);
  wv->last_spawn_area = -1;
  wv->last_spawn_index = -1;
  wv->announce_pending = false;
  wv->total_waves = KM_PHASE_COUNT;
  wv->kills_seen = ~0u; // counted from the run's first frame
  log_info("waves: run % started%", fmt_u32(wv->run), fmt_cstr(hold ? " (waves held)" : ""));
  // the start switch announces wave 1 before hold_waves resets the script, so the banner shows either way
  gr_waves_queue_next(wv, cfg, cfg->intro_delay);
}

hz_internal void gr_waves_spawn(EcsWorld *w, GrWaves *wv, const GrDroneRules *rules, const GrDroneAssets *assets, u32 type,
                                v3 at, const char *why) {
  gr_drone_spawn(w, rules, assets, type, wv->run, at, random_u64(&wv->rng));
  wv->spawned_total++;
  log_info("drones: % % at (%, %, %)", fmt_cstr(why), fmt_str(gr_label(&rules->drones.types.items[type])),
           fmt_f32_p(at.x, 1), fmt_f32_p(at.y, 1), fmt_f32_p(at.z, 1));
}

// ---------------------------------------------------------------- the Kamino campaign

// one front: the droids hold `sector` until `quota` of them fall, then the survivors fall back and the player takes `capture`
typedef struct KmPhase {
  u32 sector;
  u32 quota;
  u32 max_alive;
  u32 b2;      // super battle droids among the reinforcements
  u32 initial; // the garrison put down at once
  v2 capture;  // world XZ
} KmPhase;

#define KM_SPAWN_POINTS 7
#define KM_CAPTURE_RADIUS 5.0f

static const KmPhase km_phases[KM_PHASE_COUNT] = {
    {KM_SECTOR_BESH, 14, 7, 0, 7, {0.0f, 18.0f}},       {KM_SECTOR_CRESH, 20, 9, 2, 7, {0.0f, -28.0f}},
    {KM_SECTOR_DORN, 30, 12, 5, 10, {0.0f, -62.0f}},    {KM_SECTOR_LOBBY, 18, 9, 3, 8, {0.0f, -146.0f}},
    {KM_SECTOR_CORRIDOR, 16, 7, 2, 6, {0.0f, -187.0f}}, {KM_SECTOR_HALL, 24, 10, 5, 9, {0.0f, -232.0f}},
};

// each sector's nav zone (km_layout_fill's order)
static const u32 km_sector_zones[KM_SECTOR_COUNT] = {0, 2, 4, 6, 7, 9, 10};

// where a sector's droids come in, world XZ: its far side, away from the advancing front
static const v2 km_sector_spawns[KM_SECTOR_COUNT][KM_SPAWN_POINTS] = {
    {{-6, 56}, {6, 56}, {0, 54}, {-9, 60}, {9, 60}, {-4, 52}, {4, 52}},
    {{-8, 12}, {8, 13}, {0, 9}, {-5, 8}, {6, 9}, {-10, 16}, {10, 18}},
    {{-9, -36}, {9, -34}, {0, -39}, {-5, -40}, {6, -40}, {-11, -30}, {11, -31}},
    {{-4, -85}, {4, -85}, {0, -84}, {-16, -90}, {16, -90}, {-14, -101}, {14, -101}},
    {{-14, -154}, {14, -154}, {0, -162}, {-8, -160}, {8, -160}, {-17, -146}, {17, -146}},
    {{-3.6f, -198}, {3.6f, -198}, {0, -201}, {-5.5f, -193}, {5.5f, -193}, {-2, -196}, {2, -196}},
    {{-18, -241}, {18, -241}, {-16, -254}, {16, -254}, {0, -250}, {-10, -248}, {10, -248}},
};

hz_internal f32 km_distance_xz(v3 a, v3 b) { return m_sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

// a spawn point of the sector at least 12 m from the player, never the last one used
hz_internal v3 km_pick_spawn(GrWaves *wv, u32 sector, v3 player, u32 type) {
  f32 y = km_droid_walks(type) ? KM_WALKER_HEIGHT : 4.0f;
  u32 start = random_u32_range(&wv->rng, 0, KM_SPAWN_POINTS - 1);
  for (u32 k = 0; k < KM_SPAWN_POINTS; k++) {
    u32 i = (start + k) % KM_SPAWN_POINTS;
    v3 p = {km_sector_spawns[sector][i].x, y, km_sector_spawns[sector][i].y};
    if ((i32)i == wv->last_spawn_index || km_distance_xz(p, player) < 12.0f)
      continue;
    wv->last_spawn_index = (i32)i;
    return p;
  }
  return (v3){km_sector_spawns[sector][start].x, y, km_sector_spawns[sector][start].y};
}

// the next front: its sector and counters; announced unless it is the first (queued at the run's start)
hz_internal void km_phase_begin(GrWaves *wv) {
  wv->wave++;
  const KmPhase *ph = &km_phases[wv->wave];
  wv->sector = ph->sector;
  wv->hold_zone = km_sector_zones[ph->sector];
  wv->killed_phase = 0;
  wv->quiet_time = 0.0f;
  wv->spawned_phase = 0;
  wv->b2_spawned = 0;
  wv->spawn_timer = 0.0f;
  wv->objective_active = false;
  wv->phase = GR_WAVES_ACTIVE;
  wv->started_serial++;
  if (wv->wave > 0) {
    wv->announce_wave = wv->wave;
    wv->announce_serial++;
  }
  log_info("campaign: front % at %, quota %", fmt_i32(wv->wave + 1), fmt_cstr(km_sector_name(ph->sector)), fmt_u32(ph->quota));
}

// the quota is met: the survivors fall back to the next sector, the player takes this one's capture point
hz_internal void km_phase_fall_back(GrWaves *wv) {
  const KmPhase *ph = &km_phases[wv->wave];
  const KmPhase *next = &km_phases[wv->wave + 1];
  v2 c = km_sector_center(next->sector);
  wv->phase = GR_WAVES_SECURE;
  wv->retreat_serial++;
  wv->retreat_zone = km_sector_zones[next->sector];
  wv->retreat_point = (v3){c.x, KM_WALKER_HEIGHT, c.y};
  wv->objective = (v3){ph->capture.x, 0.0f, ph->capture.y};
  wv->objective_active = true;
  wv->fallback_serial++;
  log_info("campaign: % broken, falling back to %", fmt_cstr(km_sector_name(ph->sector)), fmt_cstr(km_sector_name(next->sector)));
}

hz_internal void km_phase_cleared(GrWaves *wv) {
  wv->cleared++;
  wv->cleared_wave = wv->wave;
  wv->cleared_bonus = 250u * (u32)(wv->wave + 1);
  wv->cleared_serial++;
  wv->objective_active = false;
}

// the active front: garrison, then reinforcements while the quota can still be met; the break when it is
hz_internal u32 km_phase_tick(EcsWorld *w, GrWaves *wv, const GrDroneRules *rules, const GrDroneAssets *assets, u32 alive, v3 player,
                              f32 dt) {
  const KmPhase *ph = &km_phases[wv->wave];
  // a line three-quarters broken that has gone quiet falls back too: a straggler out of reach never holds the front
  b32 stalled = wv->killed_phase * 4u >= ph->quota * 3u && wv->quiet_time > 40.0f;
  if (wv->killed_phase >= ph->quota || stalled) {
    if (wv->wave + 1 < KM_PHASE_COUNT) {
      km_phase_fall_back(wv);
    } else if (alive == 0 || stalled) {
      km_phase_cleared(wv);
      wv->phase = GR_WAVES_COMPLETE;
      wv->victory_serial++;
      log_info("campaign: the cloning hall is clear (victory)");
    }
    return 0;
  }
  wv->spawn_timer -= dt;
  if (wv->spawn_timer > 0.0f || alive >= ph->max_alive || wv->killed_phase + alive >= ph->quota)
    return 0;
  b32 garrison = wv->spawned_phase < ph->initial;
  u32 type = GR_DRONE_SCOUT;
  if (!garrison && wv->b2_spawned < ph->b2 && random_f32(&wv->rng) < 0.4f) {
    type = GR_DRONE_STRIKER;
    wv->b2_spawned++;
  }
  gr_waves_spawn(w, wv, rules, assets, type, km_pick_spawn(wv, ph->sector, player, type), garrison ? "garrison" : "reinforcement");
  wv->spawned_phase++;
  wv->spawn_timer = garrison ? 0.25f : random_f32_range(&wv->rng, 1.6f, 3.0f);
  return 1;
}

// securing: the player reaching the capture point advances the front
hz_internal void km_phase_secure(GrWaves *wv, v3 player, b32 automatic) {
  if (!automatic && km_distance_xz(player, wv->objective) > KM_CAPTURE_RADIUS)
    return;
  log_info("campaign: % secured", fmt_cstr(km_sector_name(km_phases[wv->wave].sector)));
  km_phase_cleared(wv);
  km_phase_begin(wv);
}

// the flow's clears: the world (run start, quit; waves reset too), the drones (results), the bolts (victory)
ecs_system(gr_waves_clear_system, EcsResInOut(GrWaves), EcsResIn(GrFlow)) {
  GrWaves *wv = ecs_sys_get_resource_mut(GrWaves);
  const GrFlow *flow = ecs_sys_get_resource(GrFlow);
  if (flow->clear_serial != wv->clear_seen) {
    wv->clear_seen = flow->clear_serial;
    wv->run++;
    wv->bolt_run++;
    gr_waves_reset(wv);
    wv->announce_pending = false;
    log_info("waves: world cleared (run %)", fmt_u32(wv->run));
  }
  if (flow->drones_clear_serial != wv->drones_clear_seen) {
    wv->drones_clear_seen = flow->drones_clear_serial;
    wv->run++;
    wv->bolt_run++;
    log_info("drones: cleared (run %)", fmt_u32(wv->run));
  }
  if (flow->bolts_clear_serial != wv->bolts_clear_seen) {
    wv->bolts_clear_seen = flow->bolts_clear_serial;
    wv->bolt_run++;
    log_info("bolts: cleared");
  }
}

// the run's wave state: one resource, one writer; its spawns are rows from the next frame (the scope applies them)
ecs_system(gr_wave_director_system, EcsResInOut(GrWaves), EcsResIn(GrDev), EcsResIn(GrClock), EcsResIn(GrDroneRules),
           EcsResIn(GrDroneField), EcsResIn(GrPlayerState), EcsResIn(GrLevel), EcsResIn(GrDroneAssets)) {
  GrWaves *wv = ecs_sys_get_resource_mut(GrWaves);
  const GrDev *dev = ecs_sys_get_resource(GrDev);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const GrPlayerState *player = ecs_sys_get_resource(GrPlayerState);
  const GrLevel *lvl = ecs_sys_get_resource(GrLevel);
  const GrDroneAssets *assets = ecs_sys_get_resource(GrDroneAssets);
  u32 st = dev->state;
  wv->frame = clock->frame;
  wv->dt = clock->run_dt;
  wv->live = rules->ready && lvl->tables_ready && assets->ready && gr_run_live(st);
  wv->pinned = dev->pin_drones;
  if (!rules->ready || !lvl->tables_ready || !assets->ready)
    return;
  const GrWaveConfig *cfg = &rules->waves;
  // last frame's spawns became rows after the field was built: count them once more
  u32 unseen = wv->spawned_last_frame + 1u == clock->frame ? wv->spawned_last : 0u;
  u32 unseen_run = wv->spawned_last_run;
  wv->spawned_last = 0;
  wv->spawned_last_frame = clock->frame;

  // a run starts on entering play from outside a run, or on any run restart
  b32 entered = st == GR_STATE_PLAYING && wv->prev_state != GR_STATE_PLAYING && wv->prev_state != GR_STATE_PAUSED &&
                wv->prev_state != GR_STATE_ENDING;
  b32 restarted = dev->restart_serial != wv->restart_seen && st == GR_STATE_PLAYING;
  wv->prev_state = st;
  wv->restart_seen = dev->restart_serial;
  if (entered || restarted)
    gr_waves_start_run(wv, cfg, dev->hold_waves);
  if (wv->run != unseen_run)
    unseen = 0; // they belong to a retired run
  if (st != GR_STATE_PLAYING)
    return;

  // hold_waves: the script stops, drones already out stay
  if (dev->hold_waves && wv->phase != GR_WAVES_IDLE) {
    gr_waves_reset(wv);
    log_info("waves: held");
  }

  u32 spawned = 0;
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    if (!dev->drones[t].requested || wv->debug_spawned[t] == wv->run)
      continue;
    wv->debug_spawned[t] = wv->run;
    gr_waves_spawn(it->world, wv, rules, assets, t, dev->drones[t].position, "debug spawn");
    spawned++;
  }

  // this front's kills from the field's run total
  if (wv->kills_seen == ~0u || wv->kills_seen > field->kills_total)
    wv->kills_seen = field->kills_total;
  if (wv->phase == GR_WAVES_ACTIVE) {
    u32 fresh = field->kills_total - wv->kills_seen;
    wv->killed_phase += fresh;
    wv->quiet_time = fresh > 0 ? 0.0f : wv->quiet_time + clock->run_dt;
  }
  wv->kills_seen = field->kills_total;

  f32 dt = clock->run_dt;
  if (wv->announce_pending && dt > 0.0f) {
    wv->announce_timer -= dt;
    if (wv->announce_timer <= 0.0f) {
      wv->announce_pending = false;
      wv->announce_serial++;
      log_info("waves: incoming % \"%\"", fmt_i32(wv->announce_wave + 1),
               fmt_str(gr_str32(&cfg->waves.items[wv->announce_wave].name)));
    }
  }

  switch (wv->phase) {
  case GR_WAVES_INCOMING:
  case GR_WAVES_INTERMISSION:
    wv->timer -= dt;
    if (wv->timer <= 0.0f)
      km_phase_begin(wv);
    break;
  case GR_WAVES_ACTIVE:
    spawned += km_phase_tick(it->world, wv, rules, assets, field->alive + unseen + spawned, player->feet, dt);
    break;
  case GR_WAVES_SECURE:
    km_phase_secure(wv, player->feet, rules->debug.auto_secure);
    break;
  default:
    break;
  }
  wv->spawned_last = spawned;
  wv->spawned_last_run = wv->run;
  wv->remaining = field->alive + unseen + spawned;
}

// Settings' drone, wave and debug components copied for the systems when they change
ecs_system(gr_drone_config_system, EcsIn(GrDroneConfig), EcsIn(GrWaveConfig), EcsIn(GrDroneDebug),
           EcsResInOut(GrDroneRules)) {
  if (!ecs_iter_changed(it))
    return;
  GrDroneRules *r = ecs_sys_get_resource_mut(GrDroneRules);
  for (i32 i = 0; i < it->count; i++) {
    r->drones = *ecs_sys_get_component(GrDroneConfig, i);
    r->waves = *ecs_sys_get_component(GrWaveConfig, i);
    r->debug = *ecs_sys_get_component(GrDroneDebug, i);
    if (!r->ready)
      log_info("drones: config ready (% types, % waves, % attackers max)", fmt_u32(r->drones.types.len),
               fmt_u32(r->waves.waves.len), fmt_u32(r->drones.max_attackers));
    r->ready = r->drones.types.len == GR_DRONE_TYPE_COUNT && r->waves.waves.len > 0;
  }
}

// models/drones/<type>.glb, one model per type, indexed by GrDroneType
hz_internal const CString gr_drone_model_paths[GR_DRONE_TYPE_COUNT] = {
    "kamino/models/droids/b1.glb",
    "kamino/models/droids/b2.glb",
    "proving_grounds/models/drones/sentinel.glb",
};

// the type's <model_node>Muzzle socket and radius-tagged loose parts, from the cooked node table
hz_internal void gr_drone_model_read(GrDroneModel *dm, ModelBlobAsset *model, const GrDroneArchetype *a) {
  Array(ModelNodeBlobAsset) nodes = blob_array_get(ModelNodeBlobAsset, &model->nodes);
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  String muzzle = str_format(&ta.allocator, "%Muzzle", fmt_str(gr_str32(&a->model_node)));
  b32 found = false;
  for (u32 n = 0; n < nodes.len; n++) {
    ModelNodeBlobAsset *node = &nodes.items[n]; // string_blob_get takes it non-const
    String name = cstr_to_str(string_blob_get(&node->name));
    if (str_equal(name, muzzle)) {
      dm->muzzle = node->model_position;
      found = true;
      continue;
    }
    if (!(node->flags & MODEL_NODE_FLAG_MESH) || node->extras.len == 0)
      continue;
    GrDronePartExtras *extras = json_parse(GrDronePartExtras, string_blob_get(&node->extras), &ta.allocator);
    if (!extras || extras->radius <= 0.0f)
      continue;
    if (dm->part_count >= GR_MAX_DRONE_PARTS) {
      log_error("drones: % has more than % parts", fmt_str(gr_str32(&a->model_node)), fmt_u32(GR_MAX_DRONE_PARTS));
      break;
    }
    dm->parts[dm->part_count++] = (GrDronePart){
        .node = n, .radius = extras->radius, .position = node->model_position, .rotation = node->model_rotation};
  }
  if (!found)
    log_error("drones: % has no % socket", fmt_str(gr_str32(&a->model_node)), fmt_str(muzzle));
  tctx_temp_allocator_end(ta);
}

// each archetype's socket and parts once its model is ready
ecs_system(gr_drone_assets_system, EcsResInOut(GrDroneAssets), EcsResIn(AssetSystem), EcsResIn(GrDroneRules)) {
  GrDroneAssets *da = ecs_sys_get_resource_mut(GrDroneAssets);
  const AssetSystem *assets = ecs_sys_get_resource(AssetSystem);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  if (da->ready || !rules->ready)
    return;
  b32 all = true;
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    GrDroneModel *dm = &da->types[t];
    if (dm->ready)
      continue;
    ModelBlobAsset *model = asset_is_ready(assets, dm->model.handle)
                                ? asset_get(assets, HANDLE_CAST(HzHandleT(ModelBlobAsset), dm->model.handle), ModelBlobAsset)
                                : NULL;
    if (!model) {
      all = false;
      continue;
    }
    const GrDroneArchetype *a = &rules->drones.types.items[t];
    gr_drone_model_read(dm, model, a);
    dm->ready = true;
    log_info("drones: % model ready, muzzle (%, %, %), % parts", fmt_str(gr_label(a)), fmt_f32_p(dm->muzzle.x, 3),
             fmt_f32_p(dm->muzzle.y, 3), fmt_f32_p(dm->muzzle.z, 3), fmt_u32(dm->part_count));
  }
  da->ready = all;
}

void gr_drones_init(EcsWorld *w, AssetSystem *assets) {
  ecs_resource_set_zero(w, GrDroneAssets);
  GrDroneAssets *da = ecs_resource_get_mut(w, GrDroneAssets);
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    HzHandleT(ModelBlobAsset) model = asset_load(ModelBlobAsset, assets, .path = gr_drone_model_paths[t]);
    da->types[t].model = blob_asset_ref_from_handle(ModelBlobAsset, model);
  }
  ecs_resource_set_zero(w, GrWaves);
  ecs_resource_get_mut(w, GrWaves)->wave = -1;
  ecs_resource_set_zero(w, GrDroneTokens);
  ecs_resource_set_zero(w, GrDroneField);
  ecs_resource_set_zero(w, GrBoltHits);
  ecs_resource_set_zero(w, GrDroneRules);
  ecs_resource_set_zero(w, GrBoltRequests);
}

void gr_drones_schedule_spawn(EcsWorld *w) {
  ecs_schedule_single(w, gr_drone_config_system);   // writes GrDroneRules; one Settings entity
  ecs_schedule_single(w, gr_drone_assets_system);   // writes GrDroneAssets as each type's model becomes ready
  ecs_schedule_single(w, gr_bolt_pool_system);      // creates the bolt pool once (writes GrBoltRequests.pool)
  ecs_schedule_single(w, gr_waves_clear_system);    // writes GrWaves: the flow's clears retire runs before the director
  ecs_schedule_single(w, gr_wave_director_system);  // owns the run's wave state and its RNG
}

void gr_drones_schedule(EcsWorld *w) {
  ecs_schedule(w, gr_drone_debug_damage_system);
  ecs_schedule(w, gr_drone_damage_system);
  ecs_schedule(w, gr_drone_update_system);
  ecs_schedule_single(w, gr_drone_token_system);           // the shared attack budget, granted in row order (deterministic)
  ecs_schedule_single(w, gr_bolt_request_system);          // fire events into one list in drone row order (GrBoltRequests)
  ecs_schedule_unsafe_resource_access(w, gr_bolt_update_system); // shared write: only the atomic claim cursor
  ecs_schedule_single(w, gr_bolt_consolidate_system);      // writes GrBoltHits and the pool counts
  ecs_schedule_single(w, gr_drone_consolidate_system);     // writes GrDroneField
  ecs_schedule(w, gr_drone_intake_system);                 // the Player row's GrPlayerIntake
  ecs_schedule_single(w, gr_drone_log_system);             // the debug log lines
}
