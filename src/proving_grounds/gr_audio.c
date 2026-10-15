// Proving Grounds audio on the HZ mixer: clips render at load across lanes, requests are components, loops follow entities.

#include "gr_audio_recipes.c"

#define GR_AUDIO_CLIP_PEAK 0.99f       // clips are stored peak-normalised; the voice gain restores the rendered level
#define GR_AUDIO_VOLUME_DEFAULT 0.8f   // the default settings volume
#define GR_AUDIO_VOLUME_TAU 0.05f      // master volume easing time constant (s)
#define GR_AUDIO_REF_DISTANCE 3.0f     // inverse distance model: reference distance 3 m, rolloff 1.1
#define GR_AUDIO_ROLLOFF 1.1f
#define GR_AUDIO_LOOP_FADE_IN_TAU 0.15f // a loop's gain eases up from silence
#define GR_AUDIO_LOOP_TAU 0.1f          // a running loop's rate and volume changes
#define GR_AUDIO_LOOP_STOP_TAU 0.08f    // a stopping loop's gain eases to 0
#define GR_AUDIO_LOOP_STOP_S 0.4f       // a stopping loop's voice ends this long after the request
#define GR_AUDIO_LOOP_SEND 0.2f         // a loop's default reverb send
#define GR_AUDIO_AMBIENCE_VOLUME 0.311126977f // 0.22 +3 dB (non-positional); no reverb
#define GR_AUDIO_DEV_ALL_GAP_S 0.35f
static_assert(GR_AUDIO_LOOP_MAX > GR_MAX_DRONES, "every drone's hum and the ambience need a loop slot");
// freeverb tuned to a 2.4 s hall at 0.55 wet: octave T60 +/-0.07 s, level +/-0.9 dB
#define GR_AUDIO_HALL_ROOM 0.74f
#define GR_AUDIO_HALL_DAMP 0.10f
#define GR_AUDIO_HALL_SEND 0.2371f

// master compressor: threshold -14 dB, knee 8, ratio 4, attack 3 ms, release 200 ms
#define GR_AUDIO_COMPRESSOR ((PgCompressorDesc){-14.0f, 8.0f, 4.0f, 0.003f, 0.2f})

// non-positional volumes carry +3 dB: the mixer's centre pan is -3 dB per channel
GrSfxPlay gr_sfx_site(GrSfxSite site) {
  GrSfxPlay p = {.volume = 1.0f, .rate = 1.0f, .jitter = 0.04f, .reverb = 0.25f}; // play defaults
  switch (site) {
  case GR_SITE_FIRE_RIFLE: p = (GrSfxPlay){GR_SOUND_RIFLE, false, {0}, 1.20208156f, 1.0f, 0.05f, 0.35f}; break;
  case GR_SITE_FIRE_BREACHER: p = (GrSfxPlay){GR_SOUND_BREACHER, false, {0}, 1.20208156f, 1.0f, 0.05f, 0.35f}; break;
  case GR_SITE_FIRE_LANCER: p = (GrSfxPlay){GR_SOUND_LANCER, false, {0}, 1.20208156f, 1.0f, 0.05f, 0.35f}; break;
  case GR_SITE_DRY_FIRE: p.sound = GR_SOUND_DRY_FIRE, p.volume = 1.1313709f; break;
  case GR_SITE_RELOAD_CUE: p.sound = GR_SOUND_RELOAD_OUT, p.volume = 1.1313709f, p.reverb = 0.1f; break;
  case GR_SITE_PUMP: p.sound = GR_SOUND_PUMP, p.volume = 1.06066012f, p.reverb = 0.1f; break;
  case GR_SITE_SHELL_INSERT: p.sound = GR_SOUND_SHELL_INSERT, p.volume = 1.1313709f, p.reverb = 0.1f; break;
  case GR_SITE_WEAPON_PICKUP: p.sound = GR_SOUND_WEAPON_PICKUP, p.volume = 0.989949465f, p.reverb = 0.1f; break;
  case GR_SITE_IMPACT: p.sound = GR_SOUND_IMPACT, p.positional = true, p.volume = 0.45f, p.jitter = 0.12f; break;
  case GR_SITE_CASING: p.sound = GR_SOUND_CASING, p.positional = true, p.volume = 0.5f; break;
  case GR_SITE_HITMARKER: p.sound = GR_SOUND_HITMARKER, p.volume = 0.63639605f, p.reverb = 0.0f; break;
  case GR_SITE_KILL: p.sound = GR_SOUND_KILL, p.volume = 0.777817488f, p.reverb = 0.0f; break;
  case GR_SITE_PLAYER_HURT: p.sound = GR_SOUND_PLAYER_HURT, p.volume = 0.919238746f; break;
  case GR_SITE_HEALTH_PICKUP: p.sound = GR_SOUND_HEALTH_PICKUP, p.volume = 0.989949465f, p.reverb = 0.15f; break;
  case GR_SITE_JUMP: p.sound = GR_SOUND_JUMP, p.volume = 0.424264073f, p.reverb = 0.1f; break;
  case GR_SITE_LAND: p.sound = GR_SOUND_LAND, p.volume = 1.2727921f, p.reverb = 0.15f; break;
  case GR_SITE_FOOTSTEP: p.sound = GR_SOUND_FOOTSTEP, p.volume = 0.311126977f, p.jitter = 0.12f, p.reverb = 0.12f; break;
  case GR_SITE_JUMP_PAD: p.sound = GR_SOUND_JUMP_PAD, p.positional = true, p.volume = 0.9f; break;
  case GR_SITE_WAVE_START: p.sound = GR_SOUND_WAVE_START, p.volume = 0.919238746f, p.reverb = 0.4f; break;
  case GR_SITE_WAVE_CLEAR: p.sound = GR_SOUND_WAVE_CLEAR, p.volume = 0.989949465f, p.reverb = 0.3f; break;
  case GR_SITE_VICTORY: p.sound = GR_SOUND_VICTORY, p.volume = 1.1313709f; break;
  case GR_SITE_GAME_OVER: p.sound = GR_SOUND_GAME_OVER, p.volume = 1.1313709f; break;
  case GR_SITE_BOLT_IMPACT: p.sound = GR_SOUND_BOLT_IMPACT, p.positional = true, p.volume = 0.7f; break;
  case GR_SITE_DRONE_FIRE: p.sound = GR_SOUND_DRONE_FIRE, p.positional = true, p.volume = 0.9f; break;
  case GR_SITE_DRONE_CHARGE: p.sound = GR_SOUND_DRONE_CHARGE, p.positional = true, p.volume = 0.7f; break;
  case GR_SITE_WARP: p.sound = GR_SOUND_WARP, p.positional = true, p.volume = 1.0f; break;
  case GR_SITE_METAL_HIT: p.sound = GR_SOUND_METAL_HIT, p.positional = true, p.volume = 0.6f; break;
  case GR_SITE_EXPLOSION:
    p.sound = GR_SOUND_EXPLOSION, p.positional = true, p.volume = 1.2f, p.rate = 1.1f, p.reverb = 0.5f;
    break;
  default: log_error("gr_sfx_site: unknown site %", fmt_u32((u32)site)); break;
  }
  return p;
}

