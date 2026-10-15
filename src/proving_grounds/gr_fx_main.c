// Proving Grounds effects module: init (app_init), the one-shot build (once its assets load), the schedule and the main-lane frame drain.

#define GR_FX_PARTICLES_PER_MATERIAL 8192 // one default material per texture; vfx opens another when its pool fills

hz_internal HzHandleT(RendererSubMesh) gr_fx_upload_mesh(RendererState *r, PgMesh *m, const char *what) {
  PgUpload up = pg_mesh_upload(r, m, 1.0f);
  if (up.count != 1)
    log_error("gr_fx: the % mesh split into % submeshes", fmt_cstr(what), fmt_u32(up.count));
  return up.parts[0];
}

// a plane with uv 0..1 across it (pg_plane's are metres), which the quad shaders read
hz_internal void gr_fx_plane(PgMesh *m, f32 width, f32 height) {
  u32 from = (u32)m->uv.len;
  pg_plane(m, width, height, 1, 1);
  for (u32 i = from; i < (u32)m->uv.len; i++)
    m->uv.items[i] = (v2){m->uv.items[i].x / width, m->uv.items[i].y / height};
}

// an open column, uv u around 0..1 and v 0 at the bottom to 1 at the top
hz_internal HzHandleT(RendererSubMesh) gr_fx_column(RendererState *r, f32 radius, f32 height, u32 segments) {
  HzHandleT(RendererSubMesh) out = {0};
  tctx_temp_allocator_scope(ta, NULL) {
    PgMesh m;
    pg_mesh_begin(&m, ta.temp.arena, (segments + 1) * 2, segments * 6);
    pg_cylinder(&m, &(PgCylinderDesc){.radius_top = radius, .radius_bottom = radius, .height = height,
                                      .radial_segments = segments, .height_segments = 1, .open_ended = true});
    f32 u_len = 2.0f * M_PI * radius;
    for (u32 i = 0; i < m.uv.len; i++)
      m.uv.items[i] = (v2){m.uv.items[i].x / u_len, m.uv.items[i].y / height};
    out = gr_fx_upload_mesh(r, &m, "beam");
  }
  return out;
}

hz_internal void gr_fx_build_meshes(GrFx *fx, RendererState *r) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  ArenaAllocator *arena = ta.temp.arena;
  PgMesh m;
  // the casing: a 4.5 mm radius, 26 mm cylinder with 8 sides
  pg_mesh_begin(&m, arena, 64, 192);
  pg_cylinder(&m, &(PgCylinderDesc){.radius_top = 0.0045f, .radius_bottom = 0.0045f, .height = 0.026f, .radial_segments = 8, .height_segments = 1});
  fx->mesh_casing = gr_fx_upload_mesh(r, &m, "casing");
  // the bolt core: a unit sphere, 16 x 10 segments
  pg_mesh_begin(&m, arena, 256, 1024);
  pg_sphere(&m, &(PgSphereDesc){.radius = 1.0f, .width_segments = 16, .height_segments = 10});
  fx->mesh_sphere = gr_fx_upload_mesh(r, &m, "bolt core");
  // a unit quad in XY: the bolt glow sprite (its VS faces the camera)
  pg_mesh_begin(&m, arena, 8, 8);
  pg_plane(&m, 1.0f, 1.0f, 1, 1);
  fx->mesh_quad = gr_fx_upload_mesh(r, &m, "glow quad");
  // the flash star: a 0.42 m plane facing back along the barrel
  pg_mesh_begin(&m, arena, 8, 8);
  gr_fx_plane(&m, 0.42f, 0.42f);
  fx->mesh_star = gr_fx_upload_mesh(r, &m, "muzzle star");
  // a flame tongue: a 0.2 x 0.62 m plane from the muzzle forward (-Z), uv.y 0 at the muzzle
  pg_mesh_begin(&m, arena, 8, 8);
  gr_fx_plane(&m, 0.2f, 0.62f);
  pg_mesh_translate(&m, (v3){0.0f, 0.31f, 0.0f}, 0);
  pg_mesh_transform(&m, quat_to_m4(quat_rotate_x(-M_PI * 0.5f)), 0);
  fx->mesh_flame = gr_fx_upload_mesh(r, &m, "muzzle flame");
  // the rifle's holo sight glass: a 46 mm plane at the Sight socket, facing the eye
  pg_mesh_begin(&m, arena, 8, 8);
  gr_fx_plane(&m, 0.046f, 0.046f);
  fx->mesh_sight = gr_fx_upload_mesh(r, &m, "holo sight");
  // the beacon's 1.6 m floor ring plane, lying in XZ facing up
  pg_mesh_begin(&m, arena, 8, 8);
  gr_fx_plane(&m, 1.6f, 1.6f);
  pg_mesh_transform(&m, quat_to_m4(quat_rotate_x(-M_PI * 0.5f)), 0);
  fx->mesh_ring = gr_fx_upload_mesh(r, &m, "beacon ring");
  tctx_temp_allocator_end(ta);
}

