// Weapon pickups and health packs, the one-time gun model build, init and the schedule.

#define GR_PICKUP_SPIN 0.9f // pickup spin, rad/s
#define GR_PACK_SPIN 1.2f   // health pack spin, rad/s

hz_internal const CString gr_pickup_names[GR_GUN_COUNT] = {"PickupRifle", "PickupBreacher", "PickupLancer"};
hz_internal const CString gr_pack_names[GR_PACK_COUNT] = {"HealthPack0", "HealthPack1"};

// a placement's bob/spin phase: deterministic per (thing, placement)
hz_internal f32 gr_placement_phase(u32 thing, u32 placement) {
  u64 h = random_seed_mix(((u64)thing << 32) | placement);
  return (f32)(h & 0xffffffu) / (f32)0x1000000u * 2.0f * M_PI;
}

// every gun lying in the world follows the weapon's take/drop events and spins and bobs over its base
ecs_system(gr_pickup_system, EcsInOut(GrPickup), EcsInOut(WorldTransform), EcsResIn(GrWeaponState), EcsResIn(GrLevel),
           EcsResIn(GrClock), EcsResIn(GrWeaponRules)) {
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const GrLevel *lvl = ecs_sys_get_resource(GrLevel);
  const GrWeaponRules *rules = ecs_sys_get_resource(GrWeaponRules);
  const GrWeaponConfig *cfg = &rules->weapons;
  f32 time = ecs_sys_get_resource(GrClock)->time;
  // the pedestals and the guns' config come from the scene: nothing to place before both
  if (!lvl->tables_ready || !rules->ready)
    return;
  for (i32 i = 0; i < it->count; i++) {
    GrPickup *pk = ecs_sys_get_component_mut(GrPickup, i);
    // reset: each non-starting gun on its own pedestal (also at construction, behind the title)
    if (pk->placements == 0 || ev->run_reset) {
      pk->home = -1;
      for (u32 s = 0; s < GR_LEVEL_PEDESTAL_COUNT; s++)
        if (lvl->pedestals[s].weapon == pk->gun && pk->gun != cfg->starting_gun)
          pk->home = (i32)s;
      pk->in_world = pk->home >= 0;
      pk->pedestal = pk->home;
      if (pk->in_world)
        pk->base = lvl->pedestals[pk->home].position;
      pk->phase = gr_placement_phase(pk->gun, ++pk->placements);
    }
    if (ev->taken_gun == (i32)pk->gun)
      pk->in_world = false;
    if (ev->dropped_gun == (i32)pk->gun) {
      pk->in_world = true;
      pk->base = ev->drop_base;
      pk->pedestal = -1;
      pk->phase = gr_placement_phase(pk->gun, ++pk->placements);
    }

    f32 lift = pk->pedestal >= 0 ? lvl->pedestals[pk->pedestal].top + cfg->pedestal_hover - cfg->drop_hover : 0.0f;
    f32 y = cfg->drop_hover + lift + m_sinf(time * 2.0f + pk->phase) * 0.05f;
    WorldTransform *xf = ecs_sys_get_component_mut(WorldTransform, i);
    xf->position = v3_add(pk->base, (v3){0.0f, y, 0.0f});
    xf->rotation = quat_rotate_y(time * GR_PICKUP_SPIN + pk->phase);
    xf->scale = v3_fill(cfg->display_scale);
  }
}

// the guns on the ground into GrPickupState (radar, pedestal glow, next frame's interaction)
ecs_system(gr_pickup_consolidate_system, EcsIn(GrPickup), EcsResInOut(GrPickupState), EcsResIn(GrClock)) {
  GrPickupState *ps = ecs_sys_get_resource_mut(GrPickupState);
  f32 time = ecs_sys_get_resource(GrClock)->time;
  for (i32 i = 0; i < it->count; i++) {
    const GrPickup *pk = ecs_sys_get_component(GrPickup, i);
    ps->in_world[pk->gun] = pk->in_world;
    ps->base[pk->gun] = pk->base;
    ps->pedestal[pk->gun] = pk->in_world ? pk->pedestal : -1;
    ps->home[pk->gun] = pk->home;
  }
  // an occupied pedestal pulses, an empty one idles
  for (u32 s = 0; s < GR_PEDESTAL_COUNT; s++) {
    ps->pedestal_occupied[s] = false;
    for (u32 g = 0; g < GR_GUN_COUNT; g++)
      ps->pedestal_occupied[s] |= ps->in_world[g] && ps->pedestal[g] == (i32)s;
    ps->pedestal_glow[s] = ps->pedestal_occupied[s] ? 2.6f + m_sinf(time * 3.0f) * 0.4f : 0.35f;
  }
}

