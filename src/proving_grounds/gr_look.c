// The look: _GrLook, sun, sky, env cube, fog, bloom, final, quality, the floor mirror, part materials and UI surfaces.
#include "hz_engine.h"
#include "rendering/features/gtao.h"
#include "rendering/features/height_fog.h"
#include "rendering/features/shadows.h"
#include "rendering/features/sky.h"
#include "rendering/render_view.h"
#include "shaders/gr_look.h"
#include "shaders/gr_sky_cube.h"

#define GR_NOISE_SIZE 256u
#define GR_NOISE_SEED 4242u
#define GR_ENV_FACE 128u
#define GR_GTAO_RADIUS 1.1f
#define GR_GTAO_POWER 1.6f
#define GR_FINAL_SCOPE_MAT "proving_grounds/materials/gr_final_scope_mat.hzt"
#define GR_SKY_MAT "proving_grounds/materials/gr_sky_mat.hzt"
#define GR_SKY_MODEL "engine/primitives/sphere/sphere.hza"
// the dome's scale: inside the far plane (400), around the whole level so it is never culled
#define GR_SKY_SCALE 390.0f

#define GR_PICKUP_WEAPON_COLOR 0xffb627u // the weapon pickup beacon colour
#define GR_SCREEN_W 128 // the ammo screen canvas (px)
#define GR_SCREEN_H 64
#define GR_SCREEN_BG 0x02080bu

// a 0xRRGGBB hex as the sRGB colour the colour setters decode
hz_internal ColorF32 gr_look_srgb(u32 hex) {
  return (ColorF32){(f32)((hex >> 16) & 255u) / 255.0f, (f32)((hex >> 8) & 255u) / 255.0f, (f32)(hex & 255u) / 255.0f, 1.0f};
}
// the mean of the fog's wisp term (0.2 + 2.2 wisps) over its 3D noise: the engine medium is uniform
#define GR_FOG_WISP_MEAN 1.2f
// planar mirror: the hub floor's plane
#define GR_PLANAR_HEIGHT 0.0f
// the viewmodel's reverse-Z depth band (a foreground band [0.99, 1])
#define GR_VIEWMODEL_DEPTH_RANGE 0.01f
// area probes: 256 cubes, far 300, two bounces
#define GR_PROBE_FACE 256u
#define GR_PROBE_FAR 300.0f
// area weights: smoothstep(-AREA_FADE, AREA_FADE) across the box face, AREA_FADE 1 m
#define GR_AREA_FADE 1.0f
hz_internal v4 gr_v4(v3 v, f32 w) { return (v4){v.x, v.y, v.z, w}; }

// ---- the tileable noise texture ----

// value noise with integer period p across the texture, so it tiles
hz_internal f32 gr_noise_value(const f32 *lattice, f32 x, f32 y, i32 p, i32 offset) {
  f32 fx = x / (f32)GR_NOISE_SIZE * (f32)p;
  f32 fy = y / (f32)GR_NOISE_SIZE * (f32)p;
  i32 x0 = (i32)m_floorf(fx);
  i32 y0 = (i32)m_floorf(fy);
  f32 tx = fx - (f32)x0;
  f32 ty = fy - (f32)y0;
  f32 sx = tx * tx * (3.0f - 2.0f * tx);
  f32 sy = ty * ty * (3.0f - 2.0f * ty);
  i32 n = (i32)(GR_NOISE_SIZE * GR_NOISE_SIZE);
#define GR_AT(i, j) lattice[((((((j) % p) + p) % p) * 131 + ((((i) % p) + p) % p) * 17 + offset) % n)]
  f32 a = GR_AT(x0, y0) + (GR_AT(x0 + 1, y0) - GR_AT(x0, y0)) * sx;
  f32 b = GR_AT(x0, y0 + 1) + (GR_AT(x0 + 1, y0 + 1) - GR_AT(x0, y0 + 1)) * sx;
#undef GR_AT
  return a + (b - a) * sy;
}

hz_internal f32 gr_noise_fbm(const f32 *lattice, f32 x, f32 y, i32 base, i32 offset) {
  f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
  for (i32 o = 0; o < 4; o++) {
    sum += amp * gr_noise_value(lattice, x, y, base << o, offset + o * 7919);
    norm += amp;
    amp *= 0.5f;
  }
  return sum / norm;
}

hz_internal u8 gr_unorm8(f32 v) { return (u8)m_clampf(v * 255.0f, 0.0f, 255.0f); }

hz_internal GpuTexture gr_noise_texture_make(void) {
  u32 mips = 9; // 256 .. 1
  GpuTextureMipsDesc desc = {.kind = GPU_TEXTURE_KIND_2D,
                             .format = GPU_TEXTURE_FORMAT_RGBA8,
                             .width = GR_NOISE_SIZE,
                             .height = GR_NOISE_SIZE,
                             .mip_count = mips,
                             .label = "gr noise"};
  desc.data_size = gpu_texture_mips_data_size(&desc);
  GpuTexture tex = {0};
  tctx_temp_allocator_scope(ta, NULL) {
    u32 n = GR_NOISE_SIZE * GR_NOISE_SIZE;
    f32 *lattice = ALLOC_ARRAY_NO_ZERO(&ta.allocator, f32, n);
    Mulberry32_State seed = {GR_NOISE_SEED};
    for (u32 i = 0; i < n; i++)
      lattice[i] = mulberry32_next_f32(&seed);
    u8 *data = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u8, desc.data_size);
    for (u32 y = 0; y < GR_NOISE_SIZE; y++) {
      for (u32 x = 0; x < GR_NOISE_SIZE; x++) {
        u8 *p = data + (y * GR_NOISE_SIZE + x) * 4u;
        f32 fx = (f32)x, fy = (f32)y;
        // truncate to a byte, not round
        p[0] = gr_unorm8(gr_noise_fbm(lattice, fx, fy, 4, 11));
        p[1] = gr_unorm8(gr_noise_fbm(lattice, fx, fy, 16, 3571));
        p[2] = gr_unorm8(gr_noise_value(lattice, fx, fy, 64, 9001));
        p[3] = gr_unorm8(gr_noise_fbm(lattice, fx, fy, 8, 20011));
      }
    }
    // the mip chain: a 2x2 box per level
    u8 *src = data;
    for (u32 m = 1; m < mips; m++) {
      GpuTextureMipLevel prev = gpu_texture_mips_level(&desc, m - 1);
      GpuTextureMipLevel cur = gpu_texture_mips_level(&desc, m);
      u8 *dst = src + prev.bytes;
      for (u32 y = 0; y < cur.height; y++) {
        for (u32 x = 0; x < cur.width; x++) {
          for (u32 c = 0; c < 4; c++) {
            u32 sx = MIN(x * 2u + 1u, prev.width - 1u), sy = MIN(y * 2u + 1u, prev.height - 1u);
            u32 sum = (u32)src[((y * 2u) * prev.width + x * 2u) * 4u + c] + src[((y * 2u) * prev.width + sx) * 4u + c] +
                      src[(sy * prev.width + x * 2u) * 4u + c] + src[(sy * prev.width + sx) * 4u + c];
            dst[(y * cur.width + x) * 4u + c] = (u8)((sum + 2u) / 4u);
          }
        }
      }
      src = dst;
    }
    desc.data = data;
    tex = gpu_make_texture_mips(&desc);
  }
  return tex;
}

// ---- the hub floor mirror ----

struct GrMirrorGpu {
  RendererFeature feature;
  PlanarReflection reflection;
  EcsWorld *world;
  GpuComputePipeline kernels[2];
  b32 active; // set by gr_look_sync before renderer_update
  b32 enabled; // the quality level mirrors
  f32 scale;   // the mirror's size relative to the output size
  aabb floor;       // the hub floor, set with active
  f32 distortion;   // the floor's normal distortion of the mirror lookup (uv)
};

#define GR_MIRROR_MIN_W 1e-4f // the floor quad is clipped here before the divide
#define GR_MIRROR_CULL_PLANES 10u

hz_internal v4 gr_plane_normalized(v4 p) {
  f32 len = m_sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
  return len > 0.0f ? (v4){p.x / len, p.y / len, p.z / len, p.w / len} : p;
}

// mirror clip x or y (row 0/1) against w (row 3): sign * (row_axis - bound * row_w) >= 0
hz_internal v4 gr_clip_bound_plane(mat4 vp, u32 axis, f32 bound, f32 sign) {
  v4 a = {vp.m[0][axis], vp.m[1][axis], vp.m[2][axis], vp.m[3][axis]};
  v4 w = {vp.m[0][3], vp.m[1][3], vp.m[2][3], vp.m[3][3]};
  return gr_plane_normalized((v4){sign * (a.x - bound * w.x), sign * (a.y - bound * w.y), sign * (a.z - bound * w.z),
                                  sign * (a.w - bound * w.w)});
}

