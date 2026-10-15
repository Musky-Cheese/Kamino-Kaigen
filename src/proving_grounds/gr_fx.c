// Proving Grounds effects: particle recipes, tracers, impact decals and flash lights as vfx effects; systems throw requests into the vfx buffer.

#define GR_FX_TRACERS 64      // live tracers
#define GR_FX_LIGHTS 3        // pooled flash point lights
#define GR_FX_BEAMS 16        // rail beams live 0.4 s at the Lancer's 1 shot per ~0.9 s; one pool per trail texture
#define GR_FX_DECALS 96       // live impact decals
#define GR_FX_SCORCH_VARIANTS 4
#define GR_FX_TRACER_LENGTH 7.0f
#define GR_FX_DECAL_DEPTH 0.12f

// transparent draw order as sort_priority; particles and trails draw after every mesh transparent
#define GR_FX_ORDER_BEAM 5
#define GR_FX_ORDER_DUST 15
#define GR_FX_ORDER_RING 20
#define GR_FX_ORDER_LABEL 30
#define GR_FX_ORDER_SIGHT 90
#define GR_FX_ORDER_FLASH 100

typedef enum GrFxTex {
  GR_FX_TEX_SPARK = 0, // radial (1 - d)^2
  GR_FX_TEX_GLOW,      // gaussian exp(-4.5 r^2)
  GR_FX_TEX_PUFF,      // soft value-noise puff
  GR_FX_TEX_TRACER,    // across core^3 x head smoothstep, brightness folded in
  GR_FX_TEX_BEAM,      // the same with the head at full brightness along the length
  GR_FX_TEX_COUNT,
} GrFxTex;

typedef enum GrFxFade {
  GR_FX_FADE_SPARK = 0, // 1 - t^2
  GR_FX_FADE_GLOW,      // (1 - t)^2
  GR_FX_FADE_SMOKE,     // min(1, 6t) (1 - t)^2
} GrFxFade;

// one particle emit shape, in look units (full sizes, drag per second, gravity down)
typedef struct GrFxSpec {
  const char *name;
  u32 tex;
  f32 additive;
  v3 hot;  // linear HDR colour at birth
  v3 cool; // at death; zero = hot
  f32 alpha;
  u32 fade;
  v2 life;
  v2 speed;
  v2 size; // {begin, end} full width
  v2 size_max; // {begin, end} upper bound of rand(size, size_max) per particle; 0 = exact
  b32 size_linked; // no end size: the end reuses the begin's draw
  f32 spread_deg;
  f32 drag;
  f32 gravity;
  f32 stretch; // seconds of travel the spark streak shows
  f32 bounce;  // floor restitution; 0 = no floor collision
  v2 spin;
  u32 shape; // ParticleShapeType; 0 = cone
  f32 radius;
  f32 count;
  u32 pool;
  f32 soft; // metres of soft depth fade
} GrFxSpec;

// main-lane fx state: everything here is touched only on the main lane, outside the frame's scope
typedef struct GrFx {
  ArenaAllocator *arena;
  Allocator alloc;
  ParticleSystem *ps;
  VfxWorld *vfx;
  TrailSystem *trails;
  GpuTexture tex[GR_FX_TEX_COUNT];
  HzHandleT(TextureBlobAsset) scorch[GR_FX_SCORCH_VARIANTS];
  HzHandleT(TextureBlobAsset) core;
  Random rng;
  // the effect objects' shared meshes and materials
  HzHandleT(RendererSubMesh) mesh_sphere, mesh_quad, mesh_ring, mesh_casing, mesh_star, mesh_flame, mesh_sight, mesh_beam;
  HzHandleT(RendererMaterial) mat_core[GR_DRONE_TYPE_COUNT], mat_glow[GR_DRONE_TYPE_COUNT], mat_ring[3], mat_beam[3];
  HzHandleT(MaterialBlobAsset) asset_muzzle, asset_glow, asset_ring, asset_unlit, asset_dust, asset_rain, asset_sight, asset_beam, asset_casing[GR_CASING_KIND_COUNT];
  EcsEntity flash_parts[GR_FX_FLASH_PART_COUNT];
  f32 scene_per_unit;   // look HDR unit -> pre-exposed scene value (pre-exposure / its EV100 default)
  f32 cd_per_unit;      // look HDR unit -> cd/m2 and candela (1 / the default pre-exposure)
  u16 *floor_tops; // the walkable tops' top-down height map, R16 (the particle floor reads it)
  b32 built;
  b32 floor_set;
} GrFx;
ECS_COMPONENT_TYPEDEF(GrFx);

// ---------------------------------------------------------------- effect colours (linear HDR, look units)

#define GR_SPARK_HOT ((v3){9.0f, 5.5f, 2.4f})
#define GR_SPARK_COOL ((v3){2.2f, 0.45f, 0.06f})
#define GR_METAL_HOT ((v3){12.0f, 10.0f, 7.0f})
#define GR_IMPACT_FLASH ((v3){7.0f, 4.5f, 2.4f})
#define GR_DUST ((v3){0.22f, 0.21f, 0.2f})
#define GR_FIRE_HOT ((v3){14.0f, 6.0f, 1.6f})
#define GR_FIRE_COOL ((v3){1.4f, 0.22f, 0.04f})
#define GR_EXPLOSION_CORE ((v3){12.0f, 8.0f, 4.0f})
#define GR_SMOKE_DARK ((v3){0.06f, 0.06f, 0.065f})
#define GR_VENT ((v3){0.5f, 0.46f, 0.46f})
#define GR_PAD_HOT ((v3){2.0f, 8.0f, 12.0f})
#define GR_PAD_COOL ((v3){0.1f, 0.6f, 1.2f})
#define GR_PAD_FLASH ((v3){1.5f, 6.0f, 9.0f})
#define GR_TRAIL_HOT ((v3){6.0f, 2.6f, 0.9f})  // the debris fire trail
#define GR_TRAIL_COOL ((v3){1.5f, 0.25f, 0.05f})
#define GR_DEBRIS_SMOKE ((v3){0.05f, 0.05f, 0.055f})

// 0xRRGGBB sRGB to linear
hz_internal v3 gr_hex_linear(u32 hex) {
  return vec3_srgb_to_linear((v3){(f32)((hex >> 16) & 255u) / 255.0f, (f32)((hex >> 8) & 255u) / 255.0f, (f32)(hex & 255u) / 255.0f});
}

