// Kamino feel: a B2 shatters into armour and frame chunks that tumble, bounce and smoke before they fade.

#define KM_CHUNKS_PER_B2 10
#define KM_CHUNK_LIFE 3.2f
#define KM_CHUNK_GRAVITY 16.0f

// one chunk of a shattered B2
typedef struct KmChunk {
  v3 position;
  v3 velocity;
  v3 spin;
  quat rotation;
  v3 size;
  f32 life;
  f32 smoke_timer;
  b32 smokes;
  b32 resting;
} KmChunk;
ECS_COMPONENT_TYPEDEF(KmChunk);

// the chunk mesh: a unit chamfered box, uploaded once on the main lane
typedef struct KmFeel {
  HzHandleT(RendererSubMesh) chunk_mesh;
  b32 ready;
} KmFeel;
ECS_COMPONENT_TYPEDEF(KmFeel);

// a B2's death: chunks burst out of its hull, the heavier ones trailing smoke
ecs_system(km_chunk_spawn_system, EcsIn(GrDrone), EcsResIn(GrClock), EcsResIn(KmFeel), EcsResIn(KmWorld), EcsResInOut(RendererState)) {
  u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const KmFeel *feel = ecs_sys_get_resource(KmFeel);
  const KmWorld *km = ecs_sys_get_resource(KmWorld);
  if (!feel->ready || !km->built)
    return;
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->events_frame != frame || !(d->events & GR_DRONE_EV_DIED) || d->type != GR_DRONE_STRIKER)
      continue;
    Random rng = random_create((u32)it->entities[i] * 2654435761u + frame);
    v3 centre = v3_add(d->position, (v3){0.0f, 0.4f, 0.0f});
    v3 inherited = v3_add(v3_scale(d->velocity, 0.5f), v3_scale(d->hit_dir, 2.5f));
    for (u32 c = 0; c < KM_CHUNKS_PER_B2; c++) {
      b32 armour = c < 6;
      v3 dir = v3_normalize((v3){random_f32_range(&rng, -1.0f, 1.0f), random_f32_range(&rng, 0.2f, 1.2f), random_f32_range(&rng, -1.0f, 1.0f)});
      f32 speed = random_f32_range(&rng, 4.0f, 9.0f);
      v3 size = armour ? (v3){random_f32_range(&rng, 0.18f, 0.4f), random_f32_range(&rng, 0.05f, 0.09f), random_f32_range(&rng, 0.16f, 0.34f)}
                       : (v3){random_f32_range(&rng, 0.05f, 0.09f), random_f32_range(&rng, 0.2f, 0.5f), random_f32_range(&rng, 0.05f, 0.09f)};
      KmChunk ch = {.position = v3_add(centre, v3_scale(dir, 0.4f)),
                    .velocity = v3_add(v3_scale(dir, speed), inherited),
                    .spin = (v3){random_f32_range(&rng, -9.0f, 9.0f), random_f32_range(&rng, -9.0f, 9.0f), random_f32_range(&rng, -9.0f, 9.0f)},
                    .rotation = quat_axis_angle(dir, random_f32_range(&rng, 0.0f, 6.28f)),
                    .size = size,
                    .life = KM_CHUNK_LIFE + random_f32_range(&rng, -0.6f, 0.8f),
                    .smokes = c < 3};
      EcsEntity e = ecs_renderer_spawn(it->world, r, &(EcsRendererSpawnDesc){.submesh = feel->chunk_mesh,
                                                                         .material = armour ? km->hull_material : km->dark_material,
                                                                         .transform = {ch.position, ch.rotation, size},
                                                                         .casts_shadow = true, .name = "B2Chunk"});
      ecs_component_set(it->world, e, KmChunk, ch);
    }
  }
}

