// Kamino droids: the rig tables from the droid models' node tables, limb tagging, and the walk / aim pose per frame.

b32 km_droid_walks(u32 type) { return type == GR_DRONE_SCOUT || type == GR_DRONE_STRIKER; }

// the B2's own clips: the ready stance (its last frame) and the guns-up walk
hz_internal const CString km_b2_clip_paths[KM_B2_CLIP_COUNT] = {
    "kamino/models/droids/b2/Ready_weapon.hza",
    "kamino/models/droids/b2/walk_guns_up.hza",
};

static const CString km_limb_names[KM_LIMB_COUNT] = {
    "",         "Pelvis",   "Chest",    "Neck",     "Head",     "LegUpperL", "LegLowerL", "FootL",    "LegUpperR",
    "LegLowerR", "FootR",   "ArmUpperL", "ArmLowerL", "HandL",  "ArmUpperR", "ArmLowerR", "HandR",    "Gun",
};

// the limb a part name ends in ("B1LegUpperL" -> KM_LIMB_LEG_UPPER_L), after the model's prefix; a primitive suffix ("_1") is allowed
hz_internal u32 km_limb_from_name(String name, String prefix) {
  if (name.len <= prefix.len || !str_starts_with(name, prefix))
    return KM_LIMB_NONE;
  String rest = {.value = name.value + prefix.len, .len = name.len - prefix.len};
  for (u32 l = 1; l < KM_LIMB_COUNT; l++) {
    String limb = cstr_to_str(km_limb_names[l]);
    if (rest.len < limb.len || !str_starts_with(rest, limb))
      continue;
    if (rest.len == limb.len || rest.value[limb.len] == '_' || rest.value[limb.len] == '.')
      return l;
  }
  return KM_LIMB_NONE;
}

// what drives a rig this frame: a droid's or a trooper's stride and aim
typedef struct KmGait {
  f32 phase;
  f32 amount;
  f32 aim_pitch;
} KmGait;

// ---------------------------------------------------------------- rigid transforms

typedef struct KmRt {
  quat q;
  v3 t;
} KmRt;

hz_internal KmRt km_rt_identity(void) { return (KmRt){quat_identity(), V3_ZERO}; }
// rotate by q about the pivot
hz_internal KmRt km_rt_about(quat q, v3 pivot) { return (KmRt){q, v3_sub(pivot, quat_rotate(q, pivot))}; }
// a, then b
hz_internal KmRt km_rt_then(KmRt a, KmRt b) { return (KmRt){quat_mul(b.q, a.q), v3_add(quat_rotate(b.q, a.t), b.t)}; }
hz_internal v3 km_rt_point(KmRt a, v3 p) { return v3_add(quat_rotate(a.q, p), a.t); }

// the rotation taking unit `from` onto unit `to`
hz_internal quat km_from_to(v3 from, v3 to) {
  f32 d = v3_dot(from, to);
  if (d > 0.99999f)
    return quat_identity();
  if (d < -0.9999f)
    return quat_axis_angle((v3){1, 0, 0}, M_PI);
  v3 axis = v3_normalize(v3_cross(from, to));
  return quat_axis_angle(axis, m_acosf(m_clampf(d, -1.0f, 1.0f)));
}

// a bone from `pivot` towards `tip` (rest) turned to point along `dir`, after `parent`
hz_internal KmRt km_aim_bone(KmRt parent, v3 pivot, v3 tip, v3 dir) {
  v3 p = km_rt_point(parent, pivot);
  v3 rest = v3_normalize(quat_rotate(parent.q, v3_sub(tip, pivot)));
  return km_rt_then(parent, km_rt_about(km_from_to(rest, v3_normalize(dir)), p));
}

