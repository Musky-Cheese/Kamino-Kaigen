// Proving Grounds level: the arena scene's sites gathered into GrLevel (tops, routes, pad launches), cylinder bodies, raycast.

#define GR_WALK_CELL 4.0f // walkable-top grid cell (m)

typedef struct GrCylinderBlob {
  GrCylinderCollider shape;
  TriMeshColliderBlob *blob;
} GrCylinderBlob;
DYN_ARRAY_TYPEDEF(GrCylinderBlob)

// the gather's cached site queries; any change regathers
typedef enum GrSiteQuery {
  GR_LEVEL_Q_AREAS,
  GR_LEVEL_Q_ZONES,
  GR_LEVEL_Q_PORTALS,
  GR_LEVEL_Q_SPAWNS,
  GR_LEVEL_Q_PADS,
  GR_LEVEL_Q_PEDESTALS,
  GR_LEVEL_Q_PACKS,
  GR_LEVEL_Q_SIGNS,
  GR_LEVEL_Q_WALKABLE,
  GR_LEVEL_Q_COUNT,
} GrSiteQuery;

// the gather's queries and storage; main lane at init, then the gather and cylinder systems only
typedef struct GrLevelRuntime {
  ArenaAllocator *tables; // tops and grid, reset by every gather
  Allocator tables_alloc;
  ArenaAllocator *blobs_arena; // cylinder trimesh blobs, for the level's lifetime
  Allocator blobs_alloc;
  DynArray(GrCylinderBlob) blobs; // one per distinct cylinder shape
  EcsQuery sites[GR_LEVEL_Q_COUNT];
  EcsQuery pending_models; // scene models without their renderer model yet
  b32 invalid; // the last gather failed validation
} GrLevelRuntime;
ECS_COMPONENT_TYPEDEF(GrLevelRuntime);

// ---------------------------------------------------------------- queries

// the walkable-top grid: one cell list per GR_WALK_CELL square over the tops' extent
hz_internal void gr_level_build_grid(GrLevel *l, Allocator *a) {
  v2 mn = {1e30f, 1e30f}, mx = {-1e30f, -1e30f};
  for (u32 i = 0; i < l->top_count; i++) {
    mn = (v2){m_minf(mn.x, l->tops[i].min_x), m_minf(mn.y, l->tops[i].min_z)};
    mx = (v2){m_maxf(mx.x, l->tops[i].max_x), m_maxf(mx.y, l->tops[i].max_z)};
  }
  f32 cell = GR_WALK_CELL;
  l->grid_cell = cell;
  l->grid_origin = mn;
  l->grid_max = mx;
  l->grid_w = MAX(1u, (u32)m_ceilf((mx.x - mn.x) / cell));
  l->grid_d = MAX(1u, (u32)m_ceilf((mx.y - mn.y) / cell));
  u32 cells = l->grid_w * l->grid_d;
  l->grid_start = ALLOC_ARRAY(a, u32, cells + 1);
  // counting sort: count into start[c + 1], prefix-sum so start[c] opens cell c, scatter advancing start[c]
  for (u32 pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      for (u32 c = 1; c <= cells; c++)
        l->grid_start[c] += l->grid_start[c - 1];
      l->grid_items = ALLOC_ARRAY(a, u32, MAX(l->grid_start[cells], 1u));
    }
    for (u32 i = 0; i < l->top_count; i++) {
      const GrWalkableTop *t = &l->tops[i];
      u32 x0 = MIN((u32)((t->min_x - mn.x) / cell), l->grid_w - 1), x1 = MIN((u32)((t->max_x - mn.x) / cell), l->grid_w - 1);
      u32 z0 = MIN((u32)((t->min_z - mn.y) / cell), l->grid_d - 1), z1 = MIN((u32)((t->max_z - mn.y) / cell), l->grid_d - 1);
      for (u32 z = z0; z <= z1; z++) {
        for (u32 x = x0; x <= x1; x++) {
          u32 c = z * l->grid_w + x;
          if (pass == 0)
            l->grid_start[c + 1]++;
          else
            l->grid_items[l->grid_start[c]++] = i;
        }
      }
    }
  }
  // the scatter advanced each cell's start onto the next cell's: shift back one
  for (u32 c = cells; c > 0; c--)
    l->grid_start[c] = l->grid_start[c - 1];
  l->grid_start[0] = 0;
}