// each drone type's energy colour
hz_internal v3 gr_drone_energy(u32 type) {
  switch (type) {
  case GR_DRONE_STRIKER: return gr_hex_linear(0xff5a1f);
  case GR_DRONE_SENTINEL: return gr_hex_linear(0xd63bff);
  default: return gr_hex_linear(0xff2d55);
  }
}

// each drone type's scale and bolt radius / speed
hz_internal f32 gr_drone_scale(u32 type) { return type == GR_DRONE_SENTINEL ? 1.7f : type == GR_DRONE_STRIKER ? 1.2f : 0.95f; }
hz_internal f32 gr_bolt_radius(u32 type) { return type == GR_DRONE_SENTINEL ? 0.42f : 0.18f; }
hz_internal f32 gr_bolt_speed(u32 type) { return type == GR_DRONE_SENTINEL ? 17.0f : type == GR_DRONE_STRIKER ? 27.0f : 30.0f; }

// each gun's tracer and muzzle light
hz_internal v3 gr_tracer_color(u32 weapon) {
  switch (weapon) {
  case GR_WEAPON_BREACHER: return (v3){10.0f, 4.2f, 1.2f};
  case GR_WEAPON_LANCER: return (v3){14.0f, 1.6f, 2.4f};
  default: return (v3){9.0f, 6.0f, 2.6f};
  }
}
hz_internal f32 gr_tracer_speed(u32 weapon) { return weapon == GR_WEAPON_BREACHER ? 260.0f : 320.0f; }
hz_internal f32 gr_tracer_width(u32 weapon) {
  return weapon == GR_WEAPON_BREACHER ? 0.025f : weapon == GR_WEAPON_LANCER ? 0.05f : 0.035f;
}
hz_internal u32 gr_muzzle_light_hex(u32 weapon) {
  return weapon == GR_WEAPON_BREACHER ? 0xff9a50u : weapon == GR_WEAPON_LANCER ? 0xff4a5au : 0xffb070u;
}

hz_internal v3 gr_v3_maxc(v3 a) { return (v3){m_maxf(a.x, 0.0f), m_maxf(a.y, 0.0f), m_maxf(a.z, 0.0f)}; }
hz_internal f32 gr_v3_peak(v3 a) { return m_maxf(a.x, m_maxf(a.y, a.z)); }

// ---------------------------------------------------------------- requests

hz_internal v3 gr_safe_normalize(v3 v, v3 fallback) {
  f32 l = v3_length(v);
  return l > 1e-6f ? v3_scale(v, 1.0f / l) : fallback;
}

hz_internal f32 gr_hash01(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return (f32)(x & 0xFFFFFFu) / 16777216.0f;
}

// a request's own variation seed, so a play needs no shared rng (systems on any lane throw requests)
hz_internal u32 gr_fx_seed(const GrFxRequest *q) {
  union { f32 f; u32 u; } x = {q->p.x}, y = {q->p.y}, z = {q->p.z};
  return x.u * 73856093u ^ y.u * 19349663u ^ z.u * 83492791u ^ q->kind * 2654435761u;
}

hz_internal void gr_fx_frame_push(GrFxFrame *f, GrFxRequest r) {
  if (f->count < GR_FX_FRAME_MAX)
    f->items[f->count++] = r;
  else
    f->dropped++;
}

hz_internal void gr_fx_play(const VfxRequests *rq, HzHandleT(VfxAsset) h, v3 at, v3 dir, f32 scale, f32 count_scale) {
  vfx_request(rq, h, &(VfxPlayDesc){.at = at, .dir = dir, .scale = scale, .count_scale = count_scale});
}

// a flash light at `at`; `scale` widens its reach and `intensity` its strength
hz_internal void gr_fx_flash(const VfxRequests *rq, HzHandleT(VfxAsset) h, v3 at, f32 scale, f32 intensity) {
  vfx_request(rq, h, &(VfxPlayDesc){.at = at, .scale = scale, .intensity_scale = intensity});
}

// the ribbon texture holds core and head with the brightness divided by 2.6; the colour carries it back
hz_internal void gr_fx_ribbon(const VfxRequests *rq, const GrFxAssets *a, const GrFxRequest *q, b32 beam) {
  v3 delta = v3_sub(q->n, q->p);
  if (v3_length(delta) < (beam ? 0.05f : 0.5f))
    return;
  u32 w = q->variant < GR_GUN_COUNT ? q->variant : GR_WEAPON_RIFLE;
  v3 c = v3_scale(gr_tracer_color(w), 2.6f * a->scene_per_unit);
  vfx_request(rq, beam ? a->beam[w] : a->tracer[w], &(VfxPlayDesc){.at = q->p, .to = q->n, .color = {c.x, c.y, c.z, 1.0f}});
}

