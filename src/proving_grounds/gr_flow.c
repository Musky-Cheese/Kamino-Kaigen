// The run's totals and events (score, banners, run end, results, feedback flashes), the radar and the HUD model.

#define GR_POPUP_LIFE 1.4f    // kill popup lifetime
#define GR_BANNER_IN_S 0.5f   // the banner's entry animation
#define GR_RADAR_RANGE 40.0f  // the motion tracker's range (m)
#define GR_AIM_RANGE 120.0f   // the crosshair's world and drone rays

// a world direction's angle relative to the view: 0 ahead, + right
hz_internal f32 gr_relative_angle(v3 from, v3 eye, f32 yaw) {
  v3 rel = v3_sub(from, eye);
  f32 forward = -rel.x * m_sinf(yaw) - rel.z * m_cosf(yaw);
  f32 right = rel.x * m_cosf(yaw) - rel.z * m_sinf(yaw);
  return m_atan2f(right, forward);
}

hz_internal void gr_run_add_score(GrRun *r, u32 points) {
  r->score += points;
  r->score_delta = points;
  r->score_serial++;
}

// the front's sector name (the HUD's wave line)
hz_internal CString gr_wave_label(i32 index) {
  return index >= 0 && index < KM_PHASE_COUNT ? km_sector_name(km_phases[index].sector) : km_sector_name(KM_SECTOR_BESH);
}

hz_internal CString gr_weapon_label(u32 weapon) {
  switch (weapon) {
  case GR_WEAPON_BREACHER: return "DC-17M SCATTER";
  case GR_WEAPON_LANCER: return "DC-15X SNIPER";
  default: return "DC-15A BLASTER";
  }
}

hz_internal CString gr_drone_label(u32 type) {
  switch (type) {
  case GR_DRONE_STRIKER: return "SUPER BATTLE DROID";
  case GR_DRONE_SENTINEL: return "VULTURE DROID";
  default: return "BATTLE DROID";
  }
}

// a banner: `duration` counts from the start, the entry animation included
hz_internal void gr_run_banner(EcsWorld *w, GrBannerText text, GrBannerKind kind, i32 value, f32 duration) {
  HudMsg m = {.channel = HUD_MSG_BANNER, .kind = (u32)kind, .duration = m_maxf(duration - GR_BANNER_IN_S, 0.0f)};
  StringBuilder tb, sb;
  sb_init(&tb, m.title.value, sizeof(m.title.value));
  sb_init(&sb, m.body.value, sizeof(m.body.value));
  switch (text) {
  case GR_BANNER_WAVE:
    sb_append(&tb, gr_wave_label(value));
    sb_append(&sb, value == 0 ? "PUSH THEM OFF THE PLATFORMS" : "THE FRONT ADVANCES");
    break;
  case GR_BANNER_WAVE_CLEARED:
    sb_append(&tb, "SECTOR SECURED");
    sb_append_format(&sb, "+% BONUS", fmt_i32(value));
    break;
  case GR_BANNER_FALLBACK:
    sb_append(&tb, "THEY'RE FALLING BACK");
    sb_append_format(&sb, "SECURE %", fmt_cstr(gr_wave_label(value)));
    break;
  case GR_BANNER_TRIAL_COMPLETE:
    sb_append(&tb, "TIPOCA CITY SECURED");
    sb_append_format(&sb, "ACCURACY BONUS +%", fmt_i32(value));
    break;
  default:
    sb_append(&tb, "TROOPER DOWN");
    sb_append(&sb, "THE LINE HAS BROKEN");
    break;
  }
  m.title.len = (u32)tb.len;
  m.body.len = (u32)sb.len;
  hud_msg_push(w, &m);
}

// the incoming-wave announcement
hz_internal void gr_run_announce(EcsWorld *w, GrRun *r, i32 wave, u32 total) {
  r->wave_index = wave;
  r->wave_total = total;
  gr_run_banner(w, GR_BANNER_WAVE, GR_BANNER_KIND_WAVE, wave, 2.6f);
}

