// The env cube: the sky without its disc, prefiltered by the sky module. (n/8, n/8, 6)
#include "common.glsl"

@cs
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 1, binding = 0) uniform _Params {
    vec4 zenith;
    vec4 horizon;
    vec4 sun_dir;
    vec4 sun_color;
    float time;
    float intensity;
    uint face_size;
    uint pad;
} params;

layout(set = 2, binding = 0, rgba16f) writeonly uniform image2DArray out_cube;

#include "gr_common.glsl"
#include "gr_sky_common.glsl"

// the engine's cube face order and orientation (env_mip_downsample.glsl)
vec3 gr_face_dir(uint face, uint x, uint y, float size) {
    float u = 2.0 * (float(x) + 0.5) / size - 1.0;
    float v = -(2.0 * (float(y) + 0.5) / size - 1.0);
    vec3 dir;
    if (face == 0u)      dir = vec3( 1, v, -u);
    else if (face == 1u) dir = vec3(-1, v,  u);
    else if (face == 2u) dir = vec3( u, 1, -v);
    else if (face == 3u) dir = vec3( u,-1,  v);
    else if (face == 4u) dir = vec3( u, v,  1);
    else                 dir = vec3(-u, v, -1);
    return normalize(dir);
}

void main() {
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= params.face_size || id.y >= params.face_size || id.z >= 6u)
        return;
    GrSkyParams p;
    p.zenith = params.zenith.rgb;
    p.horizon = params.horizon.rgb;
    p.sun_dir = params.sun_dir.xyz;
    p.sun_color = params.sun_color.rgb;
    p.time = params.time;
    p.intensity = params.intensity;
    vec3 dir = gr_face_dir(id.z, id.x, id.y, float(params.face_size));
    imageStore(out_cube, ivec3(id), vec4(gr_sky_radiance(p, dir), 1.0));
}
@end
