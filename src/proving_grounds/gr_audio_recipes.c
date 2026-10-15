// Proving Grounds sound recipes: a small synthesis kit (params, oscillators, noise, filters, drive) and 34 recipes on pg_audio.

#define GR_SYN_SR PG_AUDIO_RATE
// square/saw are scaled by the peak of a 2047-partial band-limited table (Gibbs overshoot)
#define GR_SYN_PERIODIC_NORM 0.8482f
#define GR_SYN_DRIVE_CURVE 1024u // the drive's shaping curve length

typedef enum GrNoiseColor {
  GR_NOISE_WHITE = 0,
  GR_NOISE_PINK,
  GR_NOISE_BROWN,
} GrNoiseColor;

typedef enum GrParamEvKind {
  GR_EV_SET = 0, // jump to the value at the time
  GR_EV_LIN,     // linear ramp to the value, ending at the time
  GR_EV_EXP,     // exponential ramp to the value, ending at the time
} GrParamEvKind;

typedef struct GrParamEv {
  u32 kind;
  f32 time;
  f32 value;
} GrParamEv;

// an automated parameter: the base value plus its automation events, in time order
typedef struct GrParam {
  f32 base;
  u32 count;
  u32 cap;
  GrParamEv *ev;
} GrParam;

// one render: the job's scratch allocator, its rng and the destination
typedef struct GrSyn {
  Allocator *alloc;
  Random *rng;
  u32 sr;
  u32 n;
  f32 *out;
  f32 *aux; // one param timeline at a time
} GrSyn;

typedef void GrRecipeFn(GrSyn *s);

typedef struct GrRecipe {
  const char *name; // the sound's name; its length seeds the variants
  f64 duration;     // seconds, f64: the frame count is ceil(duration * rate) in doubles
  u32 variants;
  b32 loop;
  GrRecipeFn *fn;
} GrRecipe;

hz_internal f32 gr_rng(GrSyn *s) { return random_f32(s->rng); }

hz_internal f32 *gr_buf(GrSyn *s) { return ALLOC_ARRAY(s->alloc, f32, s->n); }

// the first frame at or after t
hz_internal u32 gr_frame(const GrSyn *s, f32 t) {
  if (t <= 0.0f)
    return 0;
  f32 f = m_ceilf(t * (f32)s->sr - 1e-3f);
  return f >= (f32)s->n ? s->n : (u32)f;
}

hz_internal GrParam gr_param_cap(GrSyn *s, f32 base, u32 cap) {
  return (GrParam){.base = base, .cap = cap, .ev = ALLOC_ARRAY(s->alloc, GrParamEv, cap)};
}

hz_internal GrParam gr_param(GrSyn *s, f32 base) { return gr_param_cap(s, base, 6); }

// a param with no automation
hz_internal GrParam gr_const(f32 v) { return (GrParam){.base = v}; }

hz_internal void gr_ev(GrParam *p, u32 kind, f32 value, f32 time) {
  assert_msg(p->count < p->cap, "gr_audio: a recipe param overflows its event list");
  p->ev[p->count++] = (GrParamEv){.kind = kind, .time = time, .value = value};
}

// envelope: 1e-4 at `at`, linear to the peak, exponential back to 1e-4
hz_internal void gr_env(GrParam *p, f32 at, f32 attack, f32 decay, f32 peak) {
  gr_ev(p, GR_EV_SET, 0.0001f, at);
  gr_ev(p, GR_EV_LIN, peak, at + attack);
  gr_ev(p, GR_EV_EXP, 0.0001f, at + attack + decay);
}

// sweep: set `from` at `at`, exponential to `to`
hz_internal void gr_sweep(GrParam *p, f32 from, f32 to, f32 at, f32 duration) {
  gr_ev(p, GR_EV_SET, from, at);
  gr_ev(p, GR_EV_EXP, to, at + duration);
}

// the param timeline per frame: a ramp runs from the previous event; an exponential ramp across a sign change holds
hz_internal void gr_param_render(const GrSyn *s, const GrParam *p, f32 *out) {
  const f64 sr = (f64)s->sr;
  u32 i = 0;
  f64 t0 = 0.0, v0 = p->base;
  for (u32 k = 0; k < p->count && i < s->n; k++) {
    const GrParamEv *e = &p->ev[k];
    const u32 end = gr_frame(s, e->time);
    const f64 span = (f64)e->time - t0;
    if (e->kind == GR_EV_LIN && span > 0.0) {
      for (; i < end; i++)
        out[i] = (f32)(v0 + ((f64)e->value - v0) * ((f64)i / sr - t0) / span);
    } else if (e->kind == GR_EV_EXP && span > 0.0 && v0 * (f64)e->value > 0.0) {
      const f64 rate = m_log((f64)e->value / v0) / span;
      f64 v = v0 * m_exp(rate * ((f64)i / sr - t0));
      const f64 step = m_exp(rate / sr);
      for (; i < end; i++, v *= step)
        out[i] = (f32)v;
    } else {
      for (; i < end; i++)
        out[i] = (f32)v0;
    }
    t0 = e->time;
    v0 = e->value;
  }
  for (; i < s->n; i++)
    out[i] = (f32)v0;
}

// a noise burst of `seconds` started at `at`: white, pink or brown
hz_internal void gr_noise(GrSyn *s, f32 *dst, f32 seconds, u32 color, f32 at) {
  const u32 len = MAX((u32)(seconds * (f32)s->sr), 1u);
  const u32 start = gr_frame(s, at);
  f32 b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, last = 0.0f;
  for (u32 i = 0; i < len && start + i < s->n; i++) {
    const f32 white = random_f32(s->rng) * 2.0f - 1.0f;
    f32 v = white;
    if (color == GR_NOISE_PINK) {
      b0 = 0.99765f * b0 + white * 0.099046f;
      b1 = 0.963f * b1 + white * 0.2965164f;
      b2 = 0.57f * b2 + white * 1.0526913f;
      v = (b0 + b1 + b2 + white * 0.1848f) * 0.2f;
    } else if (color == GR_NOISE_BROWN) {
      last = (last + 0.02f * white) / 1.02f;
      v = last * 3.5f;
    }
    dst[start + i] = v;
  }
}

