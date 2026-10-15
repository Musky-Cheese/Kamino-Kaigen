// Proving Grounds effect objects: casings, debris, enemy bolt looks, the muzzle flash, beacon rings.

#define GR_CASING_LIFE 4.0f   // seconds a casing lives
#define GR_CASING_GRAVITY 16.0f
#define GR_DEBRIS_GRAVITY 18.0f // m/s^2
#define GR_DEBRIS_BOUNDS 23.8f  // x/z half-extent the parts bounce off

hz_internal f32 gr_rng_next(u32 *s) {
  *s ^= *s << 13;
  *s ^= *s >> 17;
  *s ^= *s << 5;
  return (f32)(*s & 0xFFFFFFu) / 16777216.0f;
}

// a random unit vector
hz_internal v3 gr_unit_from(f32 a, f32 b) {
  f32 u = a * 2.0f - 1.0f, phi = b * 2.0f * M_PI, s = m_sqrtf(m_maxf(0.0f, 1.0f - u * u));
  return (v3){s * m_cosf(phi), u, s * m_sinf(phi)};
}

hz_internal quat gr_spin_step(quat rot, v3 spin, f32 dt) {
  f32 rate = v3_length(spin);
  f32 angle = rate * dt;
  if (angle <= 1e-5f)
    return rot;
  return quat_normalize(quat_mul(quat_axis_angle(v3_scale(spin, 1.0f / rate), angle), rot));
}

// ---------------------------------------------------------------- casings

// consolidation: each eject takes the next ring slot, replacing the oldest
ecs_system(gr_fx_casings_consolidate, EcsResIn(GrFxFrame), EcsResInOut(GrFxCasings)) {
  const GrFxFrame *f = ecs_sys_get_resource(GrFxFrame);
  GrFxCasings *c = ecs_sys_get_resource_mut(GrFxCasings);
  c->active = 0;
  for (u32 i = 0; i < f->count; i++) {
    const GrFxRequest *q = &f->items[i];
    if (q->kind != GR_FX_CASING)
      continue;
    GrFxCasingSpawn *s = &c->spawns[c->next];
    c->next = (c->next + 1) % GR_FX_CASINGS;
    v3 axis = gr_unit_from(random_f32(&c->rng), random_f32(&c->rng));
    v3 spin_dir = gr_unit_from(random_f32(&c->rng), random_f32(&c->rng));
    *s = (GrFxCasingSpawn){.serial = s->serial + 1,
                           .kind = q->variant < GR_CASING_KIND_COUNT ? q->variant : GR_CASING_BRASS,
                           .position = q->p,
                           .velocity = q->d,
                           .rotation = quat_axis_angle(axis, random_f32(&c->rng) * M_PI),
                           .spin = v3_scale(spin_dir, 18.0f + random_f32(&c->rng) * 14.0f)};
  }
}