// health packs: respawn timers, the walk-over heal, and the spin and bob (also behind the title)
ecs_system(gr_health_pack_system, EcsInOut(GrHealthPack), EcsInOut(WorldTransform), EcsInOut(EcsVisibility),
           EcsResIn(GrPlayerState), EcsResIn(GrWeaponState), EcsResIn(GrDev), EcsResIn(GrClock),
           EcsResIn(GrWeaponRules)) {
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  // packs update while playing only, not in the ending outro
  b32 live = ecs_sys_get_resource(GrDev)->state == GR_STATE_PLAYING && clock->run_dt > 0.0f;
  for (i32 i = 0; i < it->count; i++) {
    GrHealthPack *pack = ecs_sys_get_component_mut(GrHealthPack, i);
    pack->healed = 0.0f;
    if (ev->run_reset)
      pack->cooldown = 0.0f;
    if (live) {
      if (pack->cooldown > 0.0f) {
        pack->cooldown = m_maxf(0.0f, pack->cooldown - clock->run_dt);
      } else if (m_absf(pack->base.y - p->feet.y) <= cfg->pack_reach_height &&
                 m_sqrtf((pack->base.x - p->feet.x) * (pack->base.x - p->feet.x) +
                         (pack->base.z - p->feet.z) * (pack->base.z - p->feet.z)) <= cfg->pack_reach) {
        // the heal is what it actually restores; at full health the player walks through
        f32 healed = p->alive ? m_minf(cfg->pack_heal, m_maxf(0.0f, GR_PLAYER_MAX_HEALTH - p->health)) : 0.0f;
        if (healed > 0.0f) {
          pack->healed = healed;
          pack->cooldown = cfg->pack_respawn;
        }
      }
    }
    f32 time = clock->time;
    WorldTransform *xf = ecs_sys_get_component_mut(WorldTransform, i);
    xf->position = v3_add(pack->base, (v3){0.0f, cfg->pack_hover + m_sinf(time * 2.0f + pack->phase) * 0.05f, 0.0f});
    xf->rotation = quat_rotate_y(time * GR_PACK_SPIN + pack->phase);
    b32 visible = pack->cooldown <= 0.0f;
    if (ecs_sys_get_component(EcsVisibility, i)->visible != visible)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = visible;
  }
}

// the packs into GrPickupState: the heal the player takes this frame, the heal event, radar locations
ecs_system(gr_pack_consolidate_system, EcsIn(GrHealthPack), EcsResInOut(GrPickupState), EcsResIn(GrClock)) {
  GrPickupState *ps = ecs_sys_get_resource_mut(GrPickupState);
  ps->heal = 0.0f;
  ps->heal_event = false;
  for (i32 i = 0; i < it->count; i++) {
    const GrHealthPack *pack = ecs_sys_get_component(GrHealthPack, i);
    if (pack->index >= GR_PACK_COUNT)
      continue;
    ps->pack_present[pack->index] = pack->cooldown <= 0.0f;
    ps->pack_base[pack->index] = pack->base;
    ps->pack_cooldown[pack->index] = pack->cooldown;
    // the packs stand tens of metres apart (reach 1.1 m): at most one heals on a frame
    if (pack->healed > 0.0f) {
      ps->heal += pack->healed;
      ps->heal_event = true;
      ps->heal_position = pack->base;
      log_info("pickup: health +% at t % (pack % at %, %, %)", fmt_f32(pack->healed),
               fmt_f32(ecs_sys_get_resource(GrClock)->time), fmt_u32(pack->index), fmt_f32(pack->base.x),
               fmt_f32(pack->base.y), fmt_f32(pack->base.z));
    }
  }
}

// cooked submeshes keep the glTF mesh names: a node's mesh may carry a ".001" suffix (MagCell.001)
hz_internal GrPartKind gr_part_kind(CString name) {
  if (cstr_equal(name, "Magazine") || str_starts_with(cstr_to_str(name), STR_FROM_CSTR("MagCell")))
    return GR_PART_MAGAZINE;
  if (cstr_equal(name, "ChargingHandle"))
    return GR_PART_HANDLE;
  if (cstr_equal(name, "Pump"))
    return GR_PART_PUMP;
  if (cstr_equal(name, "Shell"))
    return GR_PART_SHELL;
  return GR_PART_STATIC;
}