// the run end's score and banner (audio plays its sound); the state change is the flow's next frame
hz_internal void gr_run_end(EcsWorld *w, GrRun *r, b32 victory, u32 bonus_per_percent) {
  if (victory) {
    u32 bonus = (u32)m_roundf(r->accuracy * 100.0f) * bonus_per_percent;
    gr_run_add_score(r, bonus);
    gr_run_banner(w, GR_BANNER_TRIAL_COMPLETE, GR_BANNER_KIND_CLEAR, (i32)bonus, 3.2f);
  } else {
    gr_run_banner(w, GR_BANNER_SIGNAL_LOST, GR_BANNER_KIND_ALERT, 0, 2.4f);
  }
  r->end_serial++;
  r->end_victory = victory;
  r->ended_run = r->run_serial;
  log_info("run: end (victory %, score %)", fmt_b32(victory), fmt_u32(r->score));
}

// the run's totals from the gameplay read models; one writer for score, banners and the end request
ecs_system(gr_run_consolidate, EcsResInOut(GrRun), EcsResIn(GrFlow), EcsResIn(GrClock), EcsResIn(GrDev),
           EcsResIn(GrPlayerState), EcsResIn(GrWeaponState), EcsResIn(GrWaves), EcsResIn(GrDroneField)) {
  GrRun *r = ecs_sys_get_resource_mut(GrRun);
  const GrFlow *f = ecs_sys_get_resource(GrFlow);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  const GrDev *d = ecs_sys_get_resource(GrDev);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponState *wp = ecs_sys_get_resource(GrWeaponState);
  const GrWaves *wv = ecs_sys_get_resource(GrWaves);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const GrDroneRules *rules = ecs_resource_get(it->world, GrDroneRules); // config, settled in the first flush

  // a new run resets the run fields
  if (d->restart_serial != r->run_serial) {
    r->run_serial = d->restart_serial;
    r->score = 0;
    r->kills = 0;
    r->run_time = 0.0f;
    r->score_delta = 0;
    r->score_serial++;
    r->feedback.damage = 0.0f;
    r->feedback.heal = 0.0f;
  }
  b32 playing = f->state == GR_STATE_PLAYING;
  b32 ended = r->ended_run == r->run_serial;
  r->accuracy = wp->accuracy; // shots with at least one hit / shots

  // run time; the wave announcement's delay runs on run time in the wave director
  if (c->live)
    r->run_time += c->run_dt;

  // the run events (audio plays their sounds): kills are this frame's GrDroneField kills
  if (field->kill_serial != r->seen_kills) {
    r->seen_kills = field->kill_serial;
    for (u32 k = 0; k < field->kill_count; k++) {
      r->kills++;
      gr_run_add_score(r, field->kills[k].score);
    }
  }
  // damage and health pickups are this frame's GrPlayerState events
  for (u32 h = 0; h < p->events.hit_count; h++)
    r->feedback.damage = m_minf(1.0f, r->feedback.damage + 0.3f + p->events.hits[h].amount * 0.03f);
  if (p->events.healed > 0.0f)
    r->feedback.heal = 1.0f;
  // wave incoming (the director already holds later waves for announce_delay of run time), started, cleared
  if (wv->announce_serial != r->seen_incoming) {
    r->seen_incoming = wv->announce_serial;
    gr_run_announce(it->world, r, wv->announce_wave, wv->total_waves);
  }
  if (wv->started_serial != r->seen_started) {
    r->seen_started = wv->started_serial;
    r->wave_index = wv->wave;
    r->wave_total = wv->total_waves;
  }
  if (wv->fallback_serial != r->seen_fallback) {
    r->seen_fallback = wv->fallback_serial;
    gr_run_banner(it->world, GR_BANNER_FALLBACK, GR_BANNER_KIND_CLEAR, wv->wave, 3.0f);
  }
  if (wv->cleared_serial != r->seen_cleared) {
    r->seen_cleared = wv->cleared_serial;
    gr_run_add_score(r, wv->cleared_bonus);
    r->slow_motion_serial++;
    if (wv->cleared_wave < (i32)wv->total_waves - 1)
      gr_run_banner(it->world, GR_BANNER_WAVE_CLEARED, GR_BANNER_KIND_CLEAR, (i32)wv->cleared_bonus, 2.2f);
  }
  // the player's death or the last wave's clear (victory) end a live run once
  u32 bonus_per_percent = rules->waves.accuracy_bonus_per_percent;
  if (p->events.died && playing && !ended) {
    gr_run_end(it->world, r, false, bonus_per_percent);
    ended = true;
  }
  if (wv->victory_serial != r->seen_over) {
    r->seen_over = wv->victory_serial;
    if (playing && !ended) {
      gr_run_end(it->world, r, true, bonus_per_percent);
      ended = true;
    }
  }

  // the results payload; best is per session
  if (f->results_serial != r->results_seen) {
    r->results_seen = f->results_serial;
    b32 new_best = r->score > r->best;
    if (new_best)
      r->best = r->score;
    r->results = (GrResults){
        .victory = f->victory,
        .score = r->score,
        .best = r->best,
        .new_best = new_best,
        .kills = r->kills,
        .accuracy = r->accuracy,
        .waves_cleared = wv->cleared,
        .total_waves = wv->total_waves,
        .seconds = r->run_time,
    };
  }

  // the feedback flashes, on the presentation dt
  b32 in_run = playing || f->state == GR_STATE_PAUSED || f->state == GR_STATE_ENDING;
  r->feedback.damage = m_maxf(0.0f, r->feedback.damage - c->dt * 2.2f);
  r->feedback.heal = m_maxf(0.0f, r->feedback.heal - c->dt * 1.6f);
  f32 health = p->health / GR_PLAYER_MAX_HEALTH;
  r->feedback.low_health = in_run && p->alive ? m_maxf(0.0f, (0.35f - health) / 0.35f) : 0.0f;
  r->feedback.scope = in_run && p->alive ? wp->scope : 0.0f;
}

