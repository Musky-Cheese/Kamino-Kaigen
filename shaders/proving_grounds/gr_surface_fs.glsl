// The surface material's fragment body (gr_surface.glsl, gr_floor.glsl): include after the file's GrSurfaceMaterial.
// GR_MATERIAL_MAPS: albedo / normal_map / orm_map; GR_EMISSION_MAP: emission_map; GR_PLANAR: gr_mirror.

// the detail program's outputs (SurfaceSample)
struct GrSample {
    vec3 albedo;
    float roughness_offset;
    vec3 normal_offset;
    float cavity;
    float emissive_scale;
};

vec4 gr_noise_at(uint idx, vec2 p) { return texture_sample_aniso(gr_noise, idx, p); }

void gr_detail(inout GrSample s, GrSurfaceMaterial m, vec3 p, vec3 n, float fw) {
#ifdef GR_DETAIL_PANELS
    GrPlaneFrame f = gr_plane_frame(p, n);
    vec2 cell = floor(f.uv / m.panel.xy);
    float rnd = hz_hash12(cell + f.axis * 31.7);
    vec2 size = m.panel.xy;
    if (rnd > 0.72) size *= 0.5;
    vec3 g = gr_groove_grid(f.uv, size, m.panel.z, m.panel.z * 0.9);
    float aa = 1.0 - smoothstep(m.panel.z * 0.8, m.panel.z * 3.0, fw);
    g = mix(vec3(1.0, 0.0, 0.0), g, aa);
    float blotch = gr_noise_at(m.gr_noise, f.uv * 0.09 + f.axis * 0.37).r;
    float low = (1.0 - smoothstep(0.0, 2.2, p.y)) * (0.6 + 0.4 * gr_noise_at(m.gr_noise, f.uv * 0.08).g);
    float dirt = clamp((blotch - 0.45) * 1.2 + low * 0.8, 0.0, 1.0) * m.panel.w;
    float variation = m.detail.x;
    s.albedo *= mix(1.0 - variation, 1.0 + variation * 0.4, rnd);
    s.albedo *= mix(0.3, 1.0, g.x);
    s.albedo *= 1.0 - dirt * 0.35;
    s.roughness_offset += (rnd - 0.5) * 0.1 + (1.0 - g.x) * 0.25 + dirt * 0.18;
    s.normal_offset -= (g.y * f.t + g.z * f.b) * 0.0022;
    s.cavity *= mix(0.35, 1.0, g.x);
#endif
#ifdef GR_DETAIL_FLOOR
    GrPlaneFrame f = gr_plane_frame(p, n);
    vec2 size = m.panel.xy;
    vec2 cell = floor(f.uv / size);
    float rnd = hz_hash12(cell + f.axis * 13.1);
    vec3 g = gr_groove_grid(f.uv, size, m.panel.z, m.panel.z * 0.8);
    float aa = 1.0 - smoothstep(m.panel.z * 0.8, m.panel.z * 3.0, fw);
    g = mix(vec3(1.0, 0.0, 0.0), g, aa);
    vec2 local = fract(f.uv / size) * size;
    vec2 q = abs(local - size * 0.5) - (size * 0.5 - 0.22);
    float rect = max(q.x, q.y);
    float inlay = (1.0 - smoothstep(0.008, 0.016 + fw, abs(rect))) * step(0.55, rnd) * aa;
    float smudge = gr_noise_at(m.gr_noise, f.uv * 0.2 + 0.3).r;
    float micro = gr_noise_at(m.gr_noise, f.uv * 0.11).b;
    float dirt = clamp((gr_noise_at(m.gr_noise, f.uv * 0.06 + 0.71).a - 0.5) * 1.6, 0.0, 1.0) * m.panel.w;
    float variation = m.detail.x;
    s.albedo *= mix(1.0 - variation, 1.0 + variation * 0.3, rnd);
    s.albedo = mix(s.albedo, m.secondary_color.rgb, inlay * 0.85);
    s.albedo *= mix(0.25, 1.0, g.x) * (1.0 - dirt * 0.3);
    s.roughness_offset +=
        (smudge - 0.5) * 0.3 + (micro - 0.5) * 0.06 + (1.0 - g.x) * 0.35 + dirt * 0.25 + inlay * 0.15;
    s.normal_offset -= (g.y * f.t + g.z * f.b) * 0.002;
    s.cavity *= mix(0.3, 1.0, g.x);
#endif
#ifdef GR_DETAIL_HAZARD
    GrPlaneFrame f = gr_plane_frame(p, n);
    float coord = (f.uv.x + f.uv.y) * 2.2;
    float w = max(fwidth(coord), 1e-3);
    float stripe = smoothstep(0.5 - w, 0.5 + w, abs(fract(coord) - 0.5) * 2.0);
    float wear = smoothstep(0.55, 0.8, gr_noise_at(m.gr_noise, f.uv * 0.75).r);
    stripe *= 1.0 - wear * 0.7;
    s.albedo = mix(s.albedo, m.secondary_color.rgb, stripe);
    s.roughness_offset += wear * 0.25 - stripe * 0.1;
#endif
#ifdef GR_DETAIL_GRAIN
    GrPlaneFrame f = gr_plane_frame(p, n);
    float nn = gr_noise_at(m.gr_noise, f.uv * 0.6 + f.axis * 0.29).r;
    float fine = gr_noise_at(m.gr_noise, f.uv * 0.3).b;
    s.albedo *= 0.94 + nn * 0.1;
    s.roughness_offset += (nn - 0.5) * 0.16 + (fine - 0.5) * 0.05;
#endif
    // pulses travelling along the strip
    if (m.detail.y != 0.0) {
        float phase = dot(p, vec3(1.0, 0.6, 1.0)) * m.detail.z - gr_look.time.x * m.detail.y;
        float wave = pow(max(0.5 + 0.5 * sin(phase), 0.0), 6.0);
        s.emissive_scale = 0.8 + 0.55 * wave;
    }
}

