// The level's self-lit prop shaders: keyword GR_ENERGY_COLUMN (the core's flowing column),
// GR_PAD_DISC (the jump pad's outward pulses), GR_LIGHT_COLUMN (the additive column over a pad).
#include "renderer.glsl"
#include "gr_look.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#ifdef GR_LIGHT_COLUMN
#define HZ_NO_DEPTH_PREPASS
#endif
#include "renderer_shaders.glsl"
out vec2 v_uv;
out vec3 v_normal;
out vec3 v_world_pos;
out float v_view_depth;
flat out uint v_material_offset;

void main() {
    VertexInstanceData v = unpack_instance_data();
    vec4 world_pos = v.model * vec4(v.pos, 1.0);
    gl_Position = hz_instance_clip(v, world_pos);
    v_uv = v.uv;
    v_normal = normalize((v.model * vec4(v.normal, 0.0)).xyz);
    v_world_pos = world_pos.xyz;
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
#include "gr_common.glsl"
in vec2 v_uv;
in vec3 v_normal;
in vec3 v_world_pos;
in float v_view_depth;
flat in uint v_material_offset;

@material
struct GrPropMaterial {
    vec4 color;  // linear
    vec4 params; // x intensity, y 1 = near fade (light column), z, w unused
};

void main() {
    GrPropMaterial m = GrPropMaterial_unpack(v_material_offset);
    float t = gr_look.time.x;
    vec3 n = normalize(v_normal);
    vec3 to_eye = hz_scene.camera_pos - v_world_pos;
    float facing = abs(dot(n, normalize(to_eye)));
    HzSurface s = hz_surface_default();
    s.normal = n;
    s.geo_normal = n;
#ifdef GR_ENERGY_COLUMN
    float rim = pow(max(1.0 - facing, 0.0), 3.0);
    float h = v_world_pos.y;
    float flow = hz_fbm(vec2(v_uv.x * 10.0, h * 0.8 - t * 1.6), 4) * 0.9375;
    float flow2 = hz_fbm(vec2(v_uv.x * 23.0 + 4.0, h * 2.1 - t * 3.1), 4) * 0.9375;
    float bands = pow(max(0.5 + 0.5 * sin(h * 4.0 - t * 6.0), 0.0), 14.0);
    float intensity = 0.55 + flow * 1.3 + flow2 * flow2 * 1.6 + bands * 2.2;
    vec3 col = m.color.rgb * intensity * (0.55 + 0.45 * facing) + vec3(0.6, 0.95, 1.0) * rim * 2.5;
    vec3 lit = gr_look_scene(col);
#ifdef HZ_RENDERER_FOG
    lit = GR_FOG_APPLY(lit, v_view_depth);
#endif
    hz_output_lit(vec4(lit, 1.0), s, HZ_VELOCITY_NONE);
#endif
#ifdef GR_PAD_DISC
    vec2 p = v_uv - 0.5;
    float r = length(p) * 2.0;
    float a = atan(p.y, p.x);
    float pulses = pow(max(0.5 + 0.5 * sin(r * 16.0 - t * 7.0), 0.0), 8.0) * (1.0 - smoothstep(0.25, 1.0, r));
    float rim = smoothstep(0.84, 0.88, r) * (1.0 - smoothstep(0.95, 0.99, r));
    float ticks = step(0.7, fract(a / 6.2831 * 24.0)) * smoothstep(0.72, 0.76, r) * (1.0 - smoothstep(0.8, 0.82, r));
    float core = 1.0 - smoothstep(0.0, 0.22, r);
    vec3 col = vec3(0.02, 0.025, 0.03) + m.color.rgb * (rim * 7.0 + pulses * 2.5 + ticks * 3.0 + core * 3.0);
    vec3 lit = gr_look_scene(col);
#ifdef HZ_RENDERER_FOG
    lit = GR_FOG_APPLY(lit, v_view_depth);
#endif
    hz_output_lit(vec4(lit, 1.0), s, HZ_VELOCITY_NONE);
#endif
#ifdef GR_LIGHT_COLUMN
    // the cylinder's uv.y is 1 at the top: bright at the pad, fading upward
    float rim = pow(clamp(1.0 - facing, 0.0, 1.0), 1.5);
    float fade = pow(clamp(1.0 - v_uv.y, 0.0, 1.0), 1.8);
    float streaks = 0.55 + 0.45 * hz_value_noise(vec2(v_uv.x * 18.0, v_uv.y * 2.5 - t * 1.8));
    float a = fade * streaks * (0.12 + 0.88 * rim) * 0.55;
    a *= mix(1.0, smoothstep(0.8, 2.5, length(to_eye)), m.params.y);
    hz_output_lit(vec4(gr_look_scene(m.color.rgb * a * m.params.x), 1.0), s, HZ_VELOCITY_NONE);
#endif
}
@end