f32 gr_level_ground_height(const GrLevel *l, v3 p) {
  if (!l->grid_start || p.x < l->grid_origin.x || p.z < l->grid_origin.y || p.x > l->grid_max.x || p.z > l->grid_max.y)
    return 0.0f;
  u32 cx = MIN((u32)((p.x - l->grid_origin.x) / l->grid_cell), l->grid_w - 1);
  u32 cz = MIN((u32)((p.z - l->grid_origin.y) / l->grid_cell), l->grid_d - 1);
  u32 c = cz * l->grid_w + cx;
  f32 best = 0.0f;
  for (u32 k = l->grid_start[c]; k < l->grid_start[c + 1]; k++) {
    const GrWalkableTop *t = &l->tops[l->grid_items[k]];
    if (t->y > best && t->y <= p.y + 0.05f && p.x >= t->min_x && p.x <= t->max_x && p.z >= t->min_z && p.z <= t->max_z)
      best = t->y;
  }
  return best;
}

// distance from a box to a point
hz_internal f32 gr_box_distance(aabb b, v3 p) {
  f32 dx = m_maxf(m_maxf(b.min.x - p.x, 0.0f), p.x - b.max.x);
  f32 dy = m_maxf(m_maxf(b.min.y - p.y, 0.0f), p.y - b.max.y);
  f32 dz = m_maxf(m_maxf(b.min.z - p.z, 0.0f), p.z - b.max.z);
  return m_sqrtf(dx * dx + dy * dy + dz * dz);
}

u32 gr_level_zone_at(const GrLevel *l, v3 p) {
  u32 best = 0;
  f32 best_distance = 1e30f;
  for (u32 i = 0; i < GR_LEVEL_ZONE_COUNT; i++) {
    f32 d = gr_box_distance(l->zones[i].box, p);
    if (d < best_distance) {
      best_distance = d;
      best = i;
      if (d == 0.0f)
        break;
    }
  }
  return best;
}

u32 gr_level_area_at(const GrLevel *l, v3 p) { return l->zones[gr_level_zone_at(l, p)].area; }

b32 gr_level_inside(const GrLevel *l, v3 p) {
  for (u32 i = 0; i < GR_LEVEL_ZONE_COUNT; i++)
    if (aabb_contains_point(l->zones[i].box, p))
      return true;
  for (u32 i = 0; i < GR_LEVEL_PORTAL_COUNT; i++) {
    const GrPortal *pt = &l->portals[i];
    f32 d = m_fabsf(v3_dot(v3_sub(p, pt->center), pt->normal));
    f32 lateral = pt->normal.x != 0.0f ? m_fabsf(p.z - pt->center.z) : m_fabsf(p.x - pt->center.x);
    if (d <= 0.6f && lateral <= GR_DOOR_WIDTH * 0.5f && p.y >= 0.0f && p.y <= GR_DOOR_HEIGHT)
      return true;
  }
  return false;
}

GrNextPortal gr_level_next_portal(const GrLevel *l, u32 from_zone, u32 to_zone) {
  if (from_zone >= GR_LEVEL_ZONE_COUNT || to_zone >= GR_LEVEL_ZONE_COUNT)
    return (GrNextPortal){-1, 0};
  return l->next[from_zone][to_zone];
}

const EcsCollisionLayer *gr_level_collision(EcsWorld *w) {
  const EcsCollisionLayer *layer = ecs_physics_get_static_layer(w);
  return layer && layer->layer.count > 0 ? layer : NULL;
}

