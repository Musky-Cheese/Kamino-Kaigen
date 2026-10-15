// The first-person viewmodel: motion, gun rigs and arm clips; the gun group is the Weapon entity's WorldTransform.

#define GR_SPRINT_POS ((v3){-0.04f, -0.06f, 0.04f})
#define GR_SPRINT_ROT ((v3){-0.3f, 0.55f, 0.35f})
#define GR_SCOPE_HIDE 0.9f
#define GR_PUMP_DELAY 0.14f // seconds from a shot to the pump stroke
#define GR_PUMP_TIME 0.34f
#define GR_PUMP_EJECT 0.35f

// the arm clips in GrWeaponAssets.clips order
hz_internal const char *gr_arms_clip_paths[GR_ARMS_CLIP_COUNT] = {
    "proving_grounds/models/arms/arms/rifle_hold.hza",
    "proving_grounds/models/arms/arms/rifle_magazine_reach.hza",
    "proving_grounds/models/arms/arms/rifle_magazine.hza",
    "proving_grounds/models/arms/arms/breacher_hold.hza",
    "proving_grounds/models/arms/arms/breacher_pump.hza",
    "proving_grounds/models/arms/arms/breacher_shell_reach.hza",
    "proving_grounds/models/arms/arms/breacher_shell.hza",
    "proving_grounds/models/arms/arms/lancer_hold.hza",
    "proving_grounds/models/arms/arms/lancer_magazine_reach.hza",
    "proving_grounds/models/arms/arms/lancer_magazine.hza",
};

// [GrWeaponId][GrArmsMotion] -> clip index, -1 where the gun has no such clip
hz_internal const i32 gr_arms_clip_table[GR_GUN_COUNT][GR_ARMS_MOTION_COUNT] = {
    {0, 1, 2, -1, -1, -1},
    {3, -1, -1, 4, 5, 6},
    {7, 8, 9, -1, -1, -1},
};

hz_internal void gr_spring3_update(GrSpring3 *s, f32 dt) {
  v3 accel = v3_sub(v3_scale(v3_sub(s->target, s->value), s->stiffness), v3_scale(s->velocity, s->damping));
  s->velocity = v3_add(s->velocity, v3_scale(accel, dt));
  s->value = v3_add(s->value, v3_scale(s->velocity, dt));
}

// the rig pose offsets one frame of animation adds
typedef struct GrRigPose {
  v3 pos;
  f32 rx, ry, rz;
} GrRigPose;

// rig rest: the moving parts back home (the parts drawn are the gun in hand's, so the offsets reset with it)
hz_internal void gr_rig_rest(GrViewmodel *vm, u32 gun) {
  if (gun == GR_WEAPON_BREACHER) {
    vm->cycle = -1.0f;
    vm->end_stroke = false;
    vm->parts.shell = V3_ZERO;
    vm->parts.shell_visible = false;
    vm->parts.pump = V3_ZERO;
  } else {
    vm->parts.magazine = V3_ZERO;
    vm->parts.magazine_hidden = false;
    vm->parts.handle = V3_ZERO;
  }
  if (gun == GR_WEAPON_LANCER)
    vm->charge = 1.0f;
  vm->hands_motion = GR_ARMS_HOLD;
  vm->hands_u = 0.0f;
}

hz_internal void gr_rig_fire(GrViewmodel *vm, const GrGunConfig *g, u32 gun) {
  vm->heat[gun] = m_minf(1.0f, vm->heat[gun] + g->heat_per_shot);
  if (gun == GR_WEAPON_LANCER)
    vm->charge = 0.0f;
  if (gun == GR_WEAPON_BREACHER) {
    vm->cycle = 0.0f;
    vm->racked = false;
    vm->ejected = false;
  }
}

hz_internal void gr_vm_sound(GrViewmodel *vm, u32 sound) {
  if (vm->sound_count < ARRAY_SIZE(vm->sounds))
    vm->sounds[vm->sound_count++] = (GrSoundEvent){.sound = sound};
}