// motion-tracker contact from a world point, relative to the player's feet
hz_internal GrRadarContact gr_radar_contact(v3 at, v3 feet, f32 dy, GrRadarIcon icon, u32 color) {
  return (GrRadarContact){.x = at.x - feet.x, .z = at.z - feet.z, .dy = dy, .icon = icon, .color = color};
}

// the radar redraws only while the run updates: drones, then guns on the ground and packs available
ecs_system(gr_radar_system, EcsResInOut(GrRadar), EcsResIn(GrClock), EcsResIn(GrFlow), EcsResIn(GrPlayerState),
           EcsResIn(GrPickupState), EcsResIn(GrDroneField)) {
  GrRadar *radar = ecs_sys_get_resource_mut(GrRadar);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  const GrFlow *f = ecs_sys_get_resource(GrFlow);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrPickupState *pk = ecs_sys_get_resource(GrPickupState);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  radar->gathering = c->live || f->state == GR_STATE_ENDING;
  if (!radar->gathering)
    return;
  radar->heading = -p->yaw;
  radar->sweep = m_fmodf(radar->sweep + c->run_dt * 2.2f, 2.0f * M_PI);
  u32 n = 0;
  for (u32 i = 0; i < field->count && n < GR_RADAR_MAX_CONTACTS; i++) {
    v3 at = field->drones[i].position;
    radar->contacts[n++] = gr_radar_contact(at, p->feet, at.y - p->eye.y, GR_RADAR_DOT, field->drones[i].type);
  }
  for (u32 i = 0; i < GR_GUN_COUNT && n < GR_RADAR_MAX_CONTACTS; i++) {
    if (pk->in_world[i])
      radar->contacts[n++] = gr_radar_contact(pk->base[i], p->feet, pk->base[i].y - p->feet.y, GR_RADAR_DIAMOND,
                                              GR_RADAR_COLOR_GUN);
  }
  for (u32 i = 0; i < GR_PACK_COUNT && n < GR_RADAR_MAX_CONTACTS; i++) {
    if (pk->pack_present[i])
      radar->contacts[n++] = gr_radar_contact(pk->pack_base[i], p->feet, pk->pack_base[i].y - p->feet.y,
                                              GR_RADAR_CROSS, GR_RADAR_COLOR_PACK);
  }
  radar->count = n;
}