// the vfx plays of a request, any lane; the muzzle and casing objects read the frame too
hz_internal void gr_fx_throw(const VfxRequests *rq, const GrFxAssets *a, GrFxFrame *f, GrFxRequest r) {
  if (!a->ready)
    return;
  const GrFxRequest *q = &r;
  u32 seed = gr_fx_seed(q);
  u32 t = q->variant < GR_DRONE_TYPE_COUNT ? q->variant : 0;
  v3 n = gr_safe_normalize(q->n, V3_UP);
  switch (q->kind) {
  case GR_FX_IMPACT_WORLD: {
    v3 refl = v3_reflect(gr_safe_normalize(q->d, v3_negate(n)), n);
    v3 dir = gr_safe_normalize(v3_lerp(refl, n, 0.5f), n);
    gr_fx_play(rq, a->impact_sparks, v3_add(q->p, v3_scale(n, 0.02f)), dir, 1.0f, (10.0f + m_floorf(gr_hash01(seed) * 7.0f)) / 13.0f); // 10..16 sparks over the 13 of the recipe
    gr_fx_play(rq, a->impact_flash, v3_add(q->p, v3_scale(n, 0.06f)), V3_ZERO, (0.45f + gr_hash01(seed + 1) * 0.2f) / 0.55f, 1.0f);
    gr_fx_play(rq, a->impact_dust, v3_add(q->p, v3_scale(n, 0.12f)), n, 1.0f, 1.0f);
    gr_fx_play(rq, a->impact_decal, q->p, n, 1.0f, 1.0f);
    gr_fx_flash(rq, a->light_impact, v3_add(q->p, v3_scale(n, 0.35f)), 1.0f, 1.0f);
  } break;
  case GR_FX_IMPACT_DRONE: {
    v3 refl = v3_reflect(gr_safe_normalize(q->d, v3_negate(n)), n);
    gr_fx_play(rq, a->drone_sparks, q->p, gr_safe_normalize(v3_lerp(refl, n, 0.3f), n), 1.0f, 1.0f);
    gr_fx_play(rq, a->drone_glow[t], q->p, V3_ZERO, 1.0f, 1.0f);
  } break;
  case GR_FX_IMPACT_BOLT: {
    // the bolt hit the player at 0.8 (1.6 sentinel), a wall at 1 (1.8)
    u32 k = q->scale > 0.0f && (q->scale < 0.95f || (q->scale > 1.5f && q->scale < 1.7f)) ? 0u : 1u;
    f32 s = q->scale > 0.0f ? q->scale : 1.0f;
    gr_fx_play(rq, a->bolt_sparks[t][k], q->p, n, 1.0f, 1.0f);
    gr_fx_play(rq, a->bolt_glow[t], v3_add(q->p, v3_scale(n, 0.1f)), V3_ZERO, s, 1.0f);
    gr_fx_flash(rq, a->light_bolt[t], v3_add(q->p, v3_scale(n, 0.4f)), s, s);
  } break;
  case GR_FX_EXPLOSION:
    gr_fx_play(rq, a->ex_core[t], q->p, V3_ZERO, 1.0f, 1.0f);
    gr_fx_play(rq, a->ex_fire[t], q->p, V3_ZERO, 1.0f, 1.0f);
    gr_fx_play(rq, a->ex_energy[t], q->p, V3_ZERO, 1.0f, 1.0f);
    gr_fx_play(rq, a->ex_sparks[t], q->p, V3_UP, 1.0f, 1.0f);
    gr_fx_play(rq, a->ex_smoke[t], q->p, V3_UP, 1.0f, 1.0f);
    gr_fx_flash(rq, a->light_explosion[t], q->p, 1.0f, 1.0f);
    break;
  case GR_FX_WARP_IN:
    gr_fx_play(rq, a->warp_sparks[t], q->p, V3_ZERO, 1.0f, 1.0f);
    gr_fx_play(rq, a->warp_glow[t], q->p, V3_ZERO, 1.0f, 1.0f);
    gr_fx_flash(rq, a->light_warp[t], q->p, 1.0f, 1.0f);
    break;
  case GR_FX_DAMAGE_SMOKE:
    gr_fx_play(rq, a->damage_smoke, q->p, V3_UP, 1.0f, 1.0f);
    if (gr_hash01(seed + 2) < 0.35f)
      gr_fx_play(rq, a->damage_spark, q->p, V3_ZERO, 1.0f, 1.0f);
    break;
  case GR_FX_LAUNCH:
    gr_fx_play(rq, a->launch_sparks, v3_add(q->p, (v3){0.0f, 0.15f, 0.0f}), V3_UP, 1.0f, 1.0f);
    gr_fx_play(rq, a->launch_glow, v3_add(q->p, (v3){0.0f, 0.3f, 0.0f}), V3_ZERO, 1.0f, 1.0f);
    gr_fx_flash(rq, a->light_launch, (v3){q->p.x, 1.0f, q->p.z}, 1.0f, 1.0f);
    break;
  case GR_FX_VENT: gr_fx_play(rq, a->vent, q->p, gr_safe_normalize(q->d, V3_UP), 1.0f, 1.0f); break;
  case GR_FX_BOLT_TRAIL: gr_fx_play(rq, a->bolt_trail[t], q->p, gr_safe_normalize(q->d, V3_UP), 1.0f, 1.0f); break;
  case GR_FX_DEBRIS_SPARK: gr_fx_play(rq, a->debris_spark, q->p, V3_ZERO, 1.0f, 1.0f); break;
  case GR_FX_DEBRIS_SMOKE: gr_fx_play(rq, a->debris_smoke, q->p, V3_UP, 1.0f, 1.0f); break;
  case GR_FX_DRONE_FIRE: gr_fx_play(rq, a->drone_fire[t], q->p, gr_safe_normalize(q->d, V3_UP), 1.0f, 1.0f); break;
  case GR_FX_TRACER: gr_fx_ribbon(rq, a, q, false); break;
  case GR_FX_BEAM: gr_fx_ribbon(rq, a, q, true); break;
  case GR_FX_MUZZLE: // 22 x (1.6 shotgun, 1.4 piercing, 1); `scale` carries the multiplier
    gr_fx_flash(rq, a->light_muzzle[q->variant < GR_GUN_COUNT ? q->variant : 0], q->p, 1.0f, q->scale > 0.0f ? q->scale : 1.0f);
    gr_fx_frame_push(f, r);
    break;
  case GR_FX_CASING: gr_fx_frame_push(f, r); break;
  default: break;
  }
}

// ---------------------------------------------------------------- systems