// magazine rigs (rifle, lancer): tilt, the magazine out and back, the charging-handle yank, the coils
hz_internal void gr_rig_magazine(GrViewmodel *vm, const GrWeapon *w, const GrGunConfig *g, f32 dt, GrRigPose *pose) {
  vm->hands_motion = GR_ARMS_HOLD;
  vm->hands_u = 0.0f;
  b32 reloading = w->reload_phase == GR_RELOAD_MAGAZINE;
  f32 t = w->reload_t;
  if (reloading) {
    f32 tilt = m_smoothstepf(0.0f, 0.15f, t) * (1.0f - m_smoothstepf(0.78f, 1.0f, t));
    pose->rz += tilt * 0.55f;
    pose->rx += tilt * 0.12f;
    pose->pos.y -= tilt * 0.035f;
    f32 out = m_smoothstepf(0.15f, 0.32f, t) * (1.0f - m_smoothstepf(0.38f, 0.55f, t));
    vm->parts.magazine = v3_scale(g->magazine_dir, out * g->magazine_travel);
    vm->parts.magazine_hidden = t > 0.32f && t < 0.4f; // swapped for a fresh one while it's off screen
    // the support hand takes the magazine, pulls it, seats the new one, and is back on the handguard
    if (t < 0.15f) {
      vm->hands_motion = GR_ARMS_MAGAZINE_REACH;
      vm->hands_u = m_smoothstepf(0.05f, 0.15f, t);
    } else if (t < 0.56f) {
      vm->hands_motion = GR_ARMS_MAGAZINE;
      vm->hands_u = out;
    } else {
      vm->hands_motion = GR_ARMS_MAGAZINE_REACH;
      vm->hands_u = 1.0f - m_smoothstepf(0.56f, 0.7f, t);
    }
  }
  if (w->gun == GR_WEAPON_RIFLE && reloading) {
    f32 yank = m_smoothstepf(0.62f, 0.68f, t) * (1.0f - m_smoothstepf(0.68f, 0.8f, t));
    vm->parts.handle.z = yank * 0.045f;
    pose->pos.z += yank * 0.03f;
    pose->rx -= yank * 0.08f;
  }
  if (w->gun == GR_WEAPON_LANCER) {
    // dead while the battery is out, then a charge-up as the new one seats; else ready just before the next shot
    if (reloading)
      vm->charge = t < 0.62f ? m_minf(vm->charge, 1.0f - m_smoothstepf(0.1f, 0.22f, t)) : m_smoothstepf(0.62f, 0.92f, t);
    else
      vm->charge = m_minf(1.0f, vm->charge + dt * g->fire_rate / 0.85f);
    // the materials keep their last drive while the lancer is out of hand
    f32 c = vm->charge;
    vm->coil = 0.3f + c * c * 7.0f + (c >= 1.0f ? 0.0f : m_sinf(c * 60.0f) * 0.6f * c);
    vm->cell = reloading ? 1.0f + c * 2.0f : 3.0f;
  }
}