hz_internal void gr_spawn_part(EcsWorld *w, RendererState *r, HzHandleT(RendererSubMesh) submesh, CString name,
                               EcsEntity parent, GrGunPart part) {
  EcsEntity e = ecs_renderer_spawn(
      w, r,
      &(EcsRendererSpawnDesc){.submesh = submesh,
                              .transform = {.position = part.rest, .rotation = quat_identity(), .scale = {1, 1, 1}},
                              .casts_shadow = part.world_copy, // the first-person gun casts no shadow
                              .hidden = true,
                              .name = name});
  ecs_component_set(w, e, EcsParent, {.parent = parent});
  ecs_component_set(w, e, GrGunPart, part);
  if (part.world_copy)
    gr_look_probe_capture(w, e);
  else
    gr_look_viewmodel(w, e);
}

hz_internal const CString gr_socket_names[GR_SOCKET_COUNT] = {"Muzzle", "EjectionPort", "Sight", "LoadPort"};

// sockets and Shell rest from the node table EcsModelNode children come from: the guns are part instances, no model root
hz_internal void gr_read_sockets(GrWeaponAssets *wa, u32 gun, ModelBlobAsset *model) {
  Array(ModelNodeBlobAsset) nodes = blob_array_get(ModelNodeBlobAsset, &model->nodes);
  u32 found = 0;
  for (u32 n = 0; n < nodes.len; n++) {
    ModelNodeBlobAsset *node = &nodes.items[n];
    CString name = string_blob_get(&node->name);
    if (!name)
      continue;
    if (gun == GR_WEAPON_BREACHER && cstr_equal(name, "Shell"))
      wa->shell_rest = node->model_position;
    for (u32 s = 0; s < GR_SOCKET_COUNT; s++) {
      if (!cstr_equal(name, gr_socket_names[s]))
        continue;
      wa->sockets[gun][s] = node->model_position;
      found |= 1u << s;
      log_info("weapons: % socket % at (%, %, %)", fmt_cstr(gr_pickup_names[gun]), fmt_cstr(name),
               fmt_f32_p(node->model_position.x, 4), fmt_f32_p(node->model_position.y, 4),
               fmt_f32_p(node->model_position.z, 4));
    }
  }
  u32 needed = gun == GR_WEAPON_BREACHER ? 0xfu : 0x7u;
  if ((found & needed) != needed)
    log_error("weapons: % lacks sockets (found mask %, needs %)", fmt_cstr(gr_pickup_names[gun]), fmt_u32(found),
              fmt_u32(needed));
}

hz_internal BlobAssetRef(ModelBlobAsset) gr_model_ref(HzHandleT(ModelBlobAsset) h) {
  return (BlobAssetRef(ModelBlobAsset)){
      .type_size = sizeof(ModelBlobAsset), .typehash = TYPE_HASH(ModelBlobAsset), .handle = HANDLE_CAST(HzHandle, h)};
}

