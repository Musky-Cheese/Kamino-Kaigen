// Dust motes: one static mesh of camera-facing quads, lit only where the sun's shadow map sees them.
#include "renderer.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;
out vec3 v_center;
out float v_alpha;
flat out uint v_material_offset;

// pos is the mote's rest position, uv its quad corner, aux.xyz its 24-bit seed
void main() {
    VertexInstanceData v = unpack_instance_data();
    vec3 b = round(v.aux.xyz * 255.0);
    float seed = (b.x + b.y * 256.0 + b.z * 65536.0) / 16777216.0;
    // slow Lissajous drift, unique per mote
    float t = hz_scene.time * (0.06 + 0.06 * fract(seed * 7.13));
    vec3 p = (v.model * vec4(v.pos, 1.0)).xyz +
             vec3(sin(t + seed * 6.28), sin(t * 0.7 + seed * 3.1) * 0.5, cos(t * 0.9 + seed * 4.2)) * 0.9;
    float depth = max(-(hz_scene.view * vec4(p, 1.0)).z, 1e-3);
    float fade = smoothstep(0.8, 2.5, depth) * (1.0 - smoothstep(18.0, 30.0, depth));
    float twinkle = 0.55 + 0.45 * sin(hz_scene.time * (1.5 + seed * 2.0) + seed * 40.0);
    vec3 right = vec3(hz_scene.view[0][0], hz_scene.view[1][0], hz_scene.view[2][0]);
    vec3 up = vec3(hz_scene.view[0][1], hz_scene.view[1][1], hz_scene.view[2][1]);
    // ~1.2 cm motes, at least one pixel
    float p11 = abs((hz_scene.view_proj * vec4(up, 0.0)).y);
    float one_px = 2.0 * depth / (p11 * float(hz_scene.render_height));
    float size = max(0.012 + 0.012 * fract(seed * 13.7), one_px);
    vec2 c = v.uv - 0.5;
    gl_Position = hz_scene.view_proj * vec4(p + (right * c.x + up * c.y) * size, 1.0);
    v_uv = v.uv;
    v_center = p;
    v_alpha = fade * twinkle;
    v_material_offset = v.material_offset;
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_SHADOWS // hz_shadow UBO + shadow sampler + hz_shadow()
#define HZ_RENDERER_TRANSPARENT
#define HZ_RENDERER_OUTPUT
#include "renderer_shaders.glsl"
#include "gr_fx_common.glsl"
in vec2 v_uv;
in vec3 v_center;
in float v_alpha;
flat in uint v_material_offset;

@material
struct GrDustMaterial {
    vec4 color; // linear HDR (1, 0.9, 0.75) x 3.2
    float unit;
};

void main() {
    GrDustMaterial m = GrDustMaterial_unpack(v_material_offset);
    float d = length(v_uv - 0.5) * 2.0;
    float a = clamp(1.0 - d, 0.0, 1.0);
    // the engine's sun shadow at the mote's centre
    float lit = step(0.5, hz_shadow(v_center));
    float k = a * a * v_alpha * lit;
    if (k <= 0.0) discard;
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb * k, m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