// an oscillator from `at` to `stop` (< 0: never); waves start at 0 rising, square at +1
hz_internal void gr_osc(GrSyn *s, f32 *dst, PgOscShape shape, const GrParam *freq, f32 detune_cents, f32 at, f32 stop) {
  const u32 a = gr_frame(s, at), b = stop < 0.0f ? s->n : gr_frame(s, stop);
  if (freq->count)
    gr_param_render(s, freq, s->aux);
  const f32 detune = m_exp2f(detune_cents / 1200.0f);
  const f32 norm = (shape == PG_OSC_SAW || shape == PG_OSC_SQUARE) ? GR_SYN_PERIODIC_NORM : 1.0f;
  const f32 phase0 = shape == PG_OSC_SAW ? 0.5f : (shape == PG_OSC_TRI ? 0.75f : 0.0f);
  const f32 f0 = (freq->count && a < s->n ? s->aux[a] : freq->base) * detune;
  PgOsc o = {.phase = phase0 - f0 / (f32)s->sr}; // pg_osc_blep_next advances before it reads
  if (o.phase < 0.0f)
    o.phase += 1.0f;
  for (u32 i = a; i < b; i++) {
    const f32 f = (freq->count ? s->aux[i] : freq->base) * detune;
    dst[i] = pg_osc_blep_next(&o, s->sr, f, shape) * norm;
  }
}

// a resonant filter; lowpass/highpass Q is in dB, bandpass Q linear
hz_internal void gr_filter(GrSyn *s, f32 *buf, PgSvfMode mode, const GrParam *freq, f32 q) {
  const f32 q_lin = mode == PG_SVF_BANDPASS ? q : pg_q_from_db(q);
  PgSvf f = {0};
  if (!freq->count) {
    pg_svf_set(&f, s->sr, mode, freq->base, q_lin);
    pg_svf_process(&f, buf, s->n);
    return;
  }
  gr_param_render(s, freq, s->aux);
  for (u32 i = 0; i < s->n; i++) {
    pg_svf_set(&f, s->sr, mode, s->aux[i], q_lin);
    buf[i] = pg_svf_next(&f, buf[i]);
  }
}

hz_internal void gr_filter_c(GrSyn *s, f32 *buf, PgSvfMode mode, f32 hz, f32 q) {
  const GrParam p = gr_const(hz);
  gr_filter(s, buf, mode, &p, q);
}

// a gain on `buf`, constant or automated
hz_internal void gr_gain(GrSyn *s, f32 *buf, const GrParam *g) {
  if (!g->count) {
    for (u32 i = 0; i < s->n; i++)
      buf[i] *= g->base;
    return;
  }
  gr_param_render(s, g, s->aux);
  for (u32 i = 0; i < s->n; i++)
    buf[i] *= s->aux[i];
}

// a gain modulated at audio rate (the tremolo LFOs)
hz_internal void gr_gain_mod(GrSyn *s, f32 *buf, f32 base, const f32 *mod) {
  for (u32 i = 0; i < s->n; i++)
    buf[i] *= base + mod[i];
}

hz_internal void gr_env_on(GrSyn *s, f32 *buf, f32 at, f32 attack, f32 decay, f32 peak) {
  GrParam g = gr_param(s, 0.0f);
  gr_env(&g, at, attack, decay, peak);
  gr_gain(s, buf, &g);
}

hz_internal void gr_mix(const GrSyn *s, f32 *dst, const f32 *src) {
  for (u32 i = 0; i < s->n; i++)
    dst[i] += src[i];
}

// drive: a curve of tanh(x * amount) / tanh(amount), linear between curve points, clamped outside [-1, 1]
hz_internal void gr_drive(GrSyn *s, f32 *buf, f32 amount) {
  f32 *curve = ALLOC_ARRAY(s->alloc, f32, GR_SYN_DRIVE_CURVE);
  const f32 norm = 1.0f / m_tanhf(amount);
  for (u32 k = 0; k < GR_SYN_DRIVE_CURVE; k++)
    curve[k] = m_tanhf(((f32)k / (f32)(GR_SYN_DRIVE_CURVE - 1) * 2.0f - 1.0f) * amount) * norm;
  const f32 last = (f32)(GR_SYN_DRIVE_CURVE - 1);
  for (u32 i = 0; i < s->n; i++) {
    const f32 v = last * 0.5f * (buf[i] + 1.0f);
    if (v <= 0.0f) {
      buf[i] = curve[0];
    } else if (v >= last) {
      buf[i] = curve[GR_SYN_DRIVE_CURVE - 1];
    } else {
      const u32 k = (u32)v;
      const f32 t = v - (f32)k;
      buf[i] = curve[k] + (curve[k + 1] - curve[k]) * t;
    }
  }
}

// noise -> filter -> env: the commonest chain in the recipes
hz_internal void gr_noise_layer(GrSyn *s, f32 *dst, f32 seconds, u32 color, f32 at, PgSvfMode mode, f32 hz, f32 q,
                                f32 env_at, f32 attack, f32 decay, f32 peak) {
  f32 *x = gr_buf(s);
  gr_noise(s, x, seconds, color, at);
  gr_filter_c(s, x, mode, hz, q);
  gr_env_on(s, x, env_at, attack, decay, peak);
  gr_mix(s, dst, x);
}

// osc -> env with a constant frequency
hz_internal void gr_tone_layer(GrSyn *s, f32 *dst, PgOscShape shape, f32 hz, f32 at, f32 stop, f32 env_at, f32 attack,
                               f32 decay, f32 peak) {
  f32 *x = gr_buf(s);
  const GrParam f = gr_const(hz);
  gr_osc(s, x, shape, &f, 0.0f, at, stop);
  gr_env_on(s, x, env_at, attack, decay, peak);
  gr_mix(s, dst, x);
}

// osc with an exponential sweep -> env
hz_internal void gr_sweep_layer(GrSyn *s, f32 *dst, PgOscShape shape, f32 from, f32 to, f32 sweep_s, f32 stop,
                                f32 attack, f32 decay, f32 peak) {
  f32 *x = gr_buf(s);
  GrParam f = gr_param(s, from);
  gr_sweep(&f, from, to, 0.0f, sweep_s);
  gr_osc(s, x, shape, &f, 0.0f, 0.0f, stop);
  gr_env_on(s, x, 0.0f, attack, decay, peak);
  gr_mix(s, dst, x);
}

/* ------------------------------------------------------------ recipes (GrSound order) */

