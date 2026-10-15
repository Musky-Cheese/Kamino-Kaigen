// Proving Grounds audio: 34 procedural sounds rendered into clips at load, played through emitters, loops and the mixer.
#pragma once
#include "audio.h"
#include "ecs/ecs.h"
#include "lib/array.h"
#include "lib/hzmath.h"
#include "lib/random.h"
#include "lib/reflect.h"
#include "lib/typedefs.h"
#include "procgen/pg_audio.h"

// every sound, in recipe-table order; the recipe name's length seeds its variants
typedef enum GrSound {
  GR_SOUND_RIFLE = 0,
  GR_SOUND_DRY_FIRE,
  GR_SOUND_IMPACT,
  GR_SOUND_METAL_HIT,
  GR_SOUND_CASING,
  GR_SOUND_HITMARKER,
  GR_SOUND_KILL,
  GR_SOUND_EXPLOSION,
  GR_SOUND_DRONE_HUM,
  GR_SOUND_DRONE_FIRE,
  GR_SOUND_DRONE_CHARGE,
  GR_SOUND_WARP,
  GR_SOUND_RELOAD_OUT,
  GR_SOUND_RELOAD_IN,
  GR_SOUND_RELOAD_CHARGE,
  GR_SOUND_BREACHER,
  GR_SOUND_PUMP,
  GR_SOUND_SHELL_INSERT,
  GR_SOUND_LANCER,
  GR_SOUND_COIL_CHARGE,
  GR_SOUND_WEAPON_PICKUP,
  GR_SOUND_HEALTH_PICKUP,
  GR_SOUND_FOOTSTEP,
  GR_SOUND_JUMP,
  GR_SOUND_LAND,
  GR_SOUND_JUMP_PAD,
  GR_SOUND_PLAYER_HURT,
  GR_SOUND_BOLT_IMPACT,
  GR_SOUND_WAVE_START,
  GR_SOUND_WAVE_CLEAR,
  GR_SOUND_VICTORY,
  GR_SOUND_GAME_OVER,
  GR_SOUND_UI_CLICK,
  GR_SOUND_AMBIENCE,
  GR_SOUND_COUNT,
} GrSound;

#define GR_SOUND_MAX_VARIANTS 4
#define GR_SFX_EMIT_MAX 16 // one-shots one entity may request in one frame (a breacher volley's impacts)

// one play request: volume, rate, rate jitter, reverb send; positional voices take `position`
HZ_REFLECT()
typedef struct GrSfxPlay {
  u32 sound; // GrSound
  b32 positional;
  v3 position;
  f32 volume;
  f32 rate;
  f32 jitter; // +/- fraction of the rate
  f32 reverb; // send level into the hall
} GrSfxPlay;

// every sound call site with its levels; dynamic fields are noted per site
typedef enum GrSfxSite {
  GR_SITE_FIRE_RIFLE = 0,
  GR_SITE_FIRE_BREACHER,
  GR_SITE_FIRE_LANCER,
  GR_SITE_DRY_FIRE,
  GR_SITE_RELOAD_CUE,     // reload out/in/charge and coil charge: set .sound
  GR_SITE_PUMP,
  GR_SITE_SHELL_INSERT,
  GR_SITE_WEAPON_PICKUP,
  GR_SITE_IMPACT,         // positional
  GR_SITE_CASING,         // positional; volume = min(0.5, speed * 0.12)
  GR_SITE_HITMARKER,
  GR_SITE_KILL,
  GR_SITE_PLAYER_HURT,
  GR_SITE_HEALTH_PICKUP,
  GR_SITE_JUMP,
  GR_SITE_LAND,           // volume = min(0.9, impact / 14), +3 dB
  GR_SITE_FOOTSTEP,       // louder sprinting
  GR_SITE_JUMP_PAD,       // positional
  GR_SITE_WAVE_START,
  GR_SITE_WAVE_CLEAR,
  GR_SITE_VICTORY,
  GR_SITE_GAME_OVER,
  GR_SITE_BOLT_IMPACT,    // positional; volume 1.2 with splash
  GR_SITE_DRONE_FIRE,     // positional; rate = 1.15 - scale * 0.2
  GR_SITE_DRONE_CHARGE,   // positional; rate = 1.2 - scale * 0.2
  GR_SITE_WARP,           // positional
  GR_SITE_METAL_HIT,      // positional
  GR_SITE_EXPLOSION,      // positional; volume = 1.2 + scale * 0.4, rate = 1.1 - scale * 0.15
  GR_SITE_COUNT,
} GrSfxSite;

// sound requests: a producer writes its OWN entity's emitter (any lane) with `frame` = GrClock.frame
HZ_REFLECT()
ECS_COMPONENT(GrSfxEmitter) {
  u32 frame; // requests count only on this frame; a new frame starts the list over
  u32 count;
  u32 dropped; // requests past GR_SFX_EMIT_MAX this frame, logged by the consolidation
  GrSfxPlay plays[GR_SFX_EMIT_MAX];
};

