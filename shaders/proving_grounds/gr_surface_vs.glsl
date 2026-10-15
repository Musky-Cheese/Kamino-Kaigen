// The surface material's vertex main (gr_surface.glsl, gr_floor.glsl): include after the stage's outputs, which
// stay in the stage block (varying locations are numbered from the block's own text).
void main() {
    VertexInstanceData v = unpack_instance_data();
#ifdef HZ_RENDERER_GBUFFER
    vec3 bind_pos = v.pos;
#endif
#ifdef HZ_RENDERER_SKINNED
    apply_skinning(v.pos, v.normal, v.tangent, v.bone_offset, v.vertex_base, v.vertex_layout_flags);
#endif
    vec4 world_pos = v.model * vec4(v.pos, 1.0);
    gl_Position = hz_instance_clip(v, world_pos);
    v_uv = v.uv;
    v_color = v.color;
    v_material_offset = v.material_offset;
#ifdef HZ_RENDERER_DECALS
    v_decal_mask = v.decal_mask;
#endif
    v_normal = normalize((v.model * vec4(v.normal, 0.0)).xyz);
    v_world_pos = world_pos.xyz;
    v_tangent = normalize((v.model * vec4(v.tangent, 0.0)).xyz);
    v_bitangent_sign = v.bitangent_sign;
    v_view_depth = -(hz_scene.view * world_pos).z;
#ifdef HZ_RENDERER_GBUFFER
    HzVelocity vel = hz_instance_velocity(v, world_pos, hz_world_pos_prev(bind_pos, v));
    v_clip_cur = vel.clip_cur;
    v_clip_prev = vel.clip_prev;
#endif
}
