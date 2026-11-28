#define _USE_MATH_DEFINES
#include "nc_audio.h"
#include "nc_weapon_audio.gen.h"
#include "qrazy_core_api.h"
#include "nc_vk.h"   // logf
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(NC_WINDOWS)
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#else
#include <mpg123.h>
#include <SDL3/SDL.h>   // Linux: SDL3 audio streams (PipeWire / PulseAudio / ALSA underneath)
#endif

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);   // stb_vorbis.c (public domain); output is malloc'ed

namespace nc {

namespace {
struct Sound { std::vector<float> data; int channels = 1; };   // interleaved float at the device rate; 1 or 2 channels
struct Voice { int id; int emitter = 0; std::shared_ptr<const Sound> snd; size_t pos = 0; bool positional; double at[3]; double gain; bool loop, ui, funny; size_t limit; SoundGroup group; };

// ---- decoding
bool decodeWav(const std::vector<uint8_t>& b, std::vector<float>& out, int& ch, int& rate) {
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
  int fmtTag = 0, bits = 0; ch = 0; rate = 0; size_t at = 12; const uint8_t* data = nullptr; size_t dataLen = 0;
  while (at + 8 <= b.size()) {
    uint32_t len; memcpy(&len, &b[at + 4], 4); const uint8_t* body = &b[at + 8]; const size_t avail = std::min<size_t>(len, b.size() - at - 8);
    if (!memcmp(&b[at], "fmt ", 4) && avail >= 16) { uint16_t t, c, bp; uint32_t r; memcpy(&t, body, 2); memcpy(&c, body + 2, 2); memcpy(&r, body + 4, 4); memcpy(&bp, body + 14, 2); fmtTag = t; ch = c; rate = (int)r; bits = bp;
      if (t == 0xFFFE && avail >= 26) { uint16_t sub; memcpy(&sub, body + 24, 2); fmtTag = sub; } }
    else if (!memcmp(&b[at], "data", 4)) { data = body; dataLen = avail; }
    at += 8 + (size_t)len + (len & 1);
  }
  if (!data || ch < 1 || rate < 1000 || !((fmtTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmtTag == 3 && bits == 32))) return false;
  const size_t bytes = (size_t)bits / 8, frames = dataLen / (bytes * (size_t)ch); out.resize(frames * (size_t)ch);
  for (size_t i = 0; i < frames * (size_t)ch; ++i) {
    const uint8_t* p = data + i * bytes; float v;
    if (fmtTag == 3) memcpy(&v, p, 4); else if (bits == 8) v = ((int)p[0] - 128) / 128.f; else if (bits == 16) { int16_t s; memcpy(&s, p, 2); v = s / 32768.f; }
    else if (bits == 24) { int32_t s = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)); v = (float)(s >> 8) / 8388608.f; } else { int32_t s; memcpy(&s, p, 4); v = (float)((double)s / 2147483648.0); }
    out[i] = v;
  }
  return true;
}
bool decodeOgg(const std::vector<uint8_t>& b, std::vector<float>& out, int& ch, int& rate) {
  short* pcm = nullptr; const int frames = stb_vorbis_decode_memory(b.data(), (int)b.size(), &ch, &rate, &pcm); if (frames <= 0 || !pcm) return false;
  out.resize((size_t)frames * (size_t)ch); for (size_t i = 0; i < out.size(); ++i) out[i] = pcm[i] / 32768.f; free(pcm); return ch >= 1 && ch <= 8;
}
static void appendPcm16(std::vector<float>& pcm, const void* data, size_t bytes) {
  const auto* raw = static_cast<const uint8_t*>(data);
  const size_t offset = pcm.size();
  pcm.resize(offset + bytes / 2);
  for (size_t i = 0; i < bytes / 2; ++i) {
    int16_t sample;
    memcpy(&sample, raw + i * 2, sizeof sample);
    pcm[offset + i] = sample / 32768.f;
  }
}