// per entity: one pooled casing falls, bounces 0.35 with xz x 0.5, rests under 0.6 m/s, lives 4 s
ecs_system(gr_casing_system, EcsInOut(GrCasing), EcsResIn(GrFxCasings), EcsResIn(GrClock), EcsResIn(GrLevel)) {
  const GrFxCasings *pool = ecs_sys_get_resource(GrFxCasings);
  const GrLevel *level = ecs_sys_get_resource(GrLevel);
  f32 dt = ecs_sys_get_resource(GrClock)->dt;
  for (i32 i = 0; i < it->count; i++) {
    const GrCasing *ro = ecs_sys_get_component(GrCasing, i);
    const GrFxCasingSpawn *s = &pool->spawns[ro->slot];
    if (s->serial == ro->seen_serial && (!ro->active || dt <= 0.0f))
      continue;
    GrCasing *c = ecs_sys_get_component_mut(GrCasing, i);
    if (s->serial != c->seen_serial) {
      c->seen_serial = s->serial;
      c->kind = s->kind;
      c->active = true;
      c->resting = false;
      c->life = GR_CASING_LIFE;
      c->position = s->position;
      c->velocity = s->velocity;
      c->rotation = s->rotation;
      c->spin = s->spin;
    }
    c->life -= dt;
    if (c->life <= 0.0f) {
      c->active = false;
      continue;
    }
    if (c->resting)
      continue;
    c->velocity.y -= GR_CASING_GRAVITY * dt;
    f32 prev_y = c->position.y;
    c->position = v3_add(c->position, v3_scale(c->velocity, dt));
    f32 floor = gr_level_ground_height(level, (v3){c->position.x, prev_y, c->position.z}) + 0.006f;
    if (c->position.y < floor) {
      c->position.y = floor;
      f32 speed = -c->velocity.y;
      // the casing bounce sound (speed > 0.8) is not emitted yet
      c->velocity.y = speed * 0.35f;
      c->velocity.x *= 0.5f;
      c->velocity.z *= 0.5f;
      c->spin = v3_scale(c->spin, 0.5f);
      if (speed < 0.6f)
        c->resting = true;
    }
    c->rotation = gr_spin_step(c->rotation, c->spin, dt);
  }
}

// consolidation: every slot into its kind's batch (zero scale elsewhere); the main lane submits the 40
ecs_system(gr_fx_casings_write_system, EcsIn(GrCasing), EcsResInOut(GrFxCasings), EcsResInOut(RendererState)) {
  GrFxCasings *pool = ecs_sys_get_resource_mut(GrFxCasings);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  if (!handle_is_valid(HANDLE_CAST(HzHandle, pool->batch[0])))
    return;
  const Transform hidden = {.position = V3_ZERO, .rotation = quat_identity(), .scale = V3_ZERO};
  for (i32 i = 0; i < it->count; i++) {
    const GrCasing *c = ecs_sys_get_component(GrCasing, i);
    for (u32 k = 0; k < GR_CASING_KIND_COUNT; k++) {
      Transform xf = hidden;
      if (c->active && c->kind == k) {
        // the shell is the brass cylinder scaled (2.1, 2.3, 2.1)
        v3 size = k == GR_CASING_SHELL ? (v3){2.1f, 2.3f, 2.1f} : (v3){1.0f, 1.0f, 1.0f};
        xf = (Transform){.position = c->position, .rotation = c->rotation, .scale = size};
      }
      renderer_instance_batch_write(r, pool->batch[k], c->slot, &xf, 1);
    }
    pool->active += c->active ? 1u : 0u;
  }
}

// ---------------------------------------------------------------- debris

// the submesh instance a model instance draws for slot `slot` (lod 0)
hz_internal HzHandleT(RendererSubMeshInstance) gr_model_slot_instance(const RendererModelInstance *inst, u32 slot) {
  if (inst->lods.len == 0)
    return (HzHandleT(RendererSubMeshInstance)){0};
  const RendererModelInstanceLod *lod = &inst->lods.items[0];
  u32 idx = 0;
  for (u32 m = 0; m < lod->mesh_instances.len; m++) {
    const RendererMeshInstance *mi = &lod->mesh_instances.items[m];
    for (u32 s = 0; s < mi->children.len; s++, idx++)
      if (idx == slot)
        return mi->children.items[s];
  }
  return (HzHandleT(RendererSubMeshInstance)){0};
}

hz_internal f32 gr_material_emission(RendererState *r, HzHandleT(RendererMaterial) h) {
  const RendererMaterial *m = renderer_material(r, h);
  if (!m)
    return 0.0f;
  MaterialProperty *p = material_find_property((Material *)&m->mat, "emission_intensity");
  return p ? p->f : 0.0f;
}

