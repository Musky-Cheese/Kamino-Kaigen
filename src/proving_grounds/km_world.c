// Kamino: Tipoca City geometry over mesh_builder, and the GrLevel tables (areas, zones, portals, tops) of the same layout.
#include "gameplay/mesh_builder/mesh_builder.h"
#include "rendering/features/clustered_lights.h"

// the outdoor front runs along -Z: Aurek, Besh, Cresh, Dorn, then the facility
static const KmPlatform km_platforms[KM_PLATFORM_COUNT] = {
    {{0.0f, 62.0f}, 17.0f},
    {{0.0f, 18.0f}, 14.0f},
    {{0.0f, -28.0f}, 16.0f},
    {{0.0f, -82.0f}, 29.0f},
};

// lobby, security corridor, cloning hall; consecutive rooms share a wall
static const KmRoom km_rooms[KM_ROOM_COUNT] = {
    {{-22.0f, -169.0f}, {22.0f, -115.0f}, 10.0f},
    {{-7.0f, -205.0f}, {7.0f, -169.0f}, 4.6f},
    {{-22.0f, -261.0f}, {22.0f, -205.0f}, 13.0f},
};

// a straight bridge along Z between two platforms (z0 > z1), deck top at y 0
typedef struct KmBridge {
  f32 x, z0, z1, half_width;
} KmBridge;

static const KmBridge km_bridges[] = {
    {0.0f, 45.6f, 31.4f, 3.0f},
    {0.0f, 4.4f, -12.4f, 3.0f},
    {0.0f, -43.6f, -53.4f, 3.0f},
    {0.0f, -110.6f, -115.0f, 4.0f},
};
#define KM_BRIDGE_COUNT ARRAY_SIZE(km_bridges)

typedef enum KmCoverKind {
  KM_COVER_CRATE = 0,
  KM_COVER_BARRICADE,
  KM_COVER_POD,
  KM_COVER_TANK,
  KM_COVER_CONSOLE,
} KmCoverKind;

typedef struct KmCover {
  u32 sector;
  u32 kind;
  f32 x, z; // relative to the sector's centre
  f32 rotation;
} KmCover;

// the original layout's cover, sector-relative
static const KmCover km_cover[] = {
    {0, KM_COVER_CRATE, 7, -8, 0}, {0, KM_COVER_CRATE, 8.3f, -7.2f, 0.3f}, {0, KM_COVER_BARRICADE, -3, -11, 0},
    {0, KM_COVER_BARRICADE, 9, -12, 0.5f}, {0, KM_COVER_CRATE, 11, 3, 0}, {0, KM_COVER_POD, 12.5f, -2, 0},
    {1, KM_COVER_POD, -8, -3, 0}, {1, KM_COVER_POD, 8, 3, 0}, {1, KM_COVER_BARRICADE, -3.5f, 6, 0},
    {1, KM_COVER_BARRICADE, 4, -4.5f, 0}, {1, KM_COVER_BARRICADE, -6, -9, -0.4f}, {1, KM_COVER_CRATE, 6, 9, 0},
    {1, KM_COVER_CRATE, 7.2f, 9.6f, 0.4f}, {1, KM_COVER_BARRICADE, 9.5f, -6, 0.5f}, {1, KM_COVER_CRATE, -10, 5, 0},
    {2, KM_COVER_POD, -11, -4, 0}, {2, KM_COVER_POD, 11, 3, 0}, {2, KM_COVER_BARRICADE, 6, -9, 0},
    {2, KM_COVER_BARRICADE, -5, 9.5f, 0}, {2, KM_COVER_BARRICADE, -10.5f, 7, 0.6f}, {2, KM_COVER_BARRICADE, 10.5f, -8.5f, -0.5f},
    {2, KM_COVER_CRATE, 4, 10, 0}, {2, KM_COVER_CRATE, -6, -11, 0}, {2, KM_COVER_BARRICADE, 1, -12.5f, 0},
    {3, KM_COVER_BARRICADE, -9, 15, 0}, {3, KM_COVER_BARRICADE, 9, 15, 0}, {3, KM_COVER_BARRICADE, -19, 3, 0.9f},
    {3, KM_COVER_BARRICADE, 20, 2, -0.9f}, {3, KM_COVER_CRATE, -7, 20, 0}, {3, KM_COVER_CRATE, 7, 20, 0},
    {3, KM_COVER_CRATE, -19, 11, 0}, {3, KM_COVER_POD, 20, 12, 0}, {3, KM_COVER_BARRICADE, -6, 3, 0},
    {3, KM_COVER_BARRICADE, 8, 2, 0.3f}, {3, KM_COVER_POD, -18, -9, 0}, {3, KM_COVER_POD, 19, -11, 0},
    {3, KM_COVER_BARRICADE, -13, -17, -0.4f}, {3, KM_COVER_BARRICADE, 14, -17, 0.35f},
    {4, KM_COVER_BARRICADE, -9, 10, 0}, {4, KM_COVER_BARRICADE, 9, 10, 0}, {4, KM_COVER_POD, -15, 1, 0},
    {4, KM_COVER_POD, 15, 1, 0}, {4, KM_COVER_CRATE, -8, -7, 0}, {4, KM_COVER_CRATE, 8, -7, 0},
    {4, KM_COVER_BARRICADE, -11, -14, 0.3f}, {4, KM_COVER_BARRICADE, 11, -14, -0.3f}, {4, KM_COVER_BARRICADE, -5, -11, 0},
    {4, KM_COVER_CRATE, 5.5f, -12.5f, 0},
    {5, KM_COVER_BARRICADE, 1.2f, 13, 0}, {5, KM_COVER_CRATE, 1.9f, 5, 0}, {5, KM_COVER_BARRICADE, -1.2f, -5, 0},
    {5, KM_COVER_CRATE, 1.9f, -13, 0},
    {6, KM_COVER_BARRICADE, -1, 6, 0}, {6, KM_COVER_BARRICADE, 2, -7, 0}, {6, KM_COVER_CRATE, 4.5f, 14, 0},
    {6, KM_COVER_CRATE, -4.5f, 0, 0}, {6, KM_COVER_CONSOLE, -5, -19.5f, 0}, {6, KM_COVER_CONSOLE, 5, -19.5f, 0},
};
#define KM_COVER_COUNT ARRAY_SIZE(km_cover)

// the Republic gunship parked on Aurek, the Separatist landing craft on Dorn (its ramp faces the front)
#define KM_GUNSHIP_POS ((v3){-8.0f, 0.0f, 61.0f})
#define KM_GUNSHIP_YAW 0.35f
#define KM_LANDER_POS ((v3){0.0f, 0.0f, -93.0f})