static bool decodeMp3(const std::vector<uint8_t>& bytes, std::vector<float>& pcm, int& ch, int& rate) {
  pcm.clear();
#if defined(NC_WINDOWS)
  using Microsoft::WRL::ComPtr;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
  struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
  ComPtr<IStream> memory;
  memory.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
  ComPtr<IMFByteStream> stream;
  ComPtr<IMFSourceReader> reader;
  ComPtr<IMFMediaType> format;
  ComPtr<IMFAttributes> attributes;
  const DWORD audio = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
  if (!memory || FAILED(MFCreateMFByteStreamOnStream(memory.Get(), &stream)) ||
      FAILED(stream.As(&attributes)) || FAILED(attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"audio/mpeg")) ||
      FAILED(MFCreateSourceReaderFromByteStream(stream.Get(), nullptr, &reader)) ||
      FAILED(MFCreateMediaType(&format)) || FAILED(format->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
      FAILED(format->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
      FAILED(format->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
      FAILED(reader->SetCurrentMediaType(audio, nullptr, format.Get()))) return false;
  format.Reset();
  UINT32 channels = 0, hz = 0, bits = 0;
  if (FAILED(reader->GetCurrentMediaType(audio, &format)) ||
      FAILED(format->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
      FAILED(format->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz)) ||
      FAILED(format->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) ||
      channels < 1 || channels > 2 || hz == 0 || bits != 16) return false;
  ch = int(channels);
  rate = int(hz);
  for (;;) {
    ComPtr<IMFSample> sample;
    DWORD flags = 0;
    if (FAILED(reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample)) ||
        (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) return false;
    if (sample) {
      ComPtr<IMFMediaBuffer> buffer;
      BYTE* data = nullptr;
      DWORD length = 0;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
          FAILED(buffer->Lock(&data, nullptr, &length))) return false;
      appendPcm16(pcm, data, length);
      buffer->Unlock();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
#else
  static const bool initialized = mpg123_init() == MPG123_OK;
  if (!initialized) return false;
  std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> decoder(mpg123_new(nullptr, nullptr), mpg123_delete);
  if (!decoder || mpg123_open_feed(decoder.get()) != MPG123_OK ||
      mpg123_feed(decoder.get(), bytes.data(), bytes.size()) != MPG123_OK) return false;
  long hz = 0;
  int encoding = 0;
  if (mpg123_getformat(decoder.get(), &hz, &ch, &encoding) != MPG123_OK ||
      hz <= 0 || ch < 1 || ch > 2 || encoding != MPG123_ENC_SIGNED_16) return false;
  rate = int(hz);
  unsigned char output[16384];
  for (;;) {
    size_t read = 0;
    const int result = mpg123_read(decoder.get(), output, sizeof output, &read);
    appendPcm16(pcm, output, read);
    if (result == MPG123_DONE || result == MPG123_NEED_MORE) break;
    if (result != MPG123_OK) return false;
  }
#endif
  return !pcm.empty();
}

std::shared_ptr<Sound> makeSound(std::vector<float>& src, int ch, int rate, int devRate) {
  auto s = std::make_shared<Sound>(); const int outCh = ch >= 2 ? 2 : 1; const size_t frames = src.size() / (size_t)ch;
  std::vector<float> flat(frames * (size_t)outCh);   // 1 channel stays mono, 2+ keep the first two
  for (size_t f = 0; f < frames; ++f) for (int c = 0; c < outCh; ++c) flat[f * (size_t)outCh + (size_t)c] = src[f * (size_t)ch + (size_t)c];
  if (rate == devRate) { s->data.swap(flat); s->channels = outCh; return s; }
  const double ratio = (double)rate / devRate; const size_t outFrames = (size_t)((double)frames / ratio); s->data.resize(outFrames * (size_t)outCh); s->channels = outCh;   // linear resampling at load time
  for (size_t f = 0; f < outFrames; ++f) { const double p = (double)f * ratio; const size_t i0 = (size_t)p, i1 = std::min(i0 + 1, frames - 1); const float t = (float)(p - (double)i0);
    for (int c = 0; c < outCh; ++c) s->data[f * (size_t)outCh + (size_t)c] = flat[i0 * (size_t)outCh + (size_t)c] * (1 - t) + flat[i1 * (size_t)outCh + (size_t)c] * t; }
  return s;
}
// ---- the game's synthesized cues (audio.js SYNTH_NOTES / SYNTH_STYLE): each note is a set of oscillators with a linear attack and an exponential fade to 0.001, summed into one mono buffer
static std::shared_ptr<Sound> synthSound(const std::string& kind, int rate) {
  const int count = qc_sound_synth(kind.data(), int(kind.size()), nullptr, 0);
  if (count <= 0) return nullptr;
  std::vector<double> recipe(size_t(count) * 7);
  if (qc_sound_synth(kind.data(), int(kind.size()), recipe.data(), count) != count) return nullptr;
  double end = 0;
  for (int row = 0; row < count; ++row) end = std::max(end, recipe[row * 7 + 2] + recipe[row * 7 + 3] + .02);
  auto sound = std::make_shared<Sound>();
  sound->data.assign(size_t(end * rate) + 1, 0.f);
  for (int row = 0; row < count; ++row) {
    const double* r = recipe.data() + row * 7;
    const double hz = r[1], at = r[2], duration = r[3], glide = r[4], peak = r[5], attack = r[6];
    double phase = 0;
    for (size_t i = size_t(std::ceil(at * rate)); i < sound->data.size() && i < (at + duration + .02) * rate; ++i) {
      const double age = double(i) / rate - at;
      const double u = std::min(1.0, age / duration);
      const double frequency = glide > 0 ? hz * std::pow(glide / hz, u) : hz;
      phase += 2 * M_PI * frequency / rate;
      const double env = age < attack ? age / attack * peak
        : age < duration ? peak * std::pow(.001 / peak, (age - attack) / (duration - attack)) : .001;
      const double sine = std::sin(phase);
      const double wave = r[0] == 0 ? sine : r[0] == 1 ? 2 / M_PI * std::asin(sine) : (sine >= 0 ? 1.0 : -1.0);
      sound->data[i] += float(wave * env);
    }
  }
  return sound;
}

static std::shared_ptr<Sound> spraySound(int rate) {
  auto sound = std::make_shared<Sound>();
  sound->data.resize(size_t(std::ceil(.6 * rate)));
  uint32_t seed = 0x73707279;
  double low = 0;
  const double mix = 1 - std::exp(-2 * M_PI * 2600 / rate);
  for (size_t i = 0; i < sound->data.size(); ++i) {
    const double t = double(i) / rate;
    seed = seed * 1664525u + 1013904223u;
    const double noise = seed / 2147483648.0 - 1;
    low += mix * (noise - low);
    double value = std::sin(2 * M_PI * 1750 * t) * std::exp(-t * 260) * .5;
    if (t >= .055) value += std::sin(2 * M_PI * 2150 * (t - .055)) * std::exp(-(t - .055) * 260) * .4;
    if (t >= .09) value += (noise - low) * std::min(1.0, (t - .09) / .012) * std::min(1.0, (.6 - t) / .2)
      * (.8 + .2 * std::sin(2 * M_PI * 9 * t)) * .55;
    sound->data[i] = float(std::tanh(value * 1.3) * .8);
  }
  return sound;
}
}  // namespace

struct Audio::Impl {
  const FileSource* files = nullptr; std::string voiceName = "sarge"; std::mutex mu; std::vector<Voice> voices; std::map<std::string, std::shared_ptr<Sound>> cache; int nextId = 1;
  double listenerPos[3] = {0, 0, 0}; double rightX = -1, rightZ = 0; bool worldMuted = false; float master = 0.5f, footstep = 1.f, ambient = 1.f; std::atomic<bool> muted{false};
#if defined(NC_WINDOWS)
  IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr; HANDLE event = nullptr; std::thread thread;
  void loop();
#else
  SDL_AudioStream* stream = nullptr;
#endif
  std::atomic<bool> run{false};
  int rate = 48000, channels = 2; uint32_t bufferFrames = 0; AudioStats st; std::atomic<uint64_t> underruns{0}, played{0}, missing{0};

  void cacheWeaponAudio() {
    for (const auto& asset : weapon_audio::assets) {
      std::vector<float> pcm(asset.begin, asset.end);
      cache[asset.cue] = makeSound(pcm, 1, asset.rate, rate);
    }
  }

  std::shared_ptr<Sound> load(const std::string& name) {
    auto it = cache.find(name); if (it != cache.end()) return it->second;
    if (name.compare(0, 6, "synth:") == 0) return cache[name] = synthSound(name.substr(6), rate);
    if (name == "player/spray") return cache[name] = spraySound(rate);
    std::shared_ptr<Sound> sound;
    std::vector<uint8_t> bytes;
    std::vector<float> pcm;
    int ch = 0, sr = 0;
    const auto decode = [&](const std::string& path) {
      return files && ((files->read("sound/" + path + ".wav", bytes) && decodeWav(bytes, pcm, ch, sr)) ||
        (files->read("sound/" + path + ".ogg", bytes) && decodeOgg(bytes, pcm, ch, sr)) ||
        (files->read("sound/" + path + ".mp3", bytes) && decodeMp3(bytes, pcm, ch, sr)));
    };
    bool found = decode(name);
    const bool special = name.compare(0, 14, "funny-special/") == 0;
    if (!found && (special || name.compare(0, 6, "funny/") == 0)) {
      const auto clip = name.substr(special ? 14 : 6);
      found = decode("funny-sounds-2/" + clip) || decode("funny-sounds-3/" + clip);
    }
    if (found) sound = makeSound(pcm, ch, sr, rate);
    if (!sound) {
      ++missing;
      if (missing <= 12) logf("sound not found or undecodable: %s", name.c_str());
    }
    // A funny pack can appear after the first request.
    if (!sound && (special || name.compare(0, 6, "funny/") == 0)) return nullptr;
    return cache[name] = sound;
  }
  void mix(float* out, uint32_t frames) {
    memset(out, 0, (size_t)frames * (size_t)channels * sizeof(float)); const bool focusMuted = muted.load();
    std::lock_guard<std::mutex> g(mu);
    for (size_t vi = 0; vi < voices.size();) {
      Voice& v = voices[vi]; const Sound& s = *v.snd; const size_t total = v.limit; float gl = 1, gr = 1; double gain = v.gain * master;
      if (v.group == SoundGroup::Footstep) gain *= footstep; else if (v.group == SoundGroup::Ambient) gain *= ambient;
      if (v.positional) {
        double pan = 0;
        gain *= qc_sound_spatial(v.at[0] - listenerPos[0], v.at[1] - listenerPos[1],
          v.at[2] - listenerPos[2], rightX, rightZ, &pan);
        const double a = (pan + 1) * M_PI / 4; gl = (float)std::cos(a); gr = (float)std::sin(a);
      }
      const float gF = (focusMuted || (worldMuted && !v.ui)) ? 0.f : float(gain); uint32_t f = 0;
      for (; f < frames; ++f) {
        if (v.pos >= total) { if (v.loop) v.pos = 0; else break; }
        float l, r; if (s.channels == 1) { l = r = s.data[v.pos]; } else { l = s.data[v.pos * 2]; r = s.data[v.pos * 2 + 1]; }
        if (v.positional && s.channels == 1) { /* mono: the pan law is already in gl / gr */ } else if (v.positional) { l *= 1.4142135f * gl; r *= 1.4142135f * gr; }
        float* o = out + (size_t)f * (size_t)channels; o[0] += l * gF * ((v.positional && s.channels == 1) ? gl : 1.f); if (channels > 1) o[1] += r * gF * ((v.positional && s.channels == 1) ? gr : 1.f);
        ++v.pos;
      }
      if (v.pos >= total && !v.loop) voices.erase(voices.begin() + (long)vi); else ++vi;
    }
    for (uint32_t i = 0; i < frames * (uint32_t)channels; ++i) out[i] = std::max(-1.f, std::min(1.f, out[i]));   // hard limit: the mix of many voices must not wrap
  }
};

#if defined(NC_WINDOWS)
void Audio::Impl::loop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED); DWORD task = 0; HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task); (void)h;
  while (run) {
    if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
    uint32_t pad = 0; if (FAILED(client->GetCurrentPadding(&pad))) continue; const uint32_t avail = bufferFrames - pad; if (!avail) continue;
    BYTE* buf = nullptr; if (FAILED(render->GetBuffer(avail, &buf))) continue;
    if (pad == 0) ++underruns;   // the engine had nothing queued when we woke: a glitch happened or is about to
    mix((float*)buf, avail); render->ReleaseBuffer(avail, 0);
  }
  CoUninitialize();
}