// the draw transform of a part pivoting at `position`: its model-space vertices go through the node's inverse first
hz_internal WorldTransform gr_debris_draw(const GrDebris *p, f32 shrink) {
  quat rot = quat_mul(p->rotation, p->node_inverse);
  f32 s = p->scale * shrink;
  return (WorldTransform){.position = v3_sub(p->position, quat_rotate(rot, v3_scale(p->node_offset, s))), .rotation = rot,
                          .scale = {s, s, s}};
}

// consolidation: shatter: each radius-tagged part of a dying drone becomes its own entity
ecs_system(gr_fx_shatter_system, EcsIn(GrDrone), EcsIn(EcsModelInstance), EcsIn(WorldTransform), EcsResIn(GrClock),
           EcsResIn(GrDroneAssets), EcsResIn(GrDroneRules), EcsResIn(AssetSystem), EcsResInOut(RendererState)) {
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const GrDroneAssets *da = ecs_sys_get_resource(GrDroneAssets);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const AssetSystem *assets = ecs_sys_get_resource(AssetSystem);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  EcsWorld *w = it->world;
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->events_frame != frame || !(d->events & GR_DRONE_EV_DIED) || d->type >= GR_DRONE_TYPE_COUNT)
      continue;
    const GrDroneModel *dm = &da->types[d->type];
    const RendererModelInstance *inst = renderer_model_instance(r, ecs_sys_get_component(EcsModelInstance, i)->instance);
    ModelBlobAsset *model = dm->ready ? asset_get(assets, HANDLE_CAST(HzHandleT(ModelBlobAsset), dm->model.handle), ModelBlobAsset) : NULL;
    if (!inst || !model)
      continue;
    Array(ModelNodeBlobAsset) nodes = blob_array_get(ModelNodeBlobAsset, &model->nodes);
    Array(MeshBlobAsset) meshes = blob_array_get(MeshBlobAsset, &model->meshes);
    const WorldTransform *wx = ecs_sys_get_component(WorldTransform, i);
    f32 scale = wx->scale.x;
    v3 center = d->position;
    v3 inherited = v3_add(v3_scale(d->velocity, 0.5f), v3_scale(d->hit_dir, 3.0f));
    f32 force = 5.0f + rules->drones.types.items[d->type].scale * 2.0f;
    u32 seed = (u32)it->entities[i] * 2654435761u;
    for (u32 pi = 0; pi < dm->part_count; pi++) {
      const GrDronePart *part = &dm->parts[pi];
      u32 mesh = part->node < nodes.len ? nodes.items[part->node].mesh : MODEL_NODE_NO_MESH;
      if (mesh == MODEL_NODE_NO_MESH || mesh >= meshes.len)
        continue;
      // the model's submesh slots run mesh by mesh
      u32 first = 0;
      for (u32 m = 0; m < mesh; m++)
        first += blob_array_get(SubMeshBlobAsset, &meshes.items[m].submeshes).len;
      u32 subs = blob_array_get(SubMeshBlobAsset, &meshes.items[mesh].submeshes).len;
      u32 k = seed + pi * 7919u;
      v3 pivot = v3_add(wx->position, quat_rotate(wx->rotation, v3_scale(part->position, scale)));
      v3 out = v3_sub(pivot, center);
      out = v3_lengthsq(out) < 1e-4f ? gr_unit_from(gr_hash01(k + 1), gr_hash01(k + 2)) : v3_normalize(out);
      f32 life = 4.0f + gr_hash01(k + 4) * 2.5f;
      GrDebris deb = {.position = pivot,
                      .rotation = quat_mul(wx->rotation, part->rotation),
                      .node_offset = part->position,
                      .node_inverse = quat_conjugate(part->rotation),
                      .scale = scale,
                      .velocity = v3_add(v3_add(v3_scale(out, force * (0.6f + gr_hash01(k + 3) * 0.8f)), inherited), (v3){0.0f, force * 0.5f, 0.0f}),
                      .spin = v3_scale(gr_unit_from(gr_hash01(k + 5), gr_hash01(k + 6)), 6.0f + gr_hash01(k + 7) * 10.0f),
                      .radius = part->radius * scale,
                      .life = life,
                      .max_life = life,
                      .rng = k | 1u};
      for (u32 s = 0; s < subs; s++) {
        HzHandleT(RendererSubMeshInstance) si = gr_model_slot_instance(inst, first + s);
        const RendererSubMeshInstanceCold *cold = handle_is_valid(HANDLE_CAST(HzHandle, si)) ? renderer_submesh_instance_cold(r, si) : NULL;
        if (!cold)
          continue;
        // its own copy: the drone's materials may go with the drone this frame
        HzHandleT(RendererMaterial) own = renderer_create_material_instance(r, renderer_submesh_instance_material(r, si));
        deb.glow = gr_material_emission(r, own);
        deb.glow_now = deb.glow;
        deb.trails = s == 0;
        WorldTransform xf = gr_debris_draw(&deb, 1.0f);
        EcsEntity e = ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = cold->mesh,
                                                                         .material = own,
                                                                         .transform = {xf.position, xf.rotation, xf.scale},
                                                                         .casts_shadow = true});
        ecs_component_set(w, e, GrDebris, deb);
        ecs_component_set(w, e, EcsMaterialInstance, {.instance = own});
      }
    }
  }
}

