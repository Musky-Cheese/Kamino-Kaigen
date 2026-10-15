// Enemy bolt glow: a camera-facing quad with a radial gradient, turning 4 rad/s, additive.
#include "renderer.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;
flat out uint v_material_offset;

// the quad's centre is the entity's position and its side the transform's x scale
void main() {
    VertexInstanceData v = unpack_instance_data();
    vec3 center = v.model[3].xyz;
    float side = length(v.model[0].xyz);
    vec3 right = vec3(hz_scene.view[0][0], hz_scene.view[1][0], hz_scene.view[2][0]);
    vec3 up = vec3(hz_scene.view[0][1], hz_scene.view[1][1], hz_scene.view[2][1]);
    float a = hz_scene.time * 4.0;
    float c = cos(a), s = sin(a);
    vec2 q = vec2(c * v.pos.x - s * v.pos.y, s * v.pos.x + c * v.pos.y) * side;
    gl_Position = hz_scene.view_proj * vec4(center + right * q.x + up * q.y, 1.0);
    v_uv = v.pos.xy + 0.5;
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
struct GrGlowMaterial {
    vec4 color; // linear HDR (bolt colour x 3.5)
    float unit;
};

void main() {
    GrGlowMaterial m = GrGlowMaterial_unpack(v_material_offset);
    // the radial gradient: alpha 1 -> 0.45 at 25 % -> 0 at the edge
    float r = length(v_uv - 0.5) * 2.0;
    float a = r < 0.25 ? mix(1.0, 0.45, r / 0.25) : mix(0.45, 0.0, clamp((r - 0.25) / 0.75, 0.0, 1.0));
    if (a <= 0.0) discard;
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb * a, m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