#endif

Audio::Audio() : m(new Impl()) {}
Audio::~Audio() { shutdown(); delete m; }

#if defined(NC_WINDOWS)
bool Audio::init(const FileSource* files) {
  Impl& a = *m; a.files = files; a.voices.reserve(64); CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr; if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
  IMMDevice* dev = nullptr; HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev); en->Release(); if (FAILED(hr)) { logf("audio: no output device"); return false; }
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&a.client); dev->Release(); if (FAILED(hr)) return false;
  WAVEFORMATEX* fmt = nullptr; if (FAILED(a.client->GetMixFormat(&fmt))) return false;
  a.rate = (int)fmt->nSamplesPerSec; a.channels = (int)fmt->nChannels;
  bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT; if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) { const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)fmt; isFloat = x->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT; }
  if (!isFloat || fmt->wBitsPerSample != 32) { logf("audio: the device mix format is not 32-bit float; not supported yet"); CoTaskMemFree(fmt); return false; }
  a.event = CreateEventW(nullptr, FALSE, FALSE, nullptr); bool low = false; double periodMs = 0;
  IAudioClient3* c3 = nullptr;
  if (SUCCEEDED(a.client->QueryInterface(__uuidof(IAudioClient3), (void**)&c3))) {   // the engine's smallest shared period (typically 2.7 to 10 ms)
    uint32_t defP, fund, minP, maxP; if (SUCCEEDED(c3->GetSharedModeEnginePeriod(fmt, &defP, &fund, &minP, &maxP)) && SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, fmt, nullptr))) { low = true; periodMs = 1000.0 * minP / a.rate; }
    c3->Release();
  }
  if (!low) {
    REFERENCE_TIME def = 0, minT = 0; a.client->GetDevicePeriod(&def, &minT); periodMs = def / 10000.0;
    if (FAILED(a.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, 0, fmt, nullptr))) { logf("audio: device initialisation failed"); CoTaskMemFree(fmt); return false; }
  }
  CoTaskMemFree(fmt);
  if (FAILED(a.client->SetEventHandle(a.event)) || FAILED(a.client->GetBufferSize(&a.bufferFrames)) || FAILED(a.client->GetService(__uuidof(IAudioRenderClient), (void**)&a.render))) return false;
  a.cacheWeaponAudio();
  { BYTE* buf; if (SUCCEEDED(a.render->GetBuffer(a.bufferFrames, &buf))) { memset(buf, 0, (size_t)a.bufferFrames * (size_t)a.channels * 4); a.render->ReleaseBuffer(a.bufferFrames, 0); } }
  a.run = true; a.thread = std::thread([&a] { a.loop(); }); if (FAILED(a.client->Start())) { a.run = false; a.thread.join(); return false; }
  a.st.rate = a.rate; a.st.channels = a.channels; a.st.periodMs = periodMs; a.st.lowLatency = low;
  logf("audio: %d Hz, %d channels, %s period %.1f ms, buffer %u frames", a.rate, a.channels, low ? "low-latency" : "default", periodMs, a.bufferFrames);
  return true;
}

