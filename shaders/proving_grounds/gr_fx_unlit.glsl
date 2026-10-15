// Enemy bolt core: an unlit HDR colour, opaque.
#include "renderer.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
flat out uint v_material_offset;

void main() {
    VertexInstanceData v = unpack_instance_data();
    gl_Position = hz_instance_clip(v, v.model * vec4(v.pos, 1.0));
    v_material_offset = v.material_offset;
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_OUTPUT
#include "renderer_shaders.glsl"
#include "gr_fx_common.glsl"
flat in uint v_material_offset;

@material
struct GrUnlitMaterial {
    vec4 color; // linear HDR (bolt colour x 4 + 3)
    float unit;
};

void main() {
    GrUnlitMaterial m = GrUnlitMaterial_unpack(v_material_offset);
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb, m.unit), 1.0), HZ_VELOCITY_NONE);
}
@end