b32 gr_level_raycast(const EcsCollisionLayer *layer, v3 origin, v3 dir, f32 max_distance, GrRayHit *hit) {
  if (!layer || layer->layer.count == 0 || max_distance <= 0.0f)
    return false;
  PhysicsRaycastResult r;
  i32 body = -1;
  // physics_raycast_layer only reads the layer; its signature is non-const
  if (!physics_raycast_layer(origin, v3_add(origin, v3_scale(dir, max_distance)), (CollisionLayer *)&layer->layer,
                             PHYSICS_QUERY_FILTER_ALL, &r, &body))
    return false;
  v3 n = r.normal;
  if (v3_dot(n, dir) > 0.0f)
    n = v3_scale(n, -1.0f);
  *hit = (GrRayHit){.point = r.position,
                    .normal = n,
                    .distance = r.fraction * max_distance,
                    .entity = body >= 0 ? layer->entities[body] : ECS_ENTITY_INVALID};
  return true;
}

// ---------------------------------------------------------------- gather

// an indexed site lands once in [0, count): false (logged) on an out-of-range or repeated index
hz_internal b32 gr_level_claim(u32 *seen, u32 index, u32 count, CString what) {
  if (index >= count || (*seen & (1u << index))) {
    log_error("level: % index % is out of range or repeated (expected 0..%)", fmt_cstr(what), fmt_u32(index), fmt_u32(count - 1));
    return false;
  }
  *seen |= 1u << index;
  return true;
}

hz_internal b32 gr_level_complete(u32 seen, u32 count, CString what) {
  if (seen == (1u << count) - 1u)
    return true;
  log_error("level: the scene is missing % sites (mask %, expected %)", fmt_cstr(what), fmt_hex(seen), fmt_u32(count));
  return false;
}

// a BFS from every zone, each reached zone remembering the first doorway on its path
hz_internal void gr_level_routes(GrLevel *l) {
  tctx_temp_allocator_scope(ta, NULL) {
    u32 *queue = ALLOC_ARRAY(&ta.allocator, u32, GR_LEVEL_ZONE_COUNT);
    for (u32 from = 0; from < GR_LEVEL_ZONE_COUNT; from++) {
      GrNextPortal *first = l->next[from];
      for (u32 z = 0; z < GR_LEVEL_ZONE_COUNT; z++)
        first[z] = (GrNextPortal){-1, 0};
      u32 head = 0, tail = 0;
      u32 seen = 1u << from;
      queue[tail++] = from;
      while (head < tail) {
        u32 zone = queue[head++];
        for (u32 p = 0; p < GR_LEVEL_PORTAL_COUNT; p++) {
          const GrPortal *pt = &l->portals[p];
          i32 other = pt->a == zone ? (i32)pt->b : pt->b == zone ? (i32)pt->a : -1;
          if (other < 0 || (seen & (1u << other)))
            continue;
          seen |= 1u << other;
          first[other] = zone == from ? (GrNextPortal){(i32)p, pt->a == zone ? 1 : -1} : first[zone];
          queue[tail++] = (u32)other;
        }
      }
    }
  }
}