void Audio::shutdown() {
  Impl& a = *m; if (a.run) { a.run = false; if (a.event) SetEvent(a.event); if (a.thread.joinable()) a.thread.join(); if (a.client) a.client->Stop(); }
  if (a.render) { a.render->Release(); a.render = nullptr; } if (a.client) { a.client->Release(); a.client = nullptr; } if (a.event) { CloseHandle(a.event); a.event = nullptr; }
}

#else
static void SDLCALL sdlFeed(void* user, SDL_AudioStream* stream, int additional, int) {
  Audio::Impl& a = *(Audio::Impl*)user; if (additional <= 0) return;
  const uint32_t frames = (uint32_t)additional / (uint32_t)(sizeof(float) * (size_t)a.channels); if (!frames) return;
  static thread_local std::vector<float> buf; buf.resize((size_t)frames * (size_t)a.channels); a.mix(buf.data(), frames);
  SDL_PutAudioStreamData(stream, buf.data(), (int)(buf.size() * sizeof(float)));
}

bool Audio::init(const FileSource* files) {
  Impl& a = *m; a.files = files; a.voices.reserve(64);
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { logf("audio: SDL audio init failed: %s", SDL_GetError()); return false; }
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");   // a small buffer: about 5 ms at 48 kHz (the backend may round it)
  SDL_AudioSpec spec; SDL_zero(spec); spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = 48000;
  a.rate = spec.freq; a.channels = spec.channels;
  a.cacheWeaponAudio();
  a.run = true; a.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdlFeed, &a);
  if (!a.stream) { a.run = false; logf("audio: no output device: %s", SDL_GetError()); return false; }
  if (!SDL_ResumeAudioStreamDevice(a.stream)) { shutdown(); return false; }
  SDL_AudioSpec dev; int frames = 0; double periodMs = 0; if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &frames) && dev.freq > 0) periodMs = 1000.0 * frames / dev.freq;
  a.st.rate = a.rate; a.st.channels = a.channels; a.st.periodMs = periodMs; a.st.lowLatency = periodMs > 0 && periodMs < 8;
  logf("audio: SDL stream %d Hz, %d channels, device buffer %d frames (%.1f ms)", a.rate, a.channels, frames, periodMs);
  return true;
}