// the mirror's frustum (its oblique near drops what is under the floor) narrowed to the floor's padded screen rect
hz_internal u32 gr_mirror_cull_planes(const GrMirrorGpu *m, const RenderView *view, const RenderView *mirror, v4 *out) {
  frustum_planes(mirror->view_proj, out);
  u32 n = 6;
  v3 org = {(f32)view->origin.x, (f32)view->origin.y, (f32)view->origin.z};
  f32 y = GR_PLANAR_HEIGHT - org.y;
  v3 c[4] = {{m->floor.min.x - org.x, y, m->floor.min.z - org.z}, {m->floor.max.x - org.x, y, m->floor.min.z - org.z},
             {m->floor.max.x - org.x, y, m->floor.max.z - org.z}, {m->floor.min.x - org.x, y, m->floor.max.z - org.z}};
  v4 clip[4];
  for (u32 i = 0; i < 4; i++)
    clip[i] = m4_mulv4(view->view_proj_unjittered, (v4){c[i].x, c[i].y, c[i].z, 1.0f});
  f32 x0 = 1.0f, x1 = -1.0f, y0 = 1.0f, y1 = -1.0f;
  u32 kept = 0;
  for (u32 i = 0; i < 4; i++) {
    v4 a = clip[i], b = clip[(i + 1) % 4];
    v4 pts[2];
    u32 np = 0;
    if (a.w >= GR_MIRROR_MIN_W)
      pts[np++] = a;
    if ((a.w >= GR_MIRROR_MIN_W) != (b.w >= GR_MIRROR_MIN_W)) {
      f32 t = (GR_MIRROR_MIN_W - a.w) / (b.w - a.w);
      pts[np++] = (v4){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, GR_MIRROR_MIN_W};
    }
    for (u32 k = 0; k < np; k++) {
      f32 nx = m_clampf(pts[k].x / pts[k].w, -1.0f, 1.0f), ny = m_clampf(pts[k].y / pts[k].w, -1.0f, 1.0f);
      x0 = m_minf(x0, nx), x1 = m_maxf(x1, nx), y0 = m_minf(y0, ny), y1 = m_maxf(y1, ny);
      kept++;
    }
  }
  if (kept == 0)
    return n;
  // the lookup's widest lod (gr_surface_fs.glsl) and the downsample chain's reach at it, in mip-0 texels
  f32 w = (f32)m->reflection.width, h = (f32)m->reflection.height;
  f32 lod = m_minf(m_maxf(1.0f, m_log2f(m_maxf(w, h)) - 3.0f) * 0.85f, (f32)m->reflection.mip_count - 1.0f);
  f32 reach = 4.0f * m_powf(2.0f, lod);
  f32 mx = 2.0f * (reach / w + m->distortion), my = 2.0f * (reach / h + m->distortion);
  // the mirror image is the main view's flipped in x
  f32 mx0 = -x1 - mx, mx1 = -x0 + mx, my0 = y0 - my, my1 = y1 + my;
  if (mx0 > -1.0f)
    out[n++] = gr_clip_bound_plane(mirror->view_proj, 0, mx0, 1.0f);
  if (mx1 < 1.0f)
    out[n++] = gr_clip_bound_plane(mirror->view_proj, 0, mx1, -1.0f);
  if (my0 > -1.0f)
    out[n++] = gr_clip_bound_plane(mirror->view_proj, 1, my0, 1.0f);
  if (my1 < 1.0f)
    out[n++] = gr_clip_bound_plane(mirror->view_proj, 1, my1, -1.0f);
  // render space to the instances' world space
  for (u32 i = 0; i < n; i++)
    out[i].w -= out[i].x * org.x + out[i].y * org.y + out[i].z * org.z;
  return n;
}

hz_internal b32 gr_mirror_has_draws(void *user) { return ((GrMirrorGpu *)user)->active; }

// AFTER_SHADOWS, so the floor's lit pass reads this frame's mirror (world, effects and sky)
hz_internal void gr_mirror_record(RendererState *r, const RendererPassCtx *ctx, void *user) {
  UNUSED(ctx);
  GrMirrorGpu *m = (GrMirrorGpu *)user;
  if (!m->active)
    return;
  const RenderView *view = renderer_view(r);
  // the output size, not the render size: the mirror is unjittered, so an upscaler never rebuilds its detail
  u32 out_w = 0, out_h = 0;
  renderer_post_size(r, &out_w, &out_h);
  u32 w = (u32)m_maxf(1.0f, (f32)out_w * m->scale + 0.5f);
  u32 h = (u32)m_maxf(1.0f, (f32)out_h * m->scale + 0.5f);
  RenderView mirror = render_view_mirror(view, (v4){0.0f, 1.0f, 0.0f, -GR_PLANAR_HEIGHT}, w, h);
  const RendererInstanceList *list = ecs_render_list(ecs_resource_get(m->world, EcsRenderListsResource), GR_LIST_MIRROR);
  planar_reflection_begin(&m->reflection, &mirror);
  v4 planes[GR_MIRROR_CULL_PLANES];
  u32 plane_count = gr_mirror_cull_planes(m, view, &mirror, planes);
  if (list && list->len > 0)
    renderer_draw_instances(r, &(RendererDrawDesc){.view = &mirror,
                                                   .mode = RENDERER_DRAW_LIT,
                                                   .instances = list->items,
                                                   .count = list->len,
                                                   .cull_planes = planes,
                                                   .cull_plane_count = plane_count});
  planar_reflection_end(&m->reflection);
  // a resize recreates the chain: publish after every draw
  renderer_feature_publish(r, m->feature, "gr_mirror", planar_reflection_texture(&m->reflection));
}

// the quality level's mirror: off at Low, else its size over the main view's
hz_internal void gr_mirror_quality(RendererState *r, RendererQuality level, void *user) {
  UNUSED(r);
  GrMirrorGpu *m = (GrMirrorGpu *)user;
  PlanarReflectionQuality q = planar_reflection_quality(level);
  m->enabled = q.enabled;
  m->scale = q.scale;
}

hz_internal void gr_mirror_destroy(RendererState *r, void *user) {
  UNUSED(r);
  planar_reflection_destroy(&((GrMirrorGpu *)user)->reflection);
}

hz_internal GrMirrorGpu *gr_mirror_init(Allocator *a, RendererState *r, EcsWorld *w) {
  GrMirrorGpu *m = ALLOC(a, GrMirrorGpu);
  m->world = w;
  m->scale = 0.5f;
  planar_reflection_init(&m->reflection, r);
  m->kernels[0] = m->reflection.prepare_pipeline;
  m->kernels[1] = m->reflection.downsample_pipeline;
  m->feature = renderer_add_feature(
      r, &(RendererFeatureDesc){
             .name = "gr_mirror",
             .user = m,
             .record = {[RENDERER_PHASE_AFTER_SHADOWS] = gr_mirror_record},
             .has_draws = {[RENDERER_PHASE_AFTER_SHADOWS] = gr_mirror_has_draws},
             .quality = gr_mirror_quality,
             .destroy = gr_mirror_destroy,
             .outputs = {{.name = "gr_mirror", .kind = MAT_TEXTURE_2D, .neutral = TEX_DEFAULT_BLACK}}});
  assert_msg(m->feature.id != 0, "gr_mirror_init: the feature did not register");
  renderer_feature_require(r, m->feature, NULL, 0, m->kernels, ARRAY_SIZE(m->kernels));
  return m;
}

// ---- the look feature: the sky env cube bake (the probes' capture background and the fallback past them) ----

hz_internal void gr_look_prepare(RendererState *r, const RendererPrepareCtx *ctx, void *user) {
  UNUSED(ctx);
  GrLook *l = (GrLook *)user;
  // webgpu realises the storage cube at a later drain; a dispatch into it now would be dropped
  // the sky waits for Settings' config and the Sun; either changing re-bakes it
  if (l->env_baked || !l->cfg_ready || !l->sun_ready || !gpu_texture_is_ready(l->env_cube))
    return;
  ShaderGrSkyCubeParams p = {
      .zenith = gr_v4(gr_look_linear(l->cfg.sky_zenith), 0.0f),
      .horizon = gr_v4(gr_look_linear(l->cfg.sky_horizon), 0.0f),
      .sun_dir = gr_v4(l->sun_toward, 0.0f),
      .sun_color = gr_v4(l->sun_color, 0.0f),
      .time = 0.0f,
      .intensity = l->cfg.sky_intensity,
      .face_size = GR_ENV_FACE,
  };
  gpu_texture_access(l->env_cube, GPU_ACCESS_SHADER_WRITE);
  gpu_apply_compute_pipeline(l->env_pipeline);
  shader_gr_sky_cube_apply_params(&p);
  shader_gr_sky_cube_apply_bindings((ShaderGrSkyCubeBindings){.out_cube = l->env_cube});
  u32 groups = shader_gr_sky_cube_dispatch_groups(GR_ENV_FACE, 8);
  gpu_dispatch(groups, groups, 6);
  gpu_texture_access(l->env_cube, GPU_ACCESS_SHADER_READ);
  // radiance only: the probes' SH carries the diffuse; sky irradiance would flood the halls and the fog
  f32 env = l->cfg.env_intensity * gr_look_unit();
  renderer_sky_set_env_cubemap(r, l->env_cube, 0.0f, env, 0.0f);
  l->env_baked = true;
}