// the recipes' literal frequency/time tables
hz_global const f32 GR_IMPACT_RINGS[3] = {2150.0f, 3270.0f, 4900.0f};
hz_global const f32 GR_METAL_PARTIALS[4] = {1320.0f, 2760.0f, 4410.0f, 6100.0f};
hz_global const f32 GR_CASING_PINGS[3] = {3900.0f, 5300.0f, 7200.0f};
hz_global const f32 GR_KILL_NOTES[3][2] = {{1250.0f, 0.0f}, {1870.0f, 0.075f}, {2500.0f, 0.15f}};
hz_global const f32 GR_HUM_PARTIALS[3][2] = {{110.0f, 0.18f}, {110.5f, 0.18f}, {220.0f, 0.06f}};
hz_global const f32 GR_PUMP_STROKES[2][3] = {{0.0f, 1300.0f, 0.45f}, {0.19f, 1700.0f, 0.6f}};
hz_global const f32 GR_HEALTH_NOTES[3][2] = {{660.0f, 0.04f}, {880.0f, 0.13f}, {1320.0f, 0.22f}};
hz_global const f32 GR_WAVE_START_CHORD[4] = {110.0f, 164.81f, 220.0f, 277.18f};
hz_global const f32 GR_WAVE_CLEAR_NOTES[4] = {523.25f, 659.25f, 783.99f, 1046.5f};
hz_global const f32 GR_VICTORY_CHORD[4] = {261.63f, 329.63f, 392.0f, 523.25f};
hz_global const f32 GR_GAME_OVER_CHORD[3] = {220.0f, 207.65f, 164.81f};

hz_internal void gr_rec_rifle(GrSyn *s) {
  f32 *bus = gr_buf(s);
  gr_noise_layer(s, bus, 0.15f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 2400.0f + gr_rng(s) * 900.0f, 0.7f, 0.0f, 0.0008f,
                 0.05f, 0.85f);
  gr_noise_layer(s, bus, 0.3f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 850.0f + gr_rng(s) * 350.0f, 0.9f, 0.0f, 0.002f,
                 0.13f, 1.1f);
  gr_sweep_layer(s, bus, PG_OSC_SINE, 170.0f, 42.0f, 0.13f, 0.35f, 0.001f, 0.17f, 1.2f);
  gr_tone_layer(s, bus, PG_OSC_SQUARE, 3000.0f + gr_rng(s) * 800.0f, 0.0f, 0.05f, 0.003f, 0.0005f, 0.012f, 0.1f);
  gr_noise_layer(s, bus, 0.55f, GR_NOISE_BROWN, 0.0f, PG_SVF_LOWPASS, 650.0f, 0.5f, 0.01f, 0.02f, 0.42f, 0.55f);
  gr_drive(s, bus, 2.2f);
  gr_mix(s, s->out, bus);
}

hz_internal void gr_rec_dry_fire(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.05f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 3200.0f, 3.0f, 0.0f, 0.0005f, 0.03f, 0.6f);
}

hz_internal void gr_rec_impact(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.1f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 1400.0f + gr_rng(s) * 800.0f, 0.7f, 0.0f,
                 0.0008f, 0.05f, 0.55f);
  gr_noise_layer(s, s->out, 0.12f, GR_NOISE_BROWN, 0.0f, PG_SVF_LOWPASS, 500.0f, 0.7f, 0.0f, 0.001f, 0.07f, 0.6f);
  for (u32 k = 0; k < 3; k++) {
    const f32 hz = GR_IMPACT_RINGS[k] * (0.85f + gr_rng(s) * 0.3f);
    const f32 decay = 0.1f + gr_rng(s) * 0.12f;
    const f32 peak = 0.05f + gr_rng(s) * 0.05f;
    gr_tone_layer(s, s->out, PG_OSC_SINE, hz, 0.0f, 0.35f, 0.001f, 0.001f, decay, peak);
  }
}

hz_internal void gr_rec_metal_hit(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.08f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 2600.0f, 1.2f, 0.0f, 0.0005f, 0.04f, 0.7f);
  for (u32 k = 0; k < 4; k++) {
    const f32 hz = GR_METAL_PARTIALS[k] * (0.93f + gr_rng(s) * 0.14f);
    gr_tone_layer(s, s->out, PG_OSC_TRI, hz, 0.0f, 0.4f, 0.0f, 0.001f, 0.18f + gr_rng(s) * 0.15f, 0.09f);
  }
}

hz_internal void gr_rec_casing(GrSyn *s) {
  for (u32 k = 0; k < 3; k++) {
    const f32 hz = GR_CASING_PINGS[k] * (0.9f + gr_rng(s) * 0.2f);
    gr_tone_layer(s, s->out, PG_OSC_SINE, hz, 0.0f, 0.3f, 0.0f, 0.0005f, 0.07f + gr_rng(s) * 0.1f, 0.07f);
  }
}

hz_internal void gr_rec_hitmarker(GrSyn *s) {
  gr_tone_layer(s, s->out, PG_OSC_SINE, 1900.0f, 0.0f, 0.1f, 0.0f, 0.0008f, 0.05f, 0.35f);
  gr_tone_layer(s, s->out, PG_OSC_SINE, 3800.0f, 0.0f, 0.1f, 0.0f, 0.0008f, 0.05f, 0.12f);
}

hz_internal void gr_rec_kill(GrSyn *s) {
  for (u32 k = 0; k < 3; k++) {
    const f32 t = GR_KILL_NOTES[k][1];
    gr_tone_layer(s, s->out, PG_OSC_TRI, GR_KILL_NOTES[k][0], t, t + 0.3f, t, 0.002f, 0.16f, 0.28f);
  }
}