hz_internal HzHandleT(RendererMaterial) gr_fx_material_base(RendererState *r, AssetSystem *assets, HzHandleT(MaterialBlobAsset) h) {
  return renderer_create_material_from_asset(r, assets, asset_get(assets, h, MaterialBlobAsset));
}

hz_internal HzHandleT(RendererMaterial) gr_fx_material_colored(GrFx *fx, RendererState *r, HzHandleT(RendererMaterial) base, v3 color) {
  HzHandleT(RendererMaterial) m = renderer_create_material_instance(r, base);
  material_set_vec4(r, m, "color", (vec4){color.x, color.y, color.z, 1.0f});
  material_set_float(r, m, "unit", fx->cd_per_unit);
  return m;
}

hz_internal b32 gr_fx_assets_ready(const GrFx *fx, const AssetSystem *assets) {
  for (u32 i = 0; i < GR_FX_SCORCH_VARIANTS; i++)
    if (!asset_is_ready(assets, fx->scorch[i]))
      return false;
  b32 ready = asset_is_ready(assets, fx->core) && asset_is_ready(assets, fx->asset_muzzle) && asset_is_ready(assets, fx->asset_glow) &&
              asset_is_ready(assets, fx->asset_ring) && asset_is_ready(assets, fx->asset_unlit) && asset_is_ready(assets, fx->asset_dust) && asset_is_ready(assets, fx->asset_rain) &&
              asset_is_ready(assets, fx->asset_sight) && asset_is_ready(assets, fx->asset_beam);
  for (u32 k = 0; k < GR_CASING_KIND_COUNT; k++)
    ready = ready && asset_is_ready(assets, fx->asset_casing[k]);
  return ready;
}

#define GR_FX_DUST_COUNT 4000 // dust motes in the hub
#define KM_RAIN_DROPS 14000   // streaks in the camera's rain box
#define KM_RAIN_BOX 36.0f     // km_rain.glsl's box, m

// the storm: streak quads at rest positions in the rain box (seed 91); km_rain.glsl falls and wraps them round the camera
hz_internal void km_fx_build_rain(EcsWorld *w, RendererState *r, HzHandleT(RendererMaterial) material) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  PgMesh m;
  pg_mesh_begin(&m, ta.temp.arena, KM_RAIN_DROPS * 4, KM_RAIN_DROPS * 6);
  pg_mesh_enable_streams(&m, PG_STREAM_AUX);
  Mulberry32_State rng = {91u};
  for (u32 i = 0; i < KM_RAIN_DROPS; i++) {
    v3 p = {mulberry32_next_f32(&rng) * KM_RAIN_BOX, mulberry32_next_f32(&rng) * KM_RAIN_BOX, mulberry32_next_f32(&rng) * KM_RAIN_BOX};
    u32 seed = MIN((u32)(mulberry32_next_f32(&rng) * 16777216.0f), 0xFFFFFFu);
    u32 base = pg_mesh_vtx_count(&m);
    for (u32 k = 0; k < 4; k++) {
      v2 corner = {(k == 1 || k == 2) ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f};
      u32 v = pg_mesh_push_vertex(&m, p, V3_UP, corner);
      m.aux.items[v] = seed;
    }
    pg_mesh_push_tri(&m, base, base + 1, base + 2);
    pg_mesh_push_tri(&m, base, base + 2, base + 3);
  }
  PgUpload up = pg_mesh_upload(r, &m, 1.0f);
  tctx_temp_allocator_end(ta);
  // the vertex shader places every drop round the camera: the instance is never culled by its rest bounds
  for (u32 p = 0; p < up.count; p++)
    ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = up.parts[p], .material = material,
                                                     .transform = {.position = {-1e4f, -1e4f, -1e4f}, .rotation = quat_identity(), .scale = {2e4f / KM_RAIN_BOX, 2e4f / KM_RAIN_BOX, 2e4f / KM_RAIN_BOX}},
                                                     .sort_priority = GR_FX_ORDER_DUST, .name = "Rain"});
}