// the hitmarker sound for a hit that killed nothing (kill sounds are audio's), on the flow's own emitter
ecs_system(gr_flow_voice_system, EcsInOut(GrFlowVoice), EcsInOut(GrSfxEmitter), EcsResIn(GrWeaponState),
           EcsResIn(GrDroneField), EcsResIn(GrClock)) {
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  b32 hit = false;
  for (u32 s = 0; s < ev->shot_count; s++)
    hit = hit || ev->shots[s].hit;
  b32 killed = field->kill_frame == c->frame && field->kill_count > 0;
  if (!hit || killed)
    return;
  for (i32 i = 0; i < it->count; i++) {
    ecs_sys_get_component_mut(GrFlowVoice, i)->hitmarkers++;
    gr_sfx_emit(ecs_sys_get_component_mut(GrSfxEmitter, i), c->frame, gr_sfx_site(GR_SITE_HITMARKER));
  }
}

// the swap prompt's widget follows the HUD model, written only when it changes
ecs_system(gr_prompt_system, EcsInOut(WorldUiPrompt), EcsInOut(WorldUiLabel), EcsWith(GrPromptTag), EcsResIn(GrHud),
           EcsResIn(GrInput)) {
  const GrHud *h = ecs_sys_get_resource(GrHud);
  // on touch the SWAP button is the key
  App_InputButtonType key = ecs_sys_get_resource(GrInput)->touch ? KEY_MAX : KEY_E;
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(WorldUiPrompt, i)->key != key)
      ecs_sys_get_component_mut(WorldUiPrompt, i)->key = key;
    if (ecs_sys_get_component(WorldUiPrompt, i)->enabled != h->prompt_show)
      ecs_sys_get_component_mut(WorldUiPrompt, i)->enabled = h->prompt_show;
    if (!h->prompt_show)
      continue;
    CString name = gr_weapon_label(h->prompt_weapon);
    const WorldUiLabel *l = ecs_sys_get_component(WorldUiLabel, i);
    u32 len = (u32)cstr_len(name);
    if (l->sub.len != len || mem_cmp(l->sub.value, name, len) != 0) {
      WorldUiLabel *out = ecs_sys_get_component_mut(WorldUiLabel, i);
      mem_cpy(out->sub.value, name, len);
      out->sub.value[len] = 0;
      out->sub.len = len;
    }
  }
}