void gr_sfx_emit(GrSfxEmitter *em, u32 frame, GrSfxPlay play) {
  if (em->frame != frame) {
    em->frame = frame;
    em->count = 0;
    em->dropped = 0;
  }
  if (em->count < GR_SFX_EMIT_MAX)
    em->plays[em->count++] = play;
  else
    em->dropped++;
}

b32 gr_audio_ready(const GrAudio *a) { return a && a->ready; }

/* ------------------------------------------------------------ the master bus and the panner */

// the compressor, then the master gain eased with a 0.05 s time constant, and the meter
hz_internal void gr_audio_master_fx(void *state, f32 *lr, u32 frames, u32 sample_rate) {
  GrAudioMaster *m = (GrAudioMaster *)state;
  pg_compressor_bus_effect(&m->comp, lr, frames, sample_rate);
  const f32 k = 1.0f - m_expf(-1.0f / (GR_AUDIO_VOLUME_TAU * (f32)sample_rate));
  for (u32 i = 0; i < frames; i++) {
    m->gain += (m->target - m->gain) * k;
    const f32 l = lr[2u * i] * m->gain, r = lr[2u * i + 1u] * m->gain;
    lr[2u * i] = l;
    lr[2u * i + 1u] = r;
    m->meter_sq += (f64)(l * l) + (f64)(r * r);
    m->meter_peak = m_maxf(m->meter_peak, m_maxf(m_absf(l), m_absf(r)));
  }
  m->meter_frames += frames;
}

// inverse distance gain: no upper clamp
hz_internal f32 gr_audio_distance_gain(f32 d) {
  return GR_AUDIO_REF_DISTANCE / (GR_AUDIO_REF_DISTANCE + GR_AUDIO_ROLLOFF * (m_maxf(d, GR_AUDIO_REF_DISTANCE) -
                                                                              GR_AUDIO_REF_DISTANCE));
}

// equal-power pan from the listener-relative azimuth, folded to the front
hz_internal f32 gr_audio_pan(const GrAudio *a, v3 position) {
  const v3 rel = v3_sub(position, a->listener_pos);
  const f32 len = v3_length(rel);
  if (len <= 1e-6f)
    return 0.0f;
  const v3 dir = v3_scale(rel, 1.0f / len);
  const v3 right = v3_normalize(v3_cross(a->listener_fwd, a->listener_up));
  const v3 up = v3_cross(right, v3_normalize(a->listener_fwd));
  const v3 flat = v3_sub(dir, v3_scale(up, v3_dot(dir, up)));
  const f32 flat_len = v3_length(flat);
  if (flat_len <= 1e-6f)
    return 0.0f;
  const f32 d = m_clampf(v3_dot(v3_scale(flat, 1.0f / flat_len), right), -1.0f, 1.0f);
  return 1.0f - 2.0f * m_acosf(d) / M_PI; // azimuth / 90 after the front/back fold
}

/* ------------------------------------------------------------ one-shots and loops */

hz_internal const GrSfxJob *gr_audio_variant(GrAudio *a, u32 sound, b32 random_variant) {
  const u32 variants = GR_RECIPES[sound].variants;
  const u32 v = random_variant ? (u32)(random_f32(&a->rng) * (f32)variants) : 0u;
  return &a->jobs.items[a->first_job[sound] + (v < variants ? v : variants - 1u)];
}

// a random variant at rate * (1 +/- jitter), through the panner when positional
hz_internal void gr_audio_play(GrAudio *a, const GrSfxPlay *p) {
  if (p->sound >= GR_SOUND_COUNT) {
    log_error("gr_audio: play of unknown sound %", fmt_u32(p->sound));
    return;
  }
  const GrSfxJob *job = gr_audio_variant(a, p->sound, true);
  const f32 rate = (p->rate > 0.0f ? p->rate : 1.0f) * (1.0f + (random_f32(&a->rng) * 2.0f - 1.0f) * p->jitter);
  const f32 volume = p->volume * job->level;
  if (volume <= 0.0f)
    return; // a silent request (a casing at rest); the mixer reads volume 0 as 1
  AudioPlayDesc d = {
      .volume = volume,
      .pitch = rate,
      .bus = AUDIO_BUS_SFX,
      .send = p->reverb * GR_AUDIO_HALL_SEND,
  };
  GrAudioTrack *track = NULL;
  if (p->positional) {
    d.volume = volume * gr_audio_distance_gain(v3_length(v3_sub(p->position, a->listener_pos)));
    d.pan = gr_audio_pan(a, p->position);
    for (u32 i = 0; i < GR_AUDIO_TRACK_MAX && !track; i++)
      if (!handle_is_valid(a->tracks[i].voice))
        track = &a->tracks[i];
  }
  HzHandleT(AudioVoice) voice = audio_play(&a->system, job->clip, &d);
  if (!handle_is_valid(voice))
    return;
  a->plays++;
  if (track)
    *track = (GrAudioTrack){.voice = voice, .position = p->position, .volume = volume};
}

