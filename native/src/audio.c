/* Native SDL audio adapter for the original game's extracted PCM effects.
 * The event selection logic is shared with the source-matched 3DO port.
 */
#include "audio.h"

#include "nk_anim.h"
#include "nk_sound_logic.h"

#include <stdio.h>
#include <string.h>

static void nk_audio_callback(void *userdata, Uint8 *stream, int length)
{
    NkHostAudio *audio = (NkHostAudio *)userdata;
    int i;
    SDL_memset(stream, 0, (size_t)length);
    if (audio->music_track >= 0 && audio->music_track < NK_HOST_MUSIC_TRACKS &&
        audio->music_volume > 0) {
        const NkHostSample *track = &audio->music[audio->music_track];
        Uint32 offset = 0;
        while (track->data && track->length && offset < (Uint32)length) {
            Uint32 remaining = track->length - audio->music_position;
            Uint32 take = remaining < (Uint32)length - offset ?
                          remaining : (Uint32)length - offset;
            SDL_MixAudioFormat(stream + offset,
                               track->data + audio->music_position,
                               audio->format.format, take, audio->music_volume);
            audio->music_position += take;
            offset += take;
            if (audio->music_position >= track->length)
                audio->music_position = 0;
        }
    }
    for (i = 0; i < NK_HOST_AUDIO_VOICES; ++i) {
        NkHostVoice *voice = &audio->voices[i];
        Uint32 remaining;
        Uint32 take;
        if (!voice->sample || voice->position >= voice->sample->length) {
            voice->sample = NULL;
            continue;
        }
        remaining = voice->sample->length - voice->position;
        take = remaining < (Uint32)length ? remaining : (Uint32)length;
        SDL_MixAudioFormat(stream, voice->sample->data + voice->position,
                           audio->format.format, take, voice->volume);
        voice->position += take;
        if (voice->position >= voice->sample->length)
            voice->sample = NULL;
    }
}

static int nk_load_sample(NkHostAudio *audio, NkHostSample *sample,
                          const char *path)
{
    SDL_AudioSpec source;
    SDL_AudioCVT cvt;
    Uint8 *wav = NULL;
    Uint32 wav_length = 0;
    int conversion;
    if (SDL_LoadWAV(path, &source, &wav, &wav_length) == NULL)
        return 0;
    conversion = SDL_BuildAudioCVT(&cvt, source.format, source.channels,
                                    source.freq, audio->format.format,
                                    audio->format.channels, audio->format.freq);
    if (conversion < 0) {
        SDL_FreeWAV(wav);
        return 0;
    }
    cvt.len = (int)wav_length;
    cvt.buf = (Uint8 *)SDL_malloc((size_t)cvt.len * (size_t)cvt.len_mult);
    if (!cvt.buf) {
        SDL_FreeWAV(wav);
        return 0;
    }
    SDL_memcpy(cvt.buf, wav, wav_length);
    SDL_FreeWAV(wav);
    if (SDL_ConvertAudio(&cvt) < 0) {
        SDL_free(cvt.buf);
        return 0;
    }
    sample->data = cvt.buf;
    sample->length = (Uint32)cvt.len_cvt;
    return 1;
}

int nk_host_audio_open(NkHostAudio *audio, const char *asset_root)
{
    static const char *const banks[NK_HOST_AUDIO_BANKS] = {
        "KLUBBOR", "FETUS", "HENRY", "GURDIP", "ED",
        "SINAMMON", "BUDDY", "GONZOLES", "HEAD"
    };
    SDL_AudioSpec desired;
    char path[1024];
    int bank;
    int sample;
    static const char *const music_files[NK_HOST_MUSIC_TRACKS] = {
        "title-midi.wav", "match-cmf.wav"
    };
    if (!audio || !asset_root)
        return 0;
    SDL_memset(audio, 0, sizeof(*audio));
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = nk_audio_callback;
    desired.userdata = audio;
    audio->device = SDL_OpenAudioDevice(NULL, 0, &desired, &audio->format, 0);
    if (!audio->device)
        return 0;
    audio->music_track = NK_HOST_MUSIC_NONE;
    for (bank = 0; bank < NK_HOST_AUDIO_BANKS; ++bank) {
        for (sample = 0; sample < NK_HOST_AUDIO_SAMPLES; ++sample) {
            int count = snprintf(path, sizeof(path),
                "%s/extracted/%s/audio/character.combat_sound_%d.wav",
                asset_root, banks[bank], sample);
            if (count > 0 && count < (int)sizeof(path))
                nk_load_sample(audio, &audio->samples[bank][sample], path);
        }
    }
    for (sample = 0; sample < NK_HOST_MUSIC_TRACKS; ++sample) {
        int count = snprintf(path, sizeof(path), "%s/music/%s",
                             asset_root, music_files[sample]);
        if (count > 0 && count < (int)sizeof(path)) {
            if (!nk_load_sample(audio, &audio->music[sample], path))
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Cannot load music %s: %s",
                            path, SDL_GetError());
            else
                SDL_Log("Loaded music %s (%u audio bytes)", music_files[sample],
                        (unsigned)audio->music[sample].length);
        }
    }
    audio->ready = 1;
    SDL_PauseAudioDevice(audio->device, 0);
    return 1;
}

void nk_host_audio_set_music(NkHostAudio *audio, int track, int volume_level)
{
    if (!audio || !audio->ready)
        return;
    if (track < NK_HOST_MUSIC_NONE || track >= NK_HOST_MUSIC_TRACKS)
        track = NK_HOST_MUSIC_NONE;
    if (volume_level < 0)
        volume_level = 0;
    if (volume_level > 7)
        volume_level = 7;
    SDL_LockAudioDevice(audio->device);
    if (audio->music_track != track)
        audio->music_position = 0;
    audio->music_track = track;
    audio->music_volume = SDL_MIX_MAXVOLUME * volume_level / 7;
    SDL_Log("Music track=%d volume=%d length=%u", track, audio->music_volume,
            track >= 0 ? (unsigned)audio->music[track].length : 0U);
    SDL_UnlockAudioDevice(audio->device);
}