// ---- the _GrLook block ----

// an area's soft membership of p on the CPU (AREA_FADE 1 m), unnormalised
hz_internal f32 gr_area_weight(aabb b, v3 p) {
  v3 d = {m_maxf(b.min.x - p.x, p.x - b.max.x), m_maxf(b.min.y - p.y, p.y - b.max.y), m_maxf(b.min.z - p.z, p.z - b.max.z)};
  v3 outside = {m_maxf(d.x, 0.0f), m_maxf(d.y, 0.0f), m_maxf(d.z, 0.0f)};
  f32 sd = v3_length(outside) + m_minf(m_maxf(d.x, m_maxf(d.y, d.z)), 0.0f);
  return 1.0f - m_smoothstepf(-1.0f, 1.0f, sd) + 1e-4f / (1.0f + m_maxf(sd, 0.0f));
}

hz_internal ShaderGrLookInput gr_look_block(const GrLook *l, const GrClock *c, const GrLevel *lv, const GrFeedback *fb, f32 flash) {
  const GrLookConfig *k = &l->cfg;
  // lightning lifts the sky and the sky's fill
  f32 sky_flash = 1.0f + flash * 14.0f, fill_flash = 1.0f + flash * 7.0f;
  ShaderGrLookInput b = {
      .time = {c->time, gr_look_unit(), k->env_intensity * fill_flash, k->sky_intensity * sky_flash},
      .sun_dir = gr_v4(l->sun_toward, l->sun_intensity),
      .sun_color = gr_v4(l->sun_color, 0.0f),
      .hemi_sky = gr_v4(v3_scale(gr_look_linear(k->hemi_sky), k->hemi_intensity * fill_flash), 0.0f),
      .hemi_ground = gr_v4(v3_scale(gr_look_linear(k->hemi_ground), k->hemi_intensity), 0.0f),
      .sky_zenith = gr_v4(gr_look_linear(k->sky_zenith), 0.0f),
      .sky_horizon = gr_v4(gr_look_linear(k->sky_horizon), 0.0f),
      .planar = {GR_PLANAR_HEIGHT, l->planar_active ? 1.0f : 0.0f, k->planar_distortion, 0.0f},
      .feedback = {fb->damage, fb->low_health, fb->heal, fb->scope},
  };
  for (u32 i = 0; i < GR_AREA_COUNT; i++) {
    b.area_min[i] = gr_v4(lv->areas[i].box.min, lv->areas[i].fog_density);
    b.area_max[i] = gr_v4(lv->areas[i].box.max, 0.0f);
  }
  return b;
}

// ---- systems ----

// lists of each new renderable root: mirror = all but GrNoMirror (the hub floor, the viewmodel), probes = the static world (EcsWith/EcsNot last)
ecs_system(gr_look_lists_system, EcsOptional(GrNoMirror), EcsOptional(GrProbeCapture), EcsWith(EcsVisibility),
           EcsNot(EcsRenderLists), EcsNot(EcsSubMesh)) {
  for (i32 i = 0; i < it->count; i++) {
    b32 hidden = ecs_sys_get_component_optional(GrNoMirror, i) != NULL;
    u32 mask = hidden ? 0u : (1u << GR_LIST_MIRROR);
    if (ecs_sys_get_component_optional(GrProbeCapture, i))
      mask |= 1u << GR_LIST_PROBE;
    ecs_component_set(it->world, it->entities[i], EcsRenderLists, {.mask = mask});
  }
}

void gr_look_probe_capture(EcsWorld *w, EcsEntity e) { ecs_component_add(w, e, GrProbeCapture); }

void gr_look_viewmodel(EcsWorld *w, EcsEntity e) {
  ecs_component_add(w, e, GrNoMirror);
  ecs_component_add(w, e, EcsProjectionOverride);
}

// Settings' look config, copied on change; a change after the bake re-bakes the sky and re-captures the probes
ecs_system(gr_look_config_system, EcsIn(GrLookConfig), EcsResInOut(GrLook)) {
  if (!ecs_iter_changed(it))
    return;
  GrLook *l = ecs_sys_get_resource_mut(GrLook);
  for (i32 i = 0; i < it->count; i++)
    l->cfg = *ecs_sys_get_component(GrLookConfig, i);
  l->cfg_ready = it->count > 0;
  l->env_baked = false;
  l->probes_baked = false;
  l->applied_fog_density = -1.0f;
}

// the Sun entity as the look's shaders read it: towards the sun, linear colour, look units
ecs_system(gr_look_sun_system, EcsIn(EcsDirectionalLight), EcsIn(WorldTransform), EcsResInOut(GrLook)) {
  if (!ecs_iter_changed(it))
    return;
  GrLook *l = ecs_sys_get_resource_mut(GrLook);
  for (i32 i = 0; i < it->count; i++) {
    const EcsDirectionalLight *d = ecs_sys_get_component(EcsDirectionalLight, i);
    // the light travels along the rotation's +Z (quat_look)
    l->sun_toward = v3_scale(quat_rotate(ecs_sys_get_component(WorldTransform, i)->rotation, (v3){0.0f, 0.0f, 1.0f}), -1.0f);
    l->sun_color = gr_look_linear(d->color);
    l->sun_intensity = d->intensity / gr_look_unit();
  }
  l->sun_ready = it->count > 0;
  l->env_baked = false;
  l->probes_baked = false;
}

// the engine bloom and the final pass's grade, vignette, edges, grain and dither
ecs_system(gr_look_post_system, EcsInOut(EcsPostProcessing), EcsResIn(GrFeedback), EcsResIn(GrLook)) {
  const GrFeedback *fb = ecs_sys_get_resource(GrFeedback);
  const GrLook *l = ecs_sys_get_resource(GrLook);
  if (!l->cfg_ready)
    return;
  const GrLookConfig *k = &l->cfg;
  f32 levels = 1.0f - m_powf(k->bloom_scatter, 6.0f);
  f32 intensity = !l->no_bloom ? k->bloom_strength / levels : 0.0f;
  f32 corner = 0.5f * m_sqrtf(l->aspect * l->aspect + 1.0f);
  for (i32 i = 0; i < it->count; i++) {
    EcsPostProcessing t = *ecs_sys_get_component(EcsPostProcessing, i);
    t.bloom_intensity = intensity;
    // the engine threshold is gamma-space: 1.0 is 1.0 either way
    t.bloom_threshold = k->bloom_threshold;
    t.bloom_scatter = k->bloom_scatter;
    t.bloom_tint = (ColorF32){1.0f, 1.0f, 1.0f, 1.0f};
    // ACES fitted; saturation (less when hurt), contrast, the shadow -> highlight tint
    t.tonemap_mode = TONEMAP_ACES;
    t.exposure = 1.0f;
    t.contrast = k->contrast - 1.0f;
    t.display_saturation = k->saturation * (1.0f - fb->damage * 0.5f) - 1.0f;
    t.display_shadow_tint = (v3){-0.02f, -0.01f, 0.02f};
    t.display_shadow_range = 0.5f;
    t.display_highlight_tint = (v3){0.04f, 0.0f, -0.05f};
    t.display_highlight_start = 0.02f;
    // the vignette after the tonemap; the radii are fractions of the aspect-corrected corner distance
    t.vignette_mode = VIGNETTE_POST_TONEMAP;
    t.vignette_color = (ColorF32){0.0f, 0.0f, 0.0f, 1.0f};
    t.vignette_intensity = k->vignette;
    t.vignette_rounded = true;
    t.vignette_inner = 0.35f * corner;
    t.vignette_outer = 1.25f * corner;
    // chromatic aberration, stronger when hurt (matched near r2 = 0.25 of the engine's dc x r2 form)
    t.chromatic_aberration = k->chromatic + fb->damage * 0.016f;
    // the damage / low-health red edge and the heal glow
    t.edge_tint_color = (ColorF32){0.55f, 0.0f, 0.02f, 1.0f};
    t.edge_tint_amount = fb->damage * 0.85f + fb->low_health * 0.3575f;
    t.edge_tint_pulse = fb->low_health * 0.1925f;
    t.edge_tint_pulse_rate = 6.0f;
    t.edge_tint_inner = 0.45f * corner;
    t.edge_tint_outer = 1.15f * corner;
    t.edge_tint2_color = (ColorF32){0.06f, 0.8f, 0.28f, 1.0f};
    t.edge_tint2_amount = fb->heal * 0.6f;
    t.edge_tint2_inner = 0.45f * corner;
    t.edge_tint2_outer = 1.15f * corner;
    // grain, then the sRGB dither
    t.grain = k->grain;
    t.dither = 1.0f;
    // NaN / Inf in magenta
    t.debug_nan = l->nan;
    if (mem_cmp(&t, ecs_sys_get_component(EcsPostProcessing, i), sizeof(t)) != 0)
      *ecs_sys_get_component_mut(EcsPostProcessing, i) = t;
  }
}