// breacher rig: the pump stroke after a shot and the shell-by-shell reload
hz_internal void gr_rig_breacher(GrViewmodel *vm, const GrWeapon *w, const GrWeaponConfig *cfg,
                                 const GrWeaponAssets *wa, f32 dt, GrRigPose *pose, const GrPlayerState *p) {
  f32 stroke = -1.0f;
  if (vm->cycle >= 0.0f) {
    vm->cycle += dt;
    f32 u = (vm->cycle - GR_PUMP_DELAY) / GR_PUMP_TIME;
    if (u >= 0.0f && u < 1.0f) {
      // rack sound as the stroke starts; the spent hull flies once the pump is back
      if (!vm->racked) {
        vm->racked = true;
        gr_vm_sound(vm, GR_SND_PUMP);
      }
      stroke = u;
      if (!vm->ejected && u >= GR_PUMP_EJECT) {
        vm->ejected = true;
        v3 port = gr_gun_point(vm, p, gr_socket(wa, GR_WEAPON_BREACHER, GR_SOCKET_EJECTION_PORT));
        v3 on_screen = gr_on_screen(port, p);
        if (vm->eject_count < ARRAY_SIZE(vm->ejects)) {
          f32 sx = random_f32_range(&vm->rng, 2.2f, 3.2f) * 0.8f, sy = random_f32_range(&vm->rng, 0.6f, 1.3f),
              sz = random_f32_range(&vm->rng, 0.2f, 0.8f);
          v3 v = v3_add(quat_rotate(p->view_rot, (v3){sx, sy, sz}), p->velocity);
          vm->ejects[vm->eject_count++] = (GrEjectEvent){.kind = GR_EJECT_SHELL, .position = on_screen, .velocity = v};
        }
      }
    } else if (u >= 1.0f) {
      vm->cycle = -1.0f;
    }
  }

  v3 port = gr_socket(wa, GR_WEAPON_BREACHER, GR_SOCKET_LOAD_PORT);
  v3 rest = wa->shell_rest;
  v3 below = v3_add(cfg->shell_below, port);
  v3 mouth = v3_add(cfg->shell_mouth, port);
  v3 home = v3_add(mouth, (v3){0.0f, cfg->shell_push, 0.0f});
  v3 shell = vm->parts.shell; // offset from rest; keeps its place while hidden
  f32 tilt = 0.0f, reach = -1.0f, carry = -1.0f;
  vm->parts.shell_visible = false;
  f32 t = w->reload_t;
  switch (w->reload_phase) {
  case GR_RELOAD_SHELL_START:
    tilt = m_smoothstepf(0.0f, 1.0f, t);
    vm->end_stroke = false;
    // off the pump and down to take the first shell, in hand once the hand is below
    reach = m_smoothstepf(0.1f, 0.9f, t);
    vm->parts.shell_visible = reach > 0.8f;
    shell = v3_sub(below, rest);
    break;
  case GR_RELOAD_SHELL_INSERT: {
    tilt = 1.0f;
    // up from below to the port's mouth, where the hand lets go and presses it on into the tube
    if (t < 0.5f) {
      carry = m_smoothstepf(0.0f, 0.5f, t);
      shell = v3_sub(v3_lerp(below, mouth, carry), rest);
    } else {
      carry = 1.0f - m_smoothstepf(0.72f, 1.0f, t);
      shell = v3_sub(v3_lerp(mouth, home, m_smoothstepf(0.5f, 0.64f, t)), rest);
    }
    vm->parts.shell_visible = t < 0.64f;
    f32 seat = m_smoothstepf(0.55f, 0.62f, t) * (1.0f - m_smoothstepf(0.62f, 0.8f, t));
    pose->pos.y += seat * 0.006f;
    pose->rx += seat * 0.03f;
    break;
  }
  case GR_RELOAD_SHELL_END: {
    // back onto the pump, in time to rack it if the chamber is empty
    reach = 1.0f - m_smoothstepf(0.0f, 0.35f, t);
    if (w->reload_pump) {
      tilt = 1.0f - m_smoothstepf(0.0f, 0.4f, t);
      f32 u = (t - 0.35f) / 0.65f;
      if (u >= 0.0f) {
        if (!vm->end_stroke) {
          vm->end_stroke = true;
          gr_vm_sound(vm, GR_SND_PUMP);
        }
        stroke = m_maxf(stroke, u);
      }
    } else {
      tilt = 1.0f - m_smoothstepf(0.0f, 1.0f, t);
    }
    break;
  }
  default:
    break;
  }
  vm->parts.shell = shell;
  pose->rz += tilt * 0.62f;
  pose->rx += tilt * 0.16f;
  pose->pos.y -= tilt * 0.04f;
  pose->pos.x -= tilt * 0.015f;

  f32 back = 0.0f;
  vm->parts.pump = V3_ZERO;
  if (stroke >= 0.0f) {
    back = m_smoothstepf(0.0f, 0.35f, stroke) * (1.0f - m_smoothstepf(0.5f, 0.9f, stroke));
    vm->parts.pump.z = back * cfg->pump_travel;
    pose->rx += back * 0.05f;
    pose->pos.z += back * 0.012f;
  }
  if (carry >= 0.0f) {
    vm->hands_motion = GR_ARMS_SHELL;
    vm->hands_u = carry;
  } else if (reach > 0.0f) {
    vm->hands_motion = GR_ARMS_SHELL_REACH;
    vm->hands_u = reach;
  } else {
    vm->hands_motion = GR_ARMS_PUMP;
    vm->hands_u = back;
  }
}