// chunks fly, bounce off the level, smoke, settle and shrink away
ecs_system(km_chunk_system, EcsInOut(KmChunk), EcsInOut(WorldTransform), EcsResIn(GrClock), EcsResInOut(KmAirFx)) {
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  KmAirFx *fx = ecs_sys_get_resource_mut(KmAirFx);
  const EcsCollisionLayer *layer = gr_level_collision(it->world);
  if (fx->frame != clock->frame) {
    fx->frame = clock->frame;
    fx->count = 0;
  }
  f32 dt = m_minf(clock->dt, 0.05f);
  for (i32 i = 0; i < it->count; i++) {
    KmChunk *ch = ecs_sys_get_component_mut(KmChunk, i);
    ch->life -= dt;
    if (ch->life <= 0.0f || ch->position.y < KM_OCEAN_Y) {
      ecs_entity_destroy(it->world, it->entities[i]);
      continue;
    }
    if (!ch->resting) {
      ch->velocity.y -= KM_CHUNK_GRAVITY * dt;
      v3 step = v3_scale(ch->velocity, dt);
      f32 len = v3_length(step);
      f32 rad = m_maxf(ch->size.y, 0.05f);
      GrRayHit hit;
      if (len > 1e-5f && gr_level_raycast(layer, ch->position, v3_scale(step, 1.0f / len), len + rad, &hit)) {
        ch->position = v3_add(hit.point, v3_scale(hit.normal, rad));
        f32 vn = v3_dot(ch->velocity, hit.normal);
        v3 normal_part = v3_scale(hit.normal, vn);
        ch->velocity = v3_sub(v3_scale(v3_sub(ch->velocity, normal_part), 0.6f), v3_scale(normal_part, 0.3f));
        ch->spin = v3_scale(ch->spin, 0.5f);
        if (hit.normal.y > 0.7f && v3_length(ch->velocity) < 1.0f) {
          ch->resting = true;
          ch->velocity = V3_ZERO;
          ch->spin = V3_ZERO;
        }
        if (fx->count < KM_AIR_FX_MAX && v3_length(ch->velocity) > 2.0f)
          fx->items[fx->count++] = (GrFxRequest){.kind = GR_FX_DEBRIS_SPARK, .p = ch->position, .d = ch->velocity};
      } else {
        ch->position = v3_add(ch->position, step);
      }
      f32 w = v3_length(ch->spin);
      if (w > 1e-4f)
        ch->rotation = quat_normalize(quat_mul(quat_axis_angle(v3_scale(ch->spin, 1.0f / w), w * dt), ch->rotation));
      if (ch->smokes) {
        ch->smoke_timer -= dt;
        if (ch->smoke_timer <= 0.0f && fx->count < KM_AIR_FX_MAX) {
          ch->smoke_timer = 0.14f;
          fx->items[fx->count++] = (GrFxRequest){.kind = GR_FX_DEBRIS_SMOKE, .p = ch->position};
        }
      }
    }
    f32 fade = m_saturatef(ch->life / 0.6f);
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){ch->position, ch->rotation, v3_scale(ch->size, fade)};
  }
}

void km_feel_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_chunk_spawn_system); // spawns chunk entities
  ecs_schedule_single(w, km_chunk_system);       // writes the shared fx list
}

void km_feel_init(EcsWorld *w) {
  // resources are main-lane app_init state
  assert(is_main_thread());
  ecs_resource_set_zero(w, KmFeel);
}

// main lane, outside the scope: the chunk mesh once the renderer is up
void km_feel_main(EcsWorld *w) {
  // pg_mesh_upload records GPU uploads: main lane
  assert(is_main_thread());
  KmFeel *feel = ecs_resource_get_mut(w, KmFeel);
  if (feel->ready)
    return;
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  tctx_temp_allocator_scope(ta, NULL) {
    PgMesh m;
    pg_mesh_begin(&m, ta.temp.arena, 256, 512);
    pg_box(&m, V3_ONE, 0.12f);
    feel->chunk_mesh = gr_fx_upload_mesh(r, &m, "B2 chunk");
  }
  feel->ready = true;
}