// ---- part materials: drones, guns, pedestals, signs ----

// the role a drone material plays, from its cooked path (drones/materials/<glTF name>_mat.hzt)
hz_internal u32 gr_look_drone_role(const char *path) {
  if (cstr_contains(path, "Emit_Eye"))
    return GR_LOOK_ROLE_LENS;
  if (cstr_contains(path, "Emit_Pupil"))
    return GR_LOOK_ROLE_PUPIL;
  if (cstr_contains(path, "Emit_Energy"))
    return GR_LOOK_ROLE_ENERGY;
  if (cstr_contains(path, "Emit_Thrust"))
    return GR_LOOK_ROLE_THRUST;
  return GR_LOOK_ROLE_HULL;
}

// the gun materials the rig drives, and its ammo screen
hz_internal u32 gr_look_gun_role(const char *path, u32 gun) {
  if (cstr_contains(path, "Emit_Vents"))
    return GR_LOOK_ROLE_VENT;
  if (cstr_contains(path, "Screen"))
    return GR_LOOK_ROLE_SCREEN;
  if (gun != GR_WEAPON_LANCER)
    return GR_LOOK_ROLE_NONE;
  if (cstr_contains(path, "Emit_Coil"))
    return GR_LOOK_ROLE_COIL;
  if (cstr_contains(path, "Emit_Cell"))
    return GR_LOOK_ROLE_CELL;
  if (cstr_contains(path, "Emit_Lens"))
    return GR_LOOK_ROLE_GUN_LENS;
  return GR_LOOK_ROLE_NONE;
}

// a part's private material: the instance, its role and the first write pending
hz_internal void gr_look_part_bind(EcsWorld *w, EcsEntity part, HzHandleT(RendererMaterial) inst, GrLookPart p) {
  ecs_component_set(w, part, EcsMaterialInstance, {.instance = inst});
  ecs_component_set(w, part, GrLookPart, p);
  ecs_component_set(w, part, GrLookPartApplied, {.intensity = -1.0f, .color = {-1.0f, -1.0f, -1.0f}});
}

// the gun parts (each submesh is its own instance): once each, when its material is uploaded
ecs_system(gr_look_gun_parts_system, EcsIn(GrGunPart), EcsIn(EcsSubMeshInstance), EcsNot(GrLookPart),
           EcsResIn(AssetSystem), EcsResInOut(RendererState)) {
  const AssetSystem *a = ecs_sys_get_resource(AssetSystem);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    const GrGunPart *gp = ecs_sys_get_component(GrGunPart, i);
    HzHandleT(RendererMaterial) base =
        renderer_submesh_instance_material(r, ecs_sys_get_component(EcsSubMeshInstance, i)->instance);
    if (!renderer_material(r, base))
      continue;
    CString path = asset_get_path_by_handle(a, HANDLE_CAST(AssetHandle, renderer_material_source_asset(r, base)));
    u32 role = path ? gr_look_gun_role(path, gp->gun) : GR_LOOK_ROLE_NONE;
    if (role == GR_LOOK_ROLE_NONE) {
      ecs_component_set(it->world, it->entities[i], GrLookPart, {.role = GR_LOOK_ROLE_NONE});
      continue;
    }
    gr_look_part_bind(it->world, it->entities[i], renderer_create_material_instance(r, base),
                      (GrLookPart){.role = role, .index = gp->gun});
  }
}

// the level's signs: each its own instance of its EcsMaterial carrying its drawing
ecs_system(gr_look_sign_parts_system, EcsIn(GrSignSite), EcsIn(EcsMaterial), EcsIn(EcsSubMeshInstance), EcsNot(GrLookPart),
           EcsResInOut(RendererState)) {
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    const GrSignSite *s = ecs_sys_get_component(GrSignSite, i);
    HzHandleT(RendererMaterial) base =
        renderer_submesh_instance_material(r, ecs_sys_get_component(EcsSubMeshInstance, i)->instance);
    // the model's own material draws until the EcsMaterial binds: instance the bound one only
    if (!renderer_material(r, base) || s->kind >= GR_SIGN_COUNT ||
        !handle_equals(HANDLE_CAST(HzHandle, renderer_material_source_asset(r, base)),
                       ecs_sys_get_component(EcsMaterial, i)->material.handle))
      continue;
    gr_look_part_bind(it->world, it->entities[i], renderer_create_material_instance(r, base),
                      (GrLookPart){.role = GR_LOOK_ROLE_SIGN, .index = s->kind, .intensity = s->intensity, .color = s->emissive});
  }
}

// the material ref of a model's flattened submesh slot (the order EcsSubMesh.slot counts in)
hz_internal BlobAssetRef *gr_look_slot_material(ModelBlobAsset *model, u32 slot) {
  Array(MeshBlobAsset) meshes = blob_array_get(MeshBlobAsset, &model->meshes);
  u32 idx = 0;
  for (u32 m = 0; m < meshes.len; m++) {
    Array(SubMeshBlobAsset) subs = blob_array_get(SubMeshBlobAsset, &meshes.items[m].submeshes);
    if (slot < idx + subs.len)
      return &subs.items[slot - idx].material;
    idx += subs.len;
  }
  return NULL;
}

// the model roots the look drives (drones, pedestals): once each, when every submesh has its entity and material
ecs_system(gr_look_model_parts_system, EcsIn(EcsSubMeshChildren), EcsIn(EcsModelRef), EcsOptional(GrPedestal), EcsNot(GrLookModel),
           EcsResIn(AssetSystem), EcsResIn(GrDroneRules), EcsResInOut(RendererState)) {
  const AssetSystem *a = ecs_sys_get_resource(AssetSystem);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  for (i32 i = 0; i < it->count; i++) {
    EcsEntity root = it->entities[i];
    const GrDrone *d = ecs_component_get(it->world, root, GrDrone);
    const GrPedestal *ped = ecs_sys_get_component_optional(GrPedestal, i);
    i32 pedestal = ped ? (i32)ped->index : -1;
    if (!d && pedestal < 0) {
      ecs_component_set(it->world, root, GrLookModel, {0});
      continue;
    }
    const EcsSubMeshChildren *kids = ecs_sys_get_component(EcsSubMeshChildren, i);
    ModelBlobAsset *model = asset_get(
        a, HANDLE_CAST(HzHandleT(ModelBlobAsset), ecs_sys_get_component(EcsModelRef, i)->model.handle), ModelBlobAsset);
    if (!model || kids->by_slot.len == 0)
      continue;
    b32 ready = true;
    for (u32 s = 0; s < kids->by_slot.len && ready; s++) {
      BlobAssetRef *ref = gr_look_slot_material(model, s);
      ready = ref && kids->by_slot.items[s] != ECS_ENTITY_INVALID &&
              handle_is_valid(HANDLE_CAST(HzHandle, renderer_material_for_asset(
                                                       r, HANDLE_CAST(HzHandleT(MaterialBlobAsset), ref->handle))));
    }
    if (!ready)
      continue;
    for (u32 s = 0; s < kids->by_slot.len; s++) {
      BlobAssetRef *ref = gr_look_slot_material(model, s);
      CString path = string_blob_get(&ref->path);
      EcsEntity part = kids->by_slot.items[s];
      HzHandleT(RendererMaterial) base =
          renderer_material_for_asset(r, HANDLE_CAST(HzHandleT(MaterialBlobAsset), ref->handle));
      if (!d) {
        // every pedestal's glow takes the weapon pickup colour
        if (!cstr_contains(path, "Emit_Pedestal"))
          continue;
        HzHandleT(RendererMaterial) inst = renderer_create_material_instance(r, base);
        material_set_color4(r, inst, "emission_color", gr_look_srgb(GR_PICKUP_WEAPON_COLOR));
        gr_look_part_bind(it->world, part, inst, (GrLookPart){.role = GR_LOOK_ROLE_PEDESTAL, .index = (u32)pedestal});
        continue;
      }
      u32 role = gr_look_drone_role(path);
      HzHandleT(RendererMaterial) inst = renderer_create_material_instance(r, base);
      // the drone type's colour on the glows (pupil white), white on the hull
      ColorF32 energy = rules->drones.types.items[d->type].color;
      ColorF32 emissive = role == GR_LOOK_ROLE_HULL || role == GR_LOOK_ROLE_PUPIL ? (ColorF32){1.0f, 1.0f, 1.0f, 1.0f} : energy;
      material_set_color4(r, inst, "emission_color", emissive);
      material_set_float(r, inst, "emission_intensity", 0.0f);
      if (role != GR_LOOK_ROLE_HULL)
        material_set_color4(r, inst, "color", (ColorF32){0.0f, 0.0f, 0.0f, 1.0f});
      gr_look_part_bind(it->world, part, inst, (GrLookPart){.owner = root, .role = role});
      // glows are small or inside the armour, so they skip the shadow pass
      if (role != GR_LOOK_ROLE_HULL)
        ecs_component_set(it->world, part, EcsVisibility, {.visible = true, .casts_shadow = false});
    }
    ecs_component_set(it->world, root, GrLookModel, {0});
  }
}

