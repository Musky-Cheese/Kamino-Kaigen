// The scope overlay final pass: the engine's final chain with the lens blur in HDR, then the reticle.
#include "renderer.glsl"
#include "gr_look.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#include "renderer_shaders.glsl"
out vec2 v_uv;

void main() {
    v_uv = fullscreen_triangle_uv(uint(gl_VertexIndex));
    gl_Position = fullscreen_triangle_pos(v_uv, 0.0);
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_FINAL
#include "renderer_shaders.glsl"
#include "gr_look.glsl"
#include "gr_common.glsl"
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

#define GR_SCOPE_RADIUS 0.43

// every material's shader declares its layout; the scope final has no parameters
@material
struct GrFinalMaterial {
};

// a rotated 12-tap disc of the HDR frame around the lens
vec3 gr_scope_blur(vec2 uv, float aspect) {
    float angle = hz_ign(gl_FragCoord.xy) * 6.2831;
    vec2 radius = vec2(0.012 / aspect, 0.012);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float fi = float(i);
        float r = sqrt((fi + 0.5) / 12.0);
        float a = fi * 2.39996 + angle;
        sum += hz_final_color(uv + vec2(cos(a), sin(a)) * r * radius);
    }
    return sum / 12.0;
}

void main() {
    if (hz_final_in.debug_passthrough != 0) {
        frag_color = vec4(hz_final_color(v_uv), 1.0);
        return;
    }
    vec2 uv = v_uv;
    float aspect = hz_final_in.output_size.x / max(hz_final_in.output_size.y, 1.0);
    float scope = gr_look.feedback.w;
    vec2 lens_p = (uv - 0.5) * vec2(aspect, 1.0);
    float lens_r = length(lens_p);
    float lens_mask = 1.0 - smoothstep(GR_SCOPE_RADIUS - 0.004, GR_SCOPE_RADIUS + 0.004, lens_r);

    // hz_final_default's chain; the surround is blurred and dimmed in HDR, before bloom and the tonemap
#if defined(HZ_RENDERER_FINAL_LEAN)
    vec3 col = hz_final_source(uv, hz_final_in.sharpen_mode, hz_final_in.sharpen);
#else
    if (hz_final_in.debug_nan != 0 && hz_final_is_nan(uv)) {
        frag_color = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec3 col = hz_final_source_ca(uv);
#endif
    if (lens_mask < 1.0)
        col = mix(col, gr_scope_blur(uv, aspect) * 0.22, (1.0 - lens_mask) * scope);
    if (hz_bloom_input.intensity > 0.0)
        col += hz_final_bloom(uv) * hz_bloom_input.intensity * hz_bloom_input.tint;
    col += hz_final_god_rays(uv);
    col += hz_final_lens_flare(uv);
    if (hz_final_in.vignette_mode == 0)
        col = hz_final_vignette(col, uv, hz_final_in.vignette_center, hz_final_in.vignette_intensity,
                                hz_final_in.vignette_roundness, hz_final_in.vignette_smoothness,
                                hz_final_in.vignette_color);
    col *= exp2(hz_final_in.post_exposure) * hz_final_exposure_ratio();
    col = hz_final_grade(col);
    col = saturate(col + hz_final_in.brightness);
#if !defined(HZ_RENDERER_FINAL_LEAN)
    col = hz_final_display_fx(col, uv);
#endif

    // lens vignette, then the reticle: thin lines with heavier posts, a centre gap, ticks, a red dot, a dark rim
    float px = fwidth(lens_p.y);
    col *= mix(1.0, 0.45 + 0.55 * (1.0 - smoothstep(GR_SCOPE_RADIUS * 0.7, GR_SCOPE_RADIUS, lens_r)), lens_mask * scope);
    vec2 a = abs(lens_p);
    float wx = a.x > GR_SCOPE_RADIUS * 0.55 ? 3.0 : 1.0;
    float wy = a.y > GR_SCOPE_RADIUS * 0.55 ? 3.0 : 1.0;
    float line_h = (1.0 - smoothstep(0.5 * wx * px, (0.5 * wx + 1.0) * px, a.y)) * step(0.012, a.x);
    float line_v = (1.0 - smoothstep(0.5 * wy * px, (0.5 * wy + 1.0) * px, a.x)) * step(0.012, a.y);
    float ticks = 0.0;
    for (int i = 1; i <= 4; i++) {
        float d = float(i) * 0.045;
        ticks += (1.0 - smoothstep(1.0 * px, 2.0 * px, abs(a.x - d))) * step(a.y, 0.009);
        ticks += (1.0 - smoothstep(1.0 * px, 2.0 * px, abs(a.y - d))) * step(a.x, 0.009);
    }
    float reticle = clamp(line_h + line_v + ticks, 0.0, 1.0) * lens_mask * scope;
    col = mix(col, vec3(0.005, 0.006, 0.008), reticle * 0.92);
    float centre_dot = 1.0 - smoothstep(2.0 * px, 3.2 * px, lens_r);
    col = mix(col, vec3(1.0, 0.16, 0.12), centre_dot * scope);
    float rim = 1.0 - smoothstep(0.0, 3.0 * px, abs(lens_r - GR_SCOPE_RADIUS));
    col = mix(col, vec3(0.02), rim * scope);

    col = hz_final_gamma(col, hz_final_in.gamma);
#if !defined(HZ_RENDERER_FINAL_LEAN)
    col = hz_final_grain(col, uv);
#endif
    frag_color = vec4(col, 1.0);
}
@end