// a loop: the first variant from a random offset, the gain rising from silence
hz_internal void gr_audio_loop_start(GrAudio *a, GrAudioLoopVoice *slot, EcsEntity e, u32 sound, b32 positional,
                                     v3 position, f32 volume, f32 rate, f32 reverb) {
  const GrSfxJob *job = gr_audio_variant(a, sound, false);
  const f32 seconds = (f32)job->frames / (f32)GR_SYN_SR;
  // the volume is set before the next mix by gr_audio_loops_update; the mixer reads 0 as 1
  HzHandleT(AudioVoice) voice = audio_play(&a->system, job->clip,
                                           &(AudioPlayDesc){
                                               .volume = 1e-6f,
                                               .pitch = rate,
                                               .loop = true,
                                               .bus = AUDIO_BUS_SFX,
                                               .start_offset_s = random_f32(&a->rng) * seconds,
                                               .send = reverb * GR_AUDIO_HALL_SEND,
                                           });
  if (!handle_is_valid(voice))
    return;
  *slot = (GrAudioLoopVoice){
      .entity = e,
      .voice = voice,
      .positional = positional,
      .position = position,
      .level = job->level,
      .target = volume,
      .rate = rate,
      .rate_target = rate,
      .tau = GR_AUDIO_LOOP_FADE_IN_TAU,
      .seen_frame = a->frame,
  };
}

hz_internal void gr_audio_loop_stop(GrAudioLoopVoice *slot) {
  slot->stopping = true;
  slot->target = 0.0f;
  slot->tau = GR_AUDIO_LOOP_STOP_TAU;
  slot->stop_timer = GR_AUDIO_LOOP_STOP_S;
}

// the voice gain and pan a voice at `position` gets this frame (positional), else its volume as is
hz_internal void gr_audio_voice_apply(GrAudio *a, HzHandleT(AudioVoice) voice, b32 positional, v3 position, f32 volume) {
  if (positional) {
    audio_voice_set_volume(&a->system, voice,
                           volume * gr_audio_distance_gain(v3_length(v3_sub(position, a->listener_pos))));
    audio_voice_set_pan(&a->system, voice, gr_audio_pan(a, position));
  } else {
    audio_voice_set_volume(&a->system, voice, volume);
  }
}

// loop gains and rates ease to their targets, the 0.4 s stop, and the panners follow the listener
hz_internal void gr_audio_voices_update(GrAudio *a, f32 dt) {
  for (u32 i = 0; i < GR_AUDIO_LOOP_MAX; i++) {
    GrAudioLoopVoice *l = &a->loops[i];
    if (!handle_is_valid(l->voice))
      continue;
    if (l->stopping) {
      l->stop_timer -= dt;
      if (l->stop_timer <= 0.0f) {
        audio_voice_stop(&a->system, l->voice, 0.0f);
        *l = (GrAudioLoopVoice){0};
        continue;
      }
    }
    l->gain = l->target + (l->gain - l->target) * m_expf(-dt / l->tau);
    l->rate = l->rate_target + (l->rate - l->rate_target) * m_expf(-dt / GR_AUDIO_LOOP_TAU);
    audio_voice_set_pitch(&a->system, l->voice, l->rate);
    gr_audio_voice_apply(a, l->voice, l->positional, l->position, l->gain * l->level);
  }
  for (u32 i = 0; i < GR_AUDIO_TRACK_MAX; i++) {
    GrAudioTrack *t = &a->tracks[i];
    if (!handle_is_valid(t->voice))
      continue;
    if (!audio_voice_playing(&a->system, t->voice)) {
      *t = (GrAudioTrack){0};
      continue;
    }
    gr_audio_voice_apply(a, t->voice, true, t->position, t->volume);
  }
}

/* ------------------------------------------------------------ the load render */

// one variant into its staging PCM, loop cross-faded, peak-normalised for 16 bits
hz_internal void gr_audio_render_job(GrSfxJob *job) {
  const u64 t0 = os_time_now();
  const GrRecipe *r = &GR_RECIPES[job->sound];
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  Random rng = random_create(job->seed);
  GrSyn s = {.alloc = &ta.allocator, .rng = &rng, .sr = GR_SYN_SR, .n = gr_recipe_frames(r)};
  s.out = gr_buf(&s);
  s.aux = gr_buf(&s);
  r->fn(&s);

  u32 len = s.n;
  if (r->loop) {
    const u32 fade = gr_recipe_loop_fade(s.n);
    len = s.n - fade;
    for (u32 i = 0; i < fade; i++) {
      const f32 t = (f32)i / (f32)fade;
      s.out[i] = s.out[i] * t + s.out[len + i] * (1.0f - t);
    }
  }
  assert_msg(len == job->frames, "gr_audio: render length differs from the staged clip");

  f32 peak = 0.0f;
  f64 sq = 0.0;
  for (u32 i = 0; i < len; i++) {
    peak = m_maxf(peak, m_absf(s.out[i]));
    sq += (f64)s.out[i] * (f64)s.out[i];
  }
  const f32 scale = peak > 0.0f ? GR_AUDIO_CLIP_PEAK / peak : 0.0f;
  for (u32 i = 0; i < len; i++)
    s.out[i] *= scale;
  pg_buf_to_pcm16(s.out, len, job->pcm);
  job->peak = peak;
  job->rms = (f32)m_sqrt(sq / (f64)(len ? len : 1u));
  job->level = peak / GR_AUDIO_CLIP_PEAK;
  tctx_temp_allocator_end(ta);
  job->ms = (f32)os_ticks_to_ms(os_time_diff(os_time_now(), t0));
}