// a drone's hit flash, charge and thrust
hz_internal f32 gr_look_drone_intensity(u32 role, const GrDrone *d) {
  switch (role) {
  case GR_LOOK_ROLE_HULL:
    return d->hit_flash * 2.2f;
  case GR_LOOK_ROLE_LENS:
    return 6.0f + d->charge * 22.0f;
  case GR_LOOK_ROLE_PUPIL:
    return 10.0f + d->charge * 30.0f;
  case GR_LOOK_ROLE_ENERGY:
    return 3.5f + d->charge * 5.0f;
  default:
    return 2.2f + d->thrust * 5.0f;
  }
}

// per part: the emissive its owner's state asks for this frame, written on change
ecs_system(gr_look_part_system, EcsInOut(GrLookPart), EcsResIn(GrWeaponState), EcsResIn(GrPickupState)) {
  const GrWeaponState *ws = ecs_sys_get_resource(GrWeaponState);
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  for (i32 i = 0; i < it->count; i++) {
    const GrLookPart *p = ecs_sys_get_component(GrLookPart, i);
    f32 v = p->intensity;
    v3 color = p->color;
    switch (p->role) {
    case GR_LOOK_ROLE_NONE:
    case GR_LOOK_ROLE_SCREEN:
    case GR_LOOK_ROLE_SIGN:
      continue;
    case GR_LOOK_ROLE_VENT:
      v = ws->look[p->index].vent_intensity;
      color = ws->look[p->index].vent_color;
      break;
    case GR_LOOK_ROLE_COIL:
      v = ws->look[p->index].coil_intensity;
      break;
    case GR_LOOK_ROLE_CELL:
      v = ws->look[p->index].cell_intensity;
      break;
    case GR_LOOK_ROLE_GUN_LENS:
      v = ws->look[p->index].lens_intensity;
      break;
    case GR_LOOK_ROLE_PEDESTAL:
      v = ps->pedestal_glow[p->index];
      break;
    default: {
      const GrDrone *d = ecs_component_get(it->world, p->owner, GrDrone);
      if (!d)
        continue;
      v = gr_look_drone_intensity(p->role, d);
    } break;
    }
    if (v != p->intensity || !v3_eqv(color, p->color)) {
      GrLookPart *m = ecs_sys_get_component_mut(GrLookPart, i);
      m->intensity = v;
      m->color = color;
    }
  }
}

// the renderer sink for the parts: material writes on change, and the drawn surfaces bound once
ecs_system(gr_look_part_consolidate, EcsIn(GrLookPart), EcsIn(EcsMaterialInstance), EcsInOut(GrLookPartApplied),
           EcsResIn(GrLookSurfaces), EcsResInOut(RendererState)) {
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  const GrLookSurfaces *surf = ecs_sys_get_resource(GrLookSurfaces);
  f32 unit = gr_look_unit();
  for (i32 i = 0; i < it->count; i++) {
    const GrLookPart *p = ecs_sys_get_component(GrLookPart, i);
    const GrLookPartApplied *ap = ecs_sys_get_component(GrLookPartApplied, i);
    HzHandleT(RendererMaterial) inst = ecs_sys_get_component(EcsMaterialInstance, i)->instance;
    GpuTexture tex = INVALID_HANDLE_T(GpuTexture);
    if (p->role == GR_LOOK_ROLE_SCREEN && p->index < GR_LOOK_SCREEN_COUNT)
      tex = surf->screen_tex[p->index];
    else if (p->role == GR_LOOK_ROLE_SIGN && p->index < GR_LOOK_SIGN_COUNT)
      tex = surf->sign_tex[p->index];
    b32 bind = !ap->texture && gpu_texture_is_ready(tex);
    b32 intensity = p->role != GR_LOOK_ROLE_SCREEN && p->intensity != ap->intensity;
    b32 color = (p->role == GR_LOOK_ROLE_VENT || p->role == GR_LOOK_ROLE_SIGN) && !v3_eqv(p->color, ap->color);
    if (!bind && !intensity && !color)
      continue;
    GrLookPartApplied *m = ecs_sys_get_component_mut(GrLookPartApplied, i);
    if (bind) {
      material_set_texture(r, inst, "emission_map", tex);
      m->texture = true;
    }
    if (intensity) {
      material_set_float(r, inst, "emission_intensity", p->intensity * unit);
      m->intensity = p->intensity;
    }
    if (color) {
      // colours lerp in linear; GrSignSite.emissive is linear too
      material_set_vec4(r, inst, "emission_color", (vec4){p->color.x, p->color.y, p->color.z, 1.0f});
      m->color = p->color;
    }
  }
}

// one writer of GrFeedback: the player's hits and heals this frame, decayed
ecs_system(gr_look_feedback_system, EcsResIn(GrPlayerState), EcsResIn(GrClock), EcsResIn(GrFlow), EcsResIn(GrWeaponState),
           EcsResInOut(GrFeedback)) {
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  u32 state = ecs_sys_get_resource(GrFlow)->state;
  f32 dt = ecs_sys_get_resource(GrClock)->dt;
  GrFeedback *f = ecs_sys_get_resource_mut(GrFeedback);
  f32 damage = f->damage;
  for (u32 i = 0; i < p->events.hit_count; i++)
    damage = m_minf(1.0f, damage + 0.3f + p->events.hits[i].amount * 0.03f);
  f->damage = m_maxf(0.0f, damage - dt * 2.2f);
  f32 heal = p->events.healed > 0.0f ? 1.0f : f->heal;
  f->heal = m_maxf(0.0f, heal - dt * 1.6f);
  b32 in_run = state == GR_STATE_PLAYING || state == GR_STATE_ENDING || state == GR_STATE_PAUSED;
  b32 live = in_run && p->alive;
  f32 health = p->health / GR_PLAYER_MAX_HEALTH;
  f->low_health = live ? m_maxf(0.0f, (0.35f - health) / 0.35f) : 0.0f;
  // the scope amount of the gun in hand (a picked-up Lancer too)
  f->scope = live ? ecs_sys_get_resource(GrWeaponState)->scope : 0.0f;
}

// one row (the camera): the hub floor in view on a mirroring quality level; the view position the fog follows
ecs_system(gr_look_view_system, EcsIn(Camera), EcsResIn(GrLevel), EcsResInOut(GrLook)) {
  GrLook *l = ecs_sys_get_resource_mut(GrLook);
  const GrLevel *lv = ecs_sys_get_resource(GrLevel);
  for (i32 i = 0; i < it->count; i++) {
    const Camera *cam = ecs_sys_get_component(Camera, i);
    v4 planes[6];
    frustum_planes(cam->view_proj, planes);
    aabb hub = lv->areas[GR_AREA_HUB].box;
    aabb floor = {{hub.min.x, GR_PLANAR_HEIGHT - 0.1f, hub.min.z}, {hub.max.x, GR_PLANAR_HEIGHT + 0.1f, hub.max.z}};
    l->planar_active = lv->tables_ready && l->mirror->enabled && !l->no_planar && cam->pos.y > GR_PLANAR_HEIGHT &&
                       aabb_frustum(floor, planes);
    l->view_pos = cam->pos;
  }
}

// consolidation: the frame's look inputs into the _GrLook block every gr_ shader reads
ecs_system(gr_look_consolidate, EcsResIn(GrClock), EcsResIn(GrLevel), EcsResIn(GrFeedback), EcsResIn(GrLook), EcsResIn(KmStorm),
           EcsResInOut(RendererState)) {
  const GrLook *l = ecs_sys_get_resource(GrLook);
  RendererState *r = ecs_sys_get_resource_mut(RendererState);
  ShaderGrLookInput block =
      gr_look_block(l, ecs_sys_get_resource(GrClock), ecs_sys_get_resource(GrLevel), ecs_sys_get_resource(GrFeedback),
                    ecs_sys_get_resource(KmStorm)->flash);
  renderer_set_uniforms(r, l->feature, "_GrLook", &block, sizeof(block));
}

// ---- init, schedule, sync ----

// the upscaler tier: DLSS/MetalFX where the vendor runtime loads, TAAU otherwise
#define GR_UPSCALE_QUALITY RENDERER_UPSCALE_PERFORMANCE