// 4000 motes in the hub box (mulberry32 seed 77) as one static mesh; the VS drifts and sizes them
hz_internal void gr_fx_build_dust(EcsWorld *w, GrFx *fx, RendererState *r, HzHandleT(RendererMaterial) material, aabb box) {
  UNUSED(fx);
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  PgMesh m;
  pg_mesh_begin(&m, ta.temp.arena, GR_FX_DUST_COUNT * 4, GR_FX_DUST_COUNT * 6);
  pg_mesh_enable_streams(&m, PG_STREAM_AUX);
  Mulberry32_State rng = {77u};
  const v3 lo = box.min, size = v3_sub(box.max, box.min);
  for (u32 i = 0; i < GR_FX_DUST_COUNT; i++) {
    v3 p = {lo.x + mulberry32_next_f32(&rng) * size.x, lo.y + 0.3f + mulberry32_next_f32(&rng) * (size.y - 1.0f),
            lo.z + mulberry32_next_f32(&rng) * size.z};
    u32 seed = (u32)(mulberry32_next_f32(&rng) * 16777216.0f);
    seed = seed > 0xFFFFFFu ? 0xFFFFFFu : seed;
    u32 base = pg_mesh_vtx_count(&m);
    for (u32 k = 0; k < 4; k++) {
      v2 corner = {(k == 1 || k == 2) ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f};
      u32 v = pg_mesh_push_vertex(&m, p, V3_UP, corner);
      m.aux.items[v] = seed;
    }
    pg_mesh_push_tri(&m, base, base + 1, base + 2);
    pg_mesh_push_tri(&m, base, base + 2, base + 3);
  }
  HzHandleT(RendererSubMesh) mesh = gr_fx_upload_mesh(r, &m, "dust motes");
  tctx_temp_allocator_end(ta);
  ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = mesh,
                                                   .material = material,
                                                   .transform = {.rotation = quat_identity(), .scale = {1, 1, 1}},
                                                   .sort_priority = GR_FX_ORDER_DUST,
                                                   .name = "DustMotes"});
}