hz_internal void gr_rec_explosion(GrSyn *s) {
  f32 *bus = gr_buf(s);
  gr_sweep_layer(s, bus, PG_OSC_SINE, 75.0f, 26.0f, 0.7f, 1.5f, 0.004f, 1.0f, 1.3f);

  f32 *blast = gr_buf(s);
  gr_noise(s, blast, 1.8f, GR_NOISE_WHITE, 0.0f);
  GrParam cutoff = gr_param(s, 4500.0f);
  gr_sweep(&cutoff, 4500.0f, 180.0f, 0.0f, 1.3f);
  gr_filter(s, blast, PG_SVF_LOWPASS, &cutoff, 0.6f);
  gr_env_on(s, blast, 0.0f, 0.003f, 1.2f, 1.1f);
  gr_mix(s, bus, blast);

  // 45 crackle grains in [0.04, 0.94) s, sorted; a grain within 4 ms of the last one's end is skipped
  f32 *crackle = gr_buf(s);
  gr_noise(s, crackle, 1.2f, GR_NOISE_WHITE, 0.0f);
  gr_filter_c(s, crackle, PG_SVF_HIGHPASS, 2500.0f, 0.7f);
  f32 *times = ALLOC_ARRAY(s->alloc, f32, 45);
  for (u32 k = 0; k < 45; k++) {
    const f32 t = 0.04f + gr_rng(s) * 0.9f;
    u32 j = k;
    for (; j > 0 && times[j - 1] > t; j--)
      times[j] = times[j - 1];
    times[j] = t;
  }
  GrParam grains = gr_param_cap(s, 0.0f, 45 * 3);
  f32 prev = 0.0f;
  for (u32 k = 0; k < 45; k++) {
    const f32 t = times[k];
    if (t < prev + 0.004f)
      continue;
    gr_ev(&grains, GR_EV_SET, 0.0f, t);
    gr_ev(&grains, GR_EV_LIN, 0.25f + gr_rng(s) * 0.35f, t + 0.002f);
    gr_ev(&grains, GR_EV_LIN, 0.0f, t + 0.018f);
    prev = t + 0.018f;
  }
  gr_gain(s, crackle, &grains);
  gr_mix(s, bus, crackle);

  gr_noise_layer(s, bus, 2.2f, GR_NOISE_BROWN, 0.0f, PG_SVF_LOWPASS, 220.0f, 0.7f, 0.02f, 0.05f, 1.8f, 0.9f);
  gr_drive(s, bus, 3.0f);
  gr_mix(s, s->out, bus);
}

hz_internal void gr_rec_drone_hum(GrSyn *s) {
  // integer cycle counts over 2 s keep the loop seamless
  f32 *lp = gr_buf(s);
  for (u32 k = 0; k < 3; k++) {
    f32 *x = gr_buf(s);
    const GrParam f = gr_const(GR_HUM_PARTIALS[k][0]);
    gr_osc(s, x, PG_OSC_SAW, &f, 0.0f, 0.0f, -1.0f);
    const GrParam g = gr_const(GR_HUM_PARTIALS[k][1]);
    gr_gain(s, x, &g);
    gr_mix(s, lp, x);
  }
  gr_filter_c(s, lp, PG_SVF_LOWPASS, 700.0f, 1.5f);
  f32 *lfo = gr_buf(s);
  const GrParam lf = gr_const(6.0f);
  gr_osc(s, lfo, PG_OSC_SINE, &lf, 0.0f, 0.0f, -1.0f);
  const GrParam depth = gr_const(0.22f);
  gr_gain(s, lfo, &depth);
  gr_gain_mod(s, lp, 0.75f, lfo);
  gr_mix(s, s->out, lp);
}

hz_internal void gr_rec_drone_fire(GrSyn *s) {
  f32 *bp = gr_buf(s);
  // the random start pitch draw keeps the rng sequence; the sweep's set at 0 overrides the value
  GrParam fa = gr_param(s, 950.0f + gr_rng(s) * 150.0f);
  gr_sweep(&fa, 950.0f, 210.0f, 0.0f, 0.19f);
  gr_osc(s, bp, PG_OSC_SQUARE, &fa, 0.0f, 0.0f, 0.3f);
  f32 *b = gr_buf(s);
  GrParam fb = gr_param(s, 1900.0f + gr_rng(s) * 300.0f);
  gr_sweep(&fb, 1900.0f, 380.0f, 0.0f, 0.16f);
  gr_osc(s, b, PG_OSC_SAW, &fb, 0.0f, 0.0f, 0.3f);
  gr_mix(s, bp, b);
  gr_filter_c(s, bp, PG_SVF_BANDPASS, 1400.0f, 0.8f);
  gr_env_on(s, bp, 0.0f, 0.002f, 0.22f, 0.55f);
  gr_drive(s, bp, 2.0f);
  gr_mix(s, s->out, bp);
  gr_noise_layer(s, s->out, 0.1f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 3000.0f, 0.7f, 0.0f, 0.001f, 0.05f, 0.25f);
}

hz_internal void gr_rec_drone_charge(GrSyn *s) {
  f32 *x = gr_buf(s);
  GrParam f = gr_param(s, 260.0f);
  gr_sweep(&f, 260.0f, 1300.0f, 0.0f, 0.65f);
  gr_osc(s, x, PG_OSC_SINE, &f, 0.0f, 0.0f, 0.8f);
  f32 *lfo = gr_buf(s);
  const GrParam lf = gr_const(28.0f);
  gr_osc(s, lfo, PG_OSC_SQUARE, &lf, 0.0f, 0.0f, 0.8f);
  const GrParam depth = gr_const(0.35f);
  gr_gain(s, lfo, &depth);
  gr_gain_mod(s, x, 0.5f, lfo);
  GrParam g = gr_param(s, 0.0f);
  gr_ev(&g, GR_EV_SET, 0.0001f, 0.0f);
  gr_ev(&g, GR_EV_EXP, 0.5f, 0.6f);
  gr_ev(&g, GR_EV_EXP, 0.0001f, 0.75f);
  gr_gain(s, x, &g);
  gr_mix(s, s->out, x);
}

// 1e-4 at 0, exponential to `peak` at `t_peak`, exponential to 1e-4 at `t_end`
hz_internal void gr_swell_on(GrSyn *s, f32 *buf, f32 peak, f32 t_peak, f32 t_end) {
  GrParam g = gr_param(s, 0.0f);
  gr_ev(&g, GR_EV_SET, 0.0001f, 0.0f);
  gr_ev(&g, GR_EV_EXP, peak, t_peak);
  gr_ev(&g, GR_EV_EXP, 0.0001f, t_end);
  gr_gain(s, buf, &g);
}

