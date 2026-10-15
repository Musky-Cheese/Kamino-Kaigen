// Proving Grounds crosshair ring: a circular border, solid 1.5 px or dashed 2 px, under the crosshair's drop-shadow.
#include "common.glsl"
#include "ui/common_ui.glsl"

@vs
layout(set = 0, binding = 0) uniform _VSUniforms {
    vec2 resolution;
    vec2 _pad0;
    vec4 rect; // x, y, w, h in framebuffer px
} vs_uniforms;

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = ui_quad_uv(uint(gl_VertexIndex));
    v_uv = uv;
    gl_Position = ui_rect_to_clip(vs_uniforms.rect, uv, vs_uniforms.resolution);
}
@end

@fs
#include "gr_ui_common.glsl"

layout(set = 0, binding = 1) uniform _RingParams {
    vec4 ring;   // x outer radius (the crosshair gap), y border width, z dashed (1) or solid (0), w quad half size; UI px
    vec4 color;  // rgb sRGB 0..1, a the colour's alpha
    vec4 effect; // x opacity (the ring's .55 times every fade above it), y drop-shadow alpha, z drop-shadow sigma
} ring;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec2 p = (v_uv * 2.0 - 1.0) * ring.ring.w;
    float fw = max(fwidth(p.x), 1e-4);
    float w = ring.ring.y;
    float centre = ring.ring.x - 0.5 * w;
    float d = length(p) - centre;

    // dashed: 3w dashes, as many as fit the circumference evenly
    float along = 1.0;
    float along_blur = 1.0;
    if (ring.ring.z > 0.5) {
        float circumference = 6.28318531 * centre;
        float n = max(1.0, floor(circumference / (6.0 * w) + 0.5));
        float period = circumference / n;
        float s = mod((atan(p.y, p.x) + 3.14159265) / 6.28318531 * circumference, period) - 0.5 * period;
        float half_dash = 1.5 * w;
        along = hz_aa(half_dash - abs(s), fw);
        along_blur = gr_band_blur(-half_dash - s, half_dash - s, ring.effect.z);
    }

    float cover = hz_aa(0.5 * w - abs(d), fw) * along;
    float a = ring.color.a * ring.effect.x;
    // drop-shadow (0 0 3 px, black at 0.6) of the ring as painted
    float shadow = gr_band_blur(-0.5 * w - d, 0.5 * w - d, ring.effect.z) * along_blur * a * ring.effect.y;
    vec4 c = hz_over(vec4(0.0), vec3(0.0), shadow);
    c = hz_over(c, ring.color.rgb, cover * a);
    frag_color = gr_ui_out(c);
}
@end