void Audio::shutdown() {
  Impl& a = *m; a.run = false; if (a.stream) { SDL_DestroyAudioStream(a.stream); a.stream = nullptr; }
}

#endif

void Audio::setListener(const double pos[3], double yaw) { std::lock_guard<std::mutex> g(m->mu); for (int k = 0; k < 3; ++k) m->listenerPos[k] = pos[k]; m->rightX = -std::cos(yaw); m->rightZ = std::sin(yaw); }
void Audio::setVolume(float master, float footsteps, float ambient) { std::lock_guard<std::mutex> g(m->mu); m->master = master; m->footstep = footsteps; m->ambient = ambient; }
void Audio::setMuted(bool muted) { m->muted = muted; }
void Audio::stop(int id) { std::lock_guard<std::mutex> g(m->mu); for (size_t i = 0; i < m->voices.size(); ++i) if (m->voices[i].id == id) { m->voices.erase(m->voices.begin() + (long)i); return; } }

static int startVoice(Audio::Impl& a, const std::string& name, const double* pos, double gain, bool loop, SoundGroup group, int emitter, int flags = 0, double limit = 0);
int Audio::play(const std::string& name, const double* pos, double gain, bool loop, SoundGroup group) { return startVoice(*m, name, pos, gain, loop, group, 0); }
static int startVoice(Audio::Impl& a, const std::string& name, const double* pos, double gain, bool loop, SoundGroup group, int emitter, int flags, double limit) {
  if (!a.run) return 0;
  auto s = a.load(name); if (!s || s->data.empty()) return 0;   // decoding happens on the caller's thread (load time warms the cache); the mixer never touches the disk
  std::lock_guard<std::mutex> g(a.mu); if (a.voices.size() >= 64) a.voices.erase(a.voices.begin());   // the oldest voice is stolen
  Voice v; v.emitter = emitter; v.id = a.nextId++; v.snd = s; v.positional = pos != nullptr; if (pos) for (int k = 0; k < 3; ++k) v.at[k] = pos[k]; else v.at[0] = v.at[1] = v.at[2] = 0; v.gain = gain; v.loop = loop; v.group = group;
  v.ui = (flags & QC_CUE_UI) != 0;
  v.funny = (flags & QC_CUE_FUNNY) != 0;
  v.limit = s->data.size() / size_t(s->channels);
  if (limit > 0 && limit < double(v.limit) / a.rate) v.limit = size_t(limit * a.rate);
  if (!v.limit) return 0;
  a.voices.push_back(std::move(v)); ++a.played; return a.voices.back().id;
}