// one frame of the viewmodel motion and the gun's rig, then the gun group's world pose
ecs_system(gr_viewmodel_system, EcsIn(GrWeapon), EcsInOut(GrViewmodel), EcsInOut(WorldTransform),
           EcsResIn(GrPlayerState), EcsResIn(GrWeaponRules), EcsResIn(GrWeaponAssets)) {
  const GrPlayerState *p = ecs_sys_get_resource(GrPlayerState);
  const GrWeaponConfig *cfg = &ecs_sys_get_resource(GrWeaponRules)->weapons;
  const GrWeaponAssets *wa = ecs_sys_get_resource(GrWeaponAssets);
  for (i32 i = 0; i < it->count; i++) {
    const GrWeapon *w = ecs_sys_get_component(GrWeapon, i);
    GrViewmodel *vm = ecs_sys_get_component_mut(GrViewmodel, i);
    const GrWeaponEvents *ev = &w->events;
    const GrGunConfig *g = &cfg->guns[w->gun];
    vm->sound_count = 0;
    vm->eject_count = 0;

    // the weapon's commands this frame, in its order: rests, raise, lower, then the shots' kicks
    for (u32 k = 0; k < GR_GUN_COUNT; k++)
      if (ev->rest_mask & (1u << k))
        gr_rig_rest(vm, k);
    if (ev->raise)
      vm->equip = 0.0f;
    if (ev->view_last != GR_VIEW_NONE)
      vm->lowering = ev->view_last == GR_VIEW_LOWER;
    for (u32 s = 0; s < ev->shot_count; s++) {
      const GrShotEvent *shot = &ev->shots[s];
      // impulses are velocities: the springs turn them into a ~2 cm / ~2 deg kick
      f32 k = (1.0f - shot->zoom * 0.55f) * cfg->guns[shot->gun].kick;
      vm->kick_pos.velocity = v3_add(vm->kick_pos.velocity,
                                     (v3){(random_f32(&vm->rng) - 0.5f) * 0.06f * k, 0.06f * k, 0.55f * k});
      f32 ry = (random_f32(&vm->rng) - 0.5f) * 0.3f * k;
      f32 rz = (random_f32(&vm->rng) - 0.5f) * 0.7f * k;
      vm->kick_rot.velocity = v3_add(vm->kick_rot.velocity, (v3){0.9f * k, ry, rz});
      gr_rig_fire(vm, &cfg->guns[shot->gun], shot->gun);
    }

    f32 dt = w->dt;
    if (dt > 0.0f) {
      f32 ads = p->zoom;
      v2 mouse = w->look;
      vm->sprint = m_damp(vm->sprint, p->sprinting && !w->held ? 1.0f : 0.0f, 8.0f, dt);
      vm->equip = m_clampf(vm->equip + (vm->lowering ? -dt : dt) / (cfg->swap_time * 0.5f), 0.0f, 1.0f);

      // mouse sway lags the view, reduced while aiming
      f32 sway_scale = 1.0f - ads * 0.75f;
      vm->sway.target = (v3){m_clampf(-mouse.y * 0.0011f, -0.08f, 0.08f) * sway_scale,
                             m_clampf(-mouse.x * 0.0011f, -0.09f, 0.09f) * sway_scale,
                             m_clampf(-mouse.x * 0.0008f, -0.07f, 0.07f) * sway_scale};
      gr_spring3_update(&vm->sway, dt);
      gr_spring3_update(&vm->kick_pos, dt);
      gr_spring3_update(&vm->kick_rot, dt);

      // the rig in hand: heat, aim, moving parts, the arms' clip
      GrRigPose pose = {0};
      vm->parts = (GrRigParts){.shell = vm->parts.shell};
      if (w->gun == GR_WEAPON_LANCER)
        vm->lens = 1.0f + ads * 2.0f;
      vm->heat[w->gun] = m_maxf(0.0f, vm->heat[w->gun] - dt * 0.35f);
      if (w->gun == GR_WEAPON_BREACHER) {
        gr_rig_breacher(vm, w, cfg, wa, dt, &pose, p);
      } else {
        gr_rig_magazine(vm, w, g, dt, &pose);
      }

      f32 bob = p->move_amount * (1.0f - ads * 0.85f) * (1.0f + vm->sprint * 0.8f);
      f32 bob_x = m_sinf(p->bob_phase) * 0.011f * bob;
      f32 bob_y = -m_absf(m_cosf(p->bob_phase)) * 0.013f * bob;

      // aiming puts the Sight socket on the camera axis, ads_eye_distance in front of the eye
      v3 sight = gr_socket(wa, w->gun, GR_SOCKET_SIGHT);
      v3 ads_pos = {-sight.x, -sight.y, -g->ads_eye_distance - sight.z};
      v3 pos = v3_lerp(g->hip, ads_pos, ads);
      pos.x += bob_x;
      pos.y += bob_y;
      pos = v3_add(pos, vm->kick_pos.value);
      pos = v3_add(pos, v3_scale(GR_SPRINT_POS, vm->sprint));
      pos = v3_add(pos, pose.pos);
      f32 rx = vm->sway.value.x + vm->kick_rot.value.x + GR_SPRINT_ROT.x * vm->sprint + bob_y * 1.5f + pose.rx;
      f32 ry = vm->sway.value.y + vm->kick_rot.value.y + GR_SPRINT_ROT.y * vm->sprint +
               m_lerpf(cfg->hip_yaw, 0.0f, ads) + pose.ry;
      f32 rz = vm->sway.value.z + vm->kick_rot.value.z + GR_SPRINT_ROT.z * vm->sprint + bob_x * 2.0f + pose.rz;
      // equip: rise from below, sink back down when lowering
      f32 e = 1.0f - vm->equip;
      pos.y -= e * e * 0.3f;
      rx += e * e * 0.9f;
      vm->local_pos = pos;
      vm->local_rot = quat_from_euler_xyz((v3){rx, ry, rz});    }
    vm->lowered = vm->lowering && vm->equip <= 0.0f;
    // the root shows in a run; a scope takes over the whole view once aimed
    vm->visible = w->in_run && !(g->scope && p->zoom > GR_SCOPE_HIDE);

    // the camera's child: the gun group follows the view every frame, paused or not
    WorldTransform *xf = ecs_sys_get_component_mut(WorldTransform, i);
    xf->position = v3_add(p->eye, quat_rotate(p->view_rot, vm->local_pos));
    xf->rotation = quat_mul(p->view_rot, vm->local_rot);
    xf->scale = (v3){1.0f, 1.0f, 1.0f};
  }
}