// per entity: a part tumbles, bounces on the walkable tops, trails sparks then smoke, sputters and shrinks out
ecs_system(gr_debris_system, EcsInOut(GrDebris), EcsInOut(WorldTransform), EcsResIn(GrClock), EcsResIn(GrLevel),
           EcsResIn(VfxRequests), EcsResIn(GrFxAssets)) {
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  f32 dt = ecs_sys_get_resource(GrClock)->dt;
  const GrLevel *level = ecs_sys_get_resource(GrLevel);
  if (dt <= 0.0f)
    return;
  for (i32 i = 0; i < it->count; i++) {
    GrDebris *p = ecs_sys_get_component_mut(GrDebris, i);
    p->life -= dt;
    if (p->life <= 0.0f) {
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    f32 age = p->max_life - p->life;
    p->velocity.y -= GR_DEBRIS_GRAVITY * dt;
    p->velocity = v3_scale(p->velocity, m_expf(-0.3f * dt));
    f32 prev_y = p->position.y;
    p->position = v3_add(p->position, v3_scale(p->velocity, dt));
    f32 floor = gr_level_ground_height(level, (v3){p->position.x, prev_y - p->radius, p->position.z}) + p->radius;
    if (p->position.y < floor) {
      p->position.y = floor;
      if (p->velocity.y < 0.0f)
        p->velocity.y = -p->velocity.y * 0.35f;
      p->velocity.x *= 0.65f;
      p->velocity.z *= 0.65f;
      p->spin = v3_scale(p->spin, 0.6f);
    }
    if (m_absf(p->position.x) > GR_DEBRIS_BOUNDS) {
      p->position.x = m_signf(p->position.x) * GR_DEBRIS_BOUNDS;
      p->velocity.x *= -0.4f;
    }
    if (m_absf(p->position.z) > GR_DEBRIS_BOUNDS) {
      p->position.z = m_signf(p->position.z) * GR_DEBRIS_BOUNDS;
      p->velocity.z *= -0.4f;
    }
    p->rotation = gr_spin_step(p->rotation, p->spin, dt);

    p->trail_timer -= dt;
    if (p->trails && p->trail_timer <= 0.0f) {
      if (age < 1.1f) {
        p->trail_timer = 0.035f;
        gr_fx_throw(rq, as, NULL, (GrFxRequest){.kind = GR_FX_DEBRIS_SPARK, .p = p->position, .d = p->velocity});
      } else if (age < 3.0f) {
        p->trail_timer = 0.18f;
        gr_fx_throw(rq, as, NULL, (GrFxRequest){.kind = GR_FX_DEBRIS_SMOKE, .p = p->position});
      }
    }
    if (p->glow > 0.0f) {
      f32 flicker = age < 1.2f ? (gr_rng_next(&p->rng) < 0.3f ? 0.2f : 1.0f) : 0.0f;
      p->glow_now = p->glow * m_maxf(0.0f, 1.0f - age / 1.5f) * flicker;
    }
    *ecs_sys_get_component_mut(WorldTransform, i) = gr_debris_draw(p, p->life < 0.6f ? p->life / 0.6f : 1.0f);
  }
}

// consolidation: the sputter onto each glowing part's own material
ecs_system(gr_debris_glow_system, EcsIn(GrDebris), EcsIn(EcsMaterialInstance), EcsResInOut(RendererState)) {
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    const GrDebris *p = ecs_sys_get_component(GrDebris, i);
    if (p->glow > 0.0f && p->max_life - p->life < 1.6f)
      material_set_float(r, ecs_sys_get_component(EcsMaterialInstance, i)->instance, "emission_intensity", p->glow_now);
  }
}