// everything that needs loaded assets or a live frame (vfx_register is a frame-time call); main lane, once
hz_internal void gr_fx_build(EcsWorld *w, GrFx *fx) {
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  gr_fx_build_textures(fx);
  GrFxAssets *assets_fx = ecs_resource_get_mut(w, GrFxAssets);
  gr_fx_register_all(fx, assets_fx, &ecs_resource_get(w, GrWeaponRules)->weapons);
  assets_fx->ready = true;
  gr_fx_build_meshes(fx, r);

  HzHandleT(RendererMaterial) unlit = gr_fx_material_base(r, assets, fx->asset_unlit);
  HzHandleT(RendererMaterial) glow = gr_fx_material_base(r, assets, fx->asset_glow);
  HzHandleT(RendererMaterial) ring = gr_fx_material_base(r, assets, fx->asset_ring);
  HzHandleT(RendererMaterial) muzzle = gr_fx_material_base(r, assets, fx->asset_muzzle);
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    v3 e = gr_drone_energy(t);
    fx->mat_core[t] = gr_fx_material_colored(fx, r, unlit, v3_add(v3_scale(e, 4.0f), (v3){3.0f, 3.0f, 3.0f})); // colour x 4 + 3
    fx->mat_glow[t] = gr_fx_material_colored(fx, r, glow, v3_scale(e, 3.5f));
  }
  fx->mat_ring[0] = gr_fx_material_colored(fx, r, ring, gr_hex_linear(0xffb627)); // weapon pickup colour
  fx->mat_ring[1] = gr_fx_material_colored(fx, r, ring, gr_hex_linear(0x38ff7a)); // health pickup colour
  fx->mat_ring[2] = gr_fx_material_colored(fx, r, ring, gr_hex_linear(0x5ef2ff)); // objective colour
  // the beacon beam: a 0.32 m x 13 m light column at intensity 3 with the near fade, in the kind's colour
  fx->mesh_beam = gr_fx_column(r, 0.32f, 13.0f, 32);
  HzHandleT(RendererMaterial) beam = gr_fx_material_base(r, assets, fx->asset_beam);
  for (u32 k = 0; k < 3; k++) {
    v3 c = gr_hex_linear(k == 0 ? 0xffb627 : k == 1 ? 0x38ff7a : 0x5ef2ff);
    fx->mat_beam[k] = renderer_create_material_instance(r, beam);
    material_set_vec4(r, fx->mat_beam[k], "color", (vec4){c.x, c.y, c.z, 1.0f});
    material_set_vec4(r, fx->mat_beam[k], "params", (vec4){3.0f, 1.0f, 0.0f, 0.0f});
  }

  GrFxMuzzle *mz = ecs_resource_get_mut(w, GrFxMuzzle);
  for (u32 p = 0; p < GR_FX_FLASH_PART_COUNT; p++) {
    HzHandleT(RendererMaterial) m = gr_fx_material_colored(fx, r, muzzle, (v3){mz->color.r, mz->color.g, mz->color.b});
    material_set_uint(r, m, "flame", p == GR_FX_FLASH_STAR ? 0u : 1u);
    mz->material[p] = m;
    EcsEntity e = ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = p == GR_FX_FLASH_STAR ? fx->mesh_star : fx->mesh_flame,
                                                                     .material = m,
                                                                     .transform = {.rotation = quat_identity(), .scale = {1, 1, 1}},
                                                                     .hidden = true,
                                                                     .sort_priority = GR_FX_ORDER_FLASH,
                                                                     .name = p == 0 ? "MuzzleStar" : p == 1 ? "MuzzleFlameA" : "MuzzleFlameB"});
    ecs_component_set(w, e, GrFxFlashPart, {.role = p});
    gr_look_viewmodel(w, e); // the ViewmodelFx layer
    fx->flash_parts[p] = e;
  }
  // the rifle sight: glass and reticle on the ViewmodelFx layer, draw order 90
  EcsEntity sight = ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = fx->mesh_sight,
                                                                      .material = gr_fx_material_colored(fx, r, gr_fx_material_base(r, assets, fx->asset_sight),
                                                                                                         (v3){0.02f, 0.06f, 0.08f}),
                                                                      .transform = {.rotation = quat_identity(), .scale = {1, 1, 1}},
                                                                      .hidden = true,
                                                                      .sort_priority = GR_FX_ORDER_SIGHT,
                                                                      .name = "HoloSight"});
  ecs_component_set(w, sight, GrFxSight, {0});
  gr_look_viewmodel(w, sight);
  mz->built = true;

  GrFxCasings *cs = ecs_resource_get_mut(w, GrFxCasings);
  for (u32 k = 0; k < GR_CASING_KIND_COUNT; k++)
    cs->batch[k] = renderer_instance_batch_create(r, &(RendererInstanceBatchDesc){.submesh = fx->mesh_casing,
                                                                                   .material = gr_fx_material_base(r, assets, fx->asset_casing[k]),
                                                                                   .capacity = GR_FX_CASINGS,
                                                                                   .casts_shadow = false,
                                                                                   .velocity = true});
  km_fx_build_rain(w, r, gr_fx_material_colored(fx, r, gr_fx_material_base(r, assets, fx->asset_rain), v3_scale((v3){0.62f, 0.7f, 0.82f}, 0.9f)));
  fx->built = true;
  log_info("gr_fx: built: % particles per material, % tracers + % beams, % decal slots", fmt_u32(GR_FX_PARTICLES_PER_MATERIAL),
           fmt_u32(GR_FX_TRACERS), fmt_u32(GR_FX_BEAMS), fmt_u32(GR_FX_DECALS));
}