void gr_look_renderer_desc(RendererInitDesc *desc) {
  // TAA instead of MSAA: one sample, the gbuffer layout's temporal resolve
  desc->sample_count = 1;
  desc->max_lights = 256; // Tipoca's floodlights, blaster flashes, fires
  desc->gbuffer_enabled = true;
  desc->upscale = (RendererUpscaleDesc){.enabled = true, .quality = GR_UPSCALE_QUALITY, .prefer_hardware = true};
  desc->depth_prepass = true;
  desc->gtao_enabled = true;
  // half rate, 3 slices x 6 steps, radius 1.1, power 1.6
  desc->gtao = (GtaoInitDesc){.radius = GR_GTAO_RADIUS,
                              .final_power = GR_GTAO_POWER,
                              .slice_count = 3,
                              .steps_per_slice = 6,
                              .resolution = GTAO_RESOLUTION_HALF,
                              // the half-res AO is upsampled in the material, no render-res pass
                              .upsample = GTAO_UPSAMPLE_INLINE};
  desc->shadows_enabled = true;
  desc->shadow.cascade_size = 2048;
  desc->shadow.max_distance = 150.0f;
  desc->shadow.shadow_filter_radius_texels = 2.5f; // the sun's PCF radius
  desc->bloom_enabled = true;
  desc->height_fog_enabled = true;
  desc->height_fog = height_fog_default_params();
  // one 256 cube per area
  desc->reflection_probes_enabled = true;
  desc->reflection_probes = (RendererReflectionProbesInitDesc){.reflection_size = GR_PROBE_FACE, .reflection_count = GR_AREA_COUNT};
}

hz_internal HeightFogParams gr_look_fog_params(const GrLookConfig *k, f32 density) {
  HeightFogParams p = height_fog_default_params();
  p.density = density;
  p.base_height = 0.0f;
  p.height_falloff = k->fog_falloff;
  // the shafts' sun x shaft intensity: the albedo is sRGB, so its encoding of the linear 3
  f32 a = 1.055f * m_powf(k->fog_shaft_intensity, 1.0f / 2.4f) - 0.055f;
  p.albedo = (ColorF32){a, a, a, 1.0f};
  p.anisotropy = k->fog_anisotropy;
  p.max_distance = k->fog_max_distance;
  p.volume_distance = k->fog_max_distance;
  return p;
}

// the medium's density follows the area the camera is in (doorways blend)
hz_internal f32 gr_look_fog_density(const GrLookConfig *k, const GrLevel *lv, v3 view_pos) {
  f32 area = 0.0f, sum = 0.0f;
  for (u32 i = 0; i < GR_AREA_COUNT; i++) {
    f32 w = gr_area_weight(lv->areas[i].box, view_pos);
    area += w * lv->areas[i].fog_density;
    sum += w;
  }
  return k->fog_density * (area / sum) * GR_FOG_WISP_MEAN;
}

hz_internal EcsEntity gr_look_spawn_sky(EcsWorld *w, AssetSystem *assets) {
  HzHandleT(ModelBlobAsset) model = asset_load(ModelBlobAsset, assets, .path = GR_SKY_MODEL);
  HzHandleT(MaterialBlobAsset) mat = asset_load(MaterialBlobAsset, assets, .path = GR_SKY_MAT);
  EcsEntity e = ecs_renderer_spawn(w, NULL,
                                   &(EcsRendererSpawnDesc){
                                       .model_asset = blob_asset_ref_from_handle(ModelBlobAsset, model),
                                       .transform = {.position = {0.0f, 0.0f, 0.0f},
                                                     .rotation = quat_identity(),
                                                     .scale = {GR_SKY_SCALE, GR_SKY_SCALE, GR_SKY_SCALE}},
                                       .casts_shadow = false,
                                       .name = "Sky",
                                   });
  ecs_component_set(w, e, EcsMaterial, {.material = blob_asset_ref_from_handle(MaterialBlobAsset, mat)});
  ecs_component_set(w, e, EcsRenderLists, {.mask = 1u << GR_LIST_MIRROR});
  return e;
}

// one probe per area at its capture point, box-projected against the area box; the influence spans the box +- AREA_FADE
hz_internal void gr_look_spawn_probes(EcsWorld *w, GrLook *l) {
  const GrLevel *lv = ecs_resource_get(w, GrLevel);
  for (u32 i = 0; i < GR_AREA_COUNT; i++) {
    aabb box = lv->areas[i].box;
    v3 centre = v3_scale(v3_add(box.min, box.max), 0.5f);
    v3 size = v3_sub(box.max, box.min);
    EcsReflectionProbe p = ecs_reflection_probe_default();
    p.size = v3_add(size, v3_fill(2.0f * GR_AREA_FADE));
    p.projection_size = size;
    p.origin_offset = v3_sub(lv->areas[i].probe, centre);
    p.blend_distance = 2.0f * GR_AREA_FADE;
    p.max_distance = GR_PROBE_FAR;
    p.box_projection = true;
    p.enable_shadows = true; // each capture fits the sun shadow to its area
    p.intensity = 1.0f;      // the surfaces scale the environment by the look's environment intensity
    p.cull_mask = 1u << GR_LIST_PROBE;
    EcsEntity e = ecs_entity_create(w);
    ecs_component_set(w, e, EntityName, {.name = fixedstr64_from_cstr("AreaProbe")});
    ecs_component_set(w, e, WorldTransform, {.position = centre, .rotation = quat_identity(), .scale = {1, 1, 1}});
    ecs_component_set(w, e, EcsReflectionProbe, p);
    l->probes[i] = e;
  }
}

void gr_look_init(EcsWorld *w, Allocator *a) {
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  renderer_set_exposure_ev100(r, GR_LOOK_EV100);
  // static level + moving camera: centred cascades rebuild only on anchor moves
  renderer_shadows_set_centred(r, 0.25f);
  GrLook *l = ALLOC(a, GrLook);
  l->final_applied = -1;
  l->quality_applied = -1;
  l->aspect = 1.0f;
  l->applied_fog_density = -1.0f;
  l->noise = gr_noise_texture_make();
  l->env_cube = gpu_make_storage_texture(&(GpuStorageTextureDesc){.kind = GPU_TEXTURE_KIND_CUBE,
                                                                   .format = GPU_TEXTURE_FORMAT_RGBA16F,
                                                                   .width = GR_ENV_FACE,
                                                                   .height = GR_ENV_FACE,
                                                                   .mip_count = 1});
  l->env_pipeline = gpu_make_compute_pipeline_async(shader_gr_sky_cube_compute_pipeline_desc());
  l->feature = renderer_add_feature(
      r, &(RendererFeatureDesc){
             .name = "gr_look",
             .user = l,
             .prepare = gr_look_prepare,
             .outputs = {{.name = "gr_noise", .kind = MAT_TEXTURE_2D, .neutral = TEX_DEFAULT_WHITE}}});
  assert_msg(l->feature.id != 0, "gr_look_init: the gr_look feature did not register");
  renderer_feature_require(r, l->feature, NULL, 0, &l->env_pipeline, 1);
  renderer_feature_publish(r, l->feature, "gr_noise", l->noise);
  l->mirror = gr_mirror_init(a, r, w);
  l->final_asset = asset_load(MaterialBlobAsset, assets, .path = GR_FINAL_SCOPE_MAT);
  l->sky = gr_look_spawn_sky(w, assets);
  l->post = ecs_entity_create(w);
  ecs_component_set(w, l->post, EntityName, {.name = fixedstr64_from_cstr("Post")});
  ecs_component_set(w, l->post, EcsPostProcessing, ecs_post_processing_from_params(renderer_post_processing(r)));
  gr_look_probe_capture(w, l->sky);
  l->applied_fog_enabled = true;
  l->switches = ecs_query_cached(w, EcsIn(GrDevSwitches));
  ecs_resource_set_ref(w, GrLook, l);
  ecs_resource_set(w, GrFeedback, ((GrFeedback){0}));
  GrLookSurfaces surfaces = {0};
  for (u32 i = 0; i < GR_LOOK_SCREEN_COUNT; i++)
    surfaces.screen_key[i] = ~0u;
  ecs_resource_set(w, GrLookSurfaces, surfaces);
  // the flow's "Baking light" stage waits for the env cube (gr_look_sync opens it)
  ecs_resource_get_mut(w, GrLoadGates)->light = false;
}

void gr_look_schedule(EcsWorld *w) {
  ecs_schedule(w, gr_look_lists_system);
  ecs_schedule_single(w, gr_look_config_system); // writes the GrLook resource
  ecs_schedule_single(w, gr_look_sun_system);    // writes the GrLook resource
  ecs_schedule(w, gr_look_post_system);
  ecs_schedule_single(w, gr_look_feedback_system); // writes the GrFeedback resource
  ecs_schedule_single(w, gr_look_view_system); // one row; writes the GrLook resource
  ecs_schedule_single(w, gr_look_consolidate);  // the renderer sink (_GrLook)
  ecs_schedule_single(w, gr_look_model_parts_system); // creates renderer material instances
  ecs_schedule_single(w, gr_look_gun_parts_system);   // creates renderer material instances
  ecs_schedule_single(w, gr_look_sign_parts_system);  // creates renderer material instances
  ecs_schedule(w, gr_look_part_system);
  ecs_schedule_single(w, gr_look_part_consolidate); // the renderer sink (part material writes)
}

// ---- UI surfaces: the ammo screens and the hub signs, drawn as canvases ----