// ---------------------------------------------------------------- enemy bolt looks

// consolidation: each pooled bolt row gets a hidden core sphere and glow sprite per drone type, once
ecs_system(gr_bolt_look_build_system, EcsIn(GrBolt), EcsNot(GrFxBoltRow), EcsResIn(GrFx), EcsResInOut(RendererState)) {
  const GrFx *fx = ecs_sys_get_resource(GrFx);
  if (!fx->built)
    return;
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    ecs_component_set(it->world, it->entities[i], GrFxBoltRow, {0});
    for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
      for (u32 k = 0; k < 2; k++) {
        EcsEntity e = ecs_renderer_spawn(it->world, r, &(EcsRendererSpawnDesc){
                                                           .submesh = k == 0 ? fx->mesh_sphere : fx->mesh_quad,
                                                           .material = k == 0 ? fx->mat_core[t] : fx->mat_glow[t],
                                                           .transform = {.position = V3_ZERO, .rotation = quat_identity(), .scale = V3_ONE},
                                                           .hidden = true});
        ecs_component_set(it->world, e, GrBoltLookPart, {.bolt = it->entities[i], .type = t, .glow = k == 1});
      }
    }
  }
}

// per entity: a look part shows while its row flies a bolt of its type: core radius x 0.55, sprite radius x 7
ecs_system(gr_bolt_part_system, EcsIn(GrBoltLookPart), EcsInOut(WorldTransform), EcsInOut(EcsVisibility), EcsExternIn(GrBolt)) {
  for (i32 i = 0; i < it->count; i++) {
    const GrBoltLookPart *part = ecs_sys_get_component(GrBoltLookPart, i);
    const GrBolt *b = ecs_component_get(it->world, part->bolt, GrBolt);
    b32 on = b && b->state == GR_BOLT_FLYING && b->type == part->type;
    if (ecs_sys_get_component(EcsVisibility, i)->visible != on)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = on;
    if (!on)
      continue;
    f32 s = b->radius * (part->glow ? 7.0f : 0.55f);
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){.position = b->position, .rotation = quat_identity(), .scale = {s, s, s}};
  }
}

// ---------------------------------------------------------------- muzzle flash

