// Proving Grounds GLSL helpers: the surface plane frame and groove grid.
// Pure functions: include in any stage, after renderer_shaders.glsl.
#include "hz_noise.glsl"
#include "hz_color.glsl"
#ifndef GR_COMMON_GLSL
#define GR_COMMON_GLSL

// axis-aligned planar projection by the dominant normal axis (floors xz, walls zy or xy)
struct GrPlaneFrame {
    vec2 uv;
    vec3 t;
    vec3 b;
    float axis;
};

GrPlaneFrame gr_plane_frame(vec3 p, vec3 n) {
    vec3 an = abs(n);
    GrPlaneFrame f;
    if (an.y >= an.x && an.y >= an.z) {
        f.uv = p.xz; f.t = vec3(1.0, 0.0, 0.0); f.b = vec3(0.0, 0.0, 1.0); f.axis = 1.0;
    } else if (an.x >= an.z) {
        f.uv = p.zy; f.t = vec3(0.0, 0.0, 1.0); f.b = vec3(0.0, 1.0, 0.0); f.axis = 0.0;
    } else {
        f.uv = p.xy; f.t = vec3(1.0, 0.0, 0.0); f.b = vec3(0.0, 1.0, 0.0); f.axis = 2.0;
    }
    return f;
}

// panel seams as (height 0..1, dh/du, dh/dv) with an analytic gradient, so the bevel normal needs no derivatives
vec3 gr_groove_grid(vec2 uv, vec2 size, float width, float bevel) {
    vec2 f = fract(uv / size);
    vec2 dist = min(f, 1.0 - f) * size;
    vec2 sgn = vec2(f.x < 0.5 ? 1.0 : -1.0, f.y < 0.5 ? 1.0 : -1.0);
    vec2 t = clamp((dist - width) / bevel, 0.0, 1.0);
    vec2 h = t * t * (3.0 - 2.0 * t);
    vec2 dh = 6.0 * t * (1.0 - t) / bevel * sgn;
    return h.x < h.y ? vec3(h.x, dh.x, 0.0) : vec3(h.y, 0.0, dh.y);
}

#endif