// every variant of every recipe, partitioned across lanes; each job writes only its own PCM range
ecs_system_array(gr_audio_render_system, EcsArrayInOut(Array(GrSfxJob))) {
  Array(GrSfxJob) *jobs = ecs_sys_get_array_mut(Array(GrSfxJob));
  for (i32 i = 0; i < it->count; i++)
    gr_audio_render_job(&jobs->items[it->offset + i]);
}

// the rendered PCM becomes one AudioSource per variant; the staging arena goes. Reading the jobs orders it after
ecs_system_array(gr_audio_sources_system, EcsArrayIn(Array(GrSfxJob)), EcsResInOut(GrAudio)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  if (a->ready || it->offset != 0)
    return;
  f32 lane_ms = 0.0f, slowest = 0.0f;
  u32 frames = 0;
  for (u32 i = 0; i < a->jobs.len; i++) {
    GrSfxJob *job = &a->jobs.items[i];
    WavFile wav = create_wav_from_samples(job->pcm, job->frames, GR_SYN_SR);
    job->clip = audio_source_from_wav(&a->system, &wav, &a->alloc);
    job->pcm = NULL;
    lane_ms += job->ms;
    slowest = m_maxf(slowest, job->ms);
    frames += job->frames;
    if (!handle_is_valid(job->clip))
      log_error("gr_audio: no source for % variant %", fmt_cstr(GR_RECIPES[job->sound].name), fmt_u32(job->variant));
  }
  arena_destroy(a->staging);
  a->staging = NULL;
  a->ready = true;
  log_info("gr_audio: rendered % sounds as % clips (% s of audio), % ms of render work, slowest clip % ms",
           fmt_u32(GR_SOUND_COUNT), fmt_u32(a->jobs.len), fmt_f32_p((f32)frames / (f32)GR_SYN_SR, 2),
           fmt_f32_p(lane_ms, 1), fmt_f32_p(slowest, 1));
}

/* ------------------------------------------------------------ the frame's consolidation */

// the listener is the camera
ecs_system(gr_audio_listener_system, EcsIn(Camera), EcsResInOut(GrAudio)) {
  if (it->count == 0)
    return;
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  const Camera *cam = ecs_sys_get_component(Camera, 0);
  a->listener_pos = cam->pos;
  a->listener_fwd = quat_rotate(cam->rot, V3_BACK);
  a->listener_up = quat_rotate(cam->rot, V3_UP);
}

// this frame's requests from every emitter; dropped before the audio output is unlocked
ecs_system(gr_audio_emit_system, EcsIn(GrSfxEmitter), EcsResInOut(GrAudio), EcsResIn(GrClock)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  if (!a->ready || !a->unlocked)
    return;
  for (i32 i = 0; i < it->count; i++) {
    const GrSfxEmitter *em = ecs_sys_get_component(GrSfxEmitter, i);
    if (em->frame != clock->frame)
      continue;
    for (u32 k = 0; k < em->count; k++)
      gr_audio_play(a, &em->plays[k]);
    if (em->dropped) {
      a->dropped += em->dropped;
      log_warn("gr_audio: % requests past % on one emitter this frame", fmt_u32(em->dropped),
               fmt_u32(GR_SFX_EMIT_MAX));
    }
  }
}

// an entity's loop this frame; the first want starts it, later ones update position, rate and volume
hz_internal void gr_audio_loop_want(GrAudio *a, EcsEntity e, u32 sound, v3 pos, f32 volume, f32 rate, f32 reverb) {
  GrAudioLoopVoice *slot = NULL, *free_slot = NULL;
  for (u32 k = 0; k < GR_AUDIO_LOOP_MAX && !slot; k++) {
    GrAudioLoopVoice *l = &a->loops[k];
    if (handle_is_valid(l->voice) && l->entity == e && !l->stopping)
      slot = l;
    else if (!free_slot && !handle_is_valid(l->voice))
      free_slot = l;
  }
  if (!slot) {
    if (!free_slot) {
      log_warn("gr_audio: loop for entity % not started, % slots busy", fmt_u32((u32)e), fmt_u32(GR_AUDIO_LOOP_MAX));
      return;
    }
    gr_audio_loop_start(a, free_slot, e, sound, true, pos, volume, rate, reverb);
    return;
  }
  slot->seen_frame = a->frame;
  slot->position = pos;
  slot->rate_target = rate;
  if (slot->target != volume) {
    slot->target = volume;
    slot->tau = GR_AUDIO_LOOP_TAU;
  }
}

hz_internal f32 gr_audio_drone_scale(const GrDroneRules *rules, u32 type) {
  return type < GR_DRONE_TYPE_COUNT ? rules->drones.types.items[type].scale : 1.0f;
}