static const CString km_material_paths[KM_MAT_COUNT] = {
    "proving_grounds/materials/level/gr_wall_white.hzt", "proving_grounds/materials/level/gr_floor_plain.hzt",
    "proving_grounds/materials/level/gr_metal.hzt",      "proving_grounds/materials/level/gr_dark_metal.hzt",
    "proving_grounds/materials/level/gr_light_cyan.hzt", "proving_grounds/materials/level/gr_light_red.hzt",
    "proving_grounds/materials/level/gr_light_warm.hzt",
};

CString km_sector_name(u32 sector) {
  switch (sector) {
  case KM_SECTOR_AUREK: return "AUREK LANDING PAD";
  case KM_SECTOR_BESH: return "BESH JUNCTION";
  case KM_SECTOR_CRESH: return "CRESH PLAZA";
  case KM_SECTOR_DORN: return "DORN LANDING PLATFORM";
  case KM_SECTOR_LOBBY: return "TIPOCA FACILITY LOBBY";
  case KM_SECTOR_CORRIDOR: return "SECURITY CORRIDOR";
  case KM_SECTOR_HALL: return "CLONING HALL";
  default: return "";
  }
}

// a sector's centre in world XZ
hz_internal v2 km_sector_center(u32 sector) {
  if (sector < KM_PLATFORM_COUNT)
    return km_platforms[sector].center;
  const KmRoom *r = &km_rooms[sector - KM_PLATFORM_COUNT];
  return (v2){(r->min.x + r->max.x) * 0.5f, (r->min.y + r->max.y) * 0.5f};
}

// ---------------------------------------------------------------- layout tables

hz_internal void km_top(GrLevel *l, f32 x0, f32 x1, f32 z0, f32 z1, f32 y) {
  l->tops[l->top_count++] = (GrWalkableTop){.min_x = x0, .max_x = x1, .min_z = z0, .max_z = z1, .y = y};
}

hz_internal GrNavZone km_zone(u32 area, f32 x0, f32 x1, f32 z0, f32 z1, f32 ceiling) {
  return (GrNavZone){.area = area, .box = aabb_make((v3){x0, 0.0f, z0}, (v3){x1, ceiling, z1}), .ceiling = ceiling};
}

// the inscribed square's half size: droids keep inside it
hz_internal f32 km_platform_half(const KmPlatform *p) { return p->radius * 0.72f; }

void km_layout_fill(GrLevel *l, Allocator *a) {
  // areas: north platforms, south platforms, lobby, the deep facility
  static const v3 spawns[GR_AREA_COUNT][GR_LEVEL_AREA_SPAWN_MAX] = {
      {{-8, 1.2f, 15}, {8, 1.2f, 21}, {0, 1.2f, 10}, {-6, 1.2f, 24}, {6, 1.2f, 12}, {-9, 1.2f, 19}, {9, 1.2f, 15}, {0, 1.2f, 26}},
      {{-8, 1.2f, -32}, {8, 1.2f, -24}, {0, 1.2f, -36}, {-12, 1.2f, -80}, {12, 1.2f, -80}, {0, 1.2f, -95}, {-16, 1.2f, -70}, {16, 1.2f, -70}},
      {{-14, 1.2f, -150}, {14, 1.2f, -150}, {0, 1.2f, -160}, {-10, 1.2f, -132}, {10, 1.2f, -132}, {-17, 1.2f, -140}, {17, 1.2f, -140}, {0, 1.2f, -145}},
      {{-3.6f, 1.2f, -196}, {3.6f, 1.2f, -186}, {-15, 1.2f, -240}, {15, 1.2f, -240}, {0, 1.2f, -250}, {-16, 1.2f, -220}, {16, 1.2f, -220}, {0, 1.2f, -230}},
  };
  static const aabb boxes[GR_AREA_COUNT] = {
      {{-30.0f, 0.0f, -16.5f}, {30.0f, 30.0f, 82.0f}},
      {{-34.0f, 0.0f, -115.0f}, {34.0f, 30.0f, -16.5f}},
      {{-22.0f, 0.0f, -169.0f}, {22.0f, 10.0f, -115.0f}},
      {{-22.0f, 0.0f, -261.0f}, {22.0f, 13.0f, -169.0f}},
  };
  static const v3 probes[GR_AREA_COUNT] = {{0, 3, 40}, {0, 3, -60}, {0, 3, -142}, {0, 4, -230}};
  static const u32 neighbours[GR_AREA_COUNT] = {1u << 1, (1u << 0) | (1u << 2), (1u << 1) | (1u << 3), 1u << 2};
  for (u32 i = 0; i < GR_AREA_COUNT; i++) {
    GrArea *ar = &l->areas[i];
    *ar = (GrArea){.box = boxes[i], .fog_density = i < 2 ? 1.0f : 0.6f, .probe = probes[i], .neighbour_mask = neighbours[i],
                   .spawn_count = GR_LEVEL_AREA_SPAWN_MAX};
    for (u32 s = 0; s < GR_LEVEL_AREA_SPAWN_MAX; s++)
      ar->spawns[s] = spawns[i][s];
  }

  // zones: each platform's inscribed square, the bridges between, the rooms; consecutive zones abut at a portal
  f32 ha = km_platform_half(&km_platforms[0]), hb = km_platform_half(&km_platforms[1]);
  f32 hc = km_platform_half(&km_platforms[2]), hd = km_platform_half(&km_platforms[3]);
  f32 za = km_platforms[0].center.y, zb = km_platforms[1].center.y, zc = km_platforms[2].center.y, zd = km_platforms[3].center.y;
  const f32 sky = 7.0f; // outdoor droid ceiling
  l->zones[0] = km_zone(0, -ha, ha, za - ha, za + ha, sky);
  l->zones[1] = km_zone(0, -3.0f, 3.0f, zb + hb, za - ha, sky);
  l->zones[2] = km_zone(0, -hb, hb, zb - hb, zb + hb, sky);
  l->zones[3] = km_zone(0, -3.0f, 3.0f, zc + hc, zb - hb, sky);
  l->zones[4] = km_zone(1, -hc, hc, zc - hc, zc + hc, sky);
  l->zones[5] = km_zone(1, -3.0f, 3.0f, zd + hd, zc - hc, sky);
  l->zones[6] = km_zone(1, -hd, hd, zd - hd, zd + hd, sky);
  l->zones[7] = km_zone(2, km_rooms[0].min.x + 1.0f, km_rooms[0].max.x - 1.0f, km_rooms[0].min.y, km_rooms[0].max.y - 1.0f,
                        km_rooms[0].ceiling - 1.0f);
  l->zones[8] = km_zone(1, -4.0f, 4.0f, km_rooms[0].max.y - 1.0f, zd - hd, sky);
  l->zones[9] = km_zone(3, km_rooms[1].min.x + 0.6f, km_rooms[1].max.x - 0.6f, km_rooms[1].min.y, km_rooms[1].max.y,
                        km_rooms[1].ceiling - 0.4f);
  l->zones[10] = km_zone(3, km_rooms[2].min.x + 1.0f, km_rooms[2].max.x - 1.0f, km_rooms[2].min.y + 1.0f, km_rooms[2].max.y,
                         km_rooms[2].ceiling - 1.0f);
  static const u32 links[GR_LEVEL_PORTAL_COUNT][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 8}, {8, 7}, {7, 9}, {9, 10}};
  for (u32 p = 0; p < GR_LEVEL_PORTAL_COUNT; p++) {
    const GrNavZone *from = &l->zones[links[p][0]];
    l->portals[p] = (GrPortal){.a = links[p][0], .b = links[p][1], .center = {0.0f, 1.6f, from->box.min.z}, .normal = {0.0f, 0.0f, -1.0f}};
  }

  l->player_spawn = (v3){5.0f, 0.0f, 74.0f};
  l->player_spawn_yaw = 0.0f;
  l->health_packs[0] = (v3){-9.0f, 0.0f, 14.0f};
  l->health_packs[1] = (v3){9.0f, 0.0f, -32.0f};
  l->hub_bounds = l->areas[GR_AREA_HUB].box;

  // walkable tops: three overlapping rectangles per disc, the bridges, the room floors
  l->tops = ALLOC_ARRAY(a, GrWalkableTop, KM_PLATFORM_COUNT * 3 + KM_BRIDGE_COUNT + KM_ROOM_COUNT);
  for (u32 i = 0; i < KM_PLATFORM_COUNT; i++) {
    v2 c = km_platforms[i].center;
    f32 r = km_platforms[i].radius;
    km_top(l, c.x - r * 0.92f, c.x + r * 0.92f, c.y - r * 0.38f, c.y + r * 0.38f, 0.0f);
    km_top(l, c.x - r * 0.38f, c.x + r * 0.38f, c.y - r * 0.92f, c.y + r * 0.92f, 0.0f);
    km_top(l, c.x - r * 0.7f, c.x + r * 0.7f, c.y - r * 0.7f, c.y + r * 0.7f, 0.0f);
  }
  for (u32 i = 0; i < KM_BRIDGE_COUNT; i++) {
    const KmBridge *b = &km_bridges[i];
    km_top(l, b->x - b->half_width, b->x + b->half_width, b->z1, b->z0, 0.0f);
  }
  for (u32 i = 0; i < KM_ROOM_COUNT; i++)
    km_top(l, km_rooms[i].min.x, km_rooms[i].max.x, km_rooms[i].min.y, km_rooms[i].max.y, 0.0f);
}

