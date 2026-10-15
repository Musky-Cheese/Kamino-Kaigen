// Pickup beacon floor ring: ring + halo, pulsing, additive.
#include "renderer.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;
flat out uint v_material_offset;

void main() {
    VertexInstanceData v = unpack_instance_data();
    gl_Position = hz_instance_clip(v, v.model * vec4(v.pos, 1.0));
    v_uv = v.uv;
    v_material_offset = v.material_offset;
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_TRANSPARENT
#define HZ_RENDERER_OUTPUT
#include "renderer_shaders.glsl"
#include "gr_fx_common.glsl"
in vec2 v_uv;
flat in uint v_material_offset;

@material
struct GrRingMaterial {
    vec4 color; // linear (the pickup colour)
    float unit;
};

void main() {
    GrRingMaterial m = GrRingMaterial_unpack(v_material_offset);
    float r = length(v_uv - 0.5) * 2.0;
    float ring = smoothstep(0.62, 0.7, r) * (1.0 - smoothstep(0.74, 0.8, r));
    float halo = pow(max(1.0 - r, 0.0), 2.0) * 0.35;
    float pulse = 0.75 + 0.25 * sin(hz_scene.time * 4.0);
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb * (ring * 3.0 * pulse + halo), m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
