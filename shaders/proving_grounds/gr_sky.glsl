// The procedural HDR sky: gradient, warm haze, sun halo and disc, drifting fBm clouds.
// Drawn on a sphere instance at the far plane, so the planar mirror's view draws it too.
#include "renderer.glsl"
#include "gr_look.glsl"

@vs
#define HZ_RENDERER_VERTEX_SHADER
#define HZ_NO_DEPTH_PREPASS
#include "renderer_shaders.glsl"
out vec3 v_dir;
flat out uint v_material_offset;

void main() {
    VertexInstanceData v = unpack_instance_data();
    vec3 dir = mat3(v.model) * v.pos;
    v_dir = dir;
    v_material_offset = v.material_offset;
    // a direction (w 0) ignores the view's translation; reverse-Z 0 pins it to the far plane
    vec4 clip = hz_scene.view_proj * vec4(dir, 0.0);
    gl_Position = vec4(clip.xy, 0.0, clip.w);
}
@end

@fs
#undef HZ_RENDERER_VERTEX_SHADER
#define HZ_RENDERER_FRAGMENT_SHADER
#define HZ_RENDERER_LIGHTING_BASIC
#define HZ_RENDERER_OUTPUT
#include "renderer_shaders.glsl"
#include "gr_look.glsl"
#include "gr_common.glsl"
#include "gr_sky_common.glsl"
in vec3 v_dir;
flat in uint v_material_offset;

@material
struct GrSkyMaterial {
    float fog_distance; // > 0: the fog covers the sky (to HeightFogParams.max_distance)
};

void main() {
    GrSkyMaterial m = GrSkyMaterial_unpack(v_material_offset);
    vec3 d = normalize(v_dir);
    GrSkyParams p;
    p.zenith = gr_look.sky_zenith.rgb;
    p.horizon = gr_look.sky_horizon.rgb;
    p.sun_dir = gr_look.sun_dir.xyz;
    p.sun_color = gr_look.sun_color.rgb;
    p.time = gr_look.time.x;
    p.intensity = gr_look.time.w;
    vec3 color = gr_look_scene(gr_sky_radiance(p, d) + gr_sky_disc(p, d));
#ifdef HZ_RENDERER_FOG
    color = m.fog_distance > 0.0 && hz_view_has_screen_inputs() ? hz_fog_apply_sky(color, d) : color;
#endif
    hz_output_sky(color, HZ_VELOCITY_NONE);
}
@end