// the level tables: the authored pedestals, then Tipoca's layout; false (logged) when the pedestals are incomplete
hz_internal b32 gr_level_gather(GrLevel *l, GrLevelRuntime *rt, const GrLevelBounds *bounds, u32 bounds_count) {
  GrLevel n = {.generation = l->generation + 1, .gravity = GR_PLAYER_GRAVITY};
  b32 ok = bounds_count == 1;
  if (!ok)
    log_error("level: the scene has % GrLevelBounds sites (expected 1)", fmt_u32(bounds_count));
  n.bounds = aabb_make(bounds->min, bounds->max);

  // the weapon pedestals are the only authored sites; the rest is Tipoca's layout
  u32 seen = 0;
  ecs_query_foreach(&rt->sites[GR_LEVEL_Q_PEDESTALS], it) {
    const GrPedestal *d = ecs_iter_get_component(&it, GrPedestal, 0);
    const WorldTransform *wt = ecs_iter_get_component(&it, WorldTransform, 1);
    for (i32 i = 0; i < it.count; i++) {
      if (!gr_level_claim(&seen, d[i].index, GR_LEVEL_PEDESTAL_COUNT, "pedestal")) {
        ok = false;
        continue;
      }
      n.pedestals[d[i].index] = (GrPedestalSite){.weapon = d[i].weapon, .position = wt[i].position, .top = d[i].top};
    }
  }
  ok &= gr_level_complete(seen, GR_LEVEL_PEDESTAL_COUNT, "pedestal");
  if (!ok)
    return false;

  arena_reset(rt->tables);
  km_layout_fill(&n, &rt->tables_alloc);
  gr_level_routes(&n);
  gr_level_build_grid(&n, &rt->tables_alloc);
  n.tables_ready = true;
  n.meshes_ready = l->meshes_ready;
  *l = n;
  return true;
}

// the scene's sites into GrLevel once they spawn, again whenever one changes; GrLevelBounds (one entity) drives the run
ecs_system(gr_level_gather_system, EcsIn(GrLevelBounds), EcsResInOut(GrLevel), EcsResInOut(GrLevelRuntime),
           EcsResInOut(GrLoadGates), EcsResIn(KmWorld), EcsExternIn(WorldTransform), EcsExternIn(EcsCollider), EcsExternIn(EcsModel)) {
  GrLevel *l = ecs_sys_get_resource_mut(GrLevel);
  GrLevelRuntime *rt = ecs_sys_get_resource_mut(GrLevelRuntime);
  b32 changed = ecs_iter_changed(it) || (!l->tables_ready && !rt->invalid);
  // archetype moves while the models resolve read as changes too: a few regathers during the load
  for (u32 q = 0; q < GR_LEVEL_Q_COUNT; q++)
    changed |= ecs_query_changed(&rt->sites[q]);
  if (changed) {
    for (u32 q = 0; q < GR_LEVEL_Q_COUNT; q++)
      ecs_query_sync(&rt->sites[q]);
    rt->invalid = !gr_level_gather(l, rt, ecs_sys_get_component(GrLevelBounds, 0), (u32)it->count);
    // an invalid scene fails the load; an edit that breaks a loaded level keeps its last valid tables
    if (rt->invalid && !l->tables_ready) {
      ecs_sys_get_resource_mut(GrLoadGates)->failed = true;
      return;
    }
    if (rt->invalid)
      return;
    log_info("level: gathered (generation %): % walkable tops in % grid cells", fmt_u32(l->generation), fmt_u32(l->top_count),
             fmt_u32(l->grid_w * l->grid_d));
  }
  // the load's "models" stage: every scene model has its renderer instance
  if (l->tables_ready && !l->meshes_ready && ecs_sys_get_resource(KmWorld)->built) {
    u32 pending = 0;
    ecs_query_foreach(&rt->pending_models, mit) pending += (u32)mit.count;
    if (pending == 0) {
      l->meshes_ready = true;
      log_info("level: every scene model is instanced");
    }
  }
}

// ---------------------------------------------------------------- cylinder bodies