#define GR_FX_FLOOR_CELL 0.25f // metres per height-map texel

// the level's walkable tops as the particle floor: plane y 0 + their highest top per 25 cm cell, R16 over the tops' extent
hz_internal void gr_fx_publish_floor(GrFx *fx, const GrLevel *l) {
  v2 lo = l->grid_origin, hi = l->grid_max;
  u32 w = (u32)m_ceilf((hi.x - lo.x) / GR_FX_FLOOR_CELL), h = (u32)m_ceilf((hi.y - lo.y) / GR_FX_FLOOR_CELL);
  f32 top_max = 1.0f;
  for (u32 t = 0; t < l->top_count; t++)
    top_max = m_maxf(top_max, l->tops[t].y);
  fx->floor_tops = ALLOC_ARRAY(&fx->alloc, u16, w * h);
  for (u32 y = 0; y < h; y++) {
    for (u32 x = 0; x < w; x++) {
      v3 c = {lo.x + ((f32)x + 0.5f) * GR_FX_FLOOR_CELL, 1e9f, lo.y + ((f32)y + 0.5f) * GR_FX_FLOOR_CELL};
      f32 top = gr_level_ground_height(l, c);
      fx->floor_tops[y * w + x] = (u16)(m_clampf(top / top_max, 0.0f, 1.0f) * 65535.0f + 0.5f);
    }
  }
  GpuTexture tex = gpu_make_texture(&(GpuTextureDesc){.data = (u8 *)fx->floor_tops,
                                                      .size = w * h * (u32)sizeof(u16),
                                                      .data_type = GPU_TEXTURE_DATA_R16,
                                                      .width = w,
                                                      .height = h,
                                                      .data_persistent = true});
  particle_system_set_floor(fx->ps, &(ParticleFloorDesc){.plane_y = 0.0f,
                                                         .height_tex = tex,
                                                         .rect_min = lo,
                                                         .rect_max = {lo.x + (f32)w * GR_FX_FLOOR_CELL, lo.y + (f32)h * GR_FX_FLOOR_CELL},
                                                         .height_min = 0.0f,
                                                         .height_max = top_max});
  fx->floor_set = true;
  log_info("gr_fx: particle floor % x % texels over % walkable tops, highest %", fmt_u32(w), fmt_u32(h), fmt_u32(l->top_count),
           fmt_f32_p(top_max, 2));
}