// ---------------------------------------------------------------- geometry

typedef struct KmBuild {
  MeshBuilder mb;
  Random rng;
} KmBuild;

hz_internal MeshBuilderStyle km_style(u32 slot, u32 hex, MeshBuilderColliderKind collider) {
  return (MeshBuilderStyle){.slot = slot, .color = mesh_builder_hex(hex), .collider = collider};
}

// a box with its bottom-centre at `base`, yawed
hz_internal void km_box(KmBuild *b, u32 slot, u32 hex, v3 base, v3 size, f32 yaw, b32 solid) {
  MeshBuilderStyle st = km_style(slot, hex, solid ? MESH_BUILDER_COLLIDER_BOX : MESH_BUILDER_COLLIDER_NONE);
  v3 c = {base.x, base.y + size.y * 0.5f, base.z};
  mesh_builder_box(&b->mb, &st, m4_trs(c, quat_rotate_y(yaw), V3_ONE), size);
}

// an axis-aligned box from min to max
hz_internal void km_block(KmBuild *b, u32 slot, u32 hex, v3 mn, v3 mx, b32 solid) {
  v3 size = v3_sub(mx, mn);
  km_box(b, slot, hex, (v3){(mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f}, size, 0.0f, solid);
}

hz_internal void km_cylinder(KmBuild *b, u32 slot, u32 hex, v3 center, f32 r_top, f32 r_bottom, f32 height, u32 segments,
                             b32 solid) {
  MeshBuilderStyle st = km_style(slot, hex, solid ? MESH_BUILDER_COLLIDER_CYLINDER : MESH_BUILDER_COLLIDER_NONE);
  st.collider_segments = MIN(segments, 24u);
  mesh_builder_cylinder(&b->mb, &st, m4_trs(center, quat_identity(), V3_ONE),
                        &(PgCylinderDesc){.radius_top = r_top, .radius_bottom = r_bottom, .height = height, .radial_segments = segments});
}

// does the rim at angle `a` of platform p open onto a bridge?
hz_internal b32 km_rim_gap(const KmPlatform *p, f32 a) {
  v2 rim = {p->center.x + m_cosf(a) * p->radius, p->center.y + m_sinf(a) * p->radius};
  for (u32 i = 0; i < KM_BRIDGE_COUNT; i++) {
    const KmBridge *br = &km_bridges[i];
    b32 near_end = m_fabsf(rim.y - br->z0) < 2.5f || m_fabsf(rim.y - br->z1) < 2.5f;
    if (near_end && m_fabsf(rim.x - br->x) < br->half_width + 0.6f)
      return true;
  }
  return false;
}

// a saucer platform: deck, white rim, curved underside, stilt to the sea, a railing with gaps at the bridges, light strips
hz_internal void km_build_platform(KmBuild *b, const KmPlatform *p, b32 lander_pad) {
  v3 c = {p->center.x, 0.0f, p->center.y};
  f32 r = p->radius;
  km_cylinder(b, KM_MAT_DECK, 0x3a4048, (v3){c.x, -0.6f, c.z}, r, r, 1.2f, 64, true);
  km_cylinder(b, KM_MAT_HULL, 0xdfe6ea, (v3){c.x, -1.05f, c.z}, r + 0.45f, r + 0.2f, 0.9f, 64, false);
  // underside: a shallow bowl down to the stilt collar
  v2 profile[] = {{r + 0.2f, -1.5f}, {r * 0.92f, -2.6f}, {r * 0.7f, -4.2f}, {r * 0.42f, -6.0f}, {r * 0.2f, -7.4f}, {0.0f, -7.8f}};
  MeshBuilderStyle hull = km_style(KM_MAT_HULL, 0xcfd8de, MESH_BUILDER_COLLIDER_NONE);
  mesh_builder_lathe(&b->mb, &hull, m4_translate(c), profile, ARRAY_SIZE(profile), 48);
  // the stilt and its struts
  f32 stilt = m_maxf(r * 0.16f, 2.2f);
  km_cylinder(b, KM_MAT_HULL, 0xc4ced4, (v3){c.x, (KM_OCEAN_Y - 7.0f) * 0.5f, c.z}, stilt * 0.8f, stilt, -KM_OCEAN_Y + 1.0f - 7.0f,
              24, false);
  for (u32 k = 0; k < 3; k++) {
    f32 a = (f32)k * (2.0f * M_PI / 3.0f) + 0.5f;
    v3 top = {c.x + m_cosf(a) * r * 0.55f, -4.5f, c.z + m_sinf(a) * r * 0.55f};
    v3 bottom = {c.x + m_cosf(a) * stilt, -22.0f, c.z + m_sinf(a) * stilt};
    mesh_builder_cylinder_between(&b->mb, &hull, bottom, top, 0.7f, 0.5f, 10);
  }
  // railing: posts and a top rail per segment, colliders keep the player on the deck
  u32 segs = (u32)m_maxf(24.0f, r * 2.6f);
  f32 rail_r = r - 0.35f;
  MeshBuilderStyle metal = km_style(KM_MAT_METAL, 0x9aa4ad, MESH_BUILDER_COLLIDER_NONE);
  for (u32 s = 0; s < segs; s++) {
    f32 a0 = (f32)s / (f32)segs * 2.0f * M_PI, a1 = (f32)(s + 1) / (f32)segs * 2.0f * M_PI;
    f32 am = (a0 + a1) * 0.5f;
    if (km_rim_gap(p, am))
      continue;
    v3 p0 = {c.x + m_cosf(a0) * rail_r, 0.0f, c.z + m_sinf(a0) * rail_r};
    v3 p1 = {c.x + m_cosf(a1) * rail_r, 0.0f, c.z + m_sinf(a1) * rail_r};
    mesh_builder_cylinder_between(&b->mb, &metal, p0, v3_add(p0, (v3){0, 1.1f, 0}), 0.05f, 0.05f, 6);
    mesh_builder_cylinder_between(&b->mb, &metal, v3_add(p0, (v3){0, 1.1f, 0}), v3_add(p1, (v3){0, 1.1f, 0}), 0.045f, 0.045f, 6);
    mesh_builder_cylinder_between(&b->mb, &metal, v3_add(p0, (v3){0, 0.55f, 0}), v3_add(p1, (v3){0, 0.55f, 0}), 0.03f, 0.03f, 6);
    v3 mid = v3_scale(v3_add(p0, p1), 0.5f);
    f32 len = v3_distance(p0, p1);
    mesh_builder_collider_box(&b->mb, (v3){mid.x, 0.7f, mid.z}, quat_rotate_y(-am), (v3){0.15f, 0.7f, len * 0.5f});
    // a cyan strip along the deck edge every other segment
    if (s % 2 == 0)
      km_box(b, KM_MAT_LIGHT, 0xffffff, (v3){mid.x, 0.0f, mid.z}, (v3){0.12f, 0.06f, len * 0.8f}, -am, false);
  }
  // deck markings: a landing ring on the pad, panel seams elsewhere
  if (lander_pad) {
    MeshBuilderStyle mark = km_style(KM_MAT_HULL, 0xf2f2f0, MESH_BUILDER_COLLIDER_NONE);
    mesh_builder_torus(&b->mb, &mark, m4_trs((v3){c.x, 0.01f, c.z}, quat_rotate_x(M_PI * 0.5f), (v3){1, 1, 0.02f}),
                       &(PgTorusDesc){.radius = r * 0.55f, .tube = 0.25f, .radial_segments = 4, .tubular_segments = 64});
  }
  // floodlight masts at four points of the rim
  for (u32 k = 0; k < 4; k++) {
    f32 a = (f32)k * (M_PI * 0.5f) + M_PI * 0.25f;
    v3 base = {c.x + m_cosf(a) * (r - 1.6f), 0.0f, c.z + m_sinf(a) * (r - 1.6f)};
    km_cylinder(b, KM_MAT_HULL, 0xd8e0e4, (v3){base.x, 3.0f, base.z}, 0.16f, 0.24f, 6.0f, 10, true);
    km_box(b, KM_MAT_LIGHT, 0xffffff, (v3){base.x, 6.0f, base.z}, (v3){1.1f, 0.35f, 0.5f}, -a, false);
  }
}

// a floodlight on each mast, aimed down across the deck
hz_internal u32 km_platform_lights(RendererState *r, const KmPlatform *p) {
  for (u32 k = 0; k < 4; k++) {
    f32 a = (f32)k * (M_PI * 0.5f) + M_PI * 0.25f;
    v3 pos = {p->center.x + m_cosf(a) * (p->radius - 1.6f), 5.8f, p->center.y + m_sinf(a) * (p->radius - 1.6f)};
    v3 aim = v3_normalize(v3_sub((v3){p->center.x, 0.0f, p->center.y}, pos));
    renderer_create_local_light(r, &(RendererLocalLightDesc){.position = pos, .radius = p->radius * 2.2f, .color = {0.8f, 0.9f, 1.0f},
                                                              .intensity = 1500000.0f, .direction = aim, .cone_outer = 0.75f, .cone_inner = 0.35f});
  }
  return 4;
}

// warm panels light the rooms from the ceiling
hz_internal u32 km_room_lights(RendererState *r, const KmRoom *room) {
  u32 n = 0;
  for (f32 z = room->min.y + 6.0f; z < room->max.y - 3.0f; z += 12.0f) {
    for (f32 x = room->min.x + 6.0f; x < room->max.x - 3.0f; x += 14.0f) {
      renderer_create_local_light(r, &(RendererLocalLightDesc){.position = {x, room->ceiling - 0.6f, z}, .radius = 14.0f,
                                                                .color = {1.0f, 0.92f, 0.8f}, .intensity = 150000.0f});
      n++;
    }
  }
  return n;
}

hz_internal void km_build_bridge(KmBuild *b, const KmBridge *br) {
  f32 len = br->z0 - br->z1;
  v3 mid = {br->x, 0.0f, (br->z0 + br->z1) * 0.5f};
  km_box(b, KM_MAT_DECK, 0x353b43, (v3){mid.x, -0.5f, mid.z}, (v3){br->half_width * 2.0f, 0.5f, len + 1.0f}, 0.0f, true);
  km_box(b, KM_MAT_HULL, 0xd6dee3, (v3){mid.x, -1.3f, mid.z}, (v3){br->half_width * 1.5f, 0.8f, len}, 0.0f, false);
  for (i32 side = -1; side <= 1; side += 2) {
    f32 x = br->x + (f32)side * (br->half_width - 0.15f);
    km_box(b, KM_MAT_METAL, 0x9aa4ad, (v3){x, 1.05f, mid.z}, (v3){0.08f, 0.08f, len}, 0.0f, false);
    km_box(b, KM_MAT_HULL, 0xd8e0e4, (v3){x, 0.0f, mid.z}, (v3){0.25f, 0.45f, len}, 0.0f, false);
    km_box(b, KM_MAT_LIGHT, 0xffffff, (v3){x - (f32)side * 0.2f, 0.0f, mid.z}, (v3){0.08f, 0.05f, len}, 0.0f, false);
    mesh_builder_collider_box(&b->mb, (v3){x, 0.7f, mid.z}, quat_identity(), (v3){0.15f, 0.7f, len * 0.5f});
    for (f32 z = br->z1 + 1.0f; z < br->z0; z += 2.5f)
      km_box(b, KM_MAT_METAL, 0x9aa4ad, (v3){x, 0.0f, z}, (v3){0.08f, 1.1f, 0.08f}, 0.0f, false);
  }
}

// a wall along X at z from x0 to x1, height h, with an optional doorway [dx0, dx1] of height dh
hz_internal void km_wall_x(KmBuild *b, f32 z, f32 x0, f32 x1, f32 h, f32 dx0, f32 dx1, f32 dh) {
  const f32 t = 0.6f;
  if (dx1 <= dx0) {
    km_block(b, KM_MAT_HULL, 0xe4eaee, (v3){x0, 0.0f, z - t * 0.5f}, (v3){x1, h, z + t * 0.5f}, true);
    return;
  }
  km_block(b, KM_MAT_HULL, 0xe4eaee, (v3){x0, 0.0f, z - t * 0.5f}, (v3){dx0, h, z + t * 0.5f}, true);
  km_block(b, KM_MAT_HULL, 0xe4eaee, (v3){dx1, 0.0f, z - t * 0.5f}, (v3){x1, h, z + t * 0.5f}, true);
  km_block(b, KM_MAT_HULL, 0xe4eaee, (v3){dx0, dh, z - t * 0.5f}, (v3){dx1, h, z + t * 0.5f}, true);
  // the door frame glows
  km_block(b, KM_MAT_LIGHT, 0xffffff, (v3){dx0 - 0.15f, dh, z - t * 0.6f}, (v3){dx1 + 0.15f, dh + 0.12f, z + t * 0.6f}, false);
}

hz_internal void km_wall_z(KmBuild *b, f32 x, f32 z0, f32 z1, f32 h) {
  const f32 t = 0.6f;
  km_block(b, KM_MAT_HULL, 0xe4eaee, (v3){x - t * 0.5f, 0.0f, z0}, (v3){x + t * 0.5f, h, z1}, true);
}

// a room: floor, walls (doorways to the neighbours on the Z walls), ceiling with light panels
hz_internal void km_build_room(KmBuild *b, const KmRoom *r, f32 north_door, f32 south_door) {
  f32 h = r->ceiling;
  km_block(b, KM_MAT_DECK, 0x2f343b, (v3){r->min.x, -0.5f, r->min.y}, (v3){r->max.x, 0.0f, r->max.y}, true);
  km_wall_z(b, r->min.x, r->min.y, r->max.y, h);
  km_wall_z(b, r->max.x, r->min.y, r->max.y, h);
  f32 cx = (r->min.x + r->max.x) * 0.5f;
  km_wall_x(b, r->max.y, r->min.x, r->max.x, h, cx - north_door, cx + north_door, MIN(5.0f, h - 0.6f));
  km_wall_x(b, r->min.y, r->min.x, r->max.x, h, cx - south_door, cx + south_door, MIN(5.0f, h - 0.6f));
  km_block(b, KM_MAT_HULL, 0xd0d8dd, (v3){r->min.x - 0.3f, h, r->min.y - 0.3f}, (v3){r->max.x + 0.3f, h + 0.6f, r->max.y + 0.3f}, true);
  // ceiling light panels and wall light strips
  for (f32 z = r->min.y + 4.0f; z < r->max.y - 2.0f; z += 6.0f) {
    for (f32 x = r->min.x + 4.0f; x < r->max.x - 2.0f; x += 8.0f)
      km_block(b, KM_MAT_WARM, 0xffffff, (v3){x - 1.2f, h - 0.08f, z - 0.6f}, (v3){x + 1.2f, h, z + 0.6f}, false);
  }
  km_block(b, KM_MAT_LIGHT, 0xffffff, (v3){r->min.x + 0.3f, 2.6f, r->min.y}, (v3){r->min.x + 0.38f, 2.7f, r->max.y}, false);
  km_block(b, KM_MAT_LIGHT, 0xffffff, (v3){r->max.x - 0.38f, 2.6f, r->min.y}, (v3){r->max.x - 0.3f, 2.7f, r->max.y}, false);
}

hz_internal void km_build_cover(KmBuild *b, const KmCover *cv) {
  v2 c = km_sector_center(cv->sector);
  v3 p = {c.x + cv->x, 0.0f, c.y + cv->z};
  switch (cv->kind) {
  case KM_COVER_CRATE:
    km_box(b, KM_MAT_DARK, 0x59606a, p, (v3){1.2f, 1.2f, 1.2f}, cv->rotation, true);
    km_box(b, KM_MAT_RED, 0xffffff, (v3){p.x, 1.2f, p.z}, (v3){0.9f, 0.04f, 0.15f}, cv->rotation, false);
    break;
  case KM_COVER_BARRICADE:
    km_box(b, KM_MAT_HULL, 0xd9e0e5, p, (v3){3.4f, 1.15f, 0.55f}, cv->rotation, true);
    km_box(b, KM_MAT_METAL, 0x8e98a1, (v3){p.x, 1.15f, p.z}, (v3){3.5f, 0.1f, 0.65f}, cv->rotation, false);
    break;
  case KM_COVER_POD:
    km_cylinder(b, KM_MAT_HULL, 0xe2e8ec, (v3){p.x, 1.1f, p.z}, 0.9f, 1.15f, 2.2f, 20, true);
    km_cylinder(b, KM_MAT_LIGHT, 0xffffff, (v3){p.x, 2.25f, p.z}, 0.5f, 0.85f, 0.12f, 20, false);
    break;
  case KM_COVER_TANK:
    km_cylinder(b, KM_MAT_HULL, 0xd0d8dd, (v3){p.x, 0.4f, p.z}, 1.5f, 1.6f, 0.8f, 20, true);
    km_cylinder(b, KM_MAT_LIGHT, 0x9fe8ff, (v3){p.x, 3.3f, p.z}, 1.2f, 1.2f, 5.0f, 20, true);
    km_cylinder(b, KM_MAT_HULL, 0xd0d8dd, (v3){p.x, 6.0f, p.z}, 1.4f, 1.4f, 0.5f, 20, false);
    break;
  case KM_COVER_CONSOLE:
    km_box(b, KM_MAT_DARK, 0x4a5058, p, (v3){3.0f, 1.0f, 1.0f}, cv->rotation, true);
    km_box(b, KM_MAT_LIGHT, 0xffffff, (v3){p.x, 1.0f, p.z}, (v3){2.6f, 0.05f, 0.7f}, cv->rotation, false);
    break;
  }
}

// the facility exterior: the great dome over the lobby and halls, the needle tower, distant stilt cities
hz_internal void km_build_skyline(KmBuild *b) {
  MeshBuilderStyle hull = km_style(KM_MAT_HULL, 0xcfd8de, MESH_BUILDER_COLLIDER_NONE);
  // the facility's shell: a long white hull over the rooms
  v2 shell[] = {{0.0f, 26.0f}, {14.0f, 24.0f}, {26.0f, 18.0f}, {31.0f, 10.0f}, {32.0f, 0.0f}, {28.0f, -8.0f}, {0.0f, -10.0f}};
  for (u32 i = 0; i < 3; i++) {
    f32 z = -142.0f - (f32)i * 52.0f;
    mesh_builder_lathe(&b->mb, &hull, m4_trs((v3){0.0f, -0.1f, z}, quat_identity(), (v3){1.0f, 1.0f, 1.15f}), shell, ARRAY_SIZE(shell), 48);
  }
  km_cylinder(b, KM_MAT_HULL, 0xd6dee3, (v3){0.0f, (KM_OCEAN_Y - 10.0f) * 0.5f, -190.0f}, 26.0f, 30.0f, -KM_OCEAN_Y - 10.0f, 32, false);
  // the needle tower east of Dorn
  km_cylinder(b, KM_MAT_HULL, 0xdfe6ea, (v3){70.0f, 10.0f, -110.0f}, 2.0f, 5.0f, 96.0f, 20, false);
  mesh_builder_sphere(&b->mb, &hull, m4_trs((v3){70.0f, 50.0f, -110.0f}, quat_identity(), (v3){9.0f, 4.0f, 9.0f}), 24, 12);
  km_box(b, KM_MAT_RED, 0xffffff, (v3){70.0f, 58.0f, -110.0f}, (v3){0.6f, 0.6f, 0.6f}, 0.0f, false);
  // distant stilt cities on the horizon
  for (u32 i = 0; i < 14; i++) {
    f32 a = (f32)i / 14.0f * 2.0f * M_PI + random_f32_range(&b->rng, -0.15f, 0.15f);
    f32 d = random_f32_range(&b->rng, 260.0f, 420.0f);
    v3 c = {m_cosf(a) * d, random_f32_range(&b->rng, -8.0f, 6.0f), -90.0f + m_sinf(a) * d};
    f32 r = random_f32_range(&b->rng, 18.0f, 40.0f);
    v2 saucer[] = {{0.0f, 4.0f}, {r * 0.6f, 3.6f}, {r, 1.5f}, {r * 1.02f, 0.0f}, {r * 0.6f, -4.0f}, {r * 0.2f, -7.0f}, {0.0f, -7.5f}};
    mesh_builder_lathe(&b->mb, &hull, m4_translate(c), saucer, ARRAY_SIZE(saucer), 32);
    km_cylinder(b, KM_MAT_HULL, 0xbcc6cc, (v3){c.x, (KM_OCEAN_Y + c.y - 7.0f) * 0.5f, c.z}, r * 0.15f, r * 0.2f,
                c.y - 7.0f - KM_OCEAN_Y + 2.0f, 16, false);
    km_box(b, KM_MAT_LIGHT, 0xffffff, (v3){c.x, c.y + 4.0f, c.z}, (v3){r * 0.3f, 0.6f, r * 0.3f}, a, false);
  }
}

// the blast doors: shut at the start of a run, each its own slab and collider
hz_internal void km_build_doors(EcsWorld *w, RendererState *r, AssetSystem *assets, KmWorld *km) {
  static const v3 centres[KM_DOOR_COUNT] = {{0.0f, 2.5f, -169.0f}, {0.0f, 2.5f, -205.0f}};
  static const v3 halves[KM_DOOR_COUNT] = {{2.7f, 2.5f, 0.22f}, {3.2f, 2.5f, 0.22f}};
  HzHandleT(RendererMaterial) dark = renderer_create_material_from_asset(r, assets, asset_get(assets, km->assets[KM_MAT_DARK], MaterialBlobAsset));
  HzHandleT(RendererMaterial) red = renderer_create_material_from_asset(r, assets, asset_get(assets, km->assets[KM_MAT_RED], MaterialBlobAsset));
  for (u32 d = 0; d < KM_DOOR_COUNT; d++) {
    ArenaAllocator *arena = arena_create(MB(4), KB(64));
    Allocator alloc = make_arena_allocator(arena);
    KmBuild *kb = ALLOC(&alloc, KmBuild);
    mesh_builder_begin(&kb->mb, arena, &(MeshBuilderDesc){.slot_count = KM_MAT_COUNT});
    v3 h = halves[d];
    km_box(kb, KM_MAT_DARK, 0x3c4148, (v3){0.0f, -h.y, 0.0f}, v3_scale(h, 2.0f), 0.0f, false);
    for (f32 y = -h.y + 0.6f; y < h.y; y += 1.2f)
      km_box(kb, KM_MAT_DARK, 0x262a30, (v3){0.0f, y, 0.0f}, (v3){h.x * 2.0f + 0.02f, 0.08f, h.z * 2.0f + 0.04f}, 0.0f, false);
    km_box(kb, KM_MAT_RED, 0xffffff, (v3){0.0f, -0.05f, 0.0f}, (v3){h.x * 1.6f, 0.1f, h.z * 2.0f + 0.06f}, 0.0f, false);
    MeshBuilderResult res = mesh_builder_finish(&kb->mb, &(MeshBuilderFinishDesc){.renderer = r, .alloc = &alloc});
    EcsEntity body = ecs_entity_create(w);
    ecs_component_set(w, body, ColliderBody, ((ColliderBody){.collider = collider_box(V3_ZERO, h), .pos = centres[d], .rot = quat_identity(), .scale = 1.0f}));
    ecs_component_add(w, body, StaticBody);
    for (u32 s = 0; s < res.slot_count; s++) {
      for (u32 p = 0; p < res.slots[s].part_count; p++) {
        EcsEntity e = ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = res.slots[s].parts[p], .material = s == KM_MAT_RED ? red : dark,
                                                                       .transform = {centres[d], quat_identity(), V3_ONE}, .casts_shadow = true,
                                                                       .name = "BlastDoor"});
        ecs_component_set(w, e, KmDoor, ((KmDoor){.index = d, .closed = centres[d], .half = h, .body = body}));
      }
    }
  }
}

