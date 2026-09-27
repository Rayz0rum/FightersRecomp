#include "installer_audio.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/audio/hca_decoder.h>
#include <rex/logging.h>

namespace {

constexpr float kMusicVolume = 0.55f;
constexpr float kSoundVolume = 0.9f;
constexpr double kMusicFadeSeconds = 1.0;

struct Sound {
  rex::audio::HcaAudio audio;
  SDL_AudioStream* stream = nullptr;
};

SDL_AudioDeviceID g_device = 0;
bool g_audio_initialized = false;
Sound g_sounds[4];

rex::audio::HcaAudio g_music;
std::atomic<bool> g_music_ready = false;
std::thread g_music_thread;
SDL_AudioStream* g_music_stream = nullptr;
std::mutex g_music_mutex;
size_t g_music_position = 0;  // In sample frames.
bool g_music_playing = false;
bool g_music_fading = false;
std::chrono::steady_clock::time_point g_music_fade_start;

const char* SoundName(InstallerSound sound) {
  switch (sound) {
    case InstallerSound::Cursor: return "sound_cursor";
    case InstallerSound::Decide: return "sound_decide";
    case InstallerSound::Cancel: return "sound_cancel";
    default: return "sound_ring";
  }
}

bool Decode(const char* name, rex::audio::HcaAudio& out) {
  const uint8_t* data;
  size_t size;
  return InstallerAssets::Sound(name, data, size) && rex::audio::DecodeHca(data, size, out);
}

SDL_AudioStream* CreateStream(const rex::audio::HcaAudio& audio) {
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = int(audio.channels);
  spec.freq = int(audio.sample_rate);
  SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, nullptr);
  if (stream && !SDL_BindAudioStream(g_device, stream)) {
    SDL_DestroyAudioStream(stream);
    return nullptr;
  }
  return stream;
}

// Feeds the looping music.
void SDLCALL MusicCallback(void*, SDL_AudioStream* stream, int additional_amount, int) {
  std::lock_guard lock(g_music_mutex);
  if (!g_music_playing || g_music.samples.empty()) {
    return;
  }
  const uint32_t channels = g_music.channels;
  size_t frames_total = g_music.samples.size() / channels;
  size_t loop_start = g_music.loops ? g_music.loop_start : 0;
  size_t loop_end = g_music.loops ? g_music.loop_end : frames_total;
  int bytes_per_frame = int(sizeof(float) * channels);
  int frames_needed = (additional_amount + bytes_per_frame - 1) / bytes_per_frame;
  while (frames_needed > 0) {
    if (g_music_position >= loop_end) {
      g_music_position = loop_start;
    }
    size_t chunk = std::min<size_t>(size_t(frames_needed), loop_end - g_music_position);
    SDL_PutAudioStreamData(stream, &g_music.samples[g_music_position * channels],
                           int(chunk * bytes_per_frame));
    g_music_position += chunk;
    frames_needed -= int(chunk);
  }
}

}  // namespace

namespace InstallerAudio {

void Init() {
  if (g_audio_initialized) {
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_WARN("Installer: no audio ({})", SDL_GetError());
    return;
  }
  g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!g_device) {
    REXLOG_WARN("Installer: can't open the audio device ({})", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return;
  }
  SDL_ResumeAudioDevice(g_device);
  g_audio_initialized = true;
  for (int i = 0; i < 4; ++i) {
    if (Decode(SoundName(InstallerSound(i)), g_sounds[i].audio)) {
      g_sounds[i].stream = CreateStream(g_sounds[i].audio);
      if (g_sounds[i].stream) {
        SDL_SetAudioStreamGain(g_sounds[i].stream, kSoundVolume);
      }
    }
  }
  // The music is long - decoded in the background.
  g_music_thread = std::thread([] {
    rex::audio::HcaAudio music;
    if (Decode("music", music)) {
      std::lock_guard lock(g_music_mutex);
      g_music = std::move(music);
      g_music_ready = true;
    }
  });
}

void Shutdown() {
  if (g_music_thread.joinable()) {
    g_music_thread.join();
  }
  if (!g_audio_initialized) {
    return;
  }
  if (g_music_stream) {
    SDL_DestroyAudioStream(g_music_stream);
    g_music_stream = nullptr;
  }
  for (Sound& sound : g_sounds) {
    if (sound.stream) {
      SDL_DestroyAudioStream(sound.stream);
      sound.stream = nullptr;
    }
    sound.audio = {};
  }
  SDL_CloseAudioDevice(g_device);
  g_device = 0;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  g_audio_initialized = false;
  std::lock_guard lock(g_music_mutex);
  g_music = {};
  g_music_ready = false;
  g_music_playing = false;
  g_music_fading = false;
  g_music_position = 0;
}

void PlayMusic() {
  if (!g_audio_initialized || !g_music_ready || g_music_fading) {
    return;
  }
  if (!g_music_stream) {
    g_music_stream = CreateStream(g_music);
    if (!g_music_stream) {
      return;
    }
    SDL_SetAudioStreamGain(g_music_stream, kMusicVolume);
    {
      std::lock_guard lock(g_music_mutex);
      g_music_playing = true;
      g_music_position = 0;
    }
    SDL_SetAudioStreamGetCallback(g_music_stream, MusicCallback, nullptr);
  }
}

void FadeOutMusic() {
  if (!g_music_stream) {
    return;
  }
  if (!g_music_fading) {
    g_music_fading = true;
    g_music_fade_start = std::chrono::steady_clock::now();
  }
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_music_fade_start)
                 .count() / kMusicFadeSeconds;
  SDL_SetAudioStreamGain(g_music_stream, float(kMusicVolume * std::max(0.0, 1.0 - t)));
}

void Play(InstallerSound sound) {
  Sound& s = g_sounds[int(sound)];
  if (!g_audio_initialized || !s.stream || s.audio.samples.empty()) {
    return;
  }
  SDL_ClearAudioStream(s.stream);
  SDL_PutAudioStreamData(s.stream, s.audio.samples.data(),
                         int(s.audio.samples.size() * sizeof(float)));
}

}  // namespace InstallerAudio

void Game_PlaySound(InstallerSound sound) {
  InstallerAudio::Play(sound);
}