// drone events: warp and hum at spawn, charge, fire, the hum's rate
ecs_system(gr_audio_drone_system, EcsIn(GrDrone), EcsResInOut(GrAudio), EcsResIn(GrClock), EcsResIn(GrDroneRules),
           EcsResIn(GrWaves)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  if (!a->ready || !a->unlocked)
    return;
  const u32 frame = ecs_sys_get_resource(GrClock)->frame;
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const u32 run = ecs_sys_get_resource(GrWaves)->run;
  for (i32 i = 0; i < it->count; i++) {
    const GrDrone *d = ecs_sys_get_component(GrDrone, i);
    if (d->state == GR_DRONE_DEAD || d->run != run)
      continue; // a dead or cleared drone's hum: the sweep fades a hum nobody wants this frame
    const f32 scale = gr_audio_drone_scale(rules, d->type);
    const u32 ev = d->events_frame == frame ? d->events : 0u;
    if (ev & GR_DRONE_EV_SPAWNED) {
      GrSfxPlay p = gr_sfx_site(GR_SITE_WARP);
      p.position = d->position;
      gr_audio_play(a, &p);
    }
    if (ev & GR_DRONE_EV_TELEGRAPH) {
      GrSfxPlay p = gr_sfx_site(GR_SITE_DRONE_CHARGE);
      p.position = d->position;
      p.rate = 1.2f - scale * 0.2f;
      gr_audio_play(a, &p);
    }
    if (ev & GR_DRONE_EV_FIRED) {
      GrSfxPlay p = gr_sfx_site(GR_SITE_DRONE_FIRE);
      p.position = d->eye;
      p.rate = 1.15f - scale * 0.2f;
      gr_audio_play(a, &p);
    }
    const f32 max_speed = d->type < GR_DRONE_TYPE_COUNT ? rules->drones.types.items[d->type].max_speed : 1.0f;
    const f32 rate = (1.25f - scale * 0.3f) * (0.9f + v3_length(d->velocity) / m_maxf(max_speed, 1e-3f) * 0.25f);
    gr_audio_loop_want(a, it->entities[i], GR_SOUND_DRONE_HUM, d->position, 0.35f, rate, GR_AUDIO_LOOP_SEND);
  }
}

// bolt impacts where a bolt struck the player or a wall
ecs_system(gr_audio_bolt_system, EcsIn(GrBolt), EcsResInOut(GrAudio), EcsResIn(GrClock)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  if (!a->ready || !a->unlocked)
    return;
  const u32 frame = ecs_sys_get_resource(GrClock)->frame;
  for (i32 i = 0; i < it->count; i++) {
    const GrBolt *b = ecs_sys_get_component(GrBolt, i);
    if (b->end_frame != frame || (b->state != GR_BOLT_HIT_PLAYER && b->state != GR_BOLT_HIT_WALL))
      continue;
    GrSfxPlay p = gr_sfx_site(GR_SITE_BOLT_IMPACT);
    p.position = b->hit_point;
    if (b->state == GR_BOLT_HIT_WALL && b->splash > 0.0f)
      p.volume = 1.2f; // a player hit reports splash 0
    gr_audio_play(a, &p);
  }
}

// a weapon sound at its call site: fire, dry fire, the reload cues, shell insert, pump, weapon pickup, impact
hz_internal GrSfxPlay gr_audio_weapon_play(const GrSoundEvent *s) {
  GrSfxPlay p = {0};
  switch (s->sound) {
  case GR_SND_RIFLE: p = gr_sfx_site(GR_SITE_FIRE_RIFLE); break;
  case GR_SND_BREACHER: p = gr_sfx_site(GR_SITE_FIRE_BREACHER); break;
  case GR_SND_LANCER: p = gr_sfx_site(GR_SITE_FIRE_LANCER); break;
  case GR_SND_DRY_FIRE: p = gr_sfx_site(GR_SITE_DRY_FIRE); break;
  case GR_SND_RELOAD_OUT: p = gr_sfx_site(GR_SITE_RELOAD_CUE), p.sound = GR_SOUND_RELOAD_OUT; break;
  case GR_SND_RELOAD_IN: p = gr_sfx_site(GR_SITE_RELOAD_CUE), p.sound = GR_SOUND_RELOAD_IN; break;
  case GR_SND_RELOAD_CHARGE: p = gr_sfx_site(GR_SITE_RELOAD_CUE), p.sound = GR_SOUND_RELOAD_CHARGE; break;
  case GR_SND_COIL_CHARGE: p = gr_sfx_site(GR_SITE_RELOAD_CUE), p.sound = GR_SOUND_COIL_CHARGE; break;
  case GR_SND_SHELL_INSERT: p = gr_sfx_site(GR_SITE_SHELL_INSERT); break;
  case GR_SND_PUMP: p = gr_sfx_site(GR_SITE_PUMP); break;
  case GR_SND_WEAPON_PICKUP: p = gr_sfx_site(GR_SITE_WEAPON_PICKUP); break;
  case GR_SND_IMPACT: p = gr_sfx_site(GR_SITE_IMPACT); break;
  default: p.sound = GR_SOUND_COUNT; break; // gr_audio_play logs it
  }
  if (s->positional)
    p.position = s->position;
  return p;
}

// the weapon's sounds, a metal hit at each drone hit point, the health pack pickup
ecs_system(gr_audio_weapon_system, EcsResInOut(GrAudio), EcsResIn(GrWeaponState), EcsResIn(GrPickupState)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  if (!a->ready || !a->unlocked)
    return;
  const GrWeaponEvents *ev = &ecs_sys_get_resource(GrWeaponState)->events;
  for (u32 k = 0; k < ev->sound_count; k++) {
    const GrSfxPlay p = gr_audio_weapon_play(&ev->sounds[k]);
    gr_audio_play(a, &p);
  }
  for (u32 k = 0; k < ev->drone_hit_count; k++) {
    GrSfxPlay p = gr_sfx_site(GR_SITE_METAL_HIT);
    p.position = ev->drone_hits[k].point;
    gr_audio_play(a, &p);
  }
  if (ecs_sys_get_resource(GrPickupState)->heal_event) {
    const GrSfxPlay p = gr_sfx_site(GR_SITE_HEALTH_PICKUP);
    gr_audio_play(a, &p);
  }
}