hz_internal void gr_rec_warp(GrSyn *s) {
  f32 *swell = gr_buf(s);
  gr_noise(s, swell, 0.7f, GR_NOISE_WHITE, 0.0f);
  GrParam bp = gr_param(s, 300.0f);
  gr_sweep(&bp, 300.0f, 5200.0f, 0.0f, 0.6f);
  gr_filter(s, swell, PG_SVF_BANDPASS, &bp, 4.0f);
  gr_swell_on(s, swell, 0.6f, 0.55f, 0.65f);
  gr_mix(s, s->out, swell);

  f32 *rise = gr_buf(s);
  GrParam rf = gr_param(s, 180.0f);
  gr_sweep(&rf, 180.0f, 1500.0f, 0.0f, 0.6f);
  gr_osc(s, rise, PG_OSC_SINE, &rf, 0.0f, 0.0f, 0.7f);
  gr_swell_on(s, rise, 0.25f, 0.55f, 0.65f);
  gr_mix(s, s->out, rise);

  f32 *pop = gr_buf(s);
  GrParam pf = gr_param(s, 900.0f);
  gr_sweep(&pf, 900.0f, 140.0f, 0.58f, 0.3f);
  gr_osc(s, pop, PG_OSC_SINE, &pf, 0.0f, 0.58f, 1.1f);
  gr_env_on(s, pop, 0.58f, 0.002f, 0.35f, 0.8f);
  gr_mix(s, s->out, pop);

  gr_noise_layer(s, s->out, 0.4f, GR_NOISE_PINK, 0.58f, PG_SVF_LOWPASS, 2500.0f, 0.7f, 0.58f, 0.002f, 0.25f, 0.5f);
}

hz_internal void gr_rec_reload_out(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.05f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 2200.0f, 0.7f, 0.0f, 0.0005f, 0.03f, 0.5f);
  gr_tone_layer(s, s->out, PG_OSC_SINE, 190.0f, 0.0f, 0.2f, 0.01f, 0.002f, 0.08f, 0.5f);
}

hz_internal void gr_rec_reload_in(GrSyn *s) {
  for (u32 k = 0; k < 2; k++) {
    const f32 t = k ? 0.045f : 0.0f;
    gr_noise_layer(s, s->out, 0.05f, GR_NOISE_WHITE, t, PG_SVF_BANDPASS, 2800.0f, 1.5f, t, 0.0005f, 0.035f, 0.6f);
  }
  gr_tone_layer(s, s->out, PG_OSC_TRI, 2450.0f, 0.045f, 0.3f, 0.045f, 0.001f, 0.12f, 0.12f);
}

hz_internal void gr_rec_reload_charge(GrSyn *s) {
  f32 *slide = gr_buf(s);
  gr_noise(s, slide, 0.12f, GR_NOISE_WHITE, 0.0f);
  GrParam f = gr_param(s, 1500.0f);
  gr_sweep(&f, 1500.0f, 3400.0f, 0.0f, 0.09f);
  gr_filter(s, slide, PG_SVF_BANDPASS, &f, 2.0f);
  gr_env_on(s, slide, 0.0f, 0.01f, 0.08f, 0.35f);
  gr_mix(s, s->out, slide);
  gr_noise_layer(s, s->out, 0.08f, GR_NOISE_WHITE, 0.1f, PG_SVF_HIGHPASS, 1800.0f, 0.7f, 0.1f, 0.0005f, 0.04f, 0.7f);
  gr_tone_layer(s, s->out, PG_OSC_SINE, 240.0f, 0.1f, 0.35f, 0.1f, 0.001f, 0.09f, 0.5f);
}

hz_internal void gr_rec_breacher(GrSyn *s) {
  // a broadband crack over a deep chest thump and a long rolling tail
  f32 *bus = gr_buf(s);
  gr_noise_layer(s, bus, 0.2f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 1600.0f + gr_rng(s) * 600.0f, 0.7f, 0.0f, 0.001f,
                 0.08f, 1.0f);
  gr_noise_layer(s, bus, 0.4f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 520.0f + gr_rng(s) * 200.0f, 0.7f, 0.0f, 0.002f,
                 0.2f, 1.3f);
  gr_sweep_layer(s, bus, PG_OSC_SINE, 120.0f, 32.0f, 0.22f, 0.5f, 0.001f, 0.3f, 1.6f);
  gr_noise_layer(s, bus, 1.1f, GR_NOISE_BROWN, 0.0f, PG_SVF_LOWPASS, 420.0f, 0.5f, 0.015f, 0.03f, 0.9f, 0.8f);
  gr_drive(s, bus, 3.0f);
  gr_mix(s, s->out, bus);
}

hz_internal void gr_rec_pump(GrSyn *s) {
  // back (rasp and clack), then forward (clack and a heavier lock)
  for (u32 k = 0; k < 2; k++) {
    const f32 t = GR_PUMP_STROKES[k][0], f = GR_PUMP_STROKES[k][1], peak = GR_PUMP_STROKES[k][2];
    f32 *slide = gr_buf(s);
    gr_noise(s, slide, 0.1f, GR_NOISE_WHITE, t);
    GrParam sf = gr_param(s, f);
    gr_sweep(&sf, f, f * 1.9f, t, 0.07f);
    gr_filter(s, slide, PG_SVF_BANDPASS, &sf, 2.5f);
    gr_env_on(s, slide, t, 0.008f, 0.06f, peak * 0.6f);
    gr_mix(s, s->out, slide);
    gr_noise_layer(s, s->out, 0.06f, GR_NOISE_WHITE, t + 0.07f, PG_SVF_HIGHPASS, 2000.0f, 0.7f, t + 0.07f, 0.0005f,
                   0.035f, peak);
    gr_tone_layer(s, s->out, PG_OSC_SINE, 210.0f + t * 200.0f, t + 0.07f, t + 0.3f, t + 0.07f, 0.001f, 0.07f, peak);
  }
}

hz_internal void gr_rec_shell_insert(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.08f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 1900.0f + gr_rng(s) * 400.0f, 1.8f, 0.0f,
                 0.012f, 0.05f, 0.35f);
  gr_noise_layer(s, s->out, 0.05f, GR_NOISE_WHITE, 0.05f, PG_SVF_HIGHPASS, 2500.0f, 0.7f, 0.05f, 0.0005f, 0.03f, 0.55f);
  gr_tone_layer(s, s->out, PG_OSC_TRI, 330.0f + gr_rng(s) * 60.0f, 0.05f, 0.22f, 0.05f, 0.001f, 0.06f, 0.35f);
}