// consolidation: a shot triggers the flash (random seed, spin, sizes); intensity is sampled before ageing
ecs_system(gr_fx_muzzle_consolidate, EcsResIn(GrFxFrame), EcsResIn(GrWeaponState), EcsResIn(GrWeaponAssets),
           EcsResIn(GrWeaponRules), EcsResIn(GrClock), EcsResInOut(GrFxMuzzle), EcsResInOut(RendererState)) {
  const GrFxFrame *f = ecs_sys_get_resource(GrFxFrame);
  const GrWeaponState *ws = ecs_sys_get_resource(GrWeaponState);
  const GrWeaponAssets *wa = ecs_sys_get_resource(GrWeaponAssets);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  GrFxMuzzle *m = ecs_sys_get_resource_mut(GrFxMuzzle);
  f32 dt = ecs_sys_get_resource(GrClock)->dt;
  // the flash rides the Muzzle socket of the gun in hand
  u32 gun = ws->gun < GR_GUN_COUNT ? ws->gun : 0;
  m->socket = v3_add(ws->view_pos, quat_rotate(ws->view_rot, wa->sockets[gun][GR_SOCKET_MUZZLE]));
  m->socket_rot = ws->view_rot;
  m->visible = ws->view_visible;
  m->sight = v3_add(ws->view_pos, quat_rotate(ws->view_rot, wa->sockets[gun][GR_SOCKET_SIGHT]));
  m->sight_visible = ws->view_visible && gun == 0;
  for (u32 i = 0; i < f->count; i++) {
    if (f->items[i].kind != GR_FX_MUZZLE)
      continue;
    m->gun = f->items[i].variant < GR_GUN_COUNT ? f->items[i].variant : 0;
    m->scale = cfg->guns[m->gun].flash_scale;
    u32 seed = m->serial * 747796405u + 2891336453u;
    m->timer = GR_FX_FLASH_TIME;
    m->seed = gr_hash01(seed);
    m->star_angle = gr_hash01(seed + 1) * 2.0f * M_PI;
    m->star_scale = 0.8f + gr_hash01(seed + 2) * 0.5f;
    m->flame_len = 0.8f + gr_hash01(seed + 3) * 0.6f;
    m->flame_width_a = 0.8f + gr_hash01(seed + 4) * 0.4f;
    m->flame_width_b = 0.8f + gr_hash01(seed + 5) * 0.4f;
    m->serial++;
  }
  if (m->timer > 0.0f) {
    f32 k = m->timer / GR_FX_FLASH_TIME;
    m->intensity = k * k * 1.4f;
    m->timer -= dt;
  } else {
    m->intensity = 0.0f;
  }
  if (!m->built)
    return;
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  // the rig's flash colour, written when the firing gun changes
  b32 recolor = m->color_gun != m->gun + 1;
  m->color_gun = m->gun + 1;
  v3 c = cfg->guns[m->gun].flash_color;
  for (u32 p = 0; p < GR_FX_FLASH_PART_COUNT; p++) {
    material_set_float(r, m->material[p], "intensity", m->intensity);
    material_set_float(r, m->material[p], "seed", m->seed);
    if (recolor)
      material_set_vec4(r, m->material[p], "color", (vec4){c.x, c.y, c.z, 1.0f});
  }
}

// per entity: the star faces the camera at the muzzle; the two flame tongues cross along the barrel
ecs_system(gr_fx_flash_part_system, EcsIn(GrFxFlashPart), EcsInOut(WorldTransform), EcsInOut(EcsVisibility), EcsResIn(GrFxMuzzle)) {
  const GrFxMuzzle *m = ecs_sys_get_resource(GrFxMuzzle);
  b32 on = m->intensity > 0.0f && m->visible;
  for (i32 i = 0; i < it->count; i++) {
    const GrFxFlashPart *part = ecs_sys_get_component(GrFxFlashPart, i);
    if (ecs_sys_get_component(EcsVisibility, i)->visible != on)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = on;
    if (!on)
      continue;
    WorldTransform *xf = ecs_sys_get_component_mut(WorldTransform, i);
    xf->position = m->socket;
    f32 k = m->scale > 0.0f ? m->scale : 1.0f; // the rig's flash scale
    if (part->role == GR_FX_FLASH_STAR) {
      xf->rotation = quat_mul(m->socket_rot, quat_rotate_z(m->star_angle));
      xf->scale = (v3){m->star_scale * k, m->star_scale * k, m->star_scale * k};
    } else {
      b32 b = part->role == GR_FX_FLASH_FLAME_B;
      xf->rotation = quat_mul(m->socket_rot, quat_rotate_z(b ? M_PI * 0.5f : 0.0f));
      xf->scale = (v3){(b ? m->flame_width_b : m->flame_width_a) * k, k, m->flame_len * k};
    }
  }
}