// a canvas being drawn: canvas size in px and the next z (canvas draw order)
typedef struct GrPen {
  UISystem *ui;
  HzHandleT(UIFontFamily) font;
  f32 w, h;
  i16 z;
} GrPen;

hz_internal HzUIColor gr_pen_hex(u32 hex) {
  return (HzUIColor){(f32)((hex >> 16) & 255u), (f32)((hex >> 8) & 255u), (f32)(hex & 255u), 255.0f};
}

// filled rect
hz_internal void gr_pen_rect(GrPen *p, f32 x, f32 y, f32 w, f32 h, HzUIColor c) {
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(w), .height = ui_sizing_fixed(h)}},
              .background_color = c,
              .floating = {.attach_to = UI_ATTACH_TO_ROOT, .offset = {x, y}, .z_index = p->z++}}) {}
}

// a closed path's next vertex, in canvas px
hz_internal void gr_path_to(HzUIClipPath *path, f32 x, f32 y) {
  if (path->count < UI_CLIP_PATH_MAX_POINTS)
    path->points[path->count++] = (HzUIClipPoint){.offset = {x, y}};
}

// fill of the path: a canvas-sized box clipped to the polygon
hz_internal void gr_pen_fill(GrPen *p, const HzUIClipPath *path, HzUIColor c) {
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(p->w), .height = ui_sizing_fixed(p->h)}},
              .background_color = c,
              .clip_path = *path,
              .floating = {.attach_to = UI_ATTACH_TO_ROOT, .z_index = p->z++}}) {}
}

// text centred on (w / 2, cy), vertically middle; text wider than maxWidth shrinks to fit
hz_internal void gr_pen_text(GrPen *p, CString s, f32 cy, f32 size, u16 weight, HzUIColor c, f32 max_w) {
  u32 len = cstr_len(s);
  if (max_w > 0.0f) {
    HzHandleT(UIFontData) font = ui_family_resolve(p->ui, p->font, (UIFontStyleTag){.weight = weight});
    f32 tw = ui_measure_text_width_spaced(p->ui, font, s, len, size, 0.0f);
    if (tw > max_w)
      size *= max_w / tw;
  }
  f32 box = size * 2.0f;
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed(p->w), .height = ui_sizing_fixed(box)},
                         .child_alignment = {.x = UI_ALIGN_X_CENTER, .y = UI_ALIGN_Y_CENTER}},
              .floating = {.attach_to = UI_ATTACH_TO_ROOT, .offset = {0.0f, cy - box * 0.5f}, .z_index = p->z++}}) {
    ui_text(((HzUIString){.chars = s, .length = (i32)len}),
            ((HzUITextConfig){.font_family = p->font, .weight = weight, .fontSize = size, .textColor = c,
                              .wrapMode = UI_TEXT_WRAP_NONE}));
  }
}

// the canvas root: a full fill in `bg`
#define gr_pen_canvas(p, bg)                                                                                           \
  ui_element({.layout = {.sizing = {.width = ui_sizing_fixed((p)->w), .height = ui_sizing_fixed((p)->h)}},              \
              .background_color = (bg),                                                                                \
              .clip = {.horizontal = true, .vertical = true}})

// the ammo screen: the count and the magazine bar
hz_internal void gr_canvas_screen(GrPen *p, CString digits, f32 fill, HzUIColor ink) {
  gr_pen_canvas(p, gr_pen_hex(GR_SCREEN_BG)) {
    gr_pen_text(p, digits, 30.0f, 46.0f, 700, ink, 0.0f);
    gr_pen_rect(p, 10.0f, 56.0f, 108.0f * fill, 4.0f, ink);
  }
}

// a sector number sign
hz_internal void gr_canvas_sector(GrPen *p, CString label, CString sub) {
  f32 w = p->w, h = p->h;
  HzUIColor white = gr_pen_hex(0xffffffu), black = gr_pen_hex(0x000000u);
  gr_pen_canvas(p, black) {
    gr_pen_text(p, label, h * 0.42f, h * 0.62f, 700, white, 0.0f);
    // stencil breaks through the digits
    for (u32 i = 1; i < 4; i++)
      gr_pen_rect(p, 0.0f, h * (0.14f + (f32)i * 0.13f), w, h * 0.012f, black);
    gr_pen_rect(p, w * 0.08f, h * 0.8f, w * 0.84f, h * 0.006f, white);
    gr_pen_text(p, sub, h * 0.88f, h * 0.075f, 600, white, 0.0f);
  }
}

// a banner sign: side chevrons and the text between them
hz_internal void gr_canvas_banner(GrPen *p, CString text, u32 accent) {
  f32 w = p->w, h = p->h;
  HzUIColor ink = gr_pen_hex(accent);
  gr_pen_canvas(p, gr_pen_hex(0x000000u)) {
    for (u32 side = 0; side < 2; side++) {
      f32 x = side == 0 ? h * 0.25f : w - h * 0.25f;
      f32 dir = side == 0 ? 1.0f : -1.0f;
      HzUIClipPath chevron = {0};
      gr_path_to(&chevron, x, h * 0.2f);
      gr_path_to(&chevron, x + dir * h * 0.3f, h * 0.5f);
      gr_path_to(&chevron, x, h * 0.8f);
      gr_path_to(&chevron, x + dir * h * 0.14f, h * 0.8f);
      gr_path_to(&chevron, x + dir * h * 0.44f, h * 0.5f);
      gr_path_to(&chevron, x + dir * h * 0.14f, h * 0.2f);
      gr_pen_fill(p, &chevron, ink);
    }
    gr_pen_text(p, text, h * 0.54f, h * 0.5f, 600, ink, w - h * 1.6f);
  }
}

// a warning sign: hazard-striped top and bottom bands, title and body
hz_internal void gr_canvas_warning(GrPen *p, CString title, CString body) {
  f32 w = p->w, h = p->h, stripe = h * 0.12f;
  HzUIColor amber = gr_pen_hex(0xffae00u);
  gr_pen_canvas(p, gr_pen_hex(0x000000u)) {
    // each stripe runs corner to corner at 45 degrees: only its pieces inside the two bands show
    for (f32 x = -h; x < w + h; x += stripe * 2.0f) {
      HzUIClipPath top = {0}, bottom = {0};
      gr_path_to(&top, x, 0.0f);
      gr_path_to(&top, x + stripe, 0.0f);
      gr_path_to(&top, x + 2.0f * stripe, stripe);
      gr_path_to(&top, x + stripe, stripe);
      gr_path_to(&bottom, x + h - stripe, h - stripe);
      gr_path_to(&bottom, x + h, h - stripe);
      gr_path_to(&bottom, x + h + stripe, h);
      gr_path_to(&bottom, x + h, h);
      gr_pen_fill(p, &top, amber);
      gr_pen_fill(p, &bottom, amber);
    }
    gr_pen_text(p, title, h * 0.42f, h * 0.28f, 700, amber, w * 0.92f);
    gr_pen_text(p, body, h * 0.68f, h * 0.13f, 500, gr_pen_hex(0xffffffu), w * 0.92f);
  }
}

hz_internal void gr_canvas_sign(GrPen *p, u32 kind) {
  switch (kind) {
  case GR_SIGN_SECTOR:
    gr_canvas_sector(p, "07", "PROVING GROUNDS  \xC2\xB7  DRONE TRIALS");
    break;
  case GR_SIGN_BANNER_SOUTH:
    gr_canvas_banner(p, "PROVING GROUNDS  \xC2\xB7  SECTOR 7", 0xffffffu);
    break;
  case GR_SIGN_BANNER_WEST:
    gr_canvas_banner(p, "SIMULATION ACTIVE", 0xffb36bu);
    break;
  default:
    gr_canvas_warning(p, "CAUTION", "LIVE DRONE FIRE IN PROGRESS");
    break;
  }
}

hz_internal UISurface *gr_look_surface(UISystem *ui, RendererState *r, u32 w, u32 h) {
  UISurface *s = ui_surface_create(ui, r,
                                   &(UISurfaceDesc){.canvas_w = w,
                                                    .canvas_h = h,
                                                    .world_width = 1.0f,
                                                    .mode = UI_SURFACE_SURFACE,
                                                    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f}});
  // only its render target is used: the quad the surface owns stays hidden
  ui_surface_set_visible(s, false);
  return s;
}