// the HUD's per-frame values and its event-driven pieces (hitmarker, popups, damage arcs, score pop)
ecs_system(gr_hud_consolidate, EcsResInOut(GrHud), EcsResIn(GrRun), EcsResIn(GrFlow), EcsResIn(GrClock),
           EcsResIn(GrPlayerState), EcsResIn(GrWeaponState), EcsResIn(GrWaves), EcsResIn(GrDroneField)) {
  GrHud *h = ecs_sys_get_resource_mut(GrHud);
  const GrRun *r = ecs_sys_get_resource(GrRun);
  const GrFlow *f = ecs_sys_get_resource(GrFlow);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponState *wp = ecs_sys_get_resource(GrWeaponState);
  const GrWaves *wv = ecs_sys_get_resource(GrWaves);
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  f32 now = c->time;

  h->time = now;
  h->state = f->state;
  h->load_stage = f->load_stage;
  h->load_progress = f->load_progress;
  h->hud_visible = f->state == GR_STATE_PLAYING || f->state == GR_STATE_PAUSED || f->state == GR_STATE_ENDING;
  h->best = r->best;
  h->results = r->results;
  h->wave_index = r->wave_index;
  h->wave_total = r->wave_total;

  // a score change pops its delta
  if (h->seen_score != r->score_serial) {
    h->seen_score = r->score_serial;
    h->score = r->score;
    if (r->score_delta > 0) {
      h->score_delta = r->score_delta;
      h->score_pop_start = now;
    }
  }
  // a shot this frame damaged a drone; the drone damage applied in this flush says whether one died
  for (u32 s = 0; s < wp->events.shot_count; s++) {
    if (!wp->events.shots[s].hit)
      continue;
    h->hitmarker_start = now;
    h->hitmarker_kill = field->kill_frame == c->frame && field->kill_count > 0;
    break;
  }
  // one popup per kill this frame, stacked above the live ones
  if (field->kill_serial != h->seen_kills) {
    h->seen_kills = field->kill_serial;
    for (u32 k = 0; k < field->kill_count; k++) {
      WorldUiPopupDesc d = {.screen_anchored = true,
                            .screen_frac = {0.5f, 0.5f},
                            .screen_px = {40.0f, -34.0f},
                            .color = {1.0f, 1.0f, 1.0f, 1.0f},
                            .life = GR_POPUP_LIFE,
                            .drift_px = {6.0f, -26.0f},
                            .enter_px = {-8.0f, 8.0f},
                            .enter_frac = 0.12f,
                            .fade_from = 0.75f,
                            .scale_from = 1.4f,
                            .stack_group = 1,
                            .stack_size = {120.0f, 24.0f}};
      StringBuilder sb;
      sb_init(&sb, d.text.value, sizeof(d.text.value));
      sb_append_format(&sb, "+% %", fmt_u32(field->kills[k].score), fmt_cstr(gr_drone_label(field->kills[k].type)));
      d.text.len = (u32)sb.len;
      world_ui_popup(it->world, &d);
    }
  }
  for (u32 i = 0; i < p->events.hit_count; i++) {
    h->arcs[h->arc_count % GR_HUD_MAX_ARCS] =
        (GrDamageArc){.start = now, .angle = gr_relative_angle(p->events.hits[i].from, p->eye, p->yaw)};
    h->arc_count++;
  }

  // the run start's HUD reset and the per-frame values: only while the run updates
  b32 restarted = h->run_serial != r->run_serial;
  h->run_serial = r->run_serial;
  if (!(c->live || f->state == GR_STATE_ENDING || restarted))
    return;
  h->health = p->health;
  h->max_health = GR_PLAYER_MAX_HEALTH;
  h->critical = p->health / GR_PLAYER_MAX_HEALTH < 0.3f;
  h->weapon = wp->gun;
  h->crosshair_style = wp->crosshair;
  h->ammo = wp->ammo;
  h->magazine = wp->magazine;
  h->reloading = wp->reloading;
  h->swapping = wp->swapping_magazine;
  h->ammo_low = !wp->reloading && (f32)wp->ammo <= (f32)wp->magazine * 0.25f;
  h->reload_show = wp->reloading || h->ammo_low;
  h->hostiles = restarted ? 0 : wv->remaining;
  h->prompt_show = wp->prompt_gun >= 0;
  h->prompt_weapon = wp->prompt_gun >= 0 ? (u32)wp->prompt_gun : 0;
  h->reticle_spread = wp->reticle_spread;
  h->fov = p->fov > 0.0f ? p->fov : GR_FOV; // the camera's eased fov
  // the world ray first, then the drones nearer than the wall (the static layer is read-only after the build)
  GrRayHit wall = {0};
  f32 reach = gr_level_raycast(gr_level_collision(it->world), p->eye, p->view_forward, GR_AIM_RANGE, &wall) ? wall.distance
                                                                                                         : GR_AIM_RANGE;
  GrDroneRayHit target = {0};
  h->on_target = p->ready && gr_drones_raycast(field, p->eye, p->view_forward, reach, &target);
  h->crosshair_hidden = p->zoom > 0.6f || p->sprinting;
}