hz_internal void km_build(EcsWorld *w, KmWorld *km) {
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  ArenaAllocator *arena = arena_create(MB(256), MB(4));
  Allocator alloc = make_arena_allocator(arena);
  KmBuild *kb = ALLOC(&alloc, KmBuild);
  kb->rng = random_create(0x4b414d49u);
  mesh_builder_begin(&kb->mb, arena, &(MeshBuilderDesc){.slot_count = KM_MAT_COUNT, .vtx_reserve = 16384, .idx_reserve = 65536});

  for (u32 i = 0; i < KM_PLATFORM_COUNT; i++)
    km_build_platform(kb, &km_platforms[i], i == KM_SECTOR_AUREK || i == KM_SECTOR_DORN);
  for (u32 i = 0; i < KM_BRIDGE_COUNT; i++)
    km_build_bridge(kb, &km_bridges[i]);
  km_build_room(kb, &km_rooms[0], 4.0f, 2.5f);
  km_build_room(kb, &km_rooms[1], 2.5f, 3.0f);
  km_build_room(kb, &km_rooms[2], 3.0f, 0.0f);
  // the corridor's alcove blocks: solid cover between the alcoves
  for (u32 k = 0; k < 3; k++) {
    f32 z = -175.0f - (f32)k * 11.0f;
    km_block(kb, KM_MAT_HULL, 0xd9e0e5, (v3){-7.0f, 0.0f, z - 2.0f}, (v3){-4.0f, 4.6f, z}, true);
    km_block(kb, KM_MAT_HULL, 0xd9e0e5, (v3){4.0f, 0.0f, z - 2.0f}, (v3){7.0f, 4.6f, z}, true);
  }
  // the cloning hall: rows of growth tanks and the raised control deck
  for (u32 zi = 0; zi < 5; zi++) {
    static const f32 xs[4] = {-15.0f, -8.0f, 8.0f, 15.0f};
    for (u32 xi = 0; xi < 4; xi++) {
      KmCover tank = {KM_SECTOR_HALL, KM_COVER_TANK, xs[xi], 18.0f - (f32)zi * 8.0f, 0.0f};
      km_build_cover(kb, &tank);
    }
  }
  km_block(kb, KM_MAT_DECK, 0x30363d, (v3){-9.0f, 0.0f, -260.0f}, (v3){13.0f, 0.4f, -253.0f}, true);
  for (u32 i = 0; i < KM_COVER_COUNT; i++)
    km_build_cover(kb, &km_cover[i]);
  km_build_skyline(kb);
  // the parked gunship's fuselage and the landing craft's hull block the way; wings and fins do not
  mesh_builder_collider_box(&kb->mb, (v3){KM_GUNSHIP_POS.x, 2.0f, KM_GUNSHIP_POS.z}, quat_rotate_y(KM_GUNSHIP_YAW), (v3){1.7f, 2.0f, 7.5f});
  mesh_builder_collider_box(&kb->mb, (v3){KM_LANDER_POS.x, 2.0f, KM_LANDER_POS.z}, quat_identity(), (v3){10.5f, 2.0f, 4.2f});
  // the sea: a dark plane far below
  km_box(kb, KM_MAT_DARK, 0x0b1218, (v3){0.0f, KM_OCEAN_Y - 0.5f, -90.0f}, (v3){1400.0f, 0.5f, 1400.0f}, 0.0f, false);

  MeshBuilderResult res = mesh_builder_finish(&kb->mb, &(MeshBuilderFinishDesc){.renderer = r, .world = w, .alloc = &alloc, .uv_per_metre = 0.5f});
  for (u32 s = 0; s < res.slot_count; s++) {
    HzHandleT(RendererMaterial) mat = renderer_create_material_from_asset(r, assets, asset_get(assets, km->assets[s], MaterialBlobAsset));
    if (s == KM_MAT_DARK)
      km->dark_material = mat;
    if (s == KM_MAT_HULL)
      km->hull_material = mat;
    for (u32 p = 0; p < res.slots[s].part_count; p++) {
      EcsEntity e = ecs_renderer_spawn(w, r, &(EcsRendererSpawnDesc){.submesh = res.slots[s].parts[p], .material = mat,
                                                       .transform = {.rotation = quat_identity(), .scale = V3_ONE},
                                                       .casts_shadow = s != KM_MAT_LIGHT && s != KM_MAT_WARM && s != KM_MAT_RED,
                                                       .static_shadow = true, .name = "Tipoca"});
      gr_look_probe_capture(w, e);
      km->instances++;
    }
  }
  km->colliders = res.collider_count;
  km_build_doors(w, r, assets, km);
  EcsEntity gunship = ecs_renderer_spawn(w, NULL, &(EcsRendererSpawnDesc){.model_asset = blob_asset_ref_from_handle(ModelBlobAsset, km->gunship),
                                                                     .transform = {KM_GUNSHIP_POS, quat_rotate_y(KM_GUNSHIP_YAW), V3_ONE},
                                                                     .casts_shadow = true, .static_shadow = true, .name = "Gunship"});
  EcsEntity lander = ecs_renderer_spawn(w, NULL, &(EcsRendererSpawnDesc){.model_asset = blob_asset_ref_from_handle(ModelBlobAsset, km->lander),
                                                                    .transform = {KM_LANDER_POS, quat_identity(), V3_ONE},
                                                                    .casts_shadow = true, .static_shadow = true, .name = "Lander"});
  gr_look_probe_capture(w, gunship);
  gr_look_probe_capture(w, lander);
  for (u32 i = 0; i < KM_PLATFORM_COUNT; i++)
    km->lights += km_platform_lights(r, &km_platforms[i]);
  for (u32 i = 0; i < KM_ROOM_COUNT; i++)
    km->lights += km_room_lights(r, &km_rooms[i]);
  km->built = true;
  log_info("kamino: Tipoca built: % vertices, % instances, % colliders, % lights", fmt_u32(res.vertex_count), fmt_u32(km->instances),
           fmt_u32(res.collider_count), fmt_u32(km->lights));
}