void gr_look_surfaces(EcsWorld *w) {
  // Clay/UI: the surfaces draw through the UISystem before the frame's screen layout
  assert(is_main_thread());
  GrLookSurfaces *sf = ecs_resource_get_mut(w, GrLookSurfaces);
  const GrUi *gu = ecs_resource_get(w, GrUi);
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  if (!gu || !gu->ui)
    return;
  UISystem *ui = gu->ui;
  // the canvases draw in the HUD face: wait for every weight they use
  b32 ready = true;
  for (u16 weight = 500; weight <= 700; weight += 100) {
    ready &= ui_family_fonts_ready(ui, gu->hud, (UIFontStyleTag){.weight = weight});
    ready &= ui_font_is_ready(ui, ui_family_resolve(ui, gu->hud, (UIFontStyleTag){.weight = weight}));
  }
  // a font's glyph runtime asks for glyphs only after one collective ui_font_update has seen it
  sf->font_frames = ready ? sf->font_frames + 1 : 0;
  if (sf->font_frames < 2)
    return;
  if (!sf->screens[0]) {
    for (u32 g = 0; g < GR_LOOK_SCREEN_COUNT; g++)
      sf->screens[g] = gr_look_surface(ui, r, GR_SCREEN_W, GR_SCREEN_H);
  }
  // a runtime font's first paint only requests its glyphs
  u32 epoch = ui_glyph_epoch(ui);
  b32 repaint = epoch != sf->glyph_epoch;
  sf->glyph_epoch = epoch;
  const GrLevel *level = ecs_resource_get(w, GrLevel);
  if ((!sf->signs_drawn || repaint) && level->tables_ready) {
    for (u32 k = 0; k < GR_LOOK_SIGN_COUNT; k++) {
      const GrSignSite *site = &level->signs[k];
      u32 cw = (u32)m_roundf(site->width * site->density), ch = (u32)m_roundf(site->height * site->density);
      // Tipoca places no signs: an unsized site has no surface
      if (cw == 0 || ch == 0)
        continue;
      if (!sf->signs[k])
        sf->signs[k] = gr_look_surface(ui, r, cw, ch);
      GrPen pen = {.ui = ui, .font = gu->hud, .w = (f32)cw, .h = (f32)ch};
      ui_surface_scope(ui, sf->signs[k]) { gr_canvas_sign(&pen, k); }
      sf->sign_tex[k] = gpu_get_render_target_attachment(ui_surface_render_target(sf->signs[k]), 0);
    }
    sf->signs_drawn = true;
  }
  const GrWeaponState *ws = ecs_resource_get(w, GrWeaponState);
  const GrWeaponConfig *cfg = &ecs_resource_get(w, GrWeaponRules)->weapons;
  for (u32 g = 0; g < GR_LOOK_SCREEN_COUNT; g++) {
    const GrGunLook *gl = &ws->look[g];
    u32 max = gl->screen_max > 0 ? gl->screen_max : cfg->guns[g].magazine;
    u32 ammo = gl->screen_max > 0 ? gl->screen_ammo : max;
    b32 low = (f32)ammo <= (f32)max * 0.25f;
    u32 key = (ammo & 0xffu) | ((max & 0xffu) << 8) | ((u32)low << 16);
    if (key == sf->screen_key[g] && !repaint)
      continue;
    sf->screen_key[g] = key;
    TempAllocator ta = tctx_temp_allocator_begin(NULL);
    // the count zero-padded to two digits; the string lives until the surface scope has rendered
    String digits = ammo < 10 ? str_format(&ta.allocator, "0%", fmt_u32(ammo)) : str_format(&ta.allocator, "%", fmt_u32(ammo));
    GrPen pen = {.ui = ui, .font = gu->hud, .w = (f32)GR_SCREEN_W, .h = (f32)GR_SCREEN_H};
    HzUIColor ink = gr_pen_hex(low ? cfg->guns[g].low_color : cfg->guns[g].glow);
    ui_surface_scope(ui, sf->screens[g]) { gr_canvas_screen(&pen, digits.value, max > 0 ? (f32)ammo / (f32)max : 0.0f, ink); }
    tctx_temp_allocator_end(ta);
    sf->screen_tex[g] = gpu_get_render_target_attachment(ui_surface_render_target(sf->screens[g]), 0);
  }
}

void gr_look_sync(EcsWorld *w) {
  // the final material, the render size and the fog are main-lane renderer calls outside the frame
  assert(is_main_thread());
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  GrLook *l = ecs_resource_get_mut(w, GrLook);
  const GrDev *d = ecs_resource_get(w, GrDev);
  // null until the scene's Settings entity has loaded
  const GrDevSwitches *sw = ecs_query_get_singleton(GrDevSwitches, l->switches);
  if (sw) {
    l->no_fog = sw->no_fog;
    l->no_bloom = sw->no_bloom;
    l->no_planar = sw->no_planar;
    l->nan = sw->nan;
  }

  if (!handle_is_valid(HANDLE_CAST(HzHandle, l->final_material)) && asset_is_ready(assets, l->final_asset)) {
    MaterialBlobAsset *blob = asset_get(assets, l->final_asset, MaterialBlobAsset);
    if (blob)
      l->final_material = renderer_create_material_from_asset(r, assets, blob);
  }
  // the scope final only while the scope shows; every other frame runs the engine's default final
  i32 final = ecs_resource_get(w, GrFeedback)->scope > 0.0f ? 1 : 0;
  if (final != l->final_applied && (!final || handle_is_valid(HANDLE_CAST(HzHandle, l->final_material)))) {
    renderer_set_final_material(r, final ? l->final_material : (HzHandleT(RendererMaterial)){0});
    l->final_applied = final;
  }
  u32 render_w = 0, render_h = 0;
  renderer_render_size(r, &render_w, &render_h);
  l->aspect = render_w > 0 && render_h > 0 ? (f32)render_w / (f32)render_h : 1.0f;

  // the settings level (High until the menu or the quality switch sets one) is the renderer's level
  // the zeroed GrDev reads as Low: applying it would resize the swapchain to dpr 1 and back during load
  if (d->resolved && d->quality != l->quality_applied) {
    i32 q = MIN(MAX(d->quality, 0), (i32)RENDERER_QUALITY_ULTRA);
    renderer_set_quality(r, (RendererQuality)q);
    // quality presets render scale / max dpr: low 0.75/1, medium 1/1, high 1/1.25, ultra 1/2
    renderer_set_render_scale(r, q == GR_QUALITY_LOW ? 0.75f : 1.0f);
    // the max dpr caps the whole frame, final pass and HUD included
    hz_set_max_dpr(q == GR_QUALITY_ULTRA ? 2.0f : q == GR_QUALITY_HIGH ? 1.25f : 1.0f);
    // the preset's level and render scale reset the tier: the upscaler's own tier applies last
    if (renderer_upscale_enabled(r))
      renderer_set_upscale_quality(r, GR_UPSCALE_QUALITY);
    // AO darkens indirect light only (direct_strength 0)
    RendererGtaoParams ao = renderer_get_gtao(r);
    ao.radius = GR_GTAO_RADIUS;
    ao.final_power = GR_GTAO_POWER;
    ao.intensity = 1.0f;
    ao.direct_strength = 0.0f;
    renderer_set_gtao(r, &ao);
    l->quality_applied = d->quality;
  }

  f32 density = l->cfg_ready ? gr_look_fog_density(&l->cfg, ecs_resource_get(w, GrLevel), l->view_pos) : 0.0f;
  if (l->cfg_ready && m_absf(density - l->applied_fog_density) > 1e-7f) {
    HeightFogParams p = gr_look_fog_params(&l->cfg, density);
    renderer_set_height_fog(r, &p);
    l->applied_fog_density = density;
  }
  if (l->applied_fog_enabled == l->no_fog) {
    renderer_height_fog_set_enabled(r, !l->no_fog);
    l->applied_fog_enabled = !l->no_fog;
  }

  // the gun and arms draw with the player's viewmodel fov (hip until the player's first update)
  f32 vm_fov = ecs_resource_get(w, GrPlayerState)->viewmodel_fov;
  vm_fov = vm_fov > 0.0f ? vm_fov : GR_PLAYER_VIEWMODEL_FOV;
  if (vm_fov != l->viewmodel_fov_applied) {
    renderer_set_projection_override(
        r, &(RendererProjectionOverrideDesc){.fov_degrees = vm_fov, .depth_range = GR_VIEWMODEL_DEPTH_RANGE});
    l->viewmodel_fov_applied = vm_fov;
  }

  l->mirror->active = l->planar_active;
  l->mirror->floor = ecs_resource_get(w, GrLevel)->areas[GR_AREA_HUB].box;
  l->mirror->distortion = l->cfg.planar_distortion;
  // the area probes once the level is gathered and every scene model is instanced: the first capture sees all of it
  if (!l->probes_spawned && ecs_resource_get(w, GrLevel)->meshes_ready) {
    gr_look_spawn_probes(w, l);
    l->probes_spawned = true;
  }
  // the probes' second bounce once every area probe is live (the first capture is owed at creation)
  if (l->probes_spawned && !l->probes_baked) {
    b32 live = true;
    for (u32 i = 0; i < GR_AREA_COUNT; i++)
      live &= ecs_component_get(w, l->probes[i], EcsReflectionProbeInstance) != NULL;
    if (live) {
      renderer_reflection_probes_bake(r, 1);
      l->probes_baked = true;
    }
  }
  // the "Baking light" load stage: the sky cube and both probe bounces
  if (l->env_baked && l->probes_baked && renderer_reflection_probes_pending(r) == 0)
    ecs_resource_get_mut(w, GrLoadGates)->light = true;
}