// every gun part places and shows itself from the weapon and pickup state (it writes only its own components)
ecs_system(gr_gun_part_system, EcsIn(GrGunPart), EcsInOut(LocalTransform), EcsInOut(EcsVisibility),
           EcsResIn(GrWeaponState), EcsResIn(GrPickupState)) {
  const GrWeaponState *ws = ecs_sys_get_resource(GrWeaponState);
  const GrPickupState *ps = ecs_sys_get_resource(GrPickupState);
  for (i32 i = 0; i < it->count; i++) {
    const GrGunPart *part = ecs_sys_get_component(GrGunPart, i);
    v3 pos = part->rest;
    b32 visible;
    if (part->world_copy) {
      visible = ps->in_world[part->gun];
    } else {
      b32 in_hand = ws->gun == part->gun;
      visible = ws->view_visible && in_hand;
      const GrRigParts *rp = &ws->parts;
      switch (part->kind) {
      case GR_PART_MAGAZINE:
        if (in_hand) {
          pos = v3_add(pos, rp->magazine);
          visible = visible && !rp->magazine_hidden;
        }
        break;
      case GR_PART_HANDLE:
        if (in_hand)
          pos = v3_add(pos, rp->handle);
        break;
      case GR_PART_PUMP:
        if (in_hand)
          pos = v3_add(pos, rp->pump);
        break;
      case GR_PART_SHELL:
        pos = v3_add(pos, rp->shell);
        visible = visible && rp->shell_visible;
        break;
      default:
        break;
      }
    }
    const LocalTransform *lt = ecs_sys_get_component(LocalTransform, i);
    if (!v3_eqv(lt->position, pos))
      ecs_sys_get_component_mut(LocalTransform, i)->position = pos;
    if (ecs_sys_get_component(EcsVisibility, i)->visible != visible)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = visible;
  }
}