hz_internal void gr_rec_lancer(GrSyn *s) {
  // electric snap and crack, a falling rail zap, a heavy thump and a long humming decay
  f32 *bus = gr_buf(s);
  gr_noise_layer(s, bus, 0.12f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 3200.0f, 0.7f, 0.0f, 0.0005f, 0.05f, 1.0f);

  f32 *zap = gr_buf(s);
  GrParam zf = gr_param(s, 2600.0f + gr_rng(s) * 300.0f);
  gr_sweep(&zf, 2600.0f, 90.0f, 0.0f, 0.35f);
  gr_osc(s, zap, PG_OSC_SAW, &zf, 0.0f, 0.0f, 0.5f);
  GrParam cut = gr_param(s, 5000.0f);
  gr_sweep(&cut, 5000.0f, 400.0f, 0.0f, 0.35f);
  gr_filter(s, zap, PG_SVF_LOWPASS, &cut, 1.5f);
  gr_env_on(s, zap, 0.0f, 0.001f, 0.32f, 0.55f);
  gr_mix(s, bus, zap);

  gr_sweep_layer(s, bus, PG_OSC_SINE, 95.0f, 30.0f, 0.3f, 0.6f, 0.001f, 0.35f, 1.4f);

  f32 *hum = gr_buf(s);
  const GrParam hf = gr_const(58.0f);
  gr_osc(s, hum, PG_OSC_SQUARE, &hf, 0.0f, 0.02f, 1.4f);
  gr_filter_c(s, hum, PG_SVF_LOWPASS, 700.0f, 0.7f);
  gr_env_on(s, hum, 0.02f, 0.02f, 1.2f, 0.12f);
  gr_mix(s, bus, hum);

  gr_noise_layer(s, bus, 1.4f, GR_NOISE_PINK, 0.0f, PG_SVF_BANDPASS, 900.0f, 0.6f, 0.01f, 0.02f, 1.1f, 0.4f);
  gr_drive(s, bus, 2.5f);
  gr_mix(s, s->out, bus);
}

hz_internal void gr_rec_coil_charge(GrSyn *s) {
  // a rising capacitor whine that settles with a click
  f32 *whine = gr_buf(s);
  GrParam wf = gr_param(s, 380.0f);
  gr_sweep(&wf, 380.0f, 3100.0f, 0.0f, 0.42f);
  gr_osc(s, whine, PG_OSC_SINE, &wf, 0.0f, 0.0f, 0.6f);
  gr_swell_on(s, whine, 0.28f, 0.38f, 0.56f);
  gr_mix(s, s->out, whine);

  f32 *buzz = gr_buf(s);
  GrParam bf = gr_param(s, 120.0f);
  gr_sweep(&bf, 120.0f, 480.0f, 0.0f, 0.42f);
  gr_osc(s, buzz, PG_OSC_SAW, &bf, 0.0f, 0.0f, 0.6f);
  gr_filter_c(s, buzz, PG_SVF_BANDPASS, 900.0f, 2.0f);
  gr_swell_on(s, buzz, 0.12f, 0.38f, 0.5f);
  gr_mix(s, s->out, buzz);

  gr_noise_layer(s, s->out, 0.05f, GR_NOISE_WHITE, 0.42f, PG_SVF_HIGHPASS, 2600.0f, 0.7f, 0.42f, 0.0005f, 0.03f, 0.5f);
}

hz_internal void gr_rec_weapon_pickup(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.12f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 1200.0f, 1.2f, 0.0f, 0.01f, 0.08f, 0.4f);
  gr_tone_layer(s, s->out, PG_OSC_SINE, 170.0f, 0.06f, 0.4f, 0.06f, 0.002f, 0.12f, 0.6f);
  gr_tone_layer(s, s->out, PG_OSC_TRI, 1400.0f, 0.1f, 0.45f, 0.1f, 0.002f, 0.12f, 0.12f);
  gr_tone_layer(s, s->out, PG_OSC_TRI, 2100.0f, 0.17f, 0.45f, 0.17f, 0.002f, 0.12f, 0.12f);
}

hz_internal void gr_rec_health_pickup(GrSyn *s) {
  // a soft seal release, then a rising three-note chime
  gr_noise_layer(s, s->out, 0.2f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 3200.0f, 1.5f, 0.0f, 0.01f, 0.14f, 0.18f);
  for (u32 k = 0; k < 3; k++) {
    const f32 t = GR_HEALTH_NOTES[k][1];
    gr_tone_layer(s, s->out, PG_OSC_SINE, GR_HEALTH_NOTES[k][0], t, 0.45f, t, 0.004f, 0.3f, 0.22f);
  }
}

hz_internal void gr_rec_footstep(GrSyn *s) {
  gr_tone_layer(s, s->out, PG_OSC_SINE, 85.0f + gr_rng(s) * 25.0f, 0.0f, 0.2f, 0.0f, 0.002f, 0.07f, 0.45f);
  gr_noise_layer(s, s->out, 0.1f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 900.0f + gr_rng(s) * 900.0f, 1.2f, 0.002f,
                 0.004f, 0.05f, 0.25f);
  gr_noise_layer(s, s->out, 0.03f, GR_NOISE_WHITE, 0.0f, PG_SVF_HIGHPASS, 4000.0f, 0.7f, 0.001f, 0.0005f, 0.015f, 0.12f);
}

hz_internal void gr_rec_jump(GrSyn *s) {
  f32 *x = gr_buf(s);
  gr_noise(s, x, 0.2f, GR_NOISE_WHITE, 0.0f);
  GrParam f = gr_param(s, 600.0f);
  gr_sweep(&f, 600.0f, 1800.0f, 0.0f, 0.15f);
  gr_filter(s, x, PG_SVF_BANDPASS, &f, 1.0f);
  gr_env_on(s, x, 0.0f, 0.02f, 0.12f, 0.25f);
  gr_mix(s, s->out, x);
}

hz_internal void gr_rec_land(GrSyn *s) {
  gr_tone_layer(s, s->out, PG_OSC_SINE, 70.0f, 0.0f, 0.3f, 0.0f, 0.002f, 0.14f, 0.8f);
  gr_noise_layer(s, s->out, 0.2f, GR_NOISE_WHITE, 0.0f, PG_SVF_LOWPASS, 1300.0f, 0.7f, 0.0f, 0.002f, 0.1f, 0.5f);
}