// one-time: each gun split into one submesh entity per part (a model instance's submeshes share one transform)
ecs_system(gr_weapon_build_system, EcsResInOut(GrWeaponAssets), EcsResInOut(RendererState), EcsResIn(AssetSystem),
           EcsResIn(GrLevel)) {
  GrWeaponAssets *wa = ecs_sys_get_resource_mut(GrWeaponAssets);
  const AssetSystem *a = ecs_sys_get_resource(AssetSystem);
  const GrLevel *lvl = ecs_sys_get_resource(GrLevel);
  // health packs on the scene's floor points, once the level's tables are gathered
  if (!wa->packs_spawned && lvl->tables_ready) {
    for (u32 k = 0; k < GR_PACK_COUNT; k++) {
      v3 base = lvl->health_packs[k];
      EcsEntity e = ecs_renderer_spawn(
          it->world, NULL,
          &(EcsRendererSpawnDesc){.model_asset = gr_model_ref(wa->healthpack),
                                  .transform = {.position = base, .rotation = quat_identity(), .scale = {1, 1, 1}},
                                  .casts_shadow = true,
                                  .name = gr_pack_names[k]});
      ecs_component_set(it->world, e, GrHealthPack, {.index = k, .base = base, .phase = gr_placement_phase(GR_GUN_COUNT + k, 1)});
      gr_look_probe_capture(it->world, e);
    }
    wa->packs_spawned = true;
  }
  if (!wa->clips_ready) {
    b32 ready = true;
    for (u32 c = 0; c < GR_ARMS_CLIP_COUNT; c++)
      ready &= asset_is_ready(a, wa->clips[c]);
    if (ready) {
      for (u32 c = 0; c < GR_ARMS_CLIP_COUNT; c++)
        wa->clip_blobs[c] = asset_get(a, wa->clips[c], AnimationBlobAsset);
      wa->clips_ready = true;
    }
  }
  if (wa->built)
    return;
  for (u32 g = 0; g < GR_GUN_COUNT; g++)
    if (!asset_is_ready(a, wa->guns[g]))
      return;
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  EcsWorld *w = it->world;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 parts = 0;
  for (u32 g = 0; g < GR_GUN_COUNT; g++) {
    ModelBlobAsset *model = asset_get(a, wa->guns[g], ModelBlobAsset);
    gr_read_sockets(wa, g, model);
    HzHandleT(RendererModel) rm = renderer_upload_model_asset_v2(r, a, wa->guns[g]);
    Array(MeshBlobAsset) meshes = blob_array_get(MeshBlobAsset, &model->meshes);
    u32 count = 0;
    for (u32 m = 0; m < meshes.len; m++)
      count += blob_array_len(&meshes.items[m].submeshes);
    // the renderer model is a borrowed pointer: copy its submesh handles before instantiating
    HzHandleT(RendererSubMesh) *handles = ALLOC_ARRAY(&ta.allocator, HzHandleT(RendererSubMesh), count);
    CString *names = ALLOC_ARRAY(&ta.allocator, CString, count);
    aabb bounds = aabb_invalidate(); // the world copy without the loose Shell
    const RendererModel *rmodel = renderer_model(r, rm);
    u32 slot = 0;
    for (u32 m = 0; m < meshes.len; m++) {
      Array(SubMeshBlobAsset) subs = blob_array_get(SubMeshBlobAsset, &meshes.items[m].submeshes);
      for (u32 s = 0; s < subs.len; s++, slot++) {
        handles[slot] = rmodel->meshes.items[m].submeshes.items[s];
        names[slot] = subs.items[s].name.len ? string_blob_get(&subs.items[s].name) : "submesh";
        if (gr_part_kind(names[slot]) != GR_PART_SHELL)
          bounds = aabb_merge(bounds, (aabb){{subs.items[s].aabb_min.x, subs.items[s].aabb_min.y, subs.items[s].aabb_min.z},
                                             {subs.items[s].aabb_max.x, subs.items[s].aabb_max.y, subs.items[s].aabb_max.z}});
      }
    }
    v3 center = aabb_center(bounds);
    for (u32 s = 0; s < count; s++) {
      GrPartKind kind = gr_part_kind(names[s]);
      gr_spawn_part(w, r, handles[s], names[s], wa->weapon, (GrGunPart){.gun = g, .kind = kind});
      if (kind != GR_PART_SHELL)
        gr_spawn_part(w, r, handles[s], names[s], wa->pickups[g],
                      (GrGunPart){.gun = g, .kind = kind, .world_copy = true, .rest = v3_negate(center)});
      log_info("weapons: % part '%' kind %", fmt_cstr(gr_pickup_names[g]), fmt_cstr(names[s]), fmt_u32(kind));
      parts++;
    }
  }
  tctx_temp_allocator_end(ta);
  wa->built = true;
  log_info("weapons: % gun parts spawned (first-person + world copies)", fmt_u32(parts));
}