static void nk_play_sample(NkHostAudio *audio, const NkHostSample *sample,
                           int volume_level)
{
    int voice;
    if (!audio || !audio->ready || !sample || !sample->data ||
        !sample->length || volume_level <= 0)
        return;
    if (volume_level > 7)
        volume_level = 7;
    SDL_LockAudioDevice(audio->device);
    for (voice = 0; voice < NK_HOST_AUDIO_VOICES; ++voice)
        if (!audio->voices[voice].sample) break;
    if (voice == NK_HOST_AUDIO_VOICES) voice = 0;
    audio->voices[voice].sample = sample;
    audio->voices[voice].position = 0;
    audio->voices[voice].volume = SDL_MIX_MAXVOLUME * volume_level / 7;
    SDL_UnlockAudioDevice(audio->device);
}

int nk_host_audio_play_asset(NkHostAudio *audio, const char *asset_root,
                             const char *relative_path, int volume_level)
{
    char full_path[1024];
    NkHostExtraSample *extra = NULL;
    int i;
    int count;
    if (!audio || !audio->ready || !asset_root || !relative_path ||
        volume_level <= 0 || strlen(relative_path) >= NK_HOST_AUDIO_KEY_LENGTH)
        return 0;
    for (i = 0; i < audio->extra_count; ++i)
        if (strcmp(audio->extras[i].key, relative_path) == 0) {
            extra = &audio->extras[i];
            break;
        }
    if (!extra) {
        if (audio->extra_count >= NK_HOST_AUDIO_EXTRA_SAMPLES) return 0;
        count = snprintf(full_path, sizeof(full_path), "%s/%s",
                         asset_root, relative_path);
        if (count <= 0 || count >= (int)sizeof(full_path)) return 0;
        extra = &audio->extras[audio->extra_count];
        if (!nk_load_sample(audio, &extra->sample, full_path)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Cannot load sound %s: %s",
                        full_path, SDL_GetError());
            return 0;
        }
        strcpy(extra->key, relative_path);
        audio->extra_count++;
    }
    nk_play_sample(audio, &extra->sample, volume_level);
    return 1;
}

int nk_host_audio_asset_playing(NkHostAudio *audio, const char *relative_path)
{
    const NkHostSample *sample = NULL;
    int playing = 0;
    int i;
    if (!audio || !audio->ready || !relative_path) return 0;
    for (i = 0; i < audio->extra_count; ++i)
        if (strcmp(audio->extras[i].key, relative_path) == 0) {
            sample = &audio->extras[i].sample;
            break;
        }
    if (!sample) return 0;
    SDL_LockAudioDevice(audio->device);
    for (i = 0; i < NK_HOST_AUDIO_VOICES; ++i)
        if (audio->voices[i].sample == sample) {
            playing = 1;
            break;
        }
    SDL_UnlockAudioDevice(audio->device);
    return playing;
}

void nk_host_audio_stop_asset_kind(NkHostAudio *audio, const char *basename_prefix)
{
    size_t prefix_length;
    int voice;
    int extra;
    if (!audio || !audio->ready || !basename_prefix || !*basename_prefix) return;
    prefix_length = strlen(basename_prefix);
    SDL_LockAudioDevice(audio->device);
    for (extra = 0; extra < audio->extra_count; ++extra) {
        const char *basename = strrchr(audio->extras[extra].key, '/');
        basename = basename ? basename + 1 : audio->extras[extra].key;
        if (strncmp(basename, basename_prefix, prefix_length) != 0) continue;
        for (voice = 0; voice < NK_HOST_AUDIO_VOICES; ++voice) {
            if (audio->voices[voice].sample == &audio->extras[extra].sample) {
                audio->voices[voice].sample = NULL;
                audio->voices[voice].position = 0;
            }
        }
    }
    SDL_UnlockAudioDevice(audio->device);
}

void nk_host_audio_play_event(NkHostAudio *audio, const NkGame *game,
                              const NkGameEvent *event, int volume_level)
{
    NkSoundRequest request;
    const NkHostSample *sample;
    int bank;
    if (!audio || !audio->ready || !game || !event || volume_level <= 0 ||
        !nk_sound_request_from_game_event(event, &request))
        return;
    bank = request.bank_index == 2 ? 8 :
           game->players[request.bank_index].character_type;
    if (bank < 0 || bank >= NK_HOST_AUDIO_BANKS ||
        request.sample_index >= NK_HOST_AUDIO_SAMPLES)
        return;
    sample = &audio->samples[bank][request.sample_index];
    if (!sample->data || !sample->length)
        return;
    nk_play_sample(audio, sample, volume_level);
}

void nk_host_audio_close(NkHostAudio *audio)
{
    int bank;
    int sample;
    if (!audio)
        return;
    if (audio->device) {
        SDL_PauseAudioDevice(audio->device, 1);
        SDL_CloseAudioDevice(audio->device);
    }
    for (bank = 0; bank < NK_HOST_AUDIO_BANKS; ++bank)
        for (sample = 0; sample < NK_HOST_AUDIO_SAMPLES; ++sample)
            SDL_free(audio->samples[bank][sample].data);
    for (sample = 0; sample < NK_HOST_MUSIC_TRACKS; ++sample)
        SDL_free(audio->music[sample].data);
    for (sample = 0; sample < audio->extra_count; ++sample)
        SDL_free(audio->extras[sample].sample.data);
    SDL_memset(audio, 0, sizeof(*audio));
}