// the player's events, drone kills and the wave director's serials
ecs_system(gr_audio_events_system, EcsResInOut(GrAudio), EcsResIn(GrPlayerState), EcsResIn(GrDroneField),
           EcsResIn(GrWaves), EcsResIn(GrDroneRules), EcsResIn(GrFlow), EcsResIn(GrClock)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  const GrWaves *wv = ecs_sys_get_resource(GrWaves);
  if (!a->ready || !a->unlocked) {
    a->waves_announce_seen = wv->announce_serial;
    a->waves_cleared_seen = wv->cleared_serial;
    a->waves_victory_seen = wv->victory_serial;
    return;
  }
  const GrPlayerEvents *pe = &ecs_sys_get_resource(GrPlayerState)->events;
  const GrDroneField *field = ecs_sys_get_resource(GrDroneField);
  const GrDroneRules *rules = ecs_sys_get_resource(GrDroneRules);
  const b32 playing = ecs_sys_get_resource(GrFlow)->state == GR_STATE_PLAYING;
  const u32 frame = ecs_sys_get_resource(GrClock)->frame;

  if (pe->jumped) {
    const GrSfxPlay p = gr_sfx_site(GR_SITE_JUMP);
    gr_audio_play(a, &p);
  }
  if (pe->landed) {
    GrSfxPlay p = gr_sfx_site(GR_SITE_LAND);
    p.volume = m_minf(1.2727921f, pe->land_impact * 0.101015255f); // min(0.9, impact / 14), +3 dB
    gr_audio_play(a, &p);
  }
  if (pe->footstep) {
    GrSfxPlay p = gr_sfx_site(GR_SITE_FOOTSTEP);
    p.volume = pe->footstep_sprinting ? 0.452548325f : 0.311126977f; // 0.32 / 0.22, +3 dB
    gr_audio_play(a, &p);
  }
  if (pe->launched) {
    GrSfxPlay p = gr_sfx_site(GR_SITE_JUMP_PAD);
    p.position = pe->launch_pad;
    gr_audio_play(a, &p);
  }
  for (u32 k = 0; k < pe->hit_count; k++) { // once per hit
    const GrSfxPlay p = gr_sfx_site(GR_SITE_PLAYER_HURT);
    gr_audio_play(a, &p);
  }
  if (pe->died && playing) { // the death ends the run
    const GrSfxPlay p = gr_sfx_site(GR_SITE_GAME_OVER);
    gr_audio_play(a, &p);
  }

  // a destroyed drone: the explosion, then the kill sound
  for (u32 k = 0; field->kill_frame == frame && k < field->kill_count; k++) {
    const GrDroneKill *kill = &field->kills[k];
    const f32 scale = gr_audio_drone_scale(rules, kill->type);
    GrSfxPlay p = gr_sfx_site(GR_SITE_EXPLOSION);
    p.position = kill->position;
    p.volume = 1.2f + scale * 0.4f;
    p.rate = 1.1f - scale * 0.15f;
    gr_audio_play(a, &p);
    const GrSfxPlay kp = gr_sfx_site(GR_SITE_KILL);
    gr_audio_play(a, &kp);
  }

  if (wv->announce_serial != a->waves_announce_seen) { // wave announce (its 2.4 s delay is the director's)
    const GrSfxPlay p = gr_sfx_site(GR_SITE_WAVE_START);
    gr_audio_play(a, &p);
  }
  if (wv->cleared_serial != a->waves_cleared_seen && wv->cleared_wave < (i32)wv->total_waves - 1) {
    const GrSfxPlay p = gr_sfx_site(GR_SITE_WAVE_CLEAR);
    gr_audio_play(a, &p);
  }
  if (wv->victory_serial != a->waves_victory_seen && playing) { // the last wave cleared: victory
    const GrSfxPlay p = gr_sfx_site(GR_SITE_VICTORY);
    gr_audio_play(a, &p);
  }
  a->waves_announce_seen = wv->announce_serial;
  a->waves_cleared_seen = wv->cleared_serial;
  a->waves_victory_seen = wv->victory_serial;
}

hz_internal void gr_audio_meter(GrAudio *a, f32 dt) {
  a->meter_timer += dt;
  if (a->meter_timer < 1.0f)
    return;
  GrAudioMaster *m = a->master;
  u32 tracks = 0, loops = 0;
  for (u32 i = 0; i < GR_AUDIO_TRACK_MAX; i++)
    tracks += handle_is_valid(a->tracks[i].voice) ? 1u : 0u;
  for (u32 i = 0; i < GR_AUDIO_LOOP_MAX; i++)
    loops += handle_is_valid(a->loops[i].voice) ? 1u : 0u;
  const f32 rms = m->meter_frames ? (f32)m_sqrt(m->meter_sq / (f64)(2u * m->meter_frames)) : 0.0f;
  log_info("gr_audio: voices % (positional % loops %) plays % dropped % | master rms % dBFS peak % dBFS gain % | "
           "compressor % dB | suspended %",
           fmt_u32(ha_count(&a->system.voices)), fmt_u32(tracks), fmt_u32(loops), fmt_u32(a->plays),
           fmt_u32(a->dropped), fmt_f32_p(rms > 1e-9f ? 6.0205999f * m_log2f(rms) : -180.0f, 1),
           fmt_f32_p(m->meter_peak > 1e-9f ? 6.0205999f * m_log2f(m->meter_peak) : -180.0f, 1),
           fmt_f32_p(m->gain, 3), fmt_f32_p(pg_compressor_reduction_db(&m->comp), 2), fmt_b32(a->suspended));
  m->meter_sq = 0.0;
  m->meter_peak = 0.0f;
  m->meter_frames = 0;
  a->meter_timer = 0.0f;
}

