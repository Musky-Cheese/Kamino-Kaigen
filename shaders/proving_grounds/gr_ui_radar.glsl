// Proving Grounds motion tracker: disc, conic sweep, rings, cross, glowing contacts, rim, arrow, drop-shadow.
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

#define GR_RADAR_MAX 48

layout(set = 0, binding = 1) uniform _RadarParams {
    vec4 params;  // x canvas size (UI px), y sweep (radians), z contact count, w bleed around the canvas (UI px)
    vec4 effect;  // x opacity (the HUD fade), y drop-shadow sigma, z drop-shadow alpha, w contact glow sigma
    vec4 palette[5]; // GrDroneType colours, the pickup gun, the health pack: sRGB 0..1
    vec4 contacts[GR_RADAR_MAX]; // xy canvas px, z radius, w icon + 4 marker + 16 colour
} radar;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

float gr_box_blur(vec2 d, vec2 half_size, float sigma) {
    return gr_band_blur(-half_size.x - d.x, half_size.x - d.x, sigma) * gr_band_blur(-half_size.y - d.y, half_size.y - d.y, sigma);
}

// coverage and blurred coverage of one contact's icon around its centre
vec2 gr_icon(vec2 d, float r, int icon, float fw, float sigma) {
    if (icon == 1) {
        // diamond: vertices d = 1.25 r from the centre on each axis
        float k = r * 1.25;
        vec2 uv = vec2(d.x + d.y, d.x - d.y) * 0.70710678;
        float sd = (abs(d.x) + abs(d.y) - k) * 0.70710678;
        return vec2(hz_aa(-sd, fw), gr_box_blur(uv, vec2(k * 0.70710678), sigma));
    }
    if (icon == 2) {
        // cross: two rects (2a x 2b and 2b x 2a), nonzero union
        float a = r * 1.2, b = r * 0.42;
        float c1 = hz_aa(min(a - abs(d.x), b - abs(d.y)), fw);
        float c2 = hz_aa(min(b - abs(d.x), a - abs(d.y)), fw);
        float s1 = gr_box_blur(d, vec2(a, b), sigma);
        float s2 = gr_box_blur(d, vec2(b, a), sigma);
        float s3 = gr_box_blur(d, vec2(b, b), sigma);
        return vec2(max(c1, c2), s1 + s2 - s3);
    }
    // dot: the blur of the equal-area square
    return vec2(hz_aa(r - length(d), fw), gr_box_blur(d, vec2(r * 0.88622693), sigma));
}

// the above/below triangle: (x - 4, y + 8 dir), (x + 4, y + 8 dir), (x, y + 12 dir)
float gr_marker(vec2 d, float dir, float fw) {
    float t = d.y * dir;
    float sd = max(8.0 - t, max(t - 12.0, (abs(d.x) - (12.0 - t)) * 0.70710678));
    return hz_aa(-sd, fw);
}

void main() {
    float size = radar.params.x;
    float bleed = radar.params.w;
    vec2 p = v_uv * (size + 2.0 * bleed) - bleed;
    float fw = max(fwidth(p.x), 1e-4);
    float c = size * 0.5;
    float r = c - 6.0;
    vec2 q = p - c;
    float dist = length(q);

    // drop-shadow (0 0 8 px, rgba(94,242,255,.25)) of the canvas: its disc and rim
    float canvas_alpha = 0.55;
    vec4 col = hz_over(vec4(0.0), vec3(94.0, 242.0, 255.0) / 255.0,
                       gr_band_blur(-2.0 * r - (dist - r - 1.0), -(dist - r - 1.0), radar.effect.y) * canvas_alpha * radar.effect.z);

    // clipped to the disc
    float clip = hz_aa(r - dist, fw);
    vec4 inner = hz_over(vec4(0.0), vec3(4.0, 16.0, 24.0) / 255.0, 0.55);
    // conic sweep from -90 degrees, rotated by the sweep: 0.28 -> 0 over the first 12 % of the turn
    float t = fract((atan(q.y, q.x) - radar.params.y + 1.57079633) / 6.28318531);
    inner = hz_over(inner, vec3(94.0, 242.0, 255.0) / 255.0, t < 0.12 ? 0.28 * (1.0 - t / 0.12) : 0.0);
    // rings at 0.33 r and 0.66 r and the cross lines, 1 px rgba(94, 242, 255, 0.22)
    vec3 grid = vec3(94.0, 242.0, 255.0) / 255.0;
    inner = hz_over(inner, grid, 0.22 * hz_aa(0.5 - abs(dist - r * 0.33), fw));
    inner = hz_over(inner, grid, 0.22 * hz_aa(0.5 - abs(dist - r * 0.66), fw));
    float vline = hz_aa(0.5 - abs(q.x), fw) * step(6.0, p.y) * step(p.y, size - 6.0);
    float hline = hz_aa(0.5 - abs(q.y), fw) * step(6.0, p.x) * step(p.x, size - 6.0);
    inner = hz_over(inner, grid, 0.22 * max(vline, hline));

    // contacts arrive in paint order: drones (dots), ground guns (diamonds), health packs (crosses)
    int count = min(int(radar.params.z), GR_RADAR_MAX);
    for (int i = 0; i < count; i++) {
        vec4 k = radar.contacts[i];
        vec2 d = p - k.xy;
        if (dot(d, d) > 900.0) continue; // shape plus 3 sigma of its shadow
        int code = int(k.w + 0.5);
        int icon = code & 3;
        int marker = (code >> 2) & 3;
        vec3 kc = radar.palette[code >> 4].rgb;
        vec2 ic = gr_icon(d, k.z, icon, fw, radar.effect.w);
        inner = hz_over(inner, kc, ic.y);
        inner = hz_over(inner, kc, ic.x);
        if (marker != 0)
            inner = hz_over(inner, kc, gr_marker(d, marker == 1 ? -1.0 : 1.0, fw));
    }
    col = hz_over(col, inner.rgb, inner.a * clip);

    // after the clip: the 2 px rim and the player arrow
    col = hz_over(col, vec3(94.0, 242.0, 255.0) / 255.0, 0.55 * hz_aa(1.0 - abs(dist - r), fw));
    vec2 a = p - vec2(c);
    // arrow (0,-8) (5.5,6) (0,3) (-5.5,6): two triangles mirrored about x = 0
    vec2 m = vec2(abs(a.x), a.y);
    float e0 = (m.x * 14.0 - (m.y + 8.0) * 5.5) / 15.0469;  // edge (0,-8) -> (5.5,6), outside > 0
    float e1 = ((m.y - 3.0) * 5.5 - m.x * 3.0) / 6.2650;    // edge (0,3) -> (5.5,6), outside > 0
    float arrow = hz_aa(-max(e0, e1), fw) * step(-8.5, a.y);
    col = hz_over(col, vec3(234.0, 255.0, 255.0) / 255.0, arrow);

    col.a *= radar.effect.x;
    frag_color = gr_ui_out(col);
}
@end