// specular occlusion (Lagarde 2014)
float gr_specular_occlusion(float n_dot_v, float ao, float roughness) {
    return clamp(pow(n_dot_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

void main() {
    GrSurfaceMaterial mat = GrSurfaceMaterial_unpack(v_material_offset);
    vec3 geo_n = normalize(v_normal);
#ifdef GR_MATERIAL_MAPS
    // sampled in uniform control flow (the detail program's fwidth and the mips)
    vec4 albedo = texture_sample(albedo, mat.albedo, v_uv) * v_color;
    vec3 tangent_normal = unpack_normal_bc5(texture_sample(normal_map, mat.normal_map, v_uv).rg);
    vec4 orm = texture_sample(orm_map, mat.orm_map, v_uv);
#else
    vec4 albedo = v_color; // the maps' @white / @normal / @white defaults
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    vec4 orm = vec4(1.0);
#endif
#ifdef GR_EMISSION_MAP
    // detail.w 1: the map is a UI surface (v down) on v-up uvs, as the signs' planes are
    vec2 emission_uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, mat.detail.w));
    vec3 emission_tex = texture_sample(emission_map, mat.emission_map, emission_uv).rgb;
#else
    vec3 emission_tex = vec3(1.0);
#endif
    float fw = length(fwidth(v_world_pos));

    GrSample d;
    d.albedo = albedo.rgb * mat.color.rgb;
    d.roughness_offset = 0.0;
    d.normal_offset = vec3(0.0);
    d.cavity = 1.0;
    d.emissive_scale = 1.0;
    gr_detail(d, mat, v_world_pos, geo_n, fw);

    HzSurface s = hz_surface_default();
    s.geo_normal = geo_n;
    s.normal = normalize(apply_normal_map(geo_n, v_tangent, v_bitangent_sign, tangent_normal) + d.normal_offset);
    float roughness = clamp((1.0 - mat.smoothness) * orm.g + d.roughness_offset, 0.03, 1.0);
    s.albedo = d.albedo;
    s.metallic = mat.metallic * orm.b;
    s.smoothness = 1.0 - roughness;
    s.emission = mat.emission_color.rgb * emission_tex * mat.emission_intensity * d.emissive_scale;
#ifdef HZ_RENDERER_DECALS
    hz_apply_decals(s, v_world_pos, gl_FragCoord.xy, v_decal_mask);
#endif

    // the cavity, the glTF occlusion and the screen-space AO darken indirect light only
    float ao = d.cavity * orm.r;
#ifdef HZ_RENDERER_GTAO
    ao *= hz_ao(gl_FragCoord.xy);
#endif

    SurfaceInput surface = SurfaceInput(s.albedo, s.metallic, s.smoothness);
    LightingInput li = LightingInput(v_world_pos, s.normal, hz_scene.camera_pos);
#ifdef HZ_RENDERER_SHADOWS
    float shadow = hz_shadow(v_world_pos);
    vec3 shadow_color = hz_shadow_input.shadow_color;
#else
    float shadow = 1.0;
    vec3 shadow_color = vec3(0.0);
#endif
    // the scene ambient is the engine's flat term; the fill is the hemisphere below
    LightParams direct_only = hz_scene.lights;
    direct_only.ambient_intensity = 0.0;
    vec3 lit = pbr_directional_lighting(surface, li, direct_only, shadow, shadow_color);
#ifdef HZ_RENDERER_LIGHTING_CLUSTERS
    HZ_CLUSTER_LOOP_BEGIN(v_view_depth)
        lit += pbr_point_light(surface, li, light);
    HZ_CLUSTER_LOOP_END
#endif

    // hemisphere irradiance x Lambert (the BRDF carries the 1/pi)
    vec3 V = normalize(hz_scene.camera_pos - v_world_pos);
    float n_dot_v = clamp(dot(s.normal, V), 0.0, 1.0);
    vec3 diffuse_color = s.albedo * (1.0 - DIELECTRIC_F0) * (1.0 - s.metallic);
    vec3 hemi = mix(gr_look.hemi_ground.rgb, gr_look.hemi_sky.rgb, 0.5 * s.normal.y + 0.5);
    vec3 indirect_diffuse = gr_look_scene(hemi * diffuse_color * (1.0 / 3.14159265));
    vec3 indirect_specular = vec3(0.0);
    // the area probes (HZ_RENDERER_REFLECTION_PROBES), the sky past them, x the look's environment intensity
#ifdef HZ_RENDERER_IBL
    indirect_diffuse += hz_ambient_diffuse(surface, li) * gr_look.time.z;
    indirect_specular = hz_ambient_specular(surface, li) * gr_look.time.z;
#endif
#ifdef GR_PLANAR
    // up-facing fragments within a few cm of the mirror plane take the mirror in place of the radiance
    float plane_mask = smoothstep(0.9, 0.99, geo_n.y) *
                       (1.0 - smoothstep(0.02, 0.08, abs(v_world_pos.y - gr_look.planar.x))) * gr_look.planar.y;
    // the mirror is the main view's: none in a probe face
    if (plane_mask > 0.0 && hz_view_has_screen_inputs()) {
        vec2 ruv = hz_planar_reflection_uv(v_world_pos) + s.normal.xz * gr_look.planar.z;
        // the mirror response: mip linear in roughness, a tent around it;
        // the physical lobe footprint swings ~10x over the floor's smudge roughness and blurs ~3x wider
        vec3 info = HZ_PLANAR_COLOR_INFO(gr_mirror, mat.gr_mirror);
        float lod = min((1.0 - s.smoothness) * max(1.0, log2(max(info.x, info.y)) - 3.0) * 0.85, info.z - 1.0);
        vec2 texel = exp2(lod) / info.xy;
        vec4 refl = HZ_PLANAR_COLOR_LOD(gr_mirror, mat.gr_mirror, ruv, lod) * 0.4;
        refl += HZ_PLANAR_COLOR_LOD(gr_mirror, mat.gr_mirror, ruv + texel * vec2(0.8, 0.5), lod) * 0.15;
        refl += HZ_PLANAR_COLOR_LOD(gr_mirror, mat.gr_mirror, ruv + texel * vec2(-0.5, 0.8), lod) * 0.15;
        refl += HZ_PLANAR_COLOR_LOD(gr_mirror, mat.gr_mirror, ruv + texel * vec2(-0.8, -0.5), lod) * 0.15;
        refl += HZ_PLANAR_COLOR_LOD(gr_mirror, mat.gr_mirror, ruv + texel * vec2(0.5, -0.8), lod) * 0.15;
        vec2 edge = smoothstep(vec2(0.0), vec2(0.05), ruv) * (1.0 - smoothstep(vec2(0.95), vec2(1.0), ruv));
        float k = plane_mask * edge.x * edge.y;
        // the mirror holds pre-exposed radiance; weighted by the same split-sum term as the environment
        HzIblTerms t = hz_ibl_terms(surface, li);
        indirect_specular = indirect_specular * (1.0 - k * refl.a) + refl.rgb * t.env_brdf_spec * k;
    }
#endif
    lit += indirect_diffuse * ao + indirect_specular * gr_specular_occlusion(n_dot_v, ao, roughness);
    lit += s.emission * hz_scene.pre_exposure;
#ifdef HZ_RENDERER_FOG
    lit = GR_FOG_APPLY(lit, v_view_depth);
#endif
    hz_output_lit(vec4(lit, 1.0), s, HZ_VELOCITY(v_clip_cur, v_clip_prev));
}
