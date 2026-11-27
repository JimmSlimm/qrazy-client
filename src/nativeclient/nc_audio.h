// Audio: sound files from the archives (WAV, OGG) decoded to the device's rate, mixed on a low-latency device thread (WASAPI shared mode, event driven), played with the game's
// distance falloff and stereo pan (audio.js attenuation / panFor). The policy of WHICH sound plays for what is the game's; this is only the backend.
#pragma once
#include "nc_assets.h"
#include <cstdint>
#include <string>

namespace nc {

enum class SoundGroup { Effects, Footstep, Ambient };

struct AudioStats { int rate = 0, channels = 0; double periodMs = 0; int voices = 0; uint64_t underruns = 0, played = 0, missing = 0; bool lowLatency = false; };

class Audio {
 public:
  Audio();
  ~Audio();
  struct Impl;
  bool init(const FileSource* files);   // opens the default output device; false = no audio (the game runs silent)
  void shutdown();
  // The listener for positional sounds: eye position (game axes) and view yaw in radians (the sim's convention).
  void setListener(const double pos[3], double yaw);
  // Plays sound/<name>.wav or .ogg (name without extension, as the game writes it, e.g. "weapons/rocket/rocklf1a"). pos = null for a non-positional sound. gain 0..1. Returns a voice id, 0 if nothing plays.
  int play(const std::string& name, const double* pos, double gain = 1.0, bool loop = false, SoundGroup group = SoundGroup::Effects);
  void stop(int voice);
  // The core's sound cues (native-core/sound.hpp, NATIVE-CLIENT.md section 10): flags POS 1, LOOP_START 2, LOOP_STOP 4, LOOP_MOVE 8, VOICE 16, SYNTH 32, STOP_ALL 64. Loops are keyed by `emitter`.
  void cue(const char* name, const double* pos, double gain, int flags, int emitter, SoundGroup group);
  void setVoice(const std::string& character);   // the character id for VOICE cues: sound/player/<character>/<name>
  void setVolume(float master, float footsteps = 1.f, float ambient = 1.f);
  void setMuted(bool muted);   // unfocused / minimized: silence without dropping the voices
  AudioStats stats() const;
 private:
  Impl* m;
};

}  // namespace nc