// two-bone IK: the elbow for a chain of lengths a, b from root to target, bent towards `pole`
hz_internal v3 km_ik_elbow(v3 root, f32 a, f32 b, v3 target, v3 pole) {
  v3 to = v3_sub(target, root);
  f32 d = m_clampf(v3_length(to), 1e-3f, a + b - 1e-3f);
  v3 dir = v3_normalize(to);
  f32 cos_a = m_clampf((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
  v3 side = v3_sub(pole, v3_scale(dir, v3_dot(pole, dir)));
  side = v3_lengthsq(side) > 1e-6f ? v3_normalize(side) : (v3){0, -1, 0};
  return v3_add(root, v3_add(v3_scale(dir, a * cos_a), v3_scale(side, a * m_sqrtf(1.0f - cos_a * cos_a))));
}

// ---------------------------------------------------------------- the pose

typedef struct KmPose {
  KmRt limb[KM_LIMB_COUNT];
} KmPose;

// the walk and aim pose of every limb, model space (the droid faces +Z, +X is its left)
hz_internal void km_pose_walker(const KmRig *rig, KmGait g, KmPose *out) {
  f32 amount = g.amount;
  f32 ph = g.phase;
  v3 bob = {0.0f, -m_fabsf(m_sinf(ph)) * 0.035f * amount, 0.0f};
  KmRt body = {quat_identity(), bob};

  // legs: the thigh swings by `swing`, the shin bends back by `bend`, the foot stays level
  for (u32 side = 0; side < 2; side++) {
    u32 up = side == 0 ? KM_LIMB_LEG_UPPER_L : KM_LIMB_LEG_UPPER_R;
    f32 p = ph + (side == 0 ? 0.0f : M_PI);
    f32 swing = m_sinf(p) * 0.42f * amount;
    f32 bend = (0.12f + m_maxf(0.0f, m_cosf(p)) * 0.75f) * amount + 0.08f;
    f32 out_x = side == 0 ? 0.06f : -0.06f;
    KmRt thigh = km_aim_bone(body, rig->pivot[up], rig->tip[up], (v3){out_x, -m_cosf(swing), m_sinf(swing)});
    KmRt shin = km_aim_bone(thigh, rig->pivot[up + 1], rig->tip[up + 1], (v3){out_x * 0.3f, -m_cosf(swing - bend), m_sinf(swing - bend)});
    v3 ankle = km_rt_point(shin, rig->pivot[up + 2]);
    quat foot_q = quat_axis_angle((v3){1, 0, 0}, -m_maxf(0.0f, -m_sinf(p)) * 0.25f * amount);
    out->limb[up] = thigh;
    out->limb[up + 1] = shin;
    out->limb[up + 2] = (KmRt){foot_q, v3_sub(ankle, quat_rotate(foot_q, rig->pivot[up + 2]))};
  }
  out->limb[KM_LIMB_PELVIS] = body;

  // the upper body leans into the aim about the waist and twists a little with the gait
  v3 waist = rig->pivot[KM_LIMB_CHEST];
  quat torso_q = quat_mul(quat_axis_angle((v3){0, 1, 0}, m_sinf(ph) * 0.08f * amount), quat_axis_angle((v3){1, 0, 0}, -g.aim_pitch));
  KmRt torso = km_rt_then(km_rt_about(torso_q, waist), body);
  out->limb[KM_LIMB_CHEST] = torso;
  out->limb[KM_LIMB_NECK] = torso;
  out->limb[KM_LIMB_HEAD] = torso;
  out->limb[KM_LIMB_GUN] = torso;

  // both hands on the blaster: the right on the grip, the left on the foregrip (the gun's tip); elbows bend down and out
  v3 grip = rig->pivot[KM_LIMB_GUN];
  v3 targets[2] = {rig->tip[KM_LIMB_GUN], grip};
  for (u32 side = 0; side < 2; side++) {
    u32 up = side == 0 ? KM_LIMB_ARM_UPPER_L : KM_LIMB_ARM_UPPER_R;
    v3 shoulder = rig->pivot[up], elbow = rig->pivot[up + 1], wrist = rig->pivot[up + 2];
    f32 a = v3_distance(shoulder, elbow), b = v3_distance(elbow, wrist);
    v3 pole = {side == 0 ? 0.6f : -0.6f, -1.0f, -0.2f};
    v3 e = km_ik_elbow(shoulder, a, b, targets[side], pole);
    KmRt upper = km_aim_bone(km_rt_identity(), shoulder, elbow, v3_sub(e, shoulder));
    KmRt fore = km_aim_bone(upper, elbow, wrist, v3_sub(targets[side], e));
    out->limb[up] = km_rt_then(upper, torso);
    out->limb[up + 1] = km_rt_then(fore, torso);
    out->limb[up + 2] = km_rt_then(fore, torso);
  }
}

// ---------------------------------------------------------------- systems

// each walker type's rig from its model's limb nodes once the model is ready
ecs_system(km_rig_system, EcsResInOut(KmRigs), EcsResIn(GrDroneAssets), EcsResIn(GrDroneRules), EcsResIn(AssetSystem), EcsResIn(KmSquad)) {
  const KmSquad *squad = ecs_sys_get_resource(KmSquad);
  KmRigs *rigs = ecs_sys_get_resource_mut(KmRigs);
  const GrDroneAssets *da = ecs_sys_get_resource(GrDroneAssets);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const AssetSystem *assets = ecs_sys_get_resource(AssetSystem);
  if (!rules->ready)
    return;
  for (u32 t = 0; t < KM_RIG_TYPES; t++) {
    KmRig *rig = &rigs->types[t];
    b32 clone = t == KM_RIG_CLONE;
    AssetHandle handle = clone ? HANDLE_CAST(AssetHandle, squad->model) : HANDLE_CAST(AssetHandle, da->types[t].model.handle);
    if (rig->ready || (!clone && !km_droid_walks(t)) || !asset_is_ready(assets, handle))
      continue;
    ModelBlobAsset *model = asset_get(assets, HANDLE_CAST(HzHandleT(ModelBlobAsset), handle), ModelBlobAsset);
    String prefix = clone ? STR_FROM_CSTR("CT") : gr_str32(&rules->drones.types.items[t].model_node);
    Array(ModelNodeBlobAsset) nodes = blob_array_get(ModelNodeBlobAsset, &model->nodes);
    Array(MeshBlobAsset) meshes = blob_array_get(MeshBlobAsset, &model->meshes);
    tctx_temp_allocator_scope(ta, NULL) {
      for (u32 n = 0; n < nodes.len; n++) {
        ModelNodeBlobAsset *node = &nodes.items[n];
        u32 limb = km_limb_from_name(cstr_to_str(string_blob_get(&node->name)), prefix);
        if (limb == KM_LIMB_NONE || node->extras.len == 0)
          continue;
        KmPartExtras *ex = json_parse(KmPartExtras, string_blob_get(&node->extras), &ta.allocator);
        if (!ex)
          continue;
        rig->pivot[limb] = node->model_position;
        rig->tip[limb] = (v3){ex->tip_x, ex->tip_y, ex->tip_z};
        rig->present |= 1u << limb;
        // the node's mesh's submeshes in the model's flattened slot order
        if (node->mesh != MODEL_NODE_NO_MESH && node->mesh < meshes.len) {
          u32 first = 0;
          for (u32 m = 0; m < node->mesh; m++)
            first += (u32)blob_array_len(&meshes.items[m].submeshes);
          u32 subs = (u32)blob_array_len(&meshes.items[node->mesh].submeshes);
          for (u32 s = first; s < first + subs && s < KM_RIG_SLOTS; s++)
            rig->slot_limb[s] = (u8)limb;
        }
      }
    }
    rig->ready = true;
    u32 limbs = 0;
    for (u32 l = 0; l < KM_LIMB_COUNT; l++)
      limbs += (rig->present >> l) & 1u;
    log_info("droids: % rig: % limbs", fmt_str(prefix), fmt_u32(limbs));
  }
}

// every new submesh is looked at once: a walker droid's limb parts get their limb, the rest KM_LIMB_NONE
ecs_system(km_limb_tag_system, EcsIn(EcsSubMesh), EcsNot(KmLimb), EcsExternIn(GrDrone), EcsExternIn(KmClone), EcsResIn(KmRigs)) {
  const KmRigs *rigs = ecs_sys_get_resource(KmRigs);
  for (i32 i = 0; i < it->count; i++) {
    const EcsSubMesh *sm = ecs_sys_get_component(EcsSubMesh, i);
    const GrDrone *d = ecs_component_get(it->world, sm->model, GrDrone);
    // a rigged model's parts wait for its rig; every other submesh is marked once
    u32 rig = d && km_droid_walks(d->type) ? d->type : ecs_component_get(it->world, sm->model, KmClone) ? KM_RIG_CLONE : KM_RIG_TYPES;
    if (rig < KM_RIG_TYPES && !rigs->types[rig].ready)
      continue;
    KmLimb limb = {.limb = KM_LIMB_NONE};
    if (rig < KM_RIG_TYPES && sm->slot < KM_RIG_SLOTS)
      limb = (KmLimb){.limb = rigs->types[rig].slot_limb[sm->slot], .type = rig};
    ecs_component_set(it->world, it->entities[i], KmLimb, limb);
  }
}

// each limb's pose from its droid's gait and aim, as the part's transform under the model
ecs_system(km_limb_pose_system, EcsIn(KmLimb), EcsIn(EcsSubMesh), EcsInOut(LocalTransform), EcsExternIn(GrDrone), EcsExternIn(KmClone),
           EcsResIn(KmRigs)) {
  const KmRigs *rigs = ecs_sys_get_resource(KmRigs);
  for (i32 i = 0; i < it->count; i++) {
    const KmLimb *l = ecs_sys_get_component(KmLimb, i);
    if (l->limb == KM_LIMB_NONE || l->type >= KM_RIG_TYPES || !rigs->types[l->type].ready)
      continue;
    EcsEntity owner = ecs_sys_get_component(EcsSubMesh, i)->model;
    KmGait g;
    if (l->type == KM_RIG_CLONE) {
      const KmClone *c = ecs_component_get(it->world, owner, KmClone);
      if (!c)
        continue;
      g = (KmGait){c->gait, c->gait_amount, c->aim_pitch};
    } else {
      const GrDrone *d = ecs_component_get(it->world, owner, GrDrone);
      if (!d)
        continue;
      g = (KmGait){d->gait, d->gait_amount, d->aim_pitch};
    }
    KmPose pose;
    km_pose_walker(&rigs->types[l->type], g, &pose);
    KmRt rt = pose.limb[l->limb];
    *ecs_sys_get_component_mut(LocalTransform, i) = (LocalTransform){.position = rt.t, .rotation = rt.q, .scale = V3_ONE};
  }
}

// ---------------------------------------------------------------- the B2's clips

// the clips once loaded
ecs_system(km_b2_clips_system, EcsResInOut(KmB2Clips), EcsResIn(AssetSystem)) {
  KmB2Clips *c = ecs_sys_get_resource_mut(KmB2Clips);
  const AssetSystem *a = ecs_sys_get_resource(AssetSystem);
  if (c->ready)
    return;
  for (u32 k = 0; k < KM_B2_CLIP_COUNT; k++)
    if (!asset_is_ready(a, c->clips[k]))
      return;
  for (u32 k = 0; k < KM_B2_CLIP_COUNT; k++)
    c->blobs[k] = asset_get(a, c->clips[k], AnimationBlobAsset);
  c->ready = true;
  log_info("droids: B2 clips ready (walk % s)", fmt_f32_p(c->blobs[KM_B2_WALK]->length, 2));
}

// a new B2's animation layer, standing ready
ecs_system(km_b2_bind_system, EcsIn(GrDrone), EcsWith(EcsSkinnedModel), EcsNot(EcsAnimationLayer), EcsResInOut(EcsRendererAlloc),
           EcsResIn(KmB2Clips)) {
  const KmB2Clips *c = ecs_sys_get_resource(KmB2Clips);
  if (!c->ready)
    return;
  Allocator *alloc = &ecs_sys_get_resource_mut(EcsRendererAlloc)->alloc;
  for (i32 i = 0; i < it->count; i++) {
    if (ecs_sys_get_component(GrDrone, i)->type != GR_DRONE_STRIKER)
      continue;
    EcsAnimationLayer layer = {.layer = animation_layer_create(alloc)};
    AnimationBlobAsset *stand = c->blobs[KM_B2_STAND];
    animation_layer_play_ex(&layer.layer, stand, ANIM_TRANSITION_SNAP, 0.0f, 0.0f, false, stand->length);
    ecs_component_set(it->world, it->entities[i], EcsAnimationLayer, layer);
    ecs_component_set(it->world, it->entities[i], KmB2Anim, {.clip = KM_B2_STAND});
  }
}

// stand or walk by stride; the walk plays at the ground speed
ecs_system(km_b2_anim_system, EcsIn(GrDrone), EcsInOut(KmB2Anim), EcsInOut(EcsAnimationLayer), EcsResIn(KmB2Clips)) {
  const KmB2Clips *c = ecs_sys_get_resource(KmB2Clips);
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    KmB2Anim *anim = ecs_sys_get_component_mut(KmB2Anim, i);
    AnimationLayer *layer = &ecs_sys_get_component_mut(EcsAnimationLayer, i)->layer;
    u32 want = d->gait_amount > 0.3f ? KM_B2_WALK : KM_B2_STAND;
    if (want != anim->clip) {
      AnimationBlobAsset *blob = c->blobs[want];
      if (want == KM_B2_WALK)
        animation_layer_play_ex(layer, blob, ANIM_TRANSITION_INERTIALIZE, 0.3f, 1.0f, true, 0.0f);
      else
        animation_layer_play_ex(layer, blob, ANIM_TRANSITION_INERTIALIZE, 0.35f, 0.0f, false, blob->length);
      anim->clip = want;
    }
    u32 n = animation_layer_state_count(layer);
    if (want == KM_B2_WALK && n > 0) {
      f32 speed = m_sqrtf(d->velocity.x * d->velocity.x + d->velocity.z * d->velocity.z);
      animation_layer_state(layer, n - 1)->speed = m_clampf(speed / 1.3f, 0.6f, 1.8f);
    }
  }
}

void km_droids_init(EcsWorld *w, AssetSystem *assets) {
  ecs_resource_set_zero(w, KmRigs);
  KmB2Clips clips = {0};
  for (u32 k = 0; k < KM_B2_CLIP_COUNT; k++)
    clips.clips[k] = asset_load(AnimationBlobAsset, assets, .path = km_b2_clip_paths[k]);
  ecs_resource_set(w, KmB2Clips, clips);
  ecs_resource_set(w, KmSquad, ((KmSquad){.model = asset_load(ModelBlobAsset, assets, .path = "kamino/models/clone/clone.glb")}));
  ecs_resource_set_zero(w, KmSquadShots);
}

void km_droids_schedule(EcsWorld *w) {
  ecs_schedule_single(w, km_rig_system);      // writes the KmRigs resource
  ecs_schedule_single(w, km_limb_tag_system); // adds KmLimb: a structural change per new submesh
  ecs_schedule(w, km_limb_pose_system);
  ecs_schedule_single(w, km_b2_clips_system); // writes the KmB2Clips resource once
  ecs_schedule_single(w, km_b2_bind_system);  // adds the layer: a structural change per new B2
  ecs_schedule(w, km_b2_anim_system);
}