void gr_fx_init(EcsWorld *w) {
  // renderer objects, resources and pooled entities are created once on the main lane
  assert(is_main_thread());
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  ecs_resource_set_zero(w, GrFx);
  GrFx *fx = ecs_resource_get_mut(w, GrFx);
  fx->arena = arena_create(MB(8), KB(64)); // the floor height map is the largest block (~1 MB)
  fx->alloc = make_arena_allocator(fx->arena);
  fx->rng = random_create(0x9f1e5u);
  fx->cd_per_unit = 1.0f / renderer_pre_exposure_from_ev100(RENDERER_EV100_DEFAULT);
  fx->scene_per_unit = 1.0f;
  fx->ps = particle_system_create(&(ParticleSystemInitDesc){.renderer = r,
                                                            .sample_count = renderer_sample_count(r),
                                                            .max_total_particles = GR_FX_PARTICLES_PER_MATERIAL,
                                                            .max_emitters = 128,
                                                            .max_materials = 64,
                                                            .max_emitters_per_material = 64});
  particle_system_attach_to_renderer(fx->ps, r);
  fx->trails = trail_system_create(&(TrailSystemDesc){.renderer = r, .max_trails = GR_FX_TRACERS + GR_FX_BEAMS, .max_points_per_trail = 4, .alloc = &fx->alloc});
  trail_system_attach_to_renderer(fx->trails, r);
  vfx_init(w, &(VfxWorldDesc){.particles = fx->ps,
                              .renderer = r,
                              .trails = fx->trails,
                              .max_loops = 8,
                              .max_lights = GR_FX_LIGHTS,
                              .particles_per_material = GR_FX_PARTICLES_PER_MATERIAL});
  fx->vfx = vfx_world(w);
  vfx_set_light_unit(fx->vfx, fx->cd_per_unit);
  for (u32 i = 0; i < GR_FX_SCORCH_VARIANTS; i++) {
    TempAllocator ta = tctx_temp_allocator_begin(NULL);
    String path = str_format(&ta.allocator, "proving_grounds/fx/decals/scorch_%.hzt", fmt_u32(i));
    fx->scorch[i] = asset_load(TextureBlobAsset, assets, .path = path.value);
    tctx_temp_allocator_end(ta);
  }
  fx->core = asset_load(TextureBlobAsset, assets, .path = "proving_grounds/fx/decals/core.hzt");
  fx->asset_muzzle = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_muzzle_mat.hzt");
  fx->asset_glow = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_glow_mat.hzt");
  fx->asset_ring = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_ring_mat.hzt");
  fx->asset_unlit = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_unlit_mat.hzt");
  fx->asset_dust = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_dust_mat.hzt");
  fx->asset_rain = asset_load(MaterialBlobAsset, assets, .path = "kamino/fx/km_rain_mat.hzt");
  fx->asset_sight = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_sight_mat.hzt");
  fx->asset_beam = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/materials/level/gr_light_column.hzt");
  fx->asset_casing[GR_CASING_BRASS] = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_casing_brass_mat.hzt");
  fx->asset_casing[GR_CASING_SHELL] = asset_load(MaterialBlobAsset, assets, .path = "proving_grounds/fx/materials/gr_fx_casing_shell_mat.hzt");

  ecs_resource_set_zero(w, GrFxFrame);
  ecs_resource_get_mut(w, GrFxFrame)->items = ALLOC_ARRAY(&fx->alloc, GrFxRequest, GR_FX_FRAME_MAX);
  ecs_resource_set_zero(w, GrFxAssets);
  ecs_resource_set_zero(w, GrFxMuzzle);
  ecs_resource_get_mut(w, GrFxMuzzle)->color = (ColorF32){9.0f, 5.0f, 2.0f, 1.0f}; // default flash colour until a gun fires
  ecs_resource_set_zero(w, GrFxCasings);
  ecs_resource_get_mut(w, GrFxCasings)->rng = random_create(0xca5e5u);
  ecs_resource_set_zero(w, GrFxBeacons);

  for (u32 s = 0; s < GR_FX_CASINGS; s++) {
    EcsEntity e = ecs_entity_create(w);
    ecs_component_set(w, e, GrCasing, {.slot = s, .rotation = quat_identity()});
  }
  // pickup beacons: one per gun and per health pack; hidden until their pickup is out
  for (u32 b = 0; b < GR_GUN_COUNT + GR_PACK_COUNT; b++) {
    b32 gun = b < GR_GUN_COUNT;
    EcsEntity e = ecs_entity_create(w);
    ecs_component_set(w, e, WorldTransform, {.position = V3_ZERO, .rotation = quat_identity(), .scale = {1, 1, 1}});
    ecs_component_set(w, e, GrBeacon, {.kind = gun ? GR_BEACON_WEAPON : GR_BEACON_HEALTH,
                                       .index = gun ? b : b - GR_GUN_COUNT,
                                       .name = gun ? (String64Bytes){0} : fixedstr64_from_cstr("Health")}); // gun names: gr_beacon_update_system
  }
  // the front's capture point: shown while the droids fall back
  EcsEntity objective = ecs_entity_create(w);
  ecs_component_set(w, objective, WorldTransform, {.position = V3_ZERO, .rotation = quat_identity(), .scale = {1, 1, 1}});
  ecs_component_set(w, objective, GrBeacon, {.kind = GR_BEACON_OBJECTIVE});
}