// consolidation: the weapon's and the player's events this frame (GrWeaponState, GrPlayerState are one record each)
ecs_system(gr_fx_events_system, EcsResIn(GrWeaponState), EcsResIn(GrWeaponRules), EcsResIn(GrPlayerState),
           EcsResIn(VfxRequests), EcsResIn(GrFxAssets), EcsResInOut(GrFxFrame)) {
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  GrFxFrame *f = ecs_sys_get_resource_mut(GrFxFrame);
  for (u32 s = 0; s < ev->shot_count; s++) {
    const GrShotEvent *shot = &ev->shots[s];
    const GrGunConfig *g = &cfg->guns[shot->gun < GR_GUN_COUNT ? shot->gun : 0];
    // a shot: the flash and its light at the real muzzle; tracers (or the rail beam) from where it is seen
    gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_MUZZLE, .variant = shot->gun, .scale = shot->light_intensity / GR_MUZZLE_LIGHT,
                                      .p = shot->muzzle});
    for (u32 p = shot->first_pellet; p < shot->first_pellet + shot->pellet_count && p < ev->pellet_count; p++) {
      const GrPelletEvent *pe = &ev->pellets[p];
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = g->tracer_beam ? GR_FX_BEAM : GR_FX_TRACER, .variant = shot->gun,
                                        .p = shot->tracer_start, .n = pe->end});
      if (pe->end_kind == GR_PELLET_WORLD)
        gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_WORLD, .p = pe->end, .n = pe->normal, .d = pe->direction});
    }
  }
  // a drone impact for every extra pellet on an already-hit drone; the first hit per drone is the drones consolidation's
  for (u32 h = 0; h < ev->drone_impact_count; h++) {
    const GrDroneImpactEvent *im = &ev->drone_impacts[h];
    gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_DRONE, .variant = im->drone_type, .p = im->point, .n = im->normal,
                                      .d = im->direction});
  }
  for (u32 e = 0; e < ev->eject_count; e++) {
    const GrEjectEvent *ej = &ev->ejects[e];
    if (ej->kind == GR_EJECT_VENT)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_VENT, .p = ej->position, .d = ej->velocity});
    else
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_CASING, .variant = ej->kind == GR_EJECT_SHELL ? GR_CASING_SHELL : GR_CASING_BRASS,
                                        .p = ej->position, .d = ej->velocity});
  }
  // a jump pad launch: the launch burst at the pad
  const GrPlayerEvents *pev = &ecs_sys_get_resource(GrPlayerState)->events;
  if (pev->launched)
    gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_LAUNCH, .p = pev->launch_pad});
}

// consolidation: each drone's events and the weapon's first hit on it (a drone impact in its colour); <= GR_MAX_DRONES rows
ecs_system(gr_fx_drones_consolidate, EcsIn(GrDrone), EcsResIn(GrClock), EcsResIn(GrWeaponState), EcsResIn(VfxRequests), EcsResIn(GrFxAssets),
           EcsResInOut(GrFxFrame)) {
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  GrFxFrame *f = ecs_sys_get_resource_mut(GrFxFrame);
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    for (u32 h = 0; h < ev->drone_hit_count; h++) {
      const GrDroneHitEvent *hit = &ev->drone_hits[h];
      if (hit->drone == it->entities[i])
        gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_DRONE, .variant = d->type, .p = hit->point, .n = hit->normal,
                                          .d = hit->direction});
    }
    if (d->events_frame != frame || d->events == 0)
      continue;
    if (d->events & GR_DRONE_EV_SPAWNED)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_WARP_IN, .variant = d->type, .p = d->position});
    if (d->events & GR_DRONE_EV_SMOKE)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_DAMAGE_SMOKE, .p = d->position});
    if (d->events & GR_DRONE_EV_FIRED)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_DRONE_FIRE, .variant = d->type, .p = d->eye, .d = d->fire_dir});
    if (d->events & GR_DRONE_EV_DIED)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_EXPLOSION, .variant = d->type, .p = d->position});
  }
}

// consolidation: the bolts' trail glows and their impacts; at most bolt_capacity rows
ecs_system(gr_fx_bolts_consolidate, EcsIn(GrBolt), EcsResIn(GrClock), EcsResIn(VfxRequests), EcsResIn(GrFxAssets),
           EcsResInOut(GrFxFrame)) {
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const VfxRequests *rq = ecs_sys_get_resource(VfxRequests);
  const GrFxAssets *as = ecs_sys_get_resource(GrFxAssets);
  GrFxFrame *f = ecs_sys_get_resource_mut(GrFxFrame);
  for (i32 i = 0; i < it->count; i++) {
    const GrBolt *b = ecs_sys_get_component(GrBolt, i);
    if (b->state == GR_BOLT_FLYING && b->trail)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_BOLT_TRAIL, .variant = b->type, .p = b->position, .d = b->velocity});
    if (b->end_frame != frame)
      continue;
    // bolt impact: 0.8 (1.6 with splash) on the player, 1 (1.8) on a wall
    b32 splash = b->splash > 0.0f;
    if (b->state == GR_BOLT_HIT_PLAYER)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_BOLT, .variant = b->type, .scale = splash ? 1.6f : 0.8f,
                                        .p = b->hit_point, .n = b->hit_normal});
    else if (b->state == GR_BOLT_HIT_WALL)
      gr_fx_throw(rq, as, f, (GrFxRequest){.kind = GR_FX_IMPACT_BOLT, .variant = b->type, .scale = splash ? 1.8f : 1.0f,
                                        .p = b->hit_point, .n = b->hit_normal});
  }
}

// ---------------------------------------------------------------- textures

hz_internal GpuTexture gr_fx_upload(u8 *px, u32 w, u32 h) {
  return gpu_make_texture(&(GpuTextureDesc){.data = px, .size = w * h * 4, .data_type = GPU_TEXTURE_DATA_RGBA8, .width = w, .height = h});
}

