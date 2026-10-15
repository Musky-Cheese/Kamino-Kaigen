// Proving Grounds shared per-frame uniforms, written once per frame by gr_look_consolidate.
// Include at file top (the type, for codegen) and in each stage after renderer_shaders.glsl (the block).
#ifndef GR_LOOK_TYPES
#define GR_LOOK_TYPES
#define GR_AREA_COUNT 4

// the volumetrics are a main-view composite: mirror and probe renders take no fog
#define GR_FOG_APPLY(c, z) (hz_view_has_screen_inputs() ? hz_fog_apply(c, z) : (c))

// colours are linear in look units (scene-linear); gr_look_scene() turns them into this frame's radiance
@reflect
struct GrLookInput {
    vec4 time;          // x look clock (s, keeps running while paused), y cd/m2 per look unit, z environment intensity, w sky intensity
    vec4 sun_dir;       // xyz unit vector toward the sun, w intensity
    vec4 sun_color;     // rgb linear
    vec4 hemi_sky;      // rgb hemisphere sky colour x intensity, linear
    vec4 hemi_ground;   // rgb ground colour x intensity, linear
    vec4 sky_zenith;    // rgb linear
    vec4 sky_horizon;   // rgb linear
    vec4 area_min[GR_AREA_COUNT]; // xyz box min (hub, hangar, wing, yard), w the area's fog density multiplier
    vec4 area_max[GR_AREA_COUNT]; // xyz box max
    vec4 planar;        // x plane height, y 1 = the mirror holds this frame, z normal distortion, w unused
    vec4 feedback;      // x damage, y low health, z heal, w scope amount
};
#endif

#ifdef HZ_RENDERER_VERTEX_SHADER
#ifndef GR_LOOK_VS
#define GR_LOOK_VS
uniform _GrLook { GrLookInput gr_look; };
#endif
#else
// GR_LOOK_STANDALONE: a pass outside the renderer stage headers
#if defined(HZ_RENDERER_FRAGMENT_SHADER) || defined(GR_LOOK_STANDALONE)
#ifndef GR_LOOK_FS
#define GR_LOOK_FS
uniform _GrLook { GrLookInput gr_look; };

#ifdef HZ_RENDERER_LIGHTING_BASIC
// a look-unit radiance (scene-linear) as this frame's pre-exposed HDR value
vec3 gr_look_scene(vec3 c) { return c * gr_look.time.y * hz_scene.pre_exposure; }
#endif

// soft membership of a world position in each area, normalised to sum 1 (AREA_FADE 1 m)
float gr_area_distance(int i, vec3 p) {
    vec3 d = max(gr_look.area_min[i].xyz - p, p - gr_look.area_max[i].xyz);
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

vec4 gr_area_weights(vec3 p) {
    vec4 w;
    for (int i = 0; i < GR_AREA_COUNT; i++) {
        float sd = gr_area_distance(i, p);
        w[i] = 1.0 - smoothstep(-1.0, 1.0, sd) + 1e-4 / (1.0 + max(sd, 0.0));
    }
    return w / (w.x + w.y + w.z + w.w);
}
#endif
#endif
#endif
