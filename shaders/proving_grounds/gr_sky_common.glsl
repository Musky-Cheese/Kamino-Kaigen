// The sky in look units (scene-linear), for the dome and the env cube bake.
#ifndef GR_SKY_COMMON_GLSL
#define GR_SKY_COMMON_GLSL

struct GrSkyParams {
    vec3 zenith;    // linear
    vec3 horizon;   // linear
    vec3 sun_dir;   // unit, toward the sun
    vec3 sun_color; // linear colour (the look's sun colour, no intensity)
    float time;     // the cloud drift clock (s)
    float intensity; // the look's sky intensity
};

// gradient, haze, ground, halo and clouds times the sky intensity; the disc is separate (gr_sky_disc)
vec3 gr_sky_radiance(GrSkyParams p, vec3 d) {
    float h = d.y;
    float up = max(h, 0.0);
    vec3 sky = mix(p.horizon * 1.15, p.zenith * 0.9, pow(up, 0.55));
    sky += vec3(1.0, 0.85, 0.7) * 0.25 * exp(-up * 9.0);
    // smoothstep(0, -0.15, h): descending edges written as 1 - ascending
    sky = mix(sky, vec3(0.18, 0.2, 0.24), 1.0 - smoothstep(-0.15, 0.0, h));

    float cos_sun = dot(d, p.sun_dir);
    float c_sun = max(cos_sun, 0.0);
    sky += p.sun_color * (pow(c_sun, 12.0) * 0.6 + pow(c_sun, 200.0) * 3.0);

    if (h > 0.0) {
        vec2 cp = d.xz / (h + 0.12) * 0.9 + vec2(p.time * 0.004, p.time * 0.0015);
        float c = hz_fbm(cp * 1.4, 4) * 0.9375 * 0.65 + hz_fbm(cp * 4.1 + 11.0, 4) * 0.9375 * 0.35;
        float cover = smoothstep(0.46, 0.78, c) * smoothstep(0.0, 0.2, h);
        vec3 lit = mix(vec3(1.0, 1.02, 1.06) * 1.6, p.sun_color * 2.6, pow(c_sun, 5.0));
        vec3 shade = mix(p.horizon, vec3(0.75, 0.8, 0.9), 0.5) * 0.9;
        vec3 cloud = mix(shade, lit, smoothstep(0.45, 0.9, c));
        sky = mix(sky, cloud, cover * 0.85);
    }
    return sky * p.intensity;
}

// the sun disc, added after the intensity so it always blooms
vec3 gr_sky_disc(GrSkyParams p, vec3 d) {
    return p.sun_color * smoothstep(0.99955, 0.99975, dot(d, p.sun_dir)) * 80.0;
}

#endif
