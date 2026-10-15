// Shared by the Proving Grounds UI shaders (radar, crosshair ring, damage arcs): the UI quad vertex stage helpers' partners.
#include "hz_color.glsl"

// a band [lo, hi] along one axis blurred by a Gaussian of `sigma`, sampled at 0: a stroke blurred across its width
float gr_band_blur(float lo, float hi, float sigma) {
    vec2 e = ui_erf(vec2(hi, lo) / (sigma * 1.41421356));
    return 0.5 * (e.x - e.y);
}

vec4 gr_ui_out(vec4 c) {
    return vec4(srgb_to_linear_exact(c.rgb), c.a);
}