// ---------------------------------------------------------------- the storm

// a strike's brightness over time: a leader flicker, the main flash, one or two re-strikes, then dark
hz_internal f32 km_strike_envelope(f32 t, u32 seed) {
  if (t < 0.0f || t > 0.9f)
    return 0.0f;
  f32 f = m_expf(-m_absf(t - 0.06f) * 60.0f) * 0.5f + m_expf(-m_maxf(t - 0.14f, 0.0f) * 9.0f) * (t > 0.12f ? 1.0f : 0.0f);
  if (seed & 1u)
    f += m_expf(-m_absf(t - 0.42f) * 30.0f) * 0.7f;
  if (seed & 2u)
    f += m_expf(-m_absf(t - 0.63f) * 40.0f) * 0.45f;
  return m_minf(f, 1.0f);
}

// the storm's clock: a strike every 5..14 s somewhere around the player; its thunder follows at the speed of sound
ecs_system(km_storm_system, EcsResInOut(KmStorm), EcsResIn(GrClock), EcsResIn(GrPlayerState)) {
  KmStorm *s = ecs_sys_get_resource_mut(KmStorm);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  f32 dt = c->dt;
  v3 player = ecs_sys_get_resource(GrPlayerState)->eye;
  s->timer -= dt;
  if (s->timer <= 0.0f) {
    Random rng = random_create(0x57047u + s->seed * 7919u);
    s->seed++;
    s->timer = random_f32_range(&rng, 5.0f, 14.0f);
    s->age = 0.0f;
    f32 a = random_f32_range(&rng, 0.0f, 2.0f * M_PI);
    f32 d = random_f32_range(&rng, 60.0f, 340.0f);
    s->bolt = (v3){player.x + m_cosf(a) * d, 140.0f, player.z + m_sinf(a) * d};
    s->thunder_in = d / 343.0f + 0.15f;
    s->thunder_gain = m_clampf(1.4f - d / 340.0f, 0.35f, 1.0f);
    log_info("storm: strike % m away at t %", fmt_f32_p(d, 0), fmt_f32_p(c->time, 2));
  }
  if (s->age >= 0.0f) {
    s->age += dt;
    s->flash = km_strike_envelope(s->age, s->seed);
    if (s->age > 1.0f)
      s->age = -1.0f;
  } else {
    s->flash = 0.0f;
  }
  if (s->thunder_in >= 0.0f) {
    s->thunder_in -= dt;
    if (s->thunder_in < 0.0f)
      s->thunder_serial++;
  }
}