void gr_weapon_init(EcsWorld *w) {
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  ecs_resource_set_zero(w, GrWeaponRules);
  ecs_resource_set_zero(w, GrWeaponState);
  GrPickupState ps0 = {0};
  for (u32 g = 0; g < GR_GUN_COUNT; g++)
    ps0.home[g] = -1;
  ecs_resource_set(w, GrPickupState, ps0);

  GrWeaponAssets wa = {0};
  wa.guns[GR_WEAPON_RIFLE] = asset_load(ModelBlobAsset, assets, .path = "proving_grounds/models/rifle/rifle/rifle.hza");
  wa.guns[GR_WEAPON_BREACHER] =
      asset_load(ModelBlobAsset, assets, .path = "proving_grounds/models/breacher/breacher/breacher.hza");
  wa.guns[GR_WEAPON_LANCER] = asset_load(ModelBlobAsset, assets, .path = "proving_grounds/models/lancer/lancer/lancer.hza");
  wa.arms = asset_load(ModelBlobAsset, assets, .path = "proving_grounds/models/arms/arms/arms.hza");
  wa.healthpack =
      asset_load(ModelBlobAsset, assets, .path = "proving_grounds/models/healthpack/healthpack/healthpack.hza");
  for (u32 c = 0; c < GR_ARMS_CLIP_COUNT; c++)
    wa.clips[c] = asset_load(AnimationBlobAsset, assets, .path = gr_arms_clip_paths[c]);

  // the Weapon entity: the gun logic, the viewmodel motion, and the gun group's world pose
  EcsEntity weapon = ecs_entity_create(w);
  wa.weapon = weapon;
  ecs_component_set(w, weapon, EntityName, {.name = fixedstr64_from_cstr("Weapon")});
  ecs_component_set(w, weapon, WorldTransform, {.rotation = quat_identity(), .scale = {1, 1, 1}});
  ecs_component_set(w, weapon, GrWeapon,
                    // the run start equips the config's starting gun and fills the magazines
                    {.prompt_gun = -1,
                     .rng = random_create(0x5eed0001u),
                     .events = {.taken_gun = -1, .dropped_gun = -1}});
  ecs_component_set(w, weapon, GrViewmodel,
                    {.kick_pos = {.stiffness = 240.0f, .damping = 19.0f},
                     .kick_rot = {.stiffness = 210.0f, .damping = 17.0f},
                     .sway = {.stiffness = 80.0f, .damping = 11.0f},
                     .local_rot = quat_identity(),
                     .charge = 1.0f,
                     .coil = 7.3f, // full charge: 0.3 + 7
                     .cell = 3.0f,
                     .lens = 1.0f,
                     .cycle = -1.0f,
                     .rng = random_create(0x5eed0002u)});

  // the skinned arms: their origin is the gun's (posed onto each gun in Blender)
  EcsEntity arms = ecs_renderer_spawn(w, NULL,
                                      &(EcsRendererSpawnDesc){.model_asset = gr_model_ref(wa.arms),
                                                              .transform = {.rotation = quat_identity(), .scale = {1, 1, 1}},
                                                              .hidden = true,
                                                              .name = "Arms"});
  ecs_component_set(w, arms, EcsParent, {.parent = weapon});
  ecs_component_set(w, arms, GrArms, {.clip = -1});
  gr_look_viewmodel(w, arms);

  // the world copies' anchors: the parts are parented once the models are uploaded
  for (u32 g = 0; g < GR_GUN_COUNT; g++) {
    EcsEntity e = ecs_entity_create(w);
    wa.pickups[g] = e;
    ecs_component_set(w, e, EntityName, {.name = fixedstr64_from_cstr((char *)gr_pickup_names[g])});
    ecs_component_set(w, e, WorldTransform, {.rotation = quat_identity(), .scale = {1, 1, 1}}); // gr_pickup_system places it
    ecs_component_set(w, e, GrPickup, {.gun = g, .pedestal = -1, .home = -1});
  }

  ecs_resource_set(w, GrWeaponAssets, wa);
}

void gr_weapon_schedule(EcsWorld *w) {
  ecs_schedule_single(w, gr_weapon_config_system); // writes the GrWeaponRules resource
  ecs_schedule_single(w, gr_weapon_build_system); // uploads the gun models into RendererState and spawns their parts, once
  ecs_schedule(w, gr_weapon_flow_system);
  ecs_schedule(w, gr_weapon_fire_system);
  ecs_schedule(w, gr_weapon_interact_system);
  ecs_schedule(w, gr_viewmodel_system);
  ecs_schedule_single(w, gr_weapon_consolidate_system); // writes the GrWeaponState resource (one weapon)
  ecs_schedule(w, gr_drone_hit_intake_system);
  ecs_schedule(w, gr_pickup_system);
  ecs_schedule(w, gr_health_pack_system);
  ecs_schedule_single(w, gr_pickup_consolidate_system); // writes the GrPickupState resource (three guns)
  ecs_schedule_single(w, gr_pack_consolidate_system);   // writes the GrPickupState resource (two packs)
  ecs_schedule(w, gr_weapon_intake_system);
  ecs_schedule(w, gr_gun_part_system);
  ecs_schedule_single(w, gr_arms_bind_system); // allocates the arms' animation layer from EcsRendererAlloc, once
  ecs_schedule(w, gr_arms_system);
}