// dev: every variant as a 16-bit WAV at its stored (peak 0.99) level, with the float level in the log
hz_internal void gr_audio_dump(GrAudio *a) {
  os_create_dir("out");
  os_create_dir("out/gr_audio");
  tctx_temp_allocator_scope(ta, NULL) {
    for (u32 i = 0; i < a->jobs.len; i++) {
      const GrSfxJob *job = &a->jobs.items[i];
      const AudioSource *src = ha_get(&a->system.sources, job->clip, AudioSource);
      if (!src)
        continue;
      WavFile wav = create_wav_from_samples((i16 *)src->pcm, job->frames, GR_SYN_SR);
      Array(u8) bytes = wav_write_file_alloc(&wav, &ta.allocator);
      CString path = str_to_cstr(str_format(&ta.allocator, "out/gr_audio/%_%.wav", fmt_cstr(GR_RECIPES[job->sound].name),
                                            fmt_u32(job->variant)),
                                 &ta.allocator);
      os_write_file(path, bytes.items, bytes.len);
      log_info("gr_audio: dump % frames % peak % rms % level %", fmt_cstr(path), fmt_u32(job->frames),
               fmt_f32_p(job->peak, 4), fmt_f32_p(job->rms, 4), fmt_f32_p(job->level, 4));
    }
  }
}

// first in the frame, so this frame's run-start events are heard: unlock, master volume, the ambience, suspend
ecs_system(gr_audio_begin_system, EcsResInOut(GrAudio), EcsResIn(GrFlow), EcsResIn(GrAudioDevState),
           EcsResIn(GrSettings)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  const GrFlow *flow = ecs_sys_get_resource(GrFlow);
  const GrAudioDevState *dev = ecs_sys_get_resource(GrAudioDevState);
  if (!a->ready)
    return;
  if (dev->dump)
    gr_audio_dump(a);

  // the settings volume slider (sanitised [0, 1] by the flow); the dev override wins when set
  const f32 volume = dev->volume > 0.0f ? m_minf(dev->volume, 1.0f) : ecs_sys_get_resource(GrSettings)->volume;
  if (volume != a->master->target)
    a->master->target = volume;

  if (!a->unlocked && (flow->state == GR_STATE_PLAYING || dev->unlock)) {
    a->unlocked = true;
    a->master->gain = a->master->target; // no ease on the first unlock
    log_info("gr_audio: unlocked (state %), master %, % Hz out", fmt_u32(flow->state), fmt_f32_p(a->master->target, 2),
             fmt_i32(a->system.output_sample_rate));
  }
  if (!a->unlocked)
    return;
  // the ambience starts with the first run and is never stopped
  if (!a->ambience_started && flow->state == GR_STATE_PLAYING) {
    for (u32 i = 0; i < GR_AUDIO_LOOP_MAX && !a->ambience_started; i++) {
      if (handle_is_valid(a->loops[i].voice))
        continue;
      gr_audio_loop_start(a, &a->loops[i], ECS_ENTITY_INVALID, GR_SOUND_AMBIENCE, false, (v3){0},
                          GR_AUDIO_AMBIENCE_VOLUME, 1.0f, 0.0f);
      a->ambience_started = true;
    }
  }
  // pause and a hidden window suspend the mixer, resume and quit-to-title resume it (the flow decides)
  const b32 suspend = flow->audio_suspended;
  if (suspend != a->suspended) {
    a->suspended = suspend;
    log_info("gr_audio: %", fmt_cstr(suspend ? "suspended" : "resumed"));
  }
}

// last: hums whose drone is gone fade out, voice gains and pans follow the listener, then the mixer tick
ecs_system(gr_audio_frame_system, EcsResInOut(GrAudio), EcsResIn(GrAudioDevState)) {
  GrAudio *a = ecs_sys_get_resource_mut(GrAudio);
  const GrAudioDevState *dev = ecs_sys_get_resource(GrAudioDevState);
  const f32 dt = ecs_resource_get(it->world, AppMemory)->dt; // the mixer runs on real time, not the game's
  if (!a->ready || !a->unlocked) {
    a->frame++;
    return;
  }
  for (u32 i = 0; i < GR_AUDIO_LOOP_MAX; i++) {
    GrAudioLoopVoice *l = &a->loops[i];
    if (handle_is_valid(l->voice) && l->entity != ECS_ENTITY_INVALID && !l->stopping && l->seen_frame != a->frame)
      gr_audio_loop_stop(l);
  }
  a->frame++;
  if (a->suspended)
    return;
  gr_audio_voices_update(a, dt);
  if (dev->meter)
    gr_audio_meter(a, dt);
  audio_system_update(&a->system, dt);
}

/* ------------------------------------------------------------ dev: listening checks */

hz_internal GrSfxPlay gr_audio_dev_play(const GrAudioDev *d, u32 sound) {
  return (GrSfxPlay){
      .sound = sound,
      .positional = d->play_positional,
      .position = {d->play_x, d->play_y, d->play_z},
      .volume = d->play_volume > 0.0f ? d->play_volume : (d->play_positional ? 1.0f : 1.41421354f),
      .rate = 1.0f,
      .jitter = 0.04f,
      .reverb = 0.25f,
  };
}

