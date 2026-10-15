// The guns' ammo screens: unlit, the counter canvas as the map at colour 2.
// The canvas is a UI surface bound to emission_map at runtime; until then the screen is black.
#include "renderer.glsl"
#include "gr_look.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;
out vec3 v_normal;
out float v_view_depth;
flat out uint v_material_offset;

void main() {
    VertexInstanceData v = unpack_instance_data();
#ifdef HZ_RENDERER_SKINNED
    apply_skinning(v.pos, v.normal, v.tangent, v.bone_offset, v.vertex_base, v.vertex_layout_flags);
#endif
    vec4 world_pos = v.model * vec4(v.pos, 1.0);
    gl_Position = hz_instance_clip(v, world_pos);
    v_uv = v.uv;
    v_normal = normalize((v.model * vec4(v.normal, 0.0)).xyz);
    v_view_depth = -(hz_scene.view * world_pos).z;
    v_material_offset = v.material_offset;
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_OUTPUT
#include "renderer_shaders.glsl"
#include "gr_look.glsl"
in vec2 v_uv;
in vec3 v_normal;
in float v_view_depth;
flat in uint v_material_offset;

// the importer's names: emission_intensity carries the cd/m2 unit, emission_map the screen's canvas
@material
struct GrScreenMaterial {
    vec4 color;
    float emission_intensity;
    @texture2D @black
    uint emission_map;
};

#define GR_SCREEN_GAIN 2.0 // the screen's colour gain (2, 2, 2)

void main() {
    GrScreenMaterial m = GrScreenMaterial_unpack(v_material_offset);
    vec3 c = texture_sample(emission_map, m.emission_map, v_uv).rgb * GR_SCREEN_GAIN;
    vec3 lit = c * m.emission_intensity * hz_scene.pre_exposure;
#ifdef HZ_RENDERER_FOG
    lit = GR_FOG_APPLY(lit, v_view_depth);
#endif
    HzSurface s = hz_surface_default();
    s.normal = normalize(v_normal);
    s.geo_normal = s.normal;
    hz_output_lit(vec4(lit, 1.0), s, HZ_VELOCITY_NONE);
}
@end