// the thunder's voice: an entity whose emitter plays each strike's rumble when it reaches the player
typedef struct KmThunderVoice {
  u32 seen; // KmStorm.thunder_serial played
} KmThunderVoice;
ECS_COMPONENT_TYPEDEF(KmThunderVoice);

// a low, slow, reverberant explosion: the rumble of a strike, nearer ones louder and less muffled
ecs_system(km_thunder_system, EcsInOut(KmThunderVoice), EcsInOut(GrSfxEmitter), EcsResIn(KmStorm), EcsResIn(GrClock)) {
  const KmStorm *s = ecs_sys_get_resource(KmStorm);
  const GrClock *c = ecs_sys_get_resource(GrClock);
  for (i32 i = 0; i < it->count; i++) {
    KmThunderVoice *v = ecs_sys_get_component_mut(KmThunderVoice, i);
    if (v->seen == s->thunder_serial)
      continue;
    v->seen = s->thunder_serial;
    gr_sfx_emit(ecs_sys_get_component_mut(GrSfxEmitter, i), c->frame,
                (GrSfxPlay){.sound = GR_SOUND_EXPLOSION, .volume = 1.6f * s->thunder_gain, .rate = 0.28f + 0.12f * s->thunder_gain,
                            .jitter = 0.08f, .reverb = 1.0f});
  }
}

