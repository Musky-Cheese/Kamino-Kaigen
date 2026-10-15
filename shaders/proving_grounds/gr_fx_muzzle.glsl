// Muzzle flash: the camera-facing star and the two crossed flame tongues, additive HDR.
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
struct GrMuzzleMaterial {
    vec4 color;      // linear HDR (default (9, 5, 2))
    float intensity; // (timer / 0.06)^2 x 1.4
    float seed;
    uint flame;      // 0 star, 1 flame tongue (uv.y from the muzzle forward)
    float unit;
};

void main() {
    GrMuzzleMaterial m = GrMuzzleMaterial_unpack(v_material_offset);
    float shape;
    if (m.flame == 0u) {
        vec2 p = v_uv * 2.0 - 1.0;
        float r = length(p);
        float a = atan(p.y, p.x);
        float spikes = pow(abs(cos(a * 3.0 + m.seed * 6.2831)), 16.0) + 0.6 * pow(abs(cos(a * 5.0 - m.seed * 4.1)), 28.0);
        shape = exp(-r * r * 16.0) * 1.6 + spikes * smoothstep(1.0, 0.05, r) * 0.9;
    } else {
        float along = v_uv.y;
        float across = abs(v_uv.x - 0.5) * 2.0;
        float n = hz_hash12(vec2(floor(along * 6.0), m.seed * 17.0));
        float width = mix(0.9, 0.1, pow(along, 0.7)) * (0.75 + 0.35 * n);
        shape = smoothstep(width, width * 0.15, across) * (1.0 - smoothstep(0.45 + 0.4 * n, 1.0, along));
    }
    vec3 col = mix(m.color.rgb, vec3(max(m.color.r, m.color.g)), exp(-length(v_uv - 0.5) * 6.0) * 0.5);
    // additive blending weighs rgb by alpha: (col shape I) x (shape I)
    float k = shape * m.intensity;
    hz_output_unlit(vec4(gr_fx_to_scene(col * k * k, m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
