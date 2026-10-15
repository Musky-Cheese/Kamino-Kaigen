// The surface material for every lit object but the hub floor (gr_floor.glsl). Six material textures: the WebGPU stage limit is 16.
// Keywords: GR_DETAIL_PANELS / _FLOOR / _HAZARD / _GRAIN (one or none), the engine's HZ_RENDERER_*; the strip pulse runs when detail.y != 0.
#include "renderer.glsl"
#include "gr_look.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
invariant gl_Position;
out vec2 v_uv;
out vec4 v_color;
flat out uint v_material_offset;
#ifdef HZ_RENDERER_DECALS
flat out uint v_decal_mask;
#endif
out vec3 v_normal;
out vec3 v_world_pos;
out vec3 v_tangent;
out float v_bitangent_sign;
out float v_view_depth;
#ifdef HZ_RENDERER_GBUFFER
out vec4 v_clip_cur;
out vec4 v_clip_prev;
#endif
#include "gr_surface_vs.glsl"
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_OUTPUT
#define HZ_RENDERER_ANISO
#define GR_MATERIAL_MAPS
#define GR_EMISSION_MAP
// the environment is the area probes, applied to every lit surface
#ifdef HZ_RENDERER_IBL
#define HZ_RENDERER_REFLECTION_PROBES
#endif
#include "renderer_shaders.glsl"
#include "gr_look.glsl"
#include "gr_common.glsl"
in vec2 v_uv;
in vec4 v_color;
flat in uint v_material_offset;
#ifdef HZ_RENDERER_DECALS
flat in uint v_decal_mask;
#endif
in vec3 v_normal;
in vec3 v_world_pos;
in vec3 v_tangent;
in float v_bitangent_sign;
in float v_view_depth;
#ifdef HZ_RENDERER_GBUFFER
in vec4 v_clip_cur;
in vec4 v_clip_prev;
#endif

// names match the importer's PBR fill (color, metallic, smoothness, emission_*, albedo, normal_map, orm_map)
@material
struct GrSurfaceMaterial {
    vec4 color;               // linear base colour
    float metallic;
    float smoothness;         // 1 - roughness
    vec4 emission_color;      // linear
    float emission_intensity; // cd/m2 per unit colour (emissive intensity x the look's unit)
    vec4 panel;               // x, y panel/tile size (m), z groove width, w grime
    vec4 detail;              // x per-panel variation, y strip pulse speed, z pulse frequency, w emission map v flip
    @color vec4 secondary_color; // hazard stripes and floor inlay lines
    @texture2D @white
    uint albedo;
    @texture2D @normal
    uint normal_map;
    @texture2D @white
    uint orm_map;             // glTF: r occlusion, g roughness, b metallic (white = the factors as they are)
    @texture2D @white
    uint emission_map;        // a sign's or ammo screen's UI surface
    @texture2D @white
    uint gr_noise;            // published by the look: r fBm 4, g fBm 16, b value 64, a fBm 8
};

#include "gr_surface_fs.glsl"
@end