// once the clips are loaded: the arms' animation layer and the clip joint remaps (one-time)
ecs_system(gr_arms_bind_system, EcsInOut(GrArms), EcsIn(EcsModelRef), EcsWith(EcsSkinnedModel), EcsNot(EcsAnimationLayer),
           EcsResInOut(EcsRendererAlloc), EcsResIn(GrWeaponAssets), EcsResIn(AssetSystem)) {
  const GrWeaponAssets *wa = ecs_sys_get_resource(GrWeaponAssets);
  if (!wa->clips_ready)
    return;
  Allocator *alloc = &ecs_sys_get_resource_mut(EcsRendererAlloc)->alloc;
  const AssetSystem *assets = ecs_sys_get_resource(AssetSystem);
  for (i32 i = 0; i < it->count; i++) {
    const EcsModelRef *ref = ecs_sys_get_component(EcsModelRef, i);
    ModelBlobAsset *model = asset_get(assets, HANDLE_CAST(HzHandleT(ModelBlobAsset), ref->model.handle), ModelBlobAsset);
    if (!model)
      continue;
    GrArms *arms = ecs_sys_get_component_mut(GrArms, i);
    u32 remapped = 0;
    for (u32 c = 0; c < GR_ARMS_CLIP_COUNT; c++) {
      arms->remaps[c] = animation_build_joint_remap(alloc, wa->clip_blobs[c], model);
      remapped += arms->remaps[c] != NULL;
    }
    EcsAnimationLayer layer = {.layer = animation_layer_create(alloc)};
    animation_layer_play_ex(&layer.layer, wa->clip_blobs[0], ANIM_TRANSITION_SNAP, 0.0f, 0.0f, false, 0.0f);
    animation_layer_state(&layer.layer, 0)->remap = arms->remaps[0];
    arms->clip = 0;
    ecs_component_set(it->world, it->entities[i], EcsAnimationLayer, layer);
    log_info("arms: % clips bound (% need a joint remap), % joints", fmt_u32(GR_ARMS_CLIP_COUNT), fmt_u32(remapped),
             fmt_u32((u32)blob_array_len(&model->joints)));
  }
}

// one clip shown paused at u * duration; a new clip replaces the old (the clips meet end to end)
ecs_system(gr_arms_system, EcsInOut(GrArms), EcsInOut(EcsAnimationLayer), EcsInOut(EcsVisibility),
           EcsResIn(GrWeaponState), EcsResIn(GrWeaponAssets)) {
  const GrWeaponState *ws = ecs_sys_get_resource(GrWeaponState);
  const GrWeaponAssets *wa = ecs_sys_get_resource(GrWeaponAssets);
  for (i32 i = 0; i < it->count; i++) {
    b32 visible = ws->view_visible;
    if (ecs_sys_get_component(EcsVisibility, i)->visible != visible)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = visible;
    if (!visible)
      continue;
    i32 clip = gr_arms_clip_table[ws->gun][ws->hands_motion];
    if (clip < 0) {
      log_error("arms: % has no clip for motion %", fmt_u32(ws->gun), fmt_u32(ws->hands_motion));
      continue;
    }
    GrArms *arms = ecs_sys_get_component_mut(GrArms, i);
    AnimationLayer *layer = &ecs_sys_get_component_mut(EcsAnimationLayer, i)->layer;
    AnimationBlobAsset *blob = wa->clip_blobs[clip];
    if (arms->clip != clip) {
      // snap: reuses the layer's one state, so nothing is allocated
      animation_layer_play_ex(layer, blob, ANIM_TRANSITION_SNAP, 0.0f, 0.0f, false, 0.0f);
      animation_layer_state(layer, 0)->remap = arms->remaps[clip];
      arms->clip = clip;
    }
    // a hold may be exported with zero duration: time 0 samples its one pose
    animation_layer_state(layer, 0)->time = m_saturatef(ws->hands_u) * blob->length;  }
}