// inside the scope after the producers (player, weapon, drones and bolts) publish this frame
void gr_fx_schedule(EcsWorld *w) {
  ecs_schedule_single(w, gr_fx_shatter_system); // spawns debris and creates materials: writes RendererState
  ecs_schedule(w, gr_debris_system);
  ecs_schedule_single(w, gr_debris_glow_system);     // writes debris materials: RendererState
  ecs_schedule_single(w, gr_bolt_look_build_system); // spawns renderer instances: RendererState
  ecs_schedule(w, gr_bolt_part_system);
  ecs_schedule_single(w, gr_fx_events_system);      // writes the GrFxFrame resource
  ecs_schedule_single(w, gr_fx_drones_consolidate); // writes the GrFxFrame resource
  ecs_schedule_single(w, gr_fx_bolts_consolidate);  // writes the GrFxFrame resource
  ecs_schedule_single(w, gr_fx_casings_consolidate); // writes the GrFxCasings resource
  ecs_schedule(w, gr_casing_system);
  ecs_schedule_single(w, gr_fx_casings_write_system); // writes the casing batches: RendererState
  ecs_schedule_single(w, gr_fx_muzzle_consolidate);   // writes GrFxMuzzle and its materials: RendererState
  ecs_schedule(w, gr_fx_flash_part_system);
  ecs_schedule(w, gr_fx_sight_system);
  ecs_schedule(w, gr_beacon_update_system);
  ecs_schedule_single(w, gr_beacon_build_system);    // spawns renderer instances: RendererState
  ecs_schedule_single(w, gr_fx_beacons_consolidate); // writes the GrFxBeacons resource
  ecs_schedule(w, gr_beacon_part_system);
}

// main lane, after the frame's flush and before renderer_update: vfx, trails and batches are main-lane APIs
void gr_fx_frame(EcsWorld *w, Camera *cam) {
  // vfx_frame, trail edits and batch submits are main-lane calls
  assert(is_main_thread());
  GrFx *fx = ecs_resource_get_mut(w, GrFx);
  GrFxFrame *frame = ecs_resource_get_mut(w, GrFxFrame);
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  const GrClock *clock = ecs_resource_get(w, GrClock);
  if (!fx->built) {
    // the dust fills the gathered hub
    if (!gr_fx_assets_ready(fx, ecs_resource_get(w, AssetSystem)) || !ecs_resource_get(w, GrLevel)->tables_ready ||
        !ecs_resource_get(w, GrWeaponRules)->ready) {
      frame->count = 0;
      ecs_resource_get_mut(w, GrFxBeacons)->count = 0;
      return;
    }
    gr_fx_build(w, fx);
  }
  fx->scene_per_unit = renderer_pre_exposure(r) * fx->cd_per_unit;
  const GrLevel *level = ecs_resource_get(w, GrLevel);
  if (!fx->floor_set && level->tables_ready)
    gr_fx_publish_floor(fx, level);

  ecs_resource_get_mut(w, GrFxAssets)->scene_per_unit = fx->scene_per_unit;
  if (frame->dropped)
    log_warn("gr_fx: % requests over the frame's %", fmt_u32(frame->dropped), fmt_u32(GR_FX_FRAME_MAX));
  frame->count = 0;
  frame->dropped = 0;

  const GrFxCasings *cs = ecs_resource_get(w, GrFxCasings);
  if (cs->active > 0)
    for (u32 k = 0; k < GR_CASING_KIND_COUNT; k++)
      renderer_instance_batch_submit(r, cs->batch[k], GR_FX_CASINGS);
  gr_fx_labels_frame(w, fx, r, cam);
  ecs_resource_get_mut(w, GrFxBeacons)->count = 0;

  if (!cam)
    return;
  vfx_frame(w, cam, clock->dt);
  particle_system_update(fx->ps, &(ParticleSystemFlushDesc){.delta_time = clock->dt, .camera = cam});
  trail_system_update(fx->trails, clock->dt);
  trail_system_prepare(fx->trails, cam);
}