void Audio::setVoice(const std::string& character) { std::lock_guard<std::mutex> g(m->mu); m->voiceName = character; }

void Audio::cue(const char* nameIn, const double* pos, double gain, int flags, int emitter, SoundGroup group) {
  Impl& a = *m;
  if (flags & QC_CUE_WORLD_MUTE) {
    std::lock_guard<std::mutex> g(a.mu);
    a.worldMuted = gain != 0;
    return;
  }
  if (flags & (QC_CUE_STOP_FUNNY | QC_CUE_WR_CLIP | 64)) {
    std::lock_guard<std::mutex> g(a.mu);
    a.voices.erase(std::remove_if(a.voices.begin(), a.voices.end(), [&](const Voice& v) {
      return ((flags & 64) && v.loop) || ((flags & (QC_CUE_STOP_FUNNY | QC_CUE_WR_CLIP)) && v.funny);
    }), a.voices.end());
    if (flags & (QC_CUE_STOP_FUNNY | 64)) return;
  }
  const bool start = (flags & 2) != 0, stopLoop = (flags & 4) != 0, move = (flags & 8) != 0;
  if (stopLoop || move || start) {   // loops are keyed by their emitter
    std::lock_guard<std::mutex> g(a.mu); Voice* run = nullptr; for (Voice& v : a.voices) if (v.loop && v.emitter == emitter) { run = &v; break; }
    if (stopLoop) { if (run) a.voices.erase(a.voices.begin() + (run - a.voices.data())); return; }
    if (move) { if (run && pos) { run->positional = true; for (int k = 0; k < 3; ++k) run->at[k] = pos[k]; } return; }
    if (run) return;   // a start for a running emitter is ignored
  }
  static thread_local std::string name;
  name.clear();
  if (flags & 16) {
    name = "player/";
    if (!(flags & QC_CUE_EXPLICIT_VOICE)) {
      name += a.voiceName;
      name += '/';
    }
  } else if (flags & 32) name = "synth:";
  else if (flags & QC_CUE_FUNNY) name = flags & QC_CUE_SPECIAL ? "funny-special/" : "funny/";
  name += nameIn;
  if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos ||
      name.find_first_of("\\") != std::string::npos) return;
  if (start && name == "weapons/grapple/grappull") gain *= 0.5;
  startVoice(a, name, (flags & 1) ? pos : nullptr, gain, start, group, start ? emitter : 0,
    flags, (flags & QC_CUE_WR_CLIP) ? pos[0] : 0);
}

AudioStats Audio::stats() const { AudioStats s = m->st; s.underruns = m->underruns; s.played = m->played; s.missing = m->missing; std::lock_guard<std::mutex> g(m->mu); s.voices = (int)m->voices.size(); return s; }

}  // namespace nc