// the shape's trimesh, shared by every cylinder of that shape
hz_internal TriMeshColliderBlob *gr_cylinder_blob(GrLevelRuntime *rt, GrCylinderCollider c) {
  for (u32 i = 0; i < rt->blobs.len; i++) {
    const GrCylinderCollider *o = &rt->blobs.items[i].shape;
    if (o->radius_top == c.radius_top && o->radius_bottom == c.radius_bottom && o->height == c.height && o->segments == c.segments)
      return rt->blobs.items[i].blob;
  }
  TriMeshColliderBlob *blob = NULL;
  tctx_temp_allocator_scope(ta, rt->blobs_arena) {
    PgMesh m;
    pg_mesh_begin(&m, ta.temp.arena, 64, 256);
    pg_cylinder(&m, &(PgCylinderDesc){.radius_top = c.radius_top,
                                      .radius_bottom = c.radius_bottom,
                                      .height = c.height,
                                      .radial_segments = MAX(c.segments, 3u),
                                      .height_segments = 1});
    i32 *idx = ALLOC_ARRAY(&ta.allocator, i32, m.idx.len);
    for (u32 k = 0; k < m.idx.len; k++)
      idx[k] = (i32)m.idx.items[k];
    blob = trimesh_blob_build(m.pos.items, idx, (i32)(m.idx.len / 3), &rt->blobs_alloc);
  }
  dyn_arr_append(rt->blobs, ((GrCylinderBlob){.shape = c, .blob = blob}));
  return blob;
}

// each scene GrCylinderCollider owns an unsaved static trimesh body at its world pose, rebuilt when the shape or pose changes
ecs_system(gr_cylinder_body_system, EcsIn(GrCylinderCollider), EcsIn(WorldTransform), EcsOptional(GrCylinderBody),
           EcsWith(SceneEntityTag), EcsResInOut(GrLevelRuntime), EcsExternInOut(ColliderBody),
           EcsExternIn(GrCylinderBodyOf)) {
  if (!ecs_iter_changed(it))
    return;
  GrLevelRuntime *rt = ecs_sys_get_resource_mut(GrLevelRuntime);
  for (i32 i = 0; i < it->count; i++) {
    const WorldTransform *wt = ecs_sys_get_component(WorldTransform, i);
    const GrCylinderBody *cyl = ecs_sys_get_component_optional(GrCylinderBody, i);
    ColliderBody body = {.collider = collider_trimesh(gr_cylinder_blob(rt, *ecs_sys_get_component(GrCylinderCollider, i))),
                         .pos = wt->position,
                         .rot = wt->rotation,
                         .scale = wt->scale.x};
    // a duplicated entity carries its source's GrCylinderBody: only the owner's own body is reused
    const GrCylinderBodyOf *of = cyl ? ecs_component_get(it->world, cyl->body, GrCylinderBodyOf) : NULL;
    if (of && of->owner == it->entities[i]) {
      ecs_component_set(it->world, cyl->body, ColliderBody, body);
      continue;
    }
    EcsEntity e = ecs_entity_create(it->world);
    ecs_component_set(it->world, e, ColliderBody, body);
    ecs_component_add(it->world, e, StaticBody);
    ecs_component_set(it->world, e, GrCylinderBodyOf, {.owner = it->entities[i]});
    ecs_component_set(it->world, it->entities[i], GrCylinderBody, {.body = e});
  }
}

// a body whose cylinder is gone (deleted in the editor, a closed prefab stage) goes with it
ecs_system(gr_cylinder_reap_system, EcsIn(GrCylinderBodyOf)) {
  for (i32 i = 0; i < it->count; i++)
    if (!ecs_entity_is_alive(it->world, ecs_sys_get_component(GrCylinderBodyOf, i)->owner))
      ecs_entity_destroy(it->world, it->entities[i]);
}

// ---------------------------------------------------------------- core rings

