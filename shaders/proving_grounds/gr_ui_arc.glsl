// Proving Grounds damage arc: a 190 px circle's 5 px top border, drop-shadow, rotated, masked to its middle.
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

layout(set = 0, binding = 1) uniform _ArcParams {
    vec4 arc;    // x box size (190), y top border width (5), z rotation (radians, clockwise), w quad half size; UI px
    vec4 color;  // the border colour: rgb sRGB 0..1, a alpha
    vec4 glow;   // the drop-shadow colour: rgb sRGB 0..1, a alpha
    vec4 effect; // x opacity (damage-fade times the HUD fade), y drop-shadow sigma
} arc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec2 p = (v_uv * 2.0 - 1.0) * arc.arc.w;
    float fw = max(fwidth(p.x), 1e-4);
    // the rotation turns the box clockwise: back into the box's own frame, origin at its centre
    float cs = cos(arc.arc.z), sn = sin(arc.arc.z);
    vec2 q = vec2(cs * p.x + sn * p.y, -sn * p.x + cs * p.y);
    float half_box = 0.5 * arc.arc.x;
    vec2 box = q + half_box; // 0..size, y down

    // outer: the circle; inner: the border's inner top corners, radii (r, r - border) from y = border
    float d_outer = length(q) - half_box;
    float ry = half_box - arc.arc.y;
    vec2 e = q / vec2(half_box, ry);
    float k = length(e);
    vec2 grad = q / vec2(half_box * half_box, ry * ry) / max(k, 1e-4);
    float d_inner = q.y < 0.0 ? (k - 1.0) / max(length(grad), 1e-4) : d_outer;
    float cover = hz_aa(-d_outer, fw) * hz_aa(d_inner, fw);

    // drop-shadow of the crescent, blurred across its local thickness
    float thickness = max(d_inner - d_outer, 0.0);
    float shadow = gr_band_blur(-thickness - d_outer, -d_outer, arc.effect.y) * arc.color.a * arc.glow.a;

    vec4 c = hz_over(vec4(0.0), arc.glow.rgb, shadow);
    c = hz_over(c, arc.color.rgb, cover * arc.color.a);

    // mask: a horizontal gradient, transparent at 25 %, opaque 45-55 %, transparent at 75 %, clipped to the border box
    float x = box.x / arc.arc.x;
    float mask = clamp((x - 0.25) / 0.2, 0.0, 1.0) * clamp((0.75 - x) / 0.2, 0.0, 1.0);
    float inside = step(0.0, box.x) * step(box.x, arc.arc.x) * step(0.0, box.y) * step(box.y, arc.arc.x);
    c.a *= mask * inside * arc.effect.x;
    frag_color = gr_ui_out(c);
}
@end