// dev: `set:GrAudioDev.<field>=v` on the audio dev entity: listening checks
HZ_REFLECT()
ECS_COMPONENT(GrAudioDev) {
  i32 play;        // > 0 plays sound (play - 1) through the emitter, then clears
  b32 play_positional;
  f32 play_x;
  f32 play_y;
  f32 play_z;
  f32 play_volume; // the voice gain; 0 -> 1 (+3 dB when not positional)
  b32 all;         // plays every sound once, one every 0.35 s, through the emitter
  b32 dump;        // writes every rendered variant to out/gr_audio/<name>_<v>.wav, then clears
  b32 meter;       // logs voices, master level and compressor reduction once per second
  b32 unlock;      // starts the audio output without a run (the dev start switch does it too)
  f32 volume;      // > 0 overrides the settings volume
  f32 all_timer;   // runtime: seconds to the next `all` sound
  u32 all_next;    // runtime: the next GrSound `all` plays
};

// the dev switches for the audio systems; written by the dev system
typedef struct GrAudioDevState {
  b32 dump; // the dump request, consumed by the frame system
  b32 meter;
  b32 unlock;
  f32 volume;
} GrAudioDevState;
ECS_COMPONENT_TYPEDEF(GrAudioDevState);

// one render job: a sound variant's mono PCM, normalised to peak 0.99 and played back at `level`
typedef struct GrSfxJob {
  u32 sound;
  u32 variant;
  u64 seed;   // 1000 + 7919 * variant + recipe name length
  u32 frames; // the clip: the rendered length, less the loop cross-fade
  i16 *pcm;   // staging, `frames` samples
  f32 level;  // peak / 0.99: the playback gain that restores the rendered float level
  f32 peak;
  f32 rms;
  f32 ms; // render time
  HzHandleT(AudioSource) clip;
} GrSfxJob;
ARRAY_TYPEDEF_ALL(GrSfxJob);
ECS_ARRAY(GrSfxJob);

#define GR_AUDIO_TRACK_MAX 64 // live positional one-shots whose pan and distance gain follow the listener
#define GR_AUDIO_LOOP_MAX 96  // GR_MAX_DRONES hums, the ambience, and hums still fading out after their drone died

typedef struct GrAudioTrack {
  HzHandleT(AudioVoice) voice;
  v3 position;
  f32 volume; // site volume x clip level, before the panner
} GrAudioTrack;

typedef struct GrAudioLoopVoice {
  EcsEntity entity; // ECS_ENTITY_INVALID for the ambience
  HzHandleT(AudioVoice) voice;
  b32 positional;
  v3 position;
  f32 level;        // clip level
  f32 gain, target; // the loop's gain, easing exponentially to target
  f32 rate, rate_target;
  f32 tau;        // 0.15 s from silence, 0.1 s for setVolume, 0.08 s while stopping
  f32 stop_timer; // counts the 0.4 s from the stop request to the voice stop
  b32 stopping;
  u32 seen_frame;
} GrAudioLoopVoice;

// compressor -> master gain (volume, eased with a 0.05 s time constant), the MASTER bus effect; plus a meter
typedef struct GrAudioMaster {
  PgCompressor comp;
  f32 gain, target;
  f64 meter_sq;
  f32 meter_peak;
  u32 meter_frames;
} GrAudioMaster;

// the mixer, the effects, the clips and the live voices; written only by the audio consolidation systems
typedef struct GrAudio {
  Allocator alloc;         // the AudioSystem's arena: clip PCM, effect state
  ArenaAllocator *staging; // render output until the sources copy it; destroyed after
  AudioSystem system;
  GrAudioMaster *master;
  PgFreeverb *hall; // the hall reverb send
  b32 ready;        // the clips exist (the "Synthesizing audio" load stage is done)
  Array(GrSfxJob) jobs;
  u32 first_job[GR_SOUND_COUNT]; // a sound's variants are jobs first_job .. first_job + variants - 1
  Random rng;                    // variant choice, rate jitter, loop start offset
  v3 listener_pos, listener_fwd, listener_up;
  GrAudioTrack tracks[GR_AUDIO_TRACK_MAX];
  GrAudioLoopVoice loops[GR_AUDIO_LOOP_MAX];
  b32 unlocked;  // the first deploy/start starts the audio output
  b32 suspended; // the pause menu freezes the mixer: no mixer tick
  b32 ambience_started;
  u32 waves_announce_seen; // GrWaves serials already sounded
  u32 waves_cleared_seen;
  u32 waves_victory_seen;
  u32 frame;
  u32 plays; // one-shots started, for the meter
  u32 dropped;
  f32 meter_timer;
} GrAudio;
ECS_COMPONENT_TYPEDEF(GrAudio);

// the defaults for one call site; a producer fills in position and the dynamic fields
GrSfxPlay gr_sfx_site(GrSfxSite site);
// appends a request to the entity's own emitter; any lane, inside a per-entity system
void gr_sfx_emit(GrSfxEmitter *em, u32 frame, GrSfxPlay play);

// main lane, app_init: the mixer, the effects, the render jobs and the dev entity
void gr_audio_init(EcsWorld *w);
// in the frame's scope after every producer and the view: render, plays, loops, listener, mixer tick
void gr_audio_schedule(EcsWorld *w);
// the "Synthesizing audio" load stage is done
b32 gr_audio_ready(const GrAudio *a);