// the dev entity's requests through its emitter, and the dev switches the audio systems read
// reads only resources final before the audio scope (GrClock, GrFlow): resource terms order reader-after-writer only
ecs_system(gr_audio_dev_system, EcsInOut(GrAudioDev), EcsInOut(GrSfxEmitter), EcsResInOut(GrAudioDevState),
           EcsResIn(GrClock), EcsResIn(GrFlow)) {
  GrAudioDevState *st = ecs_sys_get_resource_mut(GrAudioDevState);
  const GrClock *clock = ecs_sys_get_resource(GrClock);
  const f32 dt = ecs_resource_get(it->world, AppMemory)->dt;
  for (i32 i = 0; i < it->count; i++) {
    const GrAudioDev *d = ecs_sys_get_component(GrAudioDev, i);
    st->dump = d->dump;
    st->meter = d->meter;
    st->unlock = d->unlock;
    st->volume = d->volume;
    if (d->play > 0 && (u32)d->play <= GR_SOUND_COUNT) {
      gr_sfx_emit(ecs_sys_get_component_mut(GrSfxEmitter, i), clock->frame, gr_audio_dev_play(d, (u32)d->play - 1u));
      log_info("gr_audio: dev play % (%)", fmt_cstr(GR_RECIPES[d->play - 1].name), fmt_b32(d->play_positional));
      ecs_sys_get_component_mut(GrAudioDev, i)->play = 0;
    }
    if (d->dump)
      ecs_sys_get_component_mut(GrAudioDev, i)->dump = false;
    if (d->all && ecs_sys_get_resource(GrFlow)->state == GR_STATE_PLAYING) {
      GrAudioDev *m = ecs_sys_get_component_mut(GrAudioDev, i);
      m->all_timer -= dt;
      if (m->all_timer <= 0.0f) {
        gr_sfx_emit(ecs_sys_get_component_mut(GrSfxEmitter, i), clock->frame, gr_audio_dev_play(d, m->all_next));
        log_info("gr_audio: dev all % %", fmt_u32(m->all_next), fmt_cstr(GR_RECIPES[m->all_next].name));
        m->all_timer = GR_AUDIO_DEV_ALL_GAP_S;
        if (++m->all_next >= GR_SOUND_COUNT) {
          m->all = false;
          m->all_next = 0;
        }
      }
    }
  }
}

/* ------------------------------------------------------------ init and schedule */

void gr_audio_init(EcsWorld *w) {
  Allocator alloc = make_arena_allocator(arena_create(MB(32), KB(64)));
  GrAudio *a = ALLOC(&alloc, GrAudio);
  a->alloc = alloc;
  // room for GR_MAX_DRONES hums under a volley's impacts, casings and drone fire
  a->system = audio_system_init(&a->alloc, &(AudioSystemDesc){.voice_pool = 256, .max_active = 96, .source_pool = 96});
  a->rng = random_create(0x6A7D10ull);

  a->hall = ALLOC(&a->alloc, PgFreeverb);
  pg_freeverb_init(a->hall, PG_AUDIO_RATE, GR_AUDIO_HALL_ROOM, GR_AUDIO_HALL_DAMP, 1.0f, 1.0f);
  audio_send_set_effect(&a->system, pg_freeverb_bus_effect, a->hall);

  a->master = ALLOC(&a->alloc, GrAudioMaster);
  const PgCompressorDesc comp = GR_AUDIO_COMPRESSOR;
  pg_compressor_init(&a->master->comp, (u32)a->system.output_sample_rate, &comp,
                     ALLOC_ARRAY(&a->alloc, f32, PG_COMPRESSOR_STORAGE));
  a->master->gain = a->master->target = GR_AUDIO_VOLUME_DEFAULT;
  audio_bus_set_effect(&a->system, AUDIO_BUS_MASTER, gr_audio_master_fx, a->master);

  // one job per variant, each with its own staging range so the render needs no synchronisation
  u32 count = 0;
  for (u32 s = 0; s < GR_SOUND_COUNT; s++)
    count += GR_RECIPES[s].variants;
  a->jobs = (Array(GrSfxJob)){.len = count, .items = ALLOC_ARRAY(&a->alloc, GrSfxJob, count)};
  a->staging = arena_create(MB(16), KB(64));
  Allocator staging = make_arena_allocator(a->staging);
  u32 at = 0;
  for (u32 s = 0; s < GR_SOUND_COUNT; s++) {
    const GrRecipe *r = &GR_RECIPES[s];
    const u32 n = gr_recipe_frames(r);
    a->first_job[s] = at;
    for (u32 v = 0; v < r->variants; v++, at++) {
      GrSfxJob *job = &a->jobs.items[at];
      job->sound = s;
      job->variant = v;
      job->seed = 1000u + 7919u * v + (u64)cstr_len(r->name);
      job->frames = r->loop ? n - gr_recipe_loop_fade(n) : n;
      job->pcm = ALLOC_ARRAY(&staging, i16, job->frames);
    }
  }

  ecs_resource_set_ref(w, GrAudio, a);
  ecs_resource_set_ref(w, Array(GrSfxJob), &a->jobs);
  ecs_resource_set(w, GrAudioDevState, ((GrAudioDevState){0}));

  EcsEntity dev = ecs_entity_create(w);
  ecs_component_set(w, dev, GrAudioDev, {0});
  ecs_component_set(w, dev, GrSfxEmitter, {0});
  log_info("gr_audio: % render jobs, mixer % Hz", fmt_u32(count), fmt_i32(a->system.output_sample_rate));
}

void gr_audio_schedule(EcsWorld *w) {
  if (!ecs_resource_get(w, GrAudio)->ready) {
    ecs_schedule_array(w, gr_audio_render_system);
    ecs_schedule_array_single(w, gr_audio_sources_system); // registers AudioSources: the AudioSystem has one writer
  }
  ecs_schedule_single(w, gr_audio_dev_system);      // one dev entity; writes the dev switches resource
  ecs_schedule_single(w, gr_audio_begin_system);    // the AudioSystem has one writer: unlock, suspend, ambience
  ecs_schedule_single(w, gr_audio_listener_system); // same writer: the listener
  ecs_schedule_single(w, gr_audio_emit_system);     // same writer: requested one-shots
  ecs_schedule_single(w, gr_audio_events_system);   // same writer: player, kill and wave sounds
  ecs_schedule_single(w, gr_audio_weapon_system);   // same writer: weapon, metal-hit and health-pack sounds
  ecs_schedule_single(w, gr_audio_drone_system);    // same writer: drone one-shots and hum loops
  ecs_schedule_single(w, gr_audio_bolt_system);     // same writer: bolt impacts
  ecs_schedule_single(w, gr_audio_frame_system);    // same writer: voice gains, pans and the mixer tick
}
