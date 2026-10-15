// Kamino rain: one static mesh of streak quads wrapped in a box around the camera; under a roof (the sun's shadow) it stops.
#include "renderer.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;
out vec3 v_center;
out float v_alpha;
flat out uint v_material_offset;

#define KM_RAIN_BOX 36.0
#define KM_RAIN_FALL vec3(2.2, -14.0, 1.1)

// pos is the drop's rest position in [0, box)^3, uv its quad corner, aux.xyz its 24-bit seed
void main() {
    VertexInstanceData v = unpack_instance_data();
    vec3 b = round(v.aux.xyz * 255.0);
    float seed = (b.x + b.y * 256.0 + b.z * 65536.0) / 16777216.0;
    vec3 fall = KM_RAIN_FALL * (0.85 + 0.3 * fract(seed * 7.31));
    vec3 cam = hz_scene.camera_pos;
    vec3 half_box = vec3(KM_RAIN_BOX * 0.5);
    vec3 p = cam + mod(v.pos + fall * hz_scene.time - cam + half_box, vec3(KM_RAIN_BOX)) - half_box;
    float depth = max(-(hz_scene.view * vec4(p, 1.0)).z, 1e-3);
    float fade = smoothstep(0.6, 2.0, depth) * (1.0 - smoothstep(KM_RAIN_BOX * 0.3, KM_RAIN_BOX * 0.5, depth));
    // a streak along the fall, as wide as a pixel at least, facing the camera about its own axis
    vec3 axis = normalize(fall);
    vec3 to_cam = normalize(cam - p);
    vec3 side = normalize(cross(axis, to_cam));
    vec3 up = vec3(hz_scene.view[0][1], hz_scene.view[1][1], hz_scene.view[2][1]);
    float p11 = abs((hz_scene.view_proj * vec4(up, 0.0)).y);
    float one_px = 2.0 * depth / (p11 * float(hz_scene.render_height));
    float width = max(0.006, one_px * 0.8);
    float len = 0.55 + 0.35 * fract(seed * 3.7);
    vec2 c = v.uv - 0.5;
    gl_Position = hz_scene.view_proj * vec4(p + side * c.x * width + axis * c.y * len, 1.0);
    v_uv = v.uv;
    v_center = p;
    v_alpha = fade * (0.55 + 0.45 * fract(seed * 11.3));
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
struct KmRainMaterial {
    vec4 color; // linear HDR
    float unit;
};

void main() {
    KmRainMaterial m = KmRainMaterial_unpack(v_material_offset);
    // soft across the streak, tapered at both ends
    float across = 1.0 - abs(v_uv.x - 0.5) * 2.0;
    float along = 1.0 - abs(v_uv.y - 0.5) * 2.0;
    float k = across * smoothstep(0.0, 0.6, along) * v_alpha;
    // no rain under a roof: the sky's light does not reach there either
    k *= mix(0.08, 1.0, step(0.5, hz_shadow(v_center)));
    if (k <= 0.001) discard;
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb * k, m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