hz_internal void gr_rec_jump_pad(GrSyn *s) {
  f32 *o = gr_buf(s);
  GrParam of = gr_param(s, 110.0f);
  gr_sweep(&of, 110.0f, 700.0f, 0.0f, 0.45f);
  gr_osc(s, o, PG_OSC_SINE, &of, 0.0f, 0.0f, 0.9f);
  gr_env_on(s, o, 0.0f, 0.01f, 0.6f, 0.6f);
  gr_mix(s, s->out, o);

  f32 *whoosh = gr_buf(s);
  gr_noise(s, whoosh, 0.9f, GR_NOISE_WHITE, 0.0f);
  GrParam f = gr_param(s, 400.0f);
  gr_sweep(&f, 400.0f, 3800.0f, 0.0f, 0.5f);
  gr_filter(s, whoosh, PG_SVF_BANDPASS, &f, 2.0f);
  gr_env_on(s, whoosh, 0.0f, 0.05f, 0.7f, 0.6f);
  gr_mix(s, s->out, whoosh);
}

hz_internal void gr_rec_player_hurt(GrSyn *s) {
  f32 *bus = gr_buf(s);
  gr_sweep_layer(s, bus, PG_OSC_SINE, 95.0f, 45.0f, 0.2f, 0.4f, 0.002f, 0.22f, 0.9f);
  gr_noise_layer(s, bus, 0.25f, GR_NOISE_WHITE, 0.0f, PG_SVF_LOWPASS, 1500.0f, 0.7f, 0.0f, 0.002f, 0.12f, 0.6f);
  f32 *buzz = gr_buf(s);
  const GrParam f = gr_const(58.0f);
  gr_osc(s, buzz, PG_OSC_SQUARE, &f, 0.0f, 0.0f, 0.3f);
  gr_filter_c(s, buzz, PG_SVF_LOWPASS, 900.0f, 0.7f);
  gr_env_on(s, buzz, 0.01f, 0.005f, 0.2f, 0.25f);
  gr_mix(s, bus, buzz);
  gr_drive(s, bus, 3.0f);
  gr_mix(s, s->out, bus);
}

hz_internal void gr_rec_bolt_impact(GrSyn *s) {
  gr_noise_layer(s, s->out, 0.2f, GR_NOISE_WHITE, 0.0f, PG_SVF_BANDPASS, 2100.0f, 3.5f, 0.0f, 0.001f, 0.12f, 0.7f);
  f32 *o = gr_buf(s);
  GrParam f = gr_param(s, 700.0f);
  gr_sweep(&f, 700.0f, 140.0f, 0.0f, 0.2f);
  gr_osc(s, o, PG_OSC_SAW, &f, 0.0f, 0.0f, 0.3f);
  gr_filter_c(s, o, PG_SVF_LOWPASS, 2000.0f, 0.7f);
  gr_env_on(s, o, 0.0f, 0.002f, 0.18f, 0.35f);
  gr_mix(s, s->out, o);
}

hz_internal void gr_rec_wave_start(GrSyn *s) {
  f32 *lp = gr_buf(s);
  for (u32 k = 0; k < 4; k++) {
    for (u32 d = 0; d < 2; d++) {
      f32 *x = gr_buf(s);
      const GrParam f = gr_const(GR_WAVE_START_CHORD[k]);
      gr_osc(s, x, PG_OSC_SAW, &f, (d ? 4.0f : -4.0f) + (gr_rng(s) - 0.5f) * 3.0f, 0.0f, 2.2f);
      const GrParam g = gr_const(0.12f);
      gr_gain(s, x, &g);
      gr_mix(s, lp, x);
    }
  }
  GrParam cut = gr_param(s, 300.0f);
  gr_ev(&cut, GR_EV_SET, 300.0f, 0.0f);
  gr_ev(&cut, GR_EV_EXP, 2800.0f, 0.12f);
  gr_ev(&cut, GR_EV_EXP, 700.0f, 1.6f);
  gr_filter(s, lp, PG_SVF_LOWPASS, &cut, 2.0f);
  GrParam g = gr_param(s, 0.0f);
  gr_ev(&g, GR_EV_SET, 0.0001f, 0.0f);
  gr_ev(&g, GR_EV_EXP, 0.5f, 0.05f);
  gr_ev(&g, GR_EV_SET, 0.5f, 0.9f);
  gr_ev(&g, GR_EV_EXP, 0.0001f, 2.1f);
  gr_gain(s, lp, &g);
  gr_drive(s, lp, 1.5f);
  gr_mix(s, s->out, lp);
  gr_tone_layer(s, s->out, PG_OSC_SINE, 55.0f, 0.0f, 1.0f, 0.0f, 0.005f, 0.8f, 0.6f);
}

hz_internal void gr_rec_wave_clear(GrSyn *s) {
  for (u32 k = 0; k < 4; k++) {
    const f32 t = (f32)k * 0.09f;
    gr_tone_layer(s, s->out, PG_OSC_TRI, GR_WAVE_CLEAR_NOTES[k], t, t + 1.2f, t, 0.005f, 0.9f, 0.22f);
    gr_tone_layer(s, s->out, PG_OSC_SINE, GR_WAVE_CLEAR_NOTES[k] * 2.0f, t, t + 1.2f, t, 0.005f, 0.5f, 0.06f);
  }
}

hz_internal void gr_rec_victory(GrSyn *s) {
  f32 *lp = gr_buf(s);
  for (u32 k = 0; k < 4; k++) {
    f32 *x = gr_buf(s);
    const GrParam f = gr_const(GR_VICTORY_CHORD[k]);
    gr_osc(s, x, PG_OSC_SAW, &f, 0.0f, 0.0f, 3.0f);
    const GrParam g = gr_const(0.1f);
    gr_gain(s, x, &g);
    gr_mix(s, lp, x);
  }
  GrParam cut = gr_param(s, 400.0f);
  gr_ev(&cut, GR_EV_SET, 400.0f, 0.0f);
  gr_ev(&cut, GR_EV_EXP, 3500.0f, 0.8f);
  gr_filter(s, lp, PG_SVF_LOWPASS, &cut, 1.0f);
  GrParam g = gr_param(s, 0.0f);
  gr_ev(&g, GR_EV_SET, 0.0001f, 0.0f);
  gr_ev(&g, GR_EV_EXP, 0.35f, 0.4f);
  gr_ev(&g, GR_EV_SET, 0.35f, 1.8f);
  gr_ev(&g, GR_EV_EXP, 0.0001f, 2.9f);
  gr_gain(s, lp, &g);
  gr_mix(s, s->out, lp);
}