hz_internal u8 gr_u8(f32 v) { return (u8)(m_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

hz_internal void gr_fx_build_textures(GrFx *fx) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  const u32 n = 64;
  u8 *spark = ALLOC_ARRAY(&ta.allocator, u8, n * n * 4);
  u8 *glow = ALLOC_ARRAY(&ta.allocator, u8, n * n * 4);
  for (u32 y = 0; y < n; y++) {
    for (u32 x = 0; x < n; x++) {
      f32 qx = ((f32)x + 0.5f) / (f32)n * 2.0f - 1.0f, qy = ((f32)y + 0.5f) / (f32)n * 2.0f - 1.0f;
      f32 d = m_sqrtf(qx * qx + qy * qy);
      f32 a = m_clampf(1.0f - d, 0.0f, 1.0f);
      u32 i = (y * n + x) * 4;
      spark[i] = spark[i + 1] = spark[i + 2] = 255;
      spark[i + 3] = gr_u8(a * a);
      glow[i] = glow[i + 1] = glow[i + 2] = 255;
      glow[i + 3] = gr_u8(m_expf(-(qx * qx + qy * qy) * 4.5f));
    }
  }
  fx->tex[GR_FX_TEX_SPARK] = gr_fx_upload(spark, n, n);
  fx->tex[GR_FX_TEX_GLOW] = gr_fx_upload(glow, n, n);

  // the puff: 128^2, value noise of an 8-cell seeded lattice at 4 and 8 cells, white RGB
  const u32 ps = 128, grid = 8, lat_n = (grid + 1) * (grid + 1);
  f32 *lattice = ALLOC_ARRAY(&ta.allocator, f32, lat_n);
  Mulberry32_State rng = {99u}; // fixed seed: the puff is the same every run
  for (u32 i = 0; i < lat_n; i++)
    lattice[i] = mulberry32_next_f32(&rng);
  u8 *puff = ALLOC_ARRAY(&ta.allocator, u8, ps * ps * 4);
  for (u32 y = 0; y < ps; y++) {
    for (u32 x = 0; x < ps; x++) {
      f32 u = (f32)x / (f32)ps, v = (f32)y / (f32)ps;
      f32 nv = 0.0f;
      for (u32 oct = 0; oct < 2; oct++) {
        f32 fq = oct == 0 ? 4.0f : 8.0f, wgt = oct == 0 ? 0.6f : 0.4f;
        f32 px = u * fq, py = v * fq;
        u32 xi = (u32)m_floorf(px), yi = (u32)m_floorf(py);
        f32 fx_ = px - (f32)xi, fy = py - (f32)yi;
        f32 sx = fx_ * fx_ * (3.0f - 2.0f * fx_), sy = fy * fy * (3.0f - 2.0f * fy);
        u32 g1 = grid + 1;
        f32 a00 = lattice[(yi % g1) * g1 + (xi % g1)], a10 = lattice[(yi % g1) * g1 + ((xi + 1) % g1)];
        f32 a01 = lattice[((yi + 1) % g1) * g1 + (xi % g1)], a11 = lattice[((yi + 1) % g1) * g1 + ((xi + 1) % g1)];
        f32 a = a00 + (a10 - a00) * sx, b = a01 + (a11 - a01) * sx;
        nv += (a + (b - a) * sy) * wgt;
      }
      f32 r = m_sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2.0f;
      f32 a = m_clampf(1.0f - r * (1.0f + (nv - 0.5f) * 1.2f), 0.0f, 1.0f);
      a = a * a * (3.0f - 2.0f * a) * (0.55f + 0.45f * nv);
      u32 i = (y * ps + x) * 4;
      puff[i] = puff[i + 1] = puff[i + 2] = 255;
      puff[i + 3] = (u8)(m_clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  fx->tex[GR_FX_TEX_PUFF] = gr_fx_upload(puff, ps, ps);

  // the tracer ribbon: x along (tail 0 -> head 1), y across; rgb = (0.6 + 2 core head) / 2.6, a = core head
  const u32 tw = 64, th = 32;
  u8 *tracer = ALLOC_ARRAY(&ta.allocator, u8, tw * th * 4);
  u8 *beam = ALLOC_ARRAY(&ta.allocator, u8, tw * th * 4);
  for (u32 y = 0; y < th; y++) {
    for (u32 x = 0; x < tw; x++) {
      f32 u = ((f32)x + 0.5f) / (f32)tw, v = ((f32)y + 0.5f) / (f32)th;
      f32 across = 1.0f - m_absf(v - 0.5f) * 2.0f;
      f32 core = across * across * across;
      f32 head = m_smoothstepf(0.0f, 1.0f, u);
      u32 i = (y * tw + x) * 4;
      u8 tb = gr_u8((0.6f + 2.0f * core * head) / 2.6f);
      tracer[i] = tracer[i + 1] = tracer[i + 2] = tb;
      tracer[i + 3] = gr_u8(core * head);
      u8 bb = gr_u8((0.6f + 2.0f * core) / 2.6f);
      beam[i] = beam[i + 1] = beam[i + 2] = bb;
      beam[i + 3] = gr_u8(core);
    }
  }
  fx->tex[GR_FX_TEX_TRACER] = gr_fx_upload(tracer, tw, th);
  fx->tex[GR_FX_TEX_BEAM] = gr_fx_upload(beam, tw, th);
  tctx_temp_allocator_end(ta);
}

// ---------------------------------------------------------------- recipes

hz_internal f32 gr_fx_fade(u32 fade, f32 t) {
  switch (fade) {
  case GR_FX_FADE_GLOW: return (1.0f - t) * (1.0f - t);
  case GR_FX_FADE_SMOKE: return m_minf(1.0f, t * 6.0f) * (1.0f - t) * (1.0f - t);
  default: return 1.0f - t * t;
  }
}

// the ramp's 8 stop times per fade curve, denser where the curve bends
hz_internal f32 gr_fx_stop_time(u32 fade, u32 k) {
  if (fade == GR_FX_FADE_SMOKE) {
    switch (k) {
    case 0: return 0.0f;
    case 1: return 1.0f / 6.0f;
    case 2: return 0.3f;
    case 3: return 0.45f;
    case 4: return 0.6f;
    case 5: return 0.75f;
    case 6: return 0.88f;
    default: return 1.0f;
    }
  }
  return (f32)k / 7.0f;
}

// an emit shape as one vfx element: colour lerp x fade curve as the ramp, normalised by the peak `emissive` restores
hz_internal HzHandleT(VfxAsset) gr_fx_register(GrFx *fx, const GrFxSpec *s) {
  v3 hot = gr_v3_maxc(s->hot);
  v3 cool = (s->cool.x == 0.0f && s->cool.y == 0.0f && s->cool.z == 0.0f) ? hot : gr_v3_maxc(s->cool);
  f32 peak = m_maxf(gr_v3_peak(hot), gr_v3_peak(cool));
  f32 k = peak > 1.0f ? peak : 1.0f;
  v2 size_hi = {s->size_max.x != 0.0f ? s->size_max.x : s->size.x, s->size_max.y != 0.0f ? s->size_max.y : s->size.y};
  f32 avg_size = (s->size.x + s->size.y + size_hi.x + size_hi.y) * 0.25f;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  VfxDesc *d = ALLOC(&ta.allocator, VfxDesc);
  d->name = s->name;
  d->count = 1;
  VfxElemDesc *e = &d->elem[0];
  e->tex = fx->tex[s->tex];
  e->additive = s->additive;
  e->stops = VFX_STOP_MAX;
  for (u32 i = 0; i < VFX_STOP_MAX; i++) {
    f32 t = gr_fx_stop_time(s->fade, i);
    v3 c = v3_scale(v3_lerp(hot, cool, t), 1.0f / k);
    e->color[i] = (VfxStop){.t = t, .color = {c.x, c.y, c.z, s->alpha * gr_fx_fade(s->fade, t)}};
  }
  e->count = s->count;
  e->life = s->life;
  e->speed = s->speed;
  e->size = (v2){s->size.x * 0.5f, s->size.y * 0.5f};
  e->size_max = (v2){s->size_max.x * 0.5f, s->size_max.y * 0.5f};
  e->size_linked = s->size_linked;
  e->size_ease = 1.0f;
  e->spread_deg = (v2){s->spread_deg, s->spread_deg};
  e->gravity = (v3){0.0f, -s->gravity, 0.0f};
  e->drag = s->drag > 0.0f ? m_expf(-s->drag / 30.0f) : 0.0f; // e^(-drag dt) as the block's per-frame-at-30-fps factor
  e->radius = s->radius;
  e->spin = s->spin;
  e->rotation = (v2){0.0f, 2.0f * M_PI}; // random start rotation
  e->shape = s->shape;
  e->pool = s->pool;
  e->emissive = k - 1.0f;
  e->velocity_stretch = s->stretch > 0.0f && avg_size > 0.0f ? s->stretch / avg_size : 0.0f;
  e->soft_fade = s->soft;
  e->collide = s->bounce > 0.0f;
  e->restitution = s->bounce;
  e->friction = 0.4f; // xz x 0.6 per floor contact
  HzHandleT(VfxAsset) h = vfx_register(fx->vfx, d);
  tctx_temp_allocator_end(ta);
  return h;
}

// a flash light as a one-element effect: intensity peak x (life / duration)^2
hz_internal HzHandleT(VfxAsset) gr_fx_register_light(GrFx *fx, const char *name, v3 color, f32 intensity, f32 radius, f32 life) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  VfxDesc *d = ALLOC(&ta.allocator, VfxDesc); // ~20 KB: off the stack (wasm frame limit)
  mem_zero(d, sizeof(*d)); // a VfxDesc literal is a ~20 KB stack temporary
  d->name = name;
  d->count = 1;
  d->elem[0] = (VfxElemDesc){.kind = VFX_ELEM_LIGHT, .light = {.color = color, .intensity = intensity, .radius = radius, .life = life, .curve = 2.0f}};
  HzHandleT(VfxAsset) h = vfx_register(fx->vfx, d);
  tctx_temp_allocator_end(ta);
  return h;
}

// the flash lights of world and bolt impacts, explosions, warp-ins, launch bursts and muzzles
hz_internal void gr_fx_register_flashes(GrFx *fx, GrFxAssets *a) {
  a->light_impact = gr_fx_register_light(fx, "light_impact", gr_hex_linear(0xffa352), 5.0f, 4.0f, 0.06f);
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    f32 ds = gr_drone_scale(t);
    a->light_bolt[t] = gr_fx_register_light(fx, "light_bolt", gr_drone_energy(t), 14.0f, 6.0f, 0.12f);
    a->light_explosion[t] = gr_fx_register_light(fx, "light_explosion", gr_hex_linear(0xff8a3a), 110.0f * ds, 12.0f * ds, 0.4f);
    a->light_warp[t] = gr_fx_register_light(fx, "light_warp", gr_drone_energy(t), 28.0f, 9.0f, 0.45f);
  }
  a->light_launch = gr_fx_register_light(fx, "light_launch", gr_hex_linear(0x3fdcff), 40.0f, 8.0f, 0.4f);
  for (u32 g = 0; g < GR_GUN_COUNT; g++)
    a->light_muzzle[g] = gr_fx_register_light(fx, "light_muzzle", gr_hex_linear(gr_muzzle_light_hex(g)), 22.0f, 7.0f, 0.06f);
}

// a tracer: a streak travelling the shot at the weapon's speed, or the rail beam that holds 0.4 s; both are
// additive ribbons whose texture folds the brightness, so the request's colour carries the 2.6
hz_internal void gr_fx_register_tracers(GrFx *fx, GrFxAssets *a, const GrWeaponConfig *cfg) {
  u32 streaks = 0;
  for (u32 g = 0; g < GR_GUN_COUNT; g++)
    streaks += cfg->guns[g].tracer_beam ? 0u : 1u;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  VfxDesc *d = ALLOC(&ta.allocator, VfxDesc); // ~20 KB: off the stack (wasm frame limit)
  for (u32 g = 0; g < GR_GUN_COUNT; g++) {
    mem_zero(d, sizeof(*d));
    d->name = "tracer";
    d->count = 1;
    if (cfg->guns[g].tracer_beam) {
      d->elem[0] = (VfxElemDesc){.kind = VFX_ELEM_BEAM, .tex = fx->tex[GR_FX_TEX_BEAM], .additive = 1.0f, .pool = GR_FX_BEAMS,
                                 .trail = {.width = gr_tracer_width(g), .width_scale = {1.0f, 1.8f}, .life = 0.4f, .fade = 2.0f}};
      a->beam[g] = vfx_register(fx->vfx, d);
    } else {
      d->elem[0] = (VfxElemDesc){.kind = VFX_ELEM_BEAM, .tex = fx->tex[GR_FX_TEX_TRACER], .additive = 1.0f, .pool = GR_FX_TRACERS / streaks,
                                 .trail = {.width = gr_tracer_width(g), .length = GR_FX_TRACER_LENGTH, .speed = gr_tracer_speed(g)}};
      a->tracer[g] = vfx_register(fx->vfx, d);
    }
  }
  tctx_temp_allocator_end(ta);
}

// an impact decal: a scorch mark (one of four, fading out over 16-22 s) over a molten core that cools as e^(-2 age)
hz_internal void gr_fx_register_decals(GrFx *fx, GrFxAssets *a) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  VfxDesc *d = ALLOC(&ta.allocator, VfxDesc); // ~20 KB: off the stack (wasm frame limit)
  mem_zero(d, sizeof(*d));
  d->name = "impact_decal";
  d->count = 2;
  VfxDecalDesc scorch = {.variants = GR_FX_SCORCH_VARIANTS, .size = {0.2f, 0.3f}, .depth = GR_FX_DECAL_DEPTH, .color = {1, 1, 1, 1},
                         .fade_start = 16.0f, .fade_end = 22.0f, .roll = true};
  for (u32 i = 0; i < GR_FX_SCORCH_VARIANTS; i++)
    scorch.albedo[i] = fx->scorch[i];
  // the molten core colour (3.5, 1.1, 0.25) normalised, as the sRGB tint the decal API takes
  VfxDecalDesc core = {.variants = 1, .albedo = {fx->core}, .size = {0.2f, 0.3f}, .depth = GR_FX_DECAL_DEPTH,
                       .color = {1.0f, 0.6015f, 0.2961f, 1.0f}, .emission = 14.0f, .heat_decay = 2.0f, .roll = true, .priority = 1};
  d->elem[0] = (VfxElemDesc){.kind = VFX_ELEM_DECAL, .pool = GR_FX_DECALS, .decal = scorch};
  d->elem[1] = (VfxElemDesc){.kind = VFX_ELEM_DECAL, .pool = GR_FX_DECALS, .decal = core};
  a->impact_decal = vfx_register(fx->vfx, d);
  tctx_temp_allocator_end(ta);
}

hz_internal void gr_fx_register_all(GrFx *fx, GrFxAssets *a, const GrWeaponConfig *cfg) {
  const f32 spread_sphere = 180.0f;
  // world impact
  a->impact_sparks = gr_fx_register(fx, &(GrFxSpec){.name = "impact_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_SPARK_HOT, .cool = GR_SPARK_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.2f, 0.6f}, .speed = {4, 13}, .size = {0.012f, 0.012f}, .size_max = {0.028f, 0.028f}, .size_linked = true, .spread_deg = 45.84f, .drag = 1.5f, .gravity = 14, .stretch = 0.025f, .bounce = 0.4f, .count = 13, .pool = 480});
  a->impact_flash = gr_fx_register(fx, &(GrFxSpec){.name = "impact_flash", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = GR_IMPACT_FLASH, .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.07f, 0.07f}, .size = {0.45f, 0.15f}, .size_max = {0.65f, 0.0f}, .count = 1, .pool = 24});
  a->impact_dust = gr_fx_register(fx, &(GrFxSpec){.name = "impact_dust", .tex = GR_FX_TEX_PUFF, .additive = 0, .hot = GR_DUST, .alpha = 0.32f, .fade = GR_FX_FADE_SMOKE, .life = {0.7f, 1.1f}, .speed = {0.9f, 0.9f}, .size = {0.14f, 0.9f}, .spread_deg = 15.5f, .drag = 2.2f, .gravity = -0.15f, .spin = {-1, 1}, .count = 1, .pool = 48, .soft = 0.6f});
  // drone impact
  a->drone_sparks = gr_fx_register(fx, &(GrFxSpec){.name = "drone_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_METAL_HOT, .cool = GR_SPARK_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.2f, 0.5f}, .speed = {5, 16}, .size = {0.015f, 0.015f}, .size_max = {0.03f, 0.03f}, .size_linked = true, .spread_deg = 57.3f, .drag = 1.8f, .gravity = 12, .stretch = 0.03f, .bounce = 0.35f, .count = 18, .pool = 400});
  for (u32 t = 0; t < GR_DRONE_TYPE_COUNT; t++) {
    v3 e = gr_drone_energy(t);
    a->drone_glow[t] = gr_fx_register(fx, &(GrFxSpec){.name = "drone_glow", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_add(v3_scale(e, 6.0f), GR_IMPACT_FLASH), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.08f, 0.08f}, .size = {0.7f, 0.25f}, .count = 1, .pool = 16});
    // bolt impact: 0.8/1.6 hitting the player, 1/1.8 on a wall; speeds scale, sizes do not
    f32 hit_scale = t == GR_DRONE_SENTINEL ? 1.6f : 0.8f, wall_scale = t == GR_DRONE_SENTINEL ? 1.8f : 1.0f;
    for (u32 k = 0; k < 2; k++) {
      f32 s = k == 0 ? hit_scale : wall_scale;
      a->bolt_sparks[t][k] = gr_fx_register(fx, &(GrFxSpec){.name = "bolt_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = v3_scale(e, 8.0f), .cool = v3_scale(e, 1.5f), .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.2f, 0.45f}, .speed = {3.0f * s, 9.0f * s}, .size = {0.02f, 0.02f}, .size_max = {0.035f, 0.035f}, .size_linked = true, .spread_deg = 68.75f, .drag = 2.5f, .gravity = 5, .stretch = 0.03f, .count = m_ceilf(14.0f * s), .pool = 160});
    }
    a->bolt_glow[t] = gr_fx_register(fx, &(GrFxSpec){.name = "bolt_glow", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_scale(e, 10.0f), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.14f, 0.14f}, .size = {0.9f, 1.6f}, .count = 1, .pool = 16});
    // explosion, at this type's scale
    f32 ds = gr_drone_scale(t), rs = m_sqrtf(ds);
    a->ex_core[t] = gr_fx_register(fx, &(GrFxSpec){.name = "ex_core", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = GR_EXPLOSION_CORE, .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.1f, 0.1f}, .size = {2.2f * ds, 3.2f * ds}, .count = 1, .pool = 8});
    a->ex_fire[t] = gr_fx_register(fx, &(GrFxSpec){.name = "ex_fire", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = GR_FIRE_HOT, .cool = GR_FIRE_COOL, .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.35f, 0.6f}, .speed = {1.5f * ds, 4.0f * ds}, .size = {0.7f * ds, 1.8f * ds}, .size_max = {1.1f * ds, 2.6f * ds}, .spread_deg = spread_sphere, .drag = 3.5f, .radius = 0.275f * ds, .count = 12, .pool = 72});
    a->ex_energy[t] = gr_fx_register(fx, &(GrFxSpec){.name = "ex_energy", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_scale(e, 12.0f), .cool = v3_scale(e, 0.5f), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.3f, 0.3f}, .speed = {4.0f * ds, 8.0f * ds}, .size = {0.6f * ds, 0.2f}, .spread_deg = spread_sphere, .drag = 4, .count = 6, .pool = 36});
    a->ex_sparks[t] = gr_fx_register(fx, &(GrFxSpec){.name = "ex_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_SPARK_HOT, .cool = GR_SPARK_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.5f, 1.5f}, .speed = {5.0f * rs, 20.0f * rs}, .size = {0.02f, 0.02f}, .size_max = {0.05f, 0.05f}, .size_linked = true, .spread_deg = 90.0f, .drag = 1.1f, .gravity = 12, .stretch = 0.035f, .bounce = 0.45f, .count = m_roundf(70.0f * ds), .pool = 720});
    a->ex_smoke[t] = gr_fx_register(fx, &(GrFxSpec){.name = "ex_smoke", .tex = GR_FX_TEX_PUFF, .additive = 0, .hot = GR_SMOKE_DARK, .alpha = 0.55f, .fade = GR_FX_FADE_SMOKE, .life = {1.8f, 2.8f}, .speed = {1.1f, 2.6f}, .size = {0.8f * ds, 2.6f * ds}, .size_max = {0.0f, 3.6f * ds}, .spread_deg = 60.0f, .drag = 1.4f, .gravity = -0.35f, .spin = {-0.6f, 0.6f}, .radius = 0.4f * ds, .count = 12, .pool = 96, .soft = 0.6f});
    // warp-in: imploding from a 2.2 m shell
    a->warp_sparks[t] = gr_fx_register(fx, &(GrFxSpec){.name = "warp_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = v3_scale(e, 7.0f), .cool = (v3){6, 6, 6}, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.35f, 0.35f}, .speed = {-6.2f * ds, -6.2f * ds}, .size = {0.03f, 0.03f}, .stretch = 0.05f, .shape = PARTICLE_SHAPE_SPHERE, .radius = 2.2f * ds, .count = 40, .pool = 240});
    a->warp_glow[t] = gr_fx_register(fx, &(GrFxSpec){.name = "warp_glow", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_add(v3_scale(e, 5.0f), (v3){1.5f, 1.5f, 1.5f}), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.4f, 0.4f}, .size = {0.4f, 2.2f * ds}, .count = 1, .pool = 8});
    // bolt trail: a glow every 12 ms along the bolt
    f32 r = gr_bolt_radius(t);
    a->bolt_trail[t] = gr_fx_register(fx, &(GrFxSpec){.name = "bolt_trail", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_scale(e, 3.0f), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.22f, 0.22f}, .speed = {gr_bolt_speed(t) * 0.05f, gr_bolt_speed(t) * 0.05f}, .size = {r * 1.8f, r * 0.3f}, .count = 1, .pool = 400});
    // the muzzle glow when a drone fires, moving 1 m/s along the bolt
    a->drone_fire[t] = gr_fx_register(fx, &(GrFxSpec){.name = "drone_fire", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = v3_scale(e, 10.0f), .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.1f, 0.1f}, .speed = {1.0f, 1.0f}, .size = {0.6f * ds, 0.1f}, .count = 1, .pool = 16});
  }
  // damage smoke
  a->damage_smoke = gr_fx_register(fx, &(GrFxSpec){.name = "damage_smoke", .tex = GR_FX_TEX_PUFF, .additive = 0, .hot = GR_SMOKE_DARK, .alpha = 0.45f, .fade = GR_FX_FADE_SMOKE, .life = {1.2f, 1.2f}, .speed = {1.2f, 1.25f}, .size = {0.3f, 1.2f}, .spread_deg = 14.0f, .drag = 1, .spin = {-1, 1}, .count = 1, .pool = 96, .soft = 0.6f});
  a->damage_spark = gr_fx_register(fx, &(GrFxSpec){.name = "damage_spark", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_SPARK_HOT, .cool = GR_SPARK_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.4f, 0.4f}, .speed = {2, 6}, .size = {0.02f, 0.02f}, .spread_deg = spread_sphere, .gravity = 10, .stretch = 0.03f, .count = 1, .pool = 32});
  // launch burst: a ring of sparks up the pad
  a->launch_sparks = gr_fx_register(fx, &(GrFxSpec){.name = "launch_sparks", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_PAD_HOT, .cool = GR_PAD_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.4f, 0.8f}, .speed = {6, 14}, .size = {0.03f, 0.03f}, .spread_deg = 8.0f, .drag = 2, .gravity = 4, .stretch = 0.04f, .radius = 1.1f, .count = 32, .pool = 128});
  a->launch_glow = gr_fx_register(fx, &(GrFxSpec){.name = "launch_glow", .tex = GR_FX_TEX_GLOW, .additive = 1, .hot = GR_PAD_FLASH, .alpha = 1, .fade = GR_FX_FADE_GLOW, .life = {0.35f, 0.35f}, .size = {3.0f, 0.5f}, .count = 1, .pool = 8});
  // vent puff
  a->vent = gr_fx_register(fx, &(GrFxSpec){.name = "vent", .tex = GR_FX_TEX_PUFF, .additive = 0, .hot = GR_VENT, .alpha = 0.28f, .fade = GR_FX_FADE_SMOKE, .life = {0.5f, 0.8f}, .speed = {0.45f, 0.95f}, .size = {0.03f, 0.25f}, .size_max = {0.05f, 0.4f}, .spread_deg = 20.0f, .drag = 3, .gravity = -0.6f, .spin = {-2, 2}, .count = 3, .pool = 24, .soft = 0.6f});
  // debris: the burning trail and the smoke wisps after it
  a->debris_spark = gr_fx_register(fx, &(GrFxSpec){.name = "debris_spark", .tex = GR_FX_TEX_SPARK, .additive = 1, .hot = GR_TRAIL_HOT, .cool = GR_TRAIL_COOL, .alpha = 1, .fade = GR_FX_FADE_SPARK, .life = {0.35f, 0.65f}, .speed = {1.5f, 1.5f}, .size = {0.035f, 0.035f}, .spread_deg = spread_sphere, .drag = 2, .gravity = 6, .stretch = 0.03f, .count = 1, .pool = 1200});
  a->debris_smoke = gr_fx_register(fx, &(GrFxSpec){.name = "debris_smoke", .tex = GR_FX_TEX_PUFF, .additive = 0, .hot = GR_DEBRIS_SMOKE, .alpha = 0.35f, .fade = GR_FX_FADE_SMOKE, .life = {1.4f, 1.4f}, .speed = {0.6f, 0.6f}, .size = {0.25f, 0.9f}, .spin = {0.6f, 0.6f}, .count = 1, .pool = 160, .soft = 0.6f});
  gr_fx_register_flashes(fx, a);
  gr_fx_register_tracers(fx, a, cfg);
  gr_fx_register_decals(fx, a);
}