// the blast doors follow the front: the next one grinds open as the droids fall back through it, all shut on a new run
ecs_system(km_door_system, EcsInOut(KmDoor), EcsInOut(LocalTransform), EcsInOut(WorldTransform), EcsResIn(GrWaves), EcsResIn(GrClock),
           EcsExternInOut(ColliderBody)) {
  const GrWaves *wv = ecs_sys_get_resource(GrWaves);
  f32 dt = ecs_sys_get_resource(GrClock)->dt;
  for (i32 i = 0; i < it->count; i++) {
    KmDoor *d = ecs_sys_get_component_mut(KmDoor, i);
    i32 front = 3 + (i32)d->index; // the lobby is the campaign's fourth front
    b32 open = wv->wave > front || (wv->wave == front && (wv->phase == GR_WAVES_SECURE || wv->phase == GR_WAVES_COMPLETE));
    f32 target = open ? 1.0f : 0.0f;
    if (wv->wave < 0)
      d->open = 0.0f;
    d->open = target > d->open ? m_minf(target, d->open + dt * 0.25f) : m_maxf(target, d->open - dt * 0.5f);
    v3 pos = v3_add(d->closed, (v3){0.0f, d->open * d->half.y * 2.0f, 0.0f});
    LocalTransform *lt = ecs_sys_get_component_mut(LocalTransform, i);
    if (!v3_eqv(lt->position, pos)) {
      lt->position = pos;
      ecs_sys_get_component_mut(WorldTransform, i)->position = pos;
      ColliderBody *b = ecs_component_get_mut(it->world, d->body, ColliderBody);
      if (b)
        b->pos = pos;
    }
  }
}