// per entity: the holo sight rides the Sight socket of the VK-7, facing the eye like the gun group
ecs_system(gr_fx_sight_system, EcsIn(GrFxSight), EcsInOut(WorldTransform), EcsInOut(EcsVisibility), EcsResIn(GrFxMuzzle)) {
  const GrFxMuzzle *m = ecs_sys_get_resource(GrFxMuzzle);
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(EcsVisibility, i)->visible != m->sight_visible)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = m->sight_visible;
    if (!m->sight_visible)
      continue;
    WorldTransform *xf = ecs_sys_get_component_mut(WorldTransform, i);
    xf->position = m->sight;
    xf->rotation = m->socket_rot;
  }
}

// ---------------------------------------------------------------- pickup beacons

// consolidation: a new beacon gets its floor ring (1.6 m at y 0.02) and light beam children, in the kind's colour
ecs_system(gr_beacon_build_system, EcsInOut(GrBeacon), EcsIn(WorldTransform), EcsResIn(GrFx), EcsResInOut(RendererState)) {
  const GrFx *fx = ecs_sys_get_resource(GrFx);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  if (!fx->built)
    return;
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(GrBeacon, i)->built)
      continue;
    GrBeacon *b = ecs_sys_get_component_mut(GrBeacon, i);
    b->built = true;
    v3 pos = v3_add(ecs_sys_get_component(WorldTransform, i)->position, (v3){0.0f, 0.02f, 0.0f});
    EcsEntity e = ecs_renderer_spawn(it->world, r, &(EcsRendererSpawnDesc){
                                                       .submesh = fx->mesh_ring,
                                                       .material = fx->mat_ring[MIN(b->kind, 2u)],
                                                       .transform = {.position = pos, .rotation = quat_identity(), .scale = {1, 1, 1}},
                                                       .hidden = !b->visible,
                                                       .sort_priority = GR_FX_ORDER_RING});
    ecs_component_set(it->world, e, LocalTransform, {.position = {0.0f, 0.02f, 0.0f}, .rotation = quat_identity(), .scale = {1, 1, 1}});
    ecs_component_set(it->world, e, EcsParent, {.parent = it->entities[i]});
    ecs_component_set(it->world, e, GrBeaconPart, {.beacon = it->entities[i]});
    // the light column, centred half its 13 m up
    v3 beam_local = {0.0f, 6.5f, 0.0f};
    EcsEntity beam = ecs_renderer_spawn(it->world, r, &(EcsRendererSpawnDesc){
                                                          .submesh = fx->mesh_beam,
                                                          .material = fx->mat_beam[MIN(b->kind, 2u)],
                                                          .transform = {.position = v3_add(ecs_sys_get_component(WorldTransform, i)->position, beam_local),
                                                                        .rotation = quat_identity(),
                                                                        .scale = {1, 1, 1}},
                                                          .hidden = !b->visible,
                                                          .sort_priority = GR_FX_ORDER_BEAM});
    ecs_component_set(it->world, beam, LocalTransform, {.position = beam_local, .rotation = quat_identity(), .scale = {1, 1, 1}});
    ecs_component_set(it->world, beam, EcsParent, {.parent = it->entities[i]});
    ecs_component_set(it->world, beam, GrBeaconPart, {.beacon = it->entities[i]});
  }
}

