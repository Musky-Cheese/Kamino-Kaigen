// Effect shaders: shared helpers (the look-unit -> scene conversion).
#include "hz_noise.glsl"
#ifndef GR_FX_COMMON_GLSL
#define GR_FX_COMMON_GLSL

// look-unit linear HDR -> this frame's pre-exposed scene value; `unit` is cd/m2 per look unit (set from C)
vec3 gr_fx_to_scene(vec3 c, float unit) { return c * (unit * hz_scene.pre_exposure); }

#endif