// each ring spins and bobs in its derived WorldTransform over the authored LocalTransform, so a save never bakes the pose
ecs_system(gr_core_ring_system, EcsIn(GrCoreRing), EcsIn(LocalTransform), EcsIn(ParentToWorldTransform),
           EcsInOut(WorldTransform), EcsResIn(GrClock)) {
  f32 t = ecs_sys_get_resource(GrClock)->time;
  for (i32 i = 0; i < it->count; i++) {
    const GrCoreRing *ring = ecs_sys_get_component(GrCoreRing, i);
    f32 k = (f32)ring->index;
    f32 dir = ring->index % 2 == 0 ? 1.0f : -1.0f;
    quat q = quat_from_euler((v3){m_sinf(t * 0.6f + k) * 0.08f, t * 0.35f * dir, m_cosf(t * 0.5f + k) * 0.05f});
    v3 bob = {0.0f, m_sinf(t * 0.8f + k * 1.7f) * 0.25f, 0.0f};
    // the cooked vertices carry the node's rest pose: undo it about the node origin, then set the ring's rotation
    quat r = quat_mul(q, quat_inv(ring->rest));
    trs pose = {.position = v3_sub(v3_add(ring->pivot, bob), quat_rotate(r, ring->pivot)), .rotation = r, .scale = V3_ONE};
    const LocalTransform *lt = ecs_sys_get_component(LocalTransform, i);
    const ParentToWorldTransform *pw = ecs_sys_get_component(ParentToWorldTransform, i);
    trs local = trs_mul((trs){lt->position, lt->rotation, lt->scale}, pose);
    trs world = trs_mul((trs){pw->position, pw->rotation, pw->scale}, local);
    *ecs_sys_get_component_mut(WorldTransform, i) = (WorldTransform){world.position, world.rotation, world.scale};
  }
}

// ---------------------------------------------------------------- init, schedule

void gr_level_init(EcsWorld *w) {
  // resources and cached queries are main-lane app_init state
  assert(is_main_thread());
  ArenaAllocator *arena = arena_create(MB(16), KB(64));
  Allocator alloc = make_arena_allocator(arena);
  GrLevelRuntime *rt = ALLOC(&alloc, GrLevelRuntime);
  rt->blobs_arena = arena;
  rt->blobs_alloc = alloc;
  rt->blobs = dyn_arr_new_alloc(&rt->blobs_alloc, GrCylinderBlob, 8);
  rt->tables = arena_create(MB(16), KB(64));
  rt->tables_alloc = make_arena_allocator(rt->tables);
  // scene entities only: an open prefab stage's instance is not part of the level
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_AREAS], EcsIn(GrAreaDef), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_ZONES], EcsIn(GrNavZoneDef), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_PORTALS], EcsIn(GrPortalDef), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_SPAWNS], EcsIn(GrPlayerSpawn), EcsIn(WorldTransform), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_PADS], EcsIn(GrJumpPad), EcsIn(WorldTransform), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_PEDESTALS], EcsIn(GrPedestal), EcsIn(WorldTransform), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_PACKS], EcsIn(GrHealthPackSite), EcsIn(WorldTransform), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_SIGNS], EcsIn(GrSignSite), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->sites[GR_LEVEL_Q_WALKABLE], EcsIn(GrWalkable), EcsIn(EcsCollider), EcsIn(WorldTransform), EcsWith(SceneEntityTag));
  ecs_query_cached_init(w, &rt->pending_models, EcsIn(EcsModelRef), EcsWith(SceneEntityTag), EcsNot(EcsModel));
  ecs_resource_set_ref(w, GrLevelRuntime, rt);
  ecs_resource_set(w, GrLevel, ((GrLevel){.gravity = GR_PLAYER_GRAVITY}));
}

void gr_level_schedule(EcsWorld *w) { ecs_physics_schedule(w); }

void gr_level_schedule_gather(EcsWorld *w) {
  ecs_schedule(w, gr_core_ring_system);            // over the hierarchy's WorldTransform, before the renderer reads it
  ecs_physics_sync_schedule(w);                    // the scene's EcsColliders become bodies posed by their WorldTransform
  ecs_schedule_single(w, gr_cylinder_body_system); // creates bodies, writes the GrLevelRuntime blob cache
  ecs_schedule(w, gr_cylinder_reap_system);
  ecs_schedule_single(w, gr_level_gather_system);  // writes the GrLevel resource
}