hz_internal void gr_rec_game_over(GrSyn *s) {
  f32 *lp = gr_buf(s);
  for (u32 k = 0; k < 3; k++) {
    f32 *x = gr_buf(s);
    const f32 hz = GR_GAME_OVER_CHORD[k];
    GrParam f = gr_param(s, hz);
    gr_sweep(&f, hz, hz * 0.7f, 0.0f, 2.5f);
    gr_osc(s, x, PG_OSC_SAW, &f, 0.0f, 0.0f, 3.0f);
    const GrParam g = gr_const(0.15f);
    gr_gain(s, x, &g);
    gr_mix(s, lp, x);
  }
  GrParam cut = gr_param(s, 2500.0f);
  gr_ev(&cut, GR_EV_SET, 2500.0f, 0.0f);
  gr_ev(&cut, GR_EV_EXP, 200.0f, 2.5f);
  gr_filter(s, lp, PG_SVF_LOWPASS, &cut, 1.0f);
  GrParam g = gr_param(s, 0.0f);
  gr_ev(&g, GR_EV_SET, 0.0001f, 0.0f);
  gr_ev(&g, GR_EV_EXP, 0.45f, 0.05f);
  gr_ev(&g, GR_EV_EXP, 0.0001f, 2.9f);
  gr_gain(s, lp, &g);
  gr_drive(s, lp, 1.5f);
  gr_mix(s, s->out, lp);
}

hz_internal void gr_rec_ui_click(GrSyn *s) {
  gr_tone_layer(s, s->out, PG_OSC_SINE, 1500.0f, 0.0f, 0.08f, 0.0f, 0.001f, 0.04f, 0.2f);
}

hz_internal void gr_rec_ambience(GrSyn *s) {
  f32 *room = gr_buf(s);
  gr_noise(s, room, 6.5f, GR_NOISE_BROWN, 0.0f);
  gr_filter_c(s, room, PG_SVF_LOWPASS, 160.0f, 0.7f);
  const GrParam rg = gr_const(0.5f);
  gr_gain(s, room, &rg);
  gr_mix(s, s->out, room);

  f32 *air = gr_buf(s);
  gr_noise(s, air, 6.5f, GR_NOISE_PINK, 0.0f);
  gr_filter_c(s, air, PG_SVF_HIGHPASS, 2500.0f, 0.7f);
  const GrParam ag = gr_const(0.02f);
  gr_gain(s, air, &ag);
  gr_mix(s, s->out, air);

  f32 *hum = gr_buf(s);
  const GrParam h1 = gr_const(60.0f), h2 = gr_const(120.0f);
  const GrParam g1 = gr_const(0.05f), g2 = gr_const(0.02f);
  gr_osc(s, hum, PG_OSC_SINE, &h1, 0.0f, 0.0f, -1.0f);
  gr_gain(s, hum, &g1);
  gr_mix(s, s->out, hum);
  gr_osc(s, hum, PG_OSC_SINE, &h2, 0.0f, 0.0f, -1.0f);
  gr_gain(s, hum, &g2);
  gr_mix(s, s->out, hum);
}

// every recipe, in GrSound order: name, duration, variants, loop
hz_global const GrRecipe GR_RECIPES[GR_SOUND_COUNT] = {
    {"rifle", 0.55, 4, false, gr_rec_rifle},
    {"dryFire", 0.12, 1, false, gr_rec_dry_fire},
    {"impact", 0.35, 4, false, gr_rec_impact},
    {"metalHit", 0.4, 3, false, gr_rec_metal_hit},
    {"casing", 0.3, 3, false, gr_rec_casing},
    {"hitmarker", 0.1, 1, false, gr_rec_hitmarker},
    {"kill", 0.5, 1, false, gr_rec_kill},
    {"explosion", 2.2, 2, false, gr_rec_explosion},
    {"droneHum", 2.0, 1, true, gr_rec_drone_hum},
    {"droneFire", 0.4, 3, false, gr_rec_drone_fire},
    {"droneCharge", 0.8, 1, false, gr_rec_drone_charge},
    {"warp", 1.1, 1, false, gr_rec_warp},
    {"reloadOut", 0.25, 1, false, gr_rec_reload_out},
    {"reloadIn", 0.3, 1, false, gr_rec_reload_in},
    {"reloadCharge", 0.35, 1, false, gr_rec_reload_charge},
    {"breacher", 1.1, 3, false, gr_rec_breacher},
    {"pump", 0.42, 1, false, gr_rec_pump},
    {"shellInsert", 0.22, 3, false, gr_rec_shell_insert},
    {"lancer", 1.4, 2, false, gr_rec_lancer},
    {"coilCharge", 0.6, 1, false, gr_rec_coil_charge},
    {"weaponPickup", 0.45, 1, false, gr_rec_weapon_pickup},
    {"healthPickup", 0.7, 1, false, gr_rec_health_pickup},
    {"footstep", 0.2, 4, false, gr_rec_footstep},
    {"jump", 0.25, 1, false, gr_rec_jump},
    {"land", 0.35, 1, false, gr_rec_land},
    {"jumpPad", 1.0, 1, false, gr_rec_jump_pad},
    {"playerHurt", 0.45, 2, false, gr_rec_player_hurt},
    {"boltImpact", 0.4, 2, false, gr_rec_bolt_impact},
    {"waveStart", 2.2, 1, false, gr_rec_wave_start},
    {"waveClear", 1.6, 1, false, gr_rec_wave_clear},
    {"victory", 3.0, 1, false, gr_rec_victory},
    {"gameOver", 3.0, 1, false, gr_rec_game_over},
    {"uiClick", 0.08, 1, false, gr_rec_ui_click},
    {"ambience", 6.5, 1, true, gr_rec_ambience},
};

// ceil(duration * rate) in doubles (0.55 s -> 26401 frames)
hz_internal u32 gr_recipe_frames(const GrRecipe *r) { return (u32)m_ceil(r->duration * (f64)GR_SYN_SR); }

// a loop's last 0.4 s (at most a quarter) cross-faded into the head, then trimmed
hz_internal u32 gr_recipe_loop_fade(u32 frames) { return MIN((u32)(0.4f * (f32)GR_SYN_SR), frames / 4u); }