void km_storm_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_storm_system); // writes the KmStorm resource
  ecs_schedule(w, km_thunder_system);
  ecs_schedule_single(w, km_door_system); // writes the door slabs' shared collider bodies
}

// main lane: the strike's light, high over the player, only while it flashes
hz_internal void km_storm_main(EcsWorld *w) {
  KmStorm *s = ecs_resource_get_mut(w, KmStorm);
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  if (!s->light_ready) {
    s->light = renderer_create_local_light(r, &(RendererLocalLightDesc){.position = {0, 140, 0}, .radius = 600.0f, .color = {0.75f, 0.82f, 1.0f}});
    s->light_ready = true;
  }
  renderer_set_local_light(r, s->light, &(RendererLocalLightDesc){.position = s->bolt, .radius = 700.0f, .color = {0.75f, 0.82f, 1.0f},
                                                                 .intensity = s->flash * 1.2e10f});
  renderer_set_local_light_visible(r, s->light, s->flash > 0.01f);
}

void km_world_init(EcsWorld *w, AssetSystem *assets) {
  // resources are main-lane app_init state
  assert(is_main_thread());
  KmWorld km = {0};
  for (u32 i = 0; i < KM_MAT_COUNT; i++)
    km.assets[i] = asset_load(MaterialBlobAsset, assets, .path = km_material_paths[i]);
  km.gunship = asset_load(ModelBlobAsset, assets, .path = "kamino/models/vehicles/gunship.glb");
  km.lander = asset_load(ModelBlobAsset, assets, .path = "kamino/models/vehicles/lander.glb");
  ecs_resource_set(w, KmWorld, km);
  ecs_resource_set(w, KmStorm, ((KmStorm){.timer = 4.0f, .age = -1.0f, .thunder_in = -1.0f}));
  EcsEntity thunder = ecs_entity_create(w);
  ecs_component_set(w, thunder, KmThunderVoice, {0});
  ecs_component_set(w, thunder, GrSfxEmitter, {0});
}

void km_world_main(EcsWorld *w) {
  // mesh_builder_finish and the renderer spawns are main-lane, outside the scope
  assert(is_main_thread());
  KmWorld *km = ecs_resource_get_mut(w, KmWorld);
  if (km->built) {
    km_storm_main(w);
    return;
  }
  const AssetSystem *assets = ecs_resource_get(w, AssetSystem);
  for (u32 i = 0; i < KM_MAT_COUNT; i++)
    if (!asset_is_ready(assets, km->assets[i]))
      return;
  km_build(w, km);
}
