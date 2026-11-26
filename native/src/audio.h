#pragma once

#include "nk_game.h"

#include <SDL.h>

#define NK_HOST_AUDIO_BANKS 9
#define NK_HOST_AUDIO_SAMPLES 14
#define NK_HOST_AUDIO_VOICES 16
#define NK_HOST_AUDIO_EXTRA_SAMPLES 64
#define NK_HOST_AUDIO_KEY_LENGTH 160
#define NK_HOST_MUSIC_TRACKS 2

#define NK_HOST_MUSIC_NONE (-1)
#define NK_HOST_MUSIC_TITLE (0)
#define NK_HOST_MUSIC_MATCH (1)

typedef struct NkHostSample {
    Uint8 *data;
    Uint32 length;
} NkHostSample;

typedef struct NkHostVoice {
    const NkHostSample *sample;
    Uint32 position;
    int volume;
} NkHostVoice;

typedef struct NkHostExtraSample {
    char key[NK_HOST_AUDIO_KEY_LENGTH];
    NkHostSample sample;
} NkHostExtraSample;

typedef struct NkHostAudio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec format;
    NkHostSample samples[NK_HOST_AUDIO_BANKS][NK_HOST_AUDIO_SAMPLES];
    NkHostSample music[NK_HOST_MUSIC_TRACKS];
    NkHostExtraSample extras[NK_HOST_AUDIO_EXTRA_SAMPLES];
    int extra_count;
    NkHostVoice voices[NK_HOST_AUDIO_VOICES];
    Uint32 music_position;
    int music_track;
    int music_volume;
    int ready;
} NkHostAudio;

/* asset_root is the directory containing extracted/<character>/audio. */
int nk_host_audio_open(NkHostAudio *audio, const char *asset_root);
void nk_host_audio_play_event(NkHostAudio *audio,
                              const NkGame *game,
                              const NkGameEvent *event,
                              int volume_level);
void nk_host_audio_set_music(NkHostAudio *audio, int track, int volume_level);
int nk_host_audio_play_asset(NkHostAudio *audio, const char *asset_root,
                             const char *relative_path, int volume_level);
int nk_host_audio_asset_playing(NkHostAudio *audio, const char *relative_path);
/* Stop active extracted asset voices whose filename begins with this prefix. */
void nk_host_audio_stop_asset_kind(NkHostAudio *audio, const char *basename_prefix);
void nk_host_audio_close(NkHostAudio *audio);
