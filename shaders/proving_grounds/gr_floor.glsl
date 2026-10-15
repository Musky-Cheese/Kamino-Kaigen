// The hub floor: gr_surface with the planar mirror in place of the emission and material maps (the floor is procedural).
// Keywords as gr_surface; without the maps the stage stays within its 16 textures with the probes.
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
// the mirror takes the IBL split-sum weight: variants without HZ_RENDERER_IBL (the codegen default) have none
#ifdef HZ_RENDERER_IBL
#define GR_PLANAR
#define HZ_RENDERER_REFLECTION_PROBES // the area probes past the mirror
#endif
#include "renderer_shaders.glsl"
#include "gr_look.glsl"
#include "gr_common.glsl"
#include "hz_planar_reflection.glsl"
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

@material
struct GrSurfaceMaterial {
    vec4 color;
    float metallic;
    float smoothness;
    vec4 emission_color;
    float emission_intensity;
    vec4 panel;   // x, y tile size (m), z groove width, w grime
    vec4 detail;  // x per-tile variation
    @color vec4 secondary_color; // the inlay lines
    @texture2D @white
    uint gr_noise;          // the look's noise texture
    @texture2D @black
    uint gr_mirror;         // the planar mirror, published while the hub floor is in view
};

#include "gr_surface_fs.glsl"
@end