void gr_flow_init(EcsWorld *w) {
  // High until the menu or the quality switch sets one; its height fog is the shadowed froxel volume
  ecs_resource_set(w, GrSettings, ((GrSettings){.quality = GR_QUALITY_HIGH, .sensitivity = 1.0f, .volume = 0.8f}));
  ecs_resource_set_zero(w, GrActions);
  ecs_resource_set(w, GrRun, ((GrRun){.wave_total = GR_WAVE_TOTAL}));
  // the HUD's initial values, rifle in hand
  ecs_resource_set(w, GrHud,
                   ((GrHud){.wave_total = GR_WAVE_TOTAL,
                            .score_pop_start = -1.0f,
                            .hitmarker_start = -1.0f,
                            .health = 100.0f,
                            .max_health = 100.0f,
                            .ammo = 36,
                            .magazine = 36,
                            .fov = 66.0f}));
  ecs_resource_set_zero(w, GrRadar);
  ecs_resource_set(w, GrLoadGates, ((GrLoadGates){.light = true}));
  EcsEntity voice = ecs_entity_create(w);
  ecs_component_set(w, voice, GrFlowVoice, {0});
  ecs_component_set(w, voice, GrSfxEmitter, {0});
  // the swap prompt: at the centre, 92 px down, a key cap and "SWAP <gun>"; the HUD model enables it
  EcsEntity prompt = ecs_entity_create(w);
  world_ui_add(w, prompt,
               &(WorldUi){.kind = WORLD_UI_PROMPT, .screen_anchored = true, .screen_frac = {0.5f, 0.5f},
                          .screen_px = {0.0f, 92.0f}, .fade_s = 0.12f},
               &(WorldUiLabel){.text = fixedstr64_from_cstr("SWAP ")});
  ecs_component_set(w, prompt, WorldUiPrompt, {.key = KEY_E});
  ecs_component_add(w, prompt, GrPromptTag);
}

void gr_flow_collect(EcsWorld *w) {
  GrActions *a = ecs_resource_get_mut(w, GrActions);
  const GrLoadGates *g = ecs_resource_get(w, GrLoadGates);
  a->pipelines_ready = renderer_pipelines_ready(ecs_resource_get(w, RendererState));
  a->models = ecs_resource_get(w, GrLevel)->meshes_ready ? 1.0f : 0.0f;
  a->audio = ecs_resource_get(w, GrAudio)->ready; // audio renders the clips at load ("Synthesizing audio")
  a->light = g->light;
  a->load_failed = g->failed;
}

// settings onto their owners: sensitivity onto the player, written only when it differs
ecs_system(gr_settings_player_system, EcsInOut(GrPlayer), EcsResIn(GrSettings)) {
  f32 sensitivity = ecs_sys_get_resource(GrSettings)->sensitivity;
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(GrPlayer, i)->sensitivity != sensitivity)
      ecs_sys_get_component_mut(GrPlayer, i)->sensitivity = sensitivity;
  }
}

void gr_flow_schedule_settings(EcsWorld *w) { ecs_schedule(w, gr_settings_player_system); }

void gr_flow_schedule_consolidate(EcsWorld *w) {
  ecs_schedule_single(w, gr_run_consolidate);    // writes the GrRun resource (score, banners, end request)
  ecs_schedule_single(w, gr_radar_system);       // writes the GrRadar resource from the consolidated drone field
  ecs_schedule_single(w, gr_hud_consolidate);    // writes the GrHud resource
  ecs_schedule(w, gr_prompt_system);             // the swap prompt widget from the HUD model
  ecs_schedule(w, gr_flow_voice_system);
}

void gr_flow_main(EcsWorld *w, u32 *lock_seen) {
  const GrFlow *f = ecs_resource_get(w, GrFlow);
  if (f->lock_serial != *lock_seen) {
    *lock_seen = f->lock_serial;
    // touch has no pointer to lock; a later mouse click locks through the input policy
    if (!ecs_resource_get(w, GrInput)->touch)
      os_lock_mouse(f->lock_value);
  }
}