// consolidation: the beacons this frame, for the main-lane labels
ecs_system(gr_fx_beacons_consolidate, EcsIn(GrBeacon), EcsIn(WorldTransform), EcsResInOut(GrFxBeacons)) {
  GrFxBeacons *out = ecs_sys_get_resource_mut(GrFxBeacons);
  for (i32 i = 0; i < it->count && out->count < GR_FX_BEACONS; i++) {
    const GrBeacon *b = ecs_sys_get_component(GrBeacon, i);
    out->items[out->count++] = (GrFxBeaconView){.position = ecs_sys_get_component(WorldTransform, i)->position,
                                                .kind = b->kind,
                                                .name = b->name,
                                                .label_height = b->label_height,
                                                .visible = b->visible};
  }
}

#define GR_LABEL_ABOVE_GUN 0.55f  // label height above a gun pickup
#define GR_LABEL_ABOVE_PACK 0.45f // label height above a health pack

// per entity: a beacon follows its pickup (placed, present); written when it changes
ecs_system(gr_beacon_update_system, EcsInOut(GrBeacon), EcsInOut(WorldTransform), EcsResIn(GrPickupState), EcsResIn(GrWeaponRules),
           EcsResIn(GrLevel), EcsResIn(GrWaves)) {
  const GrWaves *waves = ecs_sys_get_resource(GrWaves);
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrLevel *level = ecs_sys_get_resource(GrLevel);
  for (i32 i = 0; i < it->count; i++) {
    const GrBeacon *b = ecs_sys_get_component(GrBeacon, i);
    b32 visible;
    v3 base;
    f32 label;
    if (b->kind == GR_BEACON_OBJECTIVE) {
      visible = waves->objective_active;
      base = waves->objective;
      label = 2.2f;
      CString sector = km_sector_name(waves->sector);
      String64Bytes name = {0};
      StringBuilder sb;
      sb_init(&sb, name.value, sizeof(name.value));
      sb_append_format(&sb, "SECURE %", fmt_cstr(sector));
      name.len = (u32)sb.len;
      if (b->name.len != name.len || mem_cmp(b->name.value, name.value, name.len) != 0)
        ecs_sys_get_component_mut(GrBeacon, i)->name = name;
    } else if (b->kind == GR_BEACON_WEAPON) {
      u32 g = b->index < GR_GUN_COUNT ? b->index : 0;
      // the gun's name from Settings' config: it loads with the scene and edits live
      const String32Bytes *name = &cfg->guns[g].name;
      if (b->name.len != name->len || mem_cmp(b->name.value, name->value, name->len) != 0)
        ecs_sys_get_component_mut(GrBeacon, i)->name = fixedstr64_from_len((CString)name->value, name->len);
      i32 ped = ps->pedestal[g];
      visible = ps->in_world[g];
      base = ps->base[g];
      label = (ped >= 0 && ped < GR_LEVEL_PEDESTAL_COUNT ? level->pedestals[ped].top + cfg->pedestal_hover : cfg->drop_hover) +
              GR_LABEL_ABOVE_GUN;
    } else {
      u32 k = b->index < GR_PACK_COUNT ? b->index : 0;
      visible = ps->pack_present[k];
      base = ps->pack_base[k];
      label = cfg->pack_hover + GR_LABEL_ABOVE_PACK;
    }
    if (b->visible != visible || b->label_height != label) {
      GrBeacon *m = ecs_sys_get_component_mut(GrBeacon, i);
      m->visible = visible;
      m->label_height = label;
    }
    if (!v3_eqv(ecs_sys_get_component(WorldTransform, i)->position, base))
      ecs_sys_get_component_mut(WorldTransform, i)->position = base;
  }
}

// per entity: a ring shows with its beacon
ecs_system(gr_beacon_part_system, EcsIn(GrBeaconPart), EcsInOut(EcsVisibility), EcsExternIn(GrBeacon)) {
  for (i32 i = 0; i < it->count; i++) {
    const GrBeacon *b = ecs_component_get(it->world, ecs_sys_get_component(GrBeaconPart, i)->beacon, GrBeacon);
    b32 on = b && b->visible;
    if (ecs_sys_get_component(EcsVisibility, i)->visible != on)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = on;
  }
}
