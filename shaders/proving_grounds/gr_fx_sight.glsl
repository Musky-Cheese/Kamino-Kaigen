// VK-7 holo sight: the tinted glass and the red ring-and-dot reticle, additive.
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
struct GrSightMaterial {
    vec4 color; // the glass, linear (0.02, 0.06, 0.08)
    float unit;
};

void main() {
    GrSightMaterial m = GrSightMaterial_unpack(v_material_offset);
    // the reticle plane (0.03 m) centred on the glass (0.046 m), in its 64 px canvas
    vec2 rp = (v_uv - 0.5) * (0.046 / 0.03) * 64.0;
    float d = length(rp);
    float aa = max(fwidth(d), 1e-3);
    // the reticle: a 3 px ring at r 22, rgba(255,70,60,0.95); a 3.5 px dot, rgba(255,90,70,1); sRGB
    float ring = (1.0 - smoothstep(1.5 - aa, 1.5 + aa, abs(d - 22.0))) * 0.95;
    float dot_ = 1.0 - smoothstep(3.5 - aa, 3.5 + aa, d);
    vec3 reticle = vec3(1.0, 0.0612, 0.0452) * ring + vec3(1.0, 0.1022, 0.0612) * dot_;
    hz_output_unlit(vec4(gr_fx_to_scene(m.color.rgb + reticle, m.unit), 0.0), HZ_VELOCITY_NONE);
}
@end
