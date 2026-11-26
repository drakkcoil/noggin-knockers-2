#include "frontend.h"

#include "audio.h"
#include "hd_upscale.h"
#include "nk_anim.h"
#include "nk_cinema.h"
#include "nk_compositor.h"
#include "nk_dialogue.h"
#include "nk_flow.h"
#include "nk_game.h"
#include "nk_match_present.h"
#include "nk_scene.h"
#include "nk_select.h"
#include "nk_sticky.h"
#include "nk_ui.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_HOST_WIDTH 320
#define NK_HOST_HEIGHT 200
#define NK_HOST_IMAGE_LIMIT 16384
#define NK_HOST_PATH_LIMIT 2048

typedef struct NkHostImage {
    uint32_t key;
    char *path;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
    int load_attempted;
} NkHostImage;

typedef struct NkHost {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *hd_texture;
    NkHdUpscaler hd_upscaler;
    int render_hd;
    SDL_GameController *controllers[2];
    NkHostAudio audio;
    NkHostImage *images;
    size_t image_count;
    char asset_root[NK_HOST_PATH_LIMIT];
    uint8_t pixels[NK_HOST_WIDTH * NK_HOST_HEIGHT * 4];
    uint8_t floor[NK_STICKY_SURFACE_WIDTH * NK_STICKY_SURFACE_HEIGHT * 4];
    uint8_t scene_palettes[3][256][3];
    const uint8_t (*active_palette)[3];
    SDL_Rect clip;
    nk_flow flow;
    NkGame game;
    NkMatchPresentation presentation;
    NkDialogueState dialogue;
    NkSceneState scene;
    NkCinemaState cinema;
    NkSelectState selection;
    nk_title_state title;
    nk_options_state options;
    NkAnimCursor option_skull;
    nk_rng rng;
    nk_input_filter input_filters[2];
    int accept_keydown_latched;
    int active_scene;
    int scene_scroll;
    int game_initialized;
    int paused;
    int pause_down;
    int background_hidden;
    int background_toggle_down;
    int outcome_started;
    int outcome_ticks;
    int shock_phase;
    int cinema_input_latched;
    char title_voice_path[NK_HOST_AUDIO_KEY_LENGTH];
    char select_scream_path[NK_HOST_AUDIO_KEY_LENGTH];
    unsigned title_voice_events;
    unsigned cinema_sound_events;
    unsigned select_scream_events;
    unsigned audio_asset_failures;
    int running;
    int missing_images;
    int fullscreen;
    int smoke;
    char smoke_prefix[NK_HOST_PATH_LIMIT];
    uint8_t fade_level;
} NkHost;

static uint32_t nk_image_key(unsigned kind, unsigned bank, unsigned index)
{
    return ((uint32_t)kind << 24) | ((uint32_t)bank << 16) | index;
}

static int nk_image_compare(const void *a, const void *b)
{
    const NkHostImage *left = (const NkHostImage *)a;
    const NkHostImage *right = (const NkHostImage *)b;
    return (left->key > right->key) - (left->key < right->key);
}

static char *nk_copy_string(const char *source)
{
    size_t length = strlen(source) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

static int nk_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static int nk_find_asset_root(char *output, size_t capacity, const char *requested)
{
    const char *candidates[] = {
        requested, "assets", "native/assets", "../assets", "../native/assets",
        NULL
    };
    char candidate[NK_HOST_PATH_LIMIT];
    char *base = SDL_GetBasePath();
    size_t i;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i]) continue;
        snprintf(candidate, sizeof(candidate), "%s/image_index.tsv", candidates[i]);
        if (nk_file_exists(candidate)) {
            snprintf(output, capacity, "%s", candidates[i]);
            SDL_free(base);
            return 1;
        }
    }
    if (base) {
        const char *suffixes[] = {"assets", "../Resources/assets", "../assets", "../../native/assets"};
        for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            snprintf(candidate, sizeof(candidate), "%s%s/image_index.tsv", base, suffixes[i]);
            if (nk_file_exists(candidate)) {
                snprintf(output, capacity, "%s%s", base, suffixes[i]);
                SDL_free(base);
                return 1;
            }
        }
    }
    SDL_free(base);
    return 0;
}

static void nk_options_path(char *output, size_t capacity)
{
    char *directory = SDL_GetPrefPath("Bloodlust", "NogginKnockers2");
    if (directory) {
        snprintf(output, capacity, "%soptions.txt", directory);
        SDL_free(directory);
    } else output[0] = 0;
}

static void nk_save_options(const nk_options *options);

static int nk_load_options(nk_flow *flow)
{
    char path[NK_HOST_PATH_LIMIT];
    char line[256];
    int difficulty, sound, music, talking, rounds, fixes, hd = 0;
    int format_version = 4;
    FILE *file;
    nk_options_path(path, sizeof(path));
    file = path[0] ? fopen(path, "r") : NULL;
    if (!file) return 0;
    if (fgets(line, sizeof(line), file)) {
        int read = sscanf(line, "v4 %d %d %d %d %d %d %d", &difficulty, &sound,
                          &music, &talking, &rounds, &fixes, &hd);
        if (read != 7) {
            format_version = 3;
            read = sscanf(line, "v3 %d %d %d %d %d %d", &difficulty, &sound,
                          &music, &talking, &rounds, &fixes);
            if (read != 6) {
                format_version = 2;
                read = sscanf(line, "v2 %d %d %d %d %d %d", &difficulty, &sound,
                              &music, &talking, &rounds, &fixes);
                if (read != 6) {
                    format_version = 1;
                    read = sscanf(line, "%d %d %d %d %d %d", &difficulty, &sound,
                                  &music, &talking, &rounds, &fixes);
                }
            }
        }
        /* Earlier native builds saved the original silent default. Upgrade
           their zero once; a deliberate mute in v3 remains persistent. */
        if (read == 6 && format_version < 3 && music == 0) music = 5;
        if (read == (format_version == 4 ? 7 : 6) &&
            difficulty >= NK_DIFFICULTY_MIN && difficulty <= NK_DIFFICULTY_MAX &&
            sound >= NK_VOLUME_MIN && sound <= NK_VOLUME_MAX &&
            music >= NK_VOLUME_MIN && music <= NK_VOLUME_MAX &&
            talking >= NK_TOGGLE_OFF && talking <= NK_TOGGLE_ON &&
            rounds >= NK_ROUND_WIN_MIN && rounds <= NK_ROUND_WIN_MAX &&
            fixes >= NK_TOGGLE_OFF && fixes <= NK_TOGGLE_ON &&
            hd >= NK_TOGGLE_OFF && hd <= NK_TOGGLE_ON) {
            flow->options.difficulty = difficulty;
            flow->options.sound_volume = sound;
            flow->options.music_volume = music;
            flow->options.talking = talking;
            flow->options.round_setting = rounds;
            flow->options.fix_orig_bugs = fixes;
            flow->options.hd_upscale = hd;
            fclose(file);
            if (format_version < 4) nk_save_options(&flow->options);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static void nk_save_options(const nk_options *options)
{
    char path[NK_HOST_PATH_LIMIT];
    FILE *file;
    nk_options_path(path, sizeof(path));
    file = path[0] ? fopen(path, "w") : NULL;
    if (!file) return;
    fprintf(file, "v4 %ld %ld %ld %ld %ld %ld %ld\n",
            (long)options->difficulty, (long)options->sound_volume,
            (long)options->music_volume, (long)options->talking,
            (long)options->round_setting, (long)options->fix_orig_bugs,
            (long)options->hd_upscale);
    fclose(file);
}

static int nk_load_image_index(NkHost *host)
{
    char path[NK_HOST_PATH_LIMIT];
    char line[NK_HOST_PATH_LIMIT];
    FILE *file;
    size_t capacity = 0;
    snprintf(path, sizeof(path), "%s/image_index.tsv", host->asset_root);
    file = fopen(path, "rb");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        unsigned kind, bank, index;
        char relative[NK_HOST_PATH_LIMIT];
        char complete[NK_HOST_PATH_LIMIT];
        NkHostImage *image;
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        if (sscanf(line, "%u\t%u\t%u\t%2047[^\n]", &kind, &bank, &index, relative) != 4)
            continue;
        if (kind > 255 || bank > 255 || index > 65535 || host->image_count >= NK_HOST_IMAGE_LIMIT)
            continue;
        if (host->image_count == capacity) {
            size_t next = capacity ? capacity * 2 : 256;
            NkHostImage *expanded = (NkHostImage *)realloc(host->images, next * sizeof(*expanded));
            if (!expanded) { fclose(file); return 0; }
            host->images = expanded;
            capacity = next;
        }
        snprintf(complete, sizeof(complete), "%s/%s", host->asset_root, relative);
        image = &host->images[host->image_count++];
        memset(image, 0, sizeof(*image));
        image->key = nk_image_key(kind, bank, index);
        image->path = nk_copy_string(complete);
        if (!image->path) { fclose(file); return 0; }
    }
    fclose(file);
    qsort(host->images, host->image_count, sizeof(*host->images), nk_image_compare);
    SDL_Log("Loaded %lu asset references from %s", (unsigned long)host->image_count, path);
    return host->image_count > 0;
}

static uint32_t nk_read_u32le(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void nk_load_scene_palettes(NkHost *host)
{
    const char *names[3] = {"NOGTITLE", "NOGGINBG", "SELECT"};
    char index_path[NK_HOST_PATH_LIMIT];
    char line[NK_HOST_PATH_LIMIT];
    int found[3] = {0, 0, 0};
    FILE *index_file;
    snprintf(index_path, sizeof(index_path), "%s/palette_index.tsv", host->asset_root);
    index_file = fopen(index_path, "rb");
    if (index_file) {
        while (fgets(line, sizeof(line), index_file)) {
            unsigned kind, bank, index;
            char relative[NK_HOST_PATH_LIMIT];
            char path[NK_HOST_PATH_LIMIT];
            uint8_t rgba[1024];
            FILE *palette_file;
            if (sscanf(line, "%u\t%u\t%u\t%2047[^\n]", &kind, &bank,
                       &index, relative) != 4 || kind != 0 || bank >= 3 || index != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", host->asset_root, relative);
            palette_file = fopen(path, "rb");
            if (!palette_file) continue;
            if (fread(rgba, 1, sizeof(rgba), palette_file) == sizeof(rgba)) {
                for (int color = 0; color < 256; ++color)
                    memcpy(host->scene_palettes[bank][color], rgba + color * 4, 3);
                found[bank] = 1;
            }
            fclose(palette_file);
        }
        fclose(index_file);
    }
    for (int bank = 0; bank < 3; ++bank) {
        char path[NK_HOST_PATH_LIMIT];
        uint8_t vga[768];
        FILE *file;
        if (found[bank]) continue;
        snprintf(path, sizeof(path), "%s/extracted/%s/records/0000_scene.palette.bin",
                 host->asset_root, names[bank]);
        file = fopen(path, "rb");
        if (!file) continue;
        if (fread(vga, 1, sizeof(vga), file) == sizeof(vga))
            for (int color = 0; color < 256; ++color)
                for (int component = 0; component < 3; ++component) {
                    unsigned value = vga[color * 3 + component];
                    host->scene_palettes[bank][color][component] =
                        (uint8_t)((value << 2) | (value >> 4));
                }
        fclose(file);
    }
    host->active_palette = host->scene_palettes[NK_SCENE_BANK_GAME];
}

static NkHostImage *nk_get_image(NkHost *host, unsigned kind, unsigned bank, unsigned index)
{
    NkHostImage needle;
    NkHostImage *image;
    uint8_t header[8];
    FILE *file;
    size_t bytes;
    needle.key = nk_image_key(kind, bank, index);
    image = (NkHostImage *)bsearch(&needle, host->images, host->image_count,
                                   sizeof(*host->images), nk_image_compare);
    if (!image) {
        if (host->missing_images++ < 16)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Missing image kind=%u bank=%u index=%u", kind, bank, index);
        return NULL;
    }
    if (image->load_attempted) return image->pixels ? image : NULL;
    image->load_attempted = 1;
    file = fopen(image->path, "rb");
    if (!file || fread(header, 1, 8, file) != 8) {
        if (file) fclose(file);
        return NULL;
    }
    image->width = nk_read_u32le(header);
    image->height = nk_read_u32le(header + 4);
    if (!image->width || !image->height || image->width > 4096 || image->height > 4096 ||
        (uint64_t)image->width * image->height * 4 > 128U * 1024U * 1024U) {
        fclose(file);
        return NULL;
    }
    bytes = (size_t)image->width * image->height * 4;
    image->pixels = (uint8_t *)malloc(bytes);
    if (!image->pixels || fread(image->pixels, 1, bytes, file) != bytes) {
        free(image->pixels);
        image->pixels = NULL;
    }
    fclose(file);
    return image->pixels ? image : NULL;
}

static void nk_blit_pixels(uint8_t *target, int target_width, int target_height,
                           const NkHostImage *image, int x, int y, unsigned orientation,
                           const SDL_Rect *clip)
{
    int ix, iy;
    int left = x < clip->x ? clip->x : x;
    int top = y < clip->y ? clip->y : y;
    int right = x + (int)image->width;
    int bottom = y + (int)image->height;
    if (right > clip->x + clip->w) right = clip->x + clip->w;
    if (bottom > clip->y + clip->h) bottom = clip->y + clip->h;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > target_width) right = target_width;
    if (bottom > target_height) bottom = target_height;
    for (iy = top; iy < bottom; ++iy) {
        int sy = (orientation & 1U) ? (int)image->height - 1 - (iy - y) : iy - y;
        for (ix = left; ix < right; ++ix) {
            int sx = (orientation & 2U) ? (int)image->width - 1 - (ix - x) : ix - x;
            const uint8_t *source = image->pixels + ((size_t)sy * image->width + sx) * 4;
            uint8_t *destination = target + ((size_t)iy * target_width + ix) * 4;
            unsigned alpha = source[3];
            if (alpha == 255) {
                memcpy(destination, source, 4);
            } else if (alpha) {
                unsigned inverse = 255 - alpha;
                destination[0] = (uint8_t)((source[0] * alpha + destination[0] * inverse) / 255);
                destination[1] = (uint8_t)((source[1] * alpha + destination[1] * inverse) / 255);
                destination[2] = (uint8_t)((source[2] * alpha + destination[2] * inverse) / 255);
                destination[3] = 255;
            }
        }
    }
}

static bool nk_emit(void *context, const NkCompositorCommand *command)
{
    NkHost *host = (NkHost *)context;
    NkHostImage *image;
    int x, y, width, height;
    switch (command->type) {
    case NK_COMPOSITOR_BEGIN:
        host->fade_level = command->value.begin.fade_level;
        if (command->value.begin.palette.kind == NK_COMPOSITOR_PALETTE_SCENE &&
            command->value.begin.palette.bank_index < 3)
            host->active_palette =
                host->scene_palettes[command->value.begin.palette.bank_index];
        return 1;
    case NK_COMPOSITOR_CLEAR:
        for (size_t i = 0; i < sizeof(host->pixels); i += 4) {
            host->pixels[i] = host->pixels[i + 1] = host->pixels[i + 2] = 0;
            host->pixels[i + 3] = 255;
        }
        host->clip = (SDL_Rect){0, 0, NK_HOST_WIDTH, NK_HOST_HEIGHT};
        return 1;
    case NK_COMPOSITOR_CLIP:
        host->clip = (SDL_Rect){command->value.clip.x, command->value.clip.y,
                                command->value.clip.width, command->value.clip.height};
        return 1;
    case NK_COMPOSITOR_DRAW:
        image = nk_get_image(host, command->value.draw.image.kind,
                             command->value.draw.image.bank_index,
                             command->value.draw.image.index);
        if (image) nk_blit_pixels(host->pixels, NK_HOST_WIDTH, NK_HOST_HEIGHT,
                                  image, command->value.draw.x, command->value.draw.y,
                                  command->value.draw.orientation, &host->clip);
        return 1;
    case NK_COMPOSITOR_FILL:
        x = command->value.fill.x;
        y = command->value.fill.y;
        width = command->value.fill.width;
        height = command->value.fill.height;
        if (x < host->clip.x) { width -= host->clip.x - x; x = host->clip.x; }
        if (y < host->clip.y) { height -= host->clip.y - y; y = host->clip.y; }
        if (x + width > host->clip.x + host->clip.w) width = host->clip.x + host->clip.w - x;
        if (y + height > host->clip.y + host->clip.h) height = host->clip.y + host->clip.h - y;
        if (x < 0) { width += x; x = 0; }
        if (y < 0) { height += y; y = 0; }
        if (x + width > NK_HOST_WIDTH) width = NK_HOST_WIDTH - x;
        if (y + height > NK_HOST_HEIGHT) height = NK_HOST_HEIGHT - y;
        if (width > 0 && height > 0) {
            for (int row = y; row < y + height; ++row)
                for (int col = x; col < x + width; ++col) {
                    uint8_t *pixel = host->pixels + ((size_t)row * NK_HOST_WIDTH + col) * 4;
                    unsigned color = command->value.fill.color;
                    pixel[0] = host->active_palette[color][0];
                    pixel[1] = host->active_palette[color][1];
                    pixel[2] = host->active_palette[color][2];
                    pixel[3] = 255;
                }
        }
        return 1;
    case NK_COMPOSITOR_STICKY_SURFACE: {
        SDL_Rect floor_clip = {0, NK_STICKY_FLOOR_Y, NK_STICKY_SURFACE_WIDTH,
                               NK_STICKY_SURFACE_HEIGHT};
        int row;
        for (row = 0; row < floor_clip.h; ++row) {
            int destination_y = row + floor_clip.y;
            if (destination_y < host->clip.y || destination_y >= host->clip.y + host->clip.h)
                continue;
            memcpy(host->pixels + (size_t)destination_y * NK_HOST_WIDTH * 4,
                   host->floor + (size_t)row * NK_STICKY_SURFACE_WIDTH * 4,
                   NK_STICKY_SURFACE_WIDTH * 4);
        }
        return 1;
    }
    case NK_COMPOSITOR_END:
        return 1;
    default:
        return 0;
    }
}

static void nk_reset_floor(NkHost *host)
{
    NkHostImage *image = nk_get_image(host, NK_COMPOSITOR_IMAGE_SCENE_LAYER,
                                      NK_SCENE_BANK_GAME, NK_COMPOSITOR_LAYER_1A);
    memset(host->floor, 0, sizeof(host->floor));
    if (image) {
        SDL_Rect clip = {0, 0, NK_STICKY_SURFACE_WIDTH, NK_STICKY_SURFACE_HEIGHT};
        /* The retail floor is the x=160..479, y=60..99 crop of layer 1a. */
        nk_blit_pixels(host->floor, NK_STICKY_SURFACE_WIDTH, NK_STICKY_SURFACE_HEIGHT,
                       image, -NK_STICKY_VISIBLE_CROP_X,
                       -NK_STICKY_BACKGROUND_FLOOR_ROW, 0, &clip);
    }
}

static void nk_commit_sticky(NkHost *host)
{
    NkStickyQueue *queue = &host->presentation.sticky_pending;
    SDL_Rect clip = {0, 0, NK_STICKY_SURFACE_WIDTH, NK_STICKY_SURFACE_HEIGHT};
    for (u32 i = 0; i < queue->count; ++i) {
        const NkStickyOp *op = &queue->ops[i];
        NkHostImage *image = nk_get_image(host, NK_COMPOSITOR_IMAGE_ANIMATION,
                                          NK_EFFECT_BANK_INDEX, op->image);
        if (image)
            nk_blit_pixels(host->floor, NK_STICKY_SURFACE_WIDTH, NK_STICKY_SURFACE_HEIGHT,
                           image, op->x, op->y - NK_STICKY_FLOOR_Y,
                           op->orientation, &clip);
    }
    nk_sticky_queue_reset(queue);
}

static int nk_prepare_hd_texture(NkHost *host)
{
    if (host->hd_texture) return 1;
    if (!nk_hd_upscaler_init(&host->hd_upscaler, NK_HOST_WIDTH, NK_HOST_HEIGHT))
        return 0;
    host->hd_texture = SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         host->hd_upscaler.width, host->hd_upscaler.height);
    if (!host->hd_texture) {
        nk_hd_upscaler_destroy(&host->hd_upscaler);
        return 0;
    }
    return 1;
}

static int nk_render(NkHost *host)
{
    NkCompositorSink sink = {0};
    NkCinemaView cinema_view;
    NkTitleView title_view;
    NkOptionsView options_view;
    NkSelectorView selector_view;
    NkMatchView match_view;
    SDL_Texture *screen_texture;
    SDL_Rect destination;
    int upscale;
    int composed = 1;
    sink.context = host;
    sink.emit = nk_emit;
    host->fade_level = 32;
    host->clip = (SDL_Rect){0, 0, NK_HOST_WIDTH, NK_HOST_HEIGHT};
    switch (host->flow.scene) {
    case NK_SCENE_LOGO:
    case NK_SCENE_CREDITS:
    case NK_SCENE_ENDING:
        cinema_view.cinema = &host->cinema;
        composed = nk_compositor_compose_cinema(&cinema_view, &sink);
        break;
    case NK_SCENE_TITLE:
        title_view.title = &host->title;
        title_view.scene = &host->scene;
        title_view.voice_playing = 0;
        composed = nk_compositor_compose_title(&title_view, &sink);
        break;
    case NK_SCENE_OPTIONS:
        options_view.options = &host->options;
        options_view.scene = &host->scene;
        options_view.skull = &host->option_skull;
        options_view.scene_scroll = host->scene_scroll;
        composed = nk_compositor_compose_options(&options_view, &sink);
        break;
    case NK_SCENE_SELECT:
        selector_view.selection = &host->selection;
        selector_view.scene = &host->scene;
        selector_view.scene_scroll = host->scene_scroll;
        composed = nk_compositor_compose_selector(&selector_view, &sink);
        break;
    case NK_SCENE_MATCH:
        nk_match_present_set_phase(&host->presentation, NK_MATCH_PRESENT_PHASE_ONE);
        match_view.game = &host->game;
        match_view.presentation = &host->presentation;
        match_view.scene = &host->scene;
        match_view.dialogue = &host->presentation.dialogue_snapshot;
        match_view.scene_scroll = host->scene_scroll;
        match_view.fade_level = 32;
        match_view.talking = (u8)(host->flow.options.talking != 0);
        match_view.paused = (u8)host->paused;
        match_view.quit = 0;
        match_view.background_hidden = (u8)host->background_hidden;
        match_view.shock_phase = (u8)host->shock_phase;
        composed = nk_compositor_compose_match(&match_view, &sink);
        if (composed) nk_commit_sticky(host);
        break;
    default:
        memset(host->pixels, 0, sizeof(host->pixels));
        break;
    }
    if (!composed) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not compose scene %d", host->flow.scene);
    if (host->fade_level < 32) {
        for (size_t i = 0; i < sizeof(host->pixels); i += 4) {
            host->pixels[i] = (uint8_t)(host->pixels[i] * host->fade_level / 32);
            host->pixels[i + 1] = (uint8_t)(host->pixels[i + 1] * host->fade_level / 32);
            host->pixels[i + 2] = (uint8_t)(host->pixels[i + 2] * host->fade_level / 32);
        }
    }
    upscale = host->flow.scene == NK_SCENE_OPTIONS ?
        host->options.pending.hd_upscale : host->flow.options.hd_upscale;
    if (upscale && !nk_prepare_hd_texture(host)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "HD texture unavailable; using Classic mode");
        if (host->flow.scene == NK_SCENE_OPTIONS)
            host->options.pending.hd_upscale = 0;
        else
            host->flow.options.hd_upscale = 0;
        upscale = 0;
    }
    if (upscale != host->render_hd) {
        if (SDL_RenderSetLogicalSize(host->renderer,
                                     upscale ? host->hd_upscaler.width : NK_HOST_WIDTH,
                                     upscale ? 960 : 240) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not set render size: %s", SDL_GetError());
            return 0;
        }
        host->render_hd = upscale;
    }
    if (upscale) {
        const uint8_t *pixels = nk_hd_upscaler_apply(&host->hd_upscaler,
                                                     host->pixels, NK_HOST_WIDTH * 4);
        if (!pixels || SDL_UpdateTexture(host->hd_texture, NULL, pixels,
                                         host->hd_upscaler.pitch) != 0)
            return 0;
        screen_texture = host->hd_texture;
        destination = (SDL_Rect){0, 0, host->hd_upscaler.width, 960};
    } else {
        if (SDL_UpdateTexture(host->texture, NULL, host->pixels, NK_HOST_WIDTH * 4) != 0)
            return 0;
        screen_texture = host->texture;
        destination = (SDL_Rect){0, 0, NK_HOST_WIDTH, 240};
    }
    if (SDL_RenderClear(host->renderer) != 0 ||
        SDL_RenderCopy(host->renderer, screen_texture, NULL, &destination) != 0)
        return 0;
    SDL_RenderPresent(host->renderer);
    host->shock_phase = (host->shock_phase + 1) % NK_MATCH_SHOCK_PERIOD_PRESENTATIONS;
    return composed;
}

static int nk_save_smoke_frame(NkHost *host, const char *name)
{
    char path[NK_HOST_PATH_LIMIT];
    SDL_Surface *surface;
    void *pixels = host->render_hd ? host->hd_upscaler.pixels : host->pixels;
    int width = host->render_hd ? host->hd_upscaler.width : NK_HOST_WIDTH;
    int height = host->render_hd ? host->hd_upscaler.height : NK_HOST_HEIGHT;
    int success;
    snprintf(path, sizeof(path), "%s-%s.bmp", host->smoke_prefix, name);
    surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width,
                                                 height, 32, width * 4,
                                                 SDL_PIXELFORMAT_ABGR8888);
    if (!surface) return 0;
    success = SDL_SaveBMP(surface, path) == 0;
    SDL_FreeSurface(surface);
    if (success) SDL_Log("Saved %s", path);
    return success;
}

static void nk_set_scene_music(NkHost *host)
{
    int track = NK_HOST_MUSIC_NONE;
    if (host->flow.scene == NK_SCENE_TITLE || host->flow.scene == NK_SCENE_OPTIONS)
        track = NK_HOST_MUSIC_TITLE;
    else if (host->flow.scene == NK_SCENE_MATCH)
        track = NK_HOST_MUSIC_MATCH;
    nk_host_audio_set_music(&host->audio, track, host->flow.options.music_volume);
}

static void nk_play_asset(NkHost *host, const char *path, unsigned *counter)
{
    if (host->flow.options.sound_volume <= 0) return;
    if (nk_host_audio_play_asset(&host->audio, host->asset_root, path,
                                 host->flow.options.sound_volume))
        (*counter)++;
    else {
        host->audio_asset_failures++;
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Could not play %s", path);
    }
}

static void nk_play_cinema_sound(NkHost *host)
{
    static const char *const banks[NK_CINEMA_BANK_COUNT] = {
        "LOGO", "NCRED", "END1", "END2", "END3", "END4",
        "END5", "END6", "END7", "END8"
    };
    char path[NK_HOST_AUDIO_KEY_LENGTH];
    int sound = host->cinema.pending_sound;
    if (sound < 0 || sound >= NK_CINEMA_SOUND_COUNT ||
        host->cinema.bank_index >= NK_CINEMA_BANK_COUNT) return;
    snprintf(path, sizeof(path), "extracted/%s/audio/cinema.sound_%d.wav",
             banks[host->cinema.bank_index], sound);
    nk_play_asset(host, path, &host->cinema_sound_events);
}

static int nk_enter_scene(NkHost *host)
{
    int scene = host->flow.scene;
    int cinema_index;
    host->active_scene = scene;
    host->scene_scroll = 0;
    host->paused = 0;
    host->pause_down = 0;
    host->outcome_started = 0;
    host->outcome_ticks = 0;
    nk_set_scene_music(host);
    if (scene == NK_SCENE_PORT_CREDIT) {
        nk_flow_cinematic_complete(&host->flow);
        return nk_enter_scene(host);
    }
    if (scene == NK_SCENE_LOGO || scene == NK_SCENE_CREDITS || scene == NK_SCENE_ENDING) {
        host->cinema_input_latched = 1;
        cinema_index = scene == NK_SCENE_LOGO ? NK_CINEMA_LOGO :
                       scene == NK_SCENE_CREDITS ? NK_CINEMA_CREDITS :
                       NK_CINEMA_ENDING_FIRST + host->flow.ending_number - 1;
        return nk_cinema_begin(&host->cinema, (u8)cinema_index);
    }
    switch (scene) {
    case NK_SCENE_TITLE:
        host->title_voice_path[0] = 0;
        if (!nk_scene_begin(&host->scene, NK_SCENE_BANK_TITLE)) return 0;
        nk_title_begin(&host->title);
        return 1;
    case NK_SCENE_OPTIONS:
        if (!nk_scene_begin(&host->scene, NK_SCENE_BANK_SELECT)) return 0;
        if (!nk_anim_cursor_start(&host->option_skull, 9U, 27U)) return 0;
        host->scene_scroll = 160;
        nk_options_begin(&host->options, &host->flow.options);
        return 1;
    case NK_SCENE_SELECT:
        host->select_scream_path[0] = 0;
        if (!nk_scene_begin(&host->scene, NK_SCENE_BANK_SELECT)) return 0;
        nk_select_begin(&host->selection, &host->flow, &host->rng);
        return 1;
    case NK_SCENE_MATCH: {
        s32 difficulty = nk_flow_match_difficulty(&host->flow, &host->rng);
        int configured;
        if (host->game_initialized)
            configured = nk_game_reconfigure(&host->game,
                host->flow.selected_character[0], host->flow.control[0],
                host->flow.selected_character[1], host->flow.control[1],
                nk_options_round_target(&host->flow.options), difficulty,
                host->flow.options.fix_orig_bugs, host->rng.state);
        else
            configured = nk_game_configure(&host->game,
                host->flow.selected_character[0], host->flow.control[0],
                host->flow.selected_character[1], host->flow.control[1],
                nk_options_round_target(&host->flow.options), difficulty,
                host->flow.options.fix_orig_bugs, host->rng.state);
        if (!configured || !nk_scene_begin(&host->scene, NK_SCENE_BANK_GAME)) return 0;
        host->game_initialized = 1;
        nk_game_begin_match(&host->game);
        nk_match_present_reset(&host->presentation);
        nk_dialogue_begin_match(&host->dialogue);
        nk_reset_floor(host);
        return nk_match_present_prepare_frame(&host->presentation, &host->game,
                                               &host->dialogue, 0,
                                               host->flow.options.talking, 0, 0, 0);
    }
    case NK_SCENE_EXIT:
        host->running = 0;
        return 1;
    default:
        return 0;
    }
}

static int nk_any_input(const nk_input_sample input[2])
{
    return input[0].stat || input[0].buttons || input[0].commands ||
           input[1].stat || input[1].buttons || input[1].commands;
}

static int nk_tick(NkHost *host, const nk_input_sample input[2])
{
    int result;
    if (host->flow.scene == NK_SCENE_LOGO || host->flow.scene == NK_SCENE_CREDITS ||
        host->flow.scene == NK_SCENE_ENDING) {
        if (host->cinema_input_latched && !nk_any_input(input))
            host->cinema_input_latched = 0;
        else if (!host->cinema_input_latched && nk_any_input(input))
            nk_flow_cinematic_skip(&host->flow);
        if (host->flow.scene == host->active_scene) {
            nk_cinema_tick(&host->cinema);
            nk_play_cinema_sound(host);
            if (host->cinema.completed)
                nk_flow_cinematic_complete(&host->flow);
        }
    } else if (host->flow.scene == NK_SCENE_TITLE) {
        nk_ui_events events;
        int voice_playing = !host->smoke && host->title_voice_path[0] &&
            nk_host_audio_asset_playing(&host->audio, host->title_voice_path);
        nk_title_step(&host->title, &input[0], nk_any_input(input),
                      host->flow.options.sound_volume > 0, voice_playing, &events);
        if ((events.flags & NK_UI_EVENT_VOICE) && events.voice_index >= 0 &&
            events.voice_index < NK_TITLE_VOICE_COUNT) {
            snprintf(host->title_voice_path, sizeof(host->title_voice_path),
                     "extracted/TITLESND/audio/title_speech.sound_%d.wav",
                     events.voice_index);
            nk_play_asset(host, host->title_voice_path, &host->title_voice_events);
        }
        nk_scene_tick(&host->scene);
        result = nk_title_result(&host->title);
        if (result != NK_TITLE_PENDING) nk_flow_title_complete(&host->flow, result);
    } else if (host->flow.scene == NK_SCENE_OPTIONS) {
        nk_ui_events events;
        nk_options_step(&host->options, &input[0], &events);
        if ((events.flags & NK_UI_EVENT_ADJUST) &&
            host->options.cursor == NK_OPTION_MUSIC)
            nk_host_audio_set_music(&host->audio, NK_HOST_MUSIC_TITLE,
                                    host->options.pending.music_volume);
        if (nk_anim_cursor_tick(&host->option_skull) == NK_ANIM_STEP_COMPLETE)
            nk_anim_cursor_start(&host->option_skull, 9U, 27U);
        host->scene_scroll++;
        nk_scene_tick(&host->scene);
        if (host->options.done) {
            nk_flow_options_complete(&host->flow, &host->options.pending);
            if (!host->smoke) nk_save_options(&host->flow.options);
        }
    } else if (host->flow.scene == NK_SCENE_SELECT) {
        NkSelectEvents events;
        int scream_playing = !host->smoke && host->select_scream_path[0] &&
            nk_host_audio_asset_playing(&host->audio, host->select_scream_path);
        nk_select_step(&host->selection, input, &host->rng, scream_playing, &events);
        if (events.scream_character >= 0 && events.scream_character < NK_CHARACTER_COUNT &&
            events.scream_index >= 0 &&
            events.scream_index < NK_SELECT_SCREAM_VARIANT_COUNT) {
            const char *bank = nk_anim_banks[(unsigned)events.scream_character].name;
            snprintf(host->select_scream_path, sizeof(host->select_scream_path),
                     "extracted/%s/audio/character.select_scream_%d.wav",
                     bank, events.scream_index);
            nk_play_asset(host, host->select_scream_path, &host->select_scream_events);
        }
        host->scene_scroll = (host->scene_scroll + 1) % (NK_HOST_WIDTH + 1);
        nk_scene_tick(&host->scene);
        result = nk_select_result(&host->selection);
        if (result == NK_SELECT_CANCELLED) nk_flow_abort_to_title(&host->flow);
        else if (result == NK_SELECT_COMPLETE)
            nk_flow_selection_complete(&host->flow,
                                       host->selection.cursor[0], host->selection.cursor[1]);
    } else if (host->flow.scene == NK_SCENE_MATCH) {
        int pause_pressed = ((input[0].commands | input[1].commands) & NK_COMMAND_PAUSE) != 0;
        if (((input[0].commands | input[1].commands) & NK_COMMAND_CANCEL) != 0) {
            nk_flow_abort_to_title(&host->flow);
        } else {
            if (pause_pressed && !host->pause_down) host->paused = !host->paused;
            host->pause_down = pause_pressed;
            if (!host->paused) {
                nk_dialogue_advance_clock(&host->dialogue);
                nk_game_set_input(&host->game, 0, &input[0]);
                nk_game_set_input(&host->game, 1, &input[1]);
                nk_game_step(&host->game);
                for (u32 i = 0; i < host->game.event_count; ++i)
                    nk_host_audio_play_event(&host->audio, &host->game, &host->game.events[i],
                                             host->flow.options.sound_volume);
                nk_scene_tick(&host->scene);
                host->scene_scroll = (host->scene_scroll + 1) % (NK_HOST_WIDTH + 1);
            }
            if (!nk_match_present_prepare_frame(&host->presentation, &host->game,
                                                 &host->dialogue, host->outcome_started,
                                                 host->flow.options.talking,
                                                 host->background_hidden, 0,
                                                 host->paused)) return 0;
            if (host->presentation.outcome_began) host->outcome_started = 1;
            if (host->outcome_started) host->outcome_ticks++;
            if (host->game.game_state == NK_GAME_STATE_MATCH_COMPLETE &&
                host->outcome_started &&
                ((input[0].buttons | input[1].buttons) ||
                 (host->flow.mode == NK_MODE_ATTRACT && host->outcome_ticks > 100))) {
                host->rng = host->game.rng;
                nk_flow_match_complete(&host->flow, host->game.winner);
            }
        }
    }
    if (host->flow.scene != host->active_scene) {
        if (host->active_scene == NK_SCENE_LOGO ||
            host->active_scene == NK_SCENE_CREDITS ||
            host->active_scene == NK_SCENE_ENDING)
            nk_host_audio_stop_asset_kind(&host->audio, "cinema.sound_");
        else if (host->active_scene == NK_SCENE_TITLE)
            nk_host_audio_stop_asset_kind(&host->audio, "title_speech.sound_");
        return nk_enter_scene(host);
    }
    return 1;
}

static int nk_controller_button(SDL_GameController *controller, SDL_GameControllerButton button)
{
    return controller && SDL_GameControllerGetButton(controller, button);
}

static void nk_make_input(NkHost *host, nk_input_sample input[2])
{
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int menu = host->flow.scene == NK_SCENE_TITLE ||
               host->flow.scene == NK_SCENE_OPTIONS ||
               host->flow.mode == NK_MODE_ONE_PLAYER;
    int accept = menu || host->flow.scene == NK_SCENE_SELECT;
    int left[2] = {keys[SDL_SCANCODE_A] || (menu && keys[SDL_SCANCODE_LEFT]),
                   keys[SDL_SCANCODE_LEFT]};
    int right[2] = {keys[SDL_SCANCODE_D] || (menu && keys[SDL_SCANCODE_RIGHT]),
                    keys[SDL_SCANCODE_RIGHT]};
    int up[2] = {keys[SDL_SCANCODE_W] || (menu && keys[SDL_SCANCODE_UP]),
                 keys[SDL_SCANCODE_UP]};
    int down[2] = {keys[SDL_SCANCODE_S] || (menu && keys[SDL_SCANCODE_DOWN]),
                   keys[SDL_SCANCODE_DOWN]};
    int one[2] = {keys[SDL_SCANCODE_J] || keys[SDL_SCANCODE_Z] ||
                  (accept && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] ||
                              keys[SDL_SCANCODE_SPACE] || host->accept_keydown_latched)),
                  keys[SDL_SCANCODE_KP_1] || keys[SDL_SCANCODE_RCTRL]};
    int two[2] = {keys[SDL_SCANCODE_K] || keys[SDL_SCANCODE_X],
                  keys[SDL_SCANCODE_KP_2] || keys[SDL_SCANCODE_RSHIFT]};
    int pause = keys[SDL_SCANCODE_P];
    int cancel = keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_BACKSPACE];
    for (int player = 0; player < 2; ++player) {
        SDL_GameController *controller = host->controllers[player];
        u8 direction = 0, buttons = 0, commands = 0;
        int axis_x = controller ? SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) : 0;
        int axis_y = controller ? SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) : 0;
        if (left[player] || axis_x < -16000 ||
            nk_controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) direction |= NK_DIR_LEFT;
        if (right[player] || axis_x > 16000 ||
            nk_controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) direction |= NK_DIR_RIGHT;
        if (up[player] || axis_y < -16000 ||
            nk_controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) direction |= NK_DIR_UP;
        if (down[player] || axis_y > 16000 ||
            nk_controller_button(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) direction |= NK_DIR_DOWN;
        if (one[player] || nk_controller_button(controller, SDL_CONTROLLER_BUTTON_A)) buttons |= NK_BUTTON_ONE;
        if (two[player] || nk_controller_button(controller, SDL_CONTROLLER_BUTTON_B)) buttons |= NK_BUTTON_TWO;
        if ((player == 0 && pause) || nk_controller_button(controller, SDL_CONTROLLER_BUTTON_START)) commands |= NK_COMMAND_PAUSE;
        if ((player == 0 && cancel) || nk_controller_button(controller, SDL_CONTROLLER_BUTTON_BACK)) commands |= NK_COMMAND_CANCEL;
        nk_input_sample_make(&host->input_filters[player], direction, buttons, commands,
                             &input[player]);
        /* Preserve a quick Enter/Space press even if it ended between ticks. */
        if (player == 0 && accept && host->accept_keydown_latched)
            input[player].stat |= NK_STAT_BUTTON_ONE;
    }
    if (keys[SDL_SCANCODE_LCTRL] && keys[SDL_SCANCODE_B]) {
        if (!host->background_toggle_down) host->background_hidden = !host->background_hidden;
        host->background_toggle_down = 1;
    } else host->background_toggle_down = 0;
}

static void nk_close(NkHost *host)
{
    nk_host_audio_close(&host->audio);
    for (size_t i = 0; i < host->image_count; ++i) {
        free(host->images[i].pixels);
        free(host->images[i].path);
    }
    free(host->images);
    for (int i = 0; i < 2; ++i)
        if (host->controllers[i]) SDL_GameControllerClose(host->controllers[i]);
    nk_hd_upscaler_destroy(&host->hd_upscaler);
    if (host->hd_texture) SDL_DestroyTexture(host->hd_texture);
    if (host->texture) SDL_DestroyTexture(host->texture);
    if (host->renderer) SDL_DestroyRenderer(host->renderer);
    if (host->window) SDL_DestroyWindow(host->window);
    SDL_Quit();
}

static int nk_run_smoke(NkHost *host)
{
    nk_input_sample input[2] = {{0}, {0}};
    const int cinema_scenes[3] = {NK_SCENE_LOGO, NK_SCENE_CREDITS, NK_SCENE_ENDING};
    const char *cinema_names[3] = {"logo", "credits", "ending"};
    for (int scene = 0; scene < 3; ++scene) {
        host->flow.scene = cinema_scenes[scene];
        if (host->flow.scene == NK_SCENE_ENDING) host->flow.ending_number = 1;
        if (!nk_enter_scene(host)) return 0;
        for (int i = 0; i < 100 && !host->cinema.completed; ++i) {
            nk_cinema_tick(&host->cinema);
            nk_play_cinema_sound(host);
        }
        if (!nk_render(host) || !nk_save_smoke_frame(host, cinema_names[scene]))
            return 0;
    }
    {
        const char *intro_sound = "extracted/LOGO/audio/cinema.sound_0.wav";
        host->flow.scene = NK_SCENE_LOGO;
        if (!nk_enter_scene(host) || !nk_tick(host, input) ||
            !nk_host_audio_play_asset(&host->audio, host->asset_root,
                                      intro_sound, host->flow.options.sound_volume) ||
            !nk_host_audio_asset_playing(&host->audio, intro_sound))
            return 0;
        input[0].stat = NK_STAT_BUTTON_ONE;
        input[0].buttons = NK_BUTTON_ONE;
        if (!nk_tick(host, input) || host->flow.scene != NK_SCENE_TITLE ||
            nk_host_audio_asset_playing(&host->audio, intro_sound)) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Smoke: intro sound continued after skip");
            return 0;
        }
        input[0].stat = input[0].buttons = 0;
    }
    host->flow.scene = NK_SCENE_TITLE;
    if (!nk_enter_scene(host)) return 0;
    for (int i = 0; i < 250; ++i) if (!nk_tick(host, input)) return 0;
    if (!nk_render(host) ||
        !nk_save_smoke_frame(host, "title")) return 0;
    host->flow.options.hd_upscale = 1;
    if (!nk_render(host) || !nk_save_smoke_frame(host, "hd-title")) return 0;
    host->flow.options.hd_upscale = 0;
    for (int i = 0; i < 2; ++i) {
        input[0].stat = NK_DIR_DOWN;
        if (!nk_tick(host, input)) return 0;
        input[0].stat = 0;
        if (!nk_tick(host, input)) return 0;
    }
    input[0].stat = NK_STAT_BUTTON_ONE;
    input[0].buttons = NK_BUTTON_ONE;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = input[0].buttons = 0;
    for (int i = 0; i < 40 && host->flow.scene == NK_SCENE_TITLE; ++i)
        if (!nk_tick(host, input)) return 0;
    if (host->flow.scene != NK_SCENE_OPTIONS ||
        (host->title_voice_path[0] &&
         nk_host_audio_asset_playing(&host->audio, host->title_voice_path))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Smoke: title Enter was delayed or title speech continued");
        return 0;
    }
    for (int i = 0; i < 80; ++i) if (!nk_tick(host, input)) return 0;
    if (!nk_render(host) ||
        !nk_save_smoke_frame(host, "options")) return 0;
    host->options.pending.hd_upscale = 1;
    if (!nk_render(host) || !nk_save_smoke_frame(host, "hd-options")) return 0;
    host->options.pending.hd_upscale = 0;
    nk_flow_options_complete(&host->flow, &host->flow.options);
    if (!nk_enter_scene(host)) return 0;
    for (int i = 0; i < 250; ++i) if (!nk_tick(host, input)) return 0;
    input[0].stat = NK_STAT_BUTTON_ONE;
    input[0].buttons = NK_BUTTON_ONE;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = input[0].buttons = 0;
    for (int i = 0; i < 100 && host->flow.scene == NK_SCENE_TITLE; ++i)
        if (!nk_tick(host, input)) return 0;
    if (host->flow.scene != NK_SCENE_SELECT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Smoke: title input did not enter selection");
        return 0;
    }
    for (int i = 0; i < 88; ++i) if (!nk_tick(host, input)) return 0;
    if (!nk_render(host) ||
        !nk_save_smoke_frame(host, "select")) return 0;
    input[0].stat = NK_STAT_BUTTON_ONE;
    input[0].buttons = NK_BUTTON_ONE;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = input[0].buttons = 0;
    {
        int both_locked_tick = -1;
        for (int i = 0; i < 600 && host->flow.scene == NK_SCENE_SELECT; ++i) {
            if (!nk_tick(host, input)) return 0;
            if (both_locked_tick < 0 && host->selection.locked[0] &&
                host->selection.locked[1])
                both_locked_tick = i;
            if (both_locked_tick >= 0 && i - both_locked_tick > 40) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Smoke: selection waited for its sound after confirmation");
                return 0;
            }
        }
    }
    if (host->flow.scene != NK_SCENE_MATCH) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Smoke: fighter selection did not enter match");
        return 0;
    }
    {
        int active_ticks = 0;
        for (int i = 0; i < 1200 && active_ticks < 30; ++i) {
            if (!nk_tick(host, input)) return 0;
            if (host->game.game_state == NK_GAME_STATE_ACTIVE) active_ticks++;
        }
        if (active_ticks < 30) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Smoke: match never entered active ball play");
            return 0;
        }
    }
    if (!nk_render(host) || !nk_save_smoke_frame(host, "match")) return 0;
    host->flow.options.hd_upscale = 1;
    if (!nk_render(host) || !nk_save_smoke_frame(host, "hd-match")) return 0;
    host->flow.options.hd_upscale = 0;
    input[0].commands = NK_COMMAND_CANCEL;
    if (!nk_tick(host, input) || host->flow.scene != NK_SCENE_TITLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Smoke: cancel did not return to title");
        return 0;
    }
    input[0].commands = 0;
    for (int i = 0; i < 250; ++i) if (!nk_tick(host, input)) return 0;
    input[0].stat = NK_DIR_DOWN;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = 0;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = NK_STAT_BUTTON_ONE;
    input[0].buttons = NK_BUTTON_ONE;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = input[0].buttons = 0;
    for (int i = 0; i < 100 && host->flow.scene == NK_SCENE_TITLE; ++i)
        if (!nk_tick(host, input)) return 0;
    if (host->flow.scene != NK_SCENE_SELECT || host->flow.mode != NK_MODE_TWO_PLAYER)
        return 0;
    host->accept_keydown_latched = 1;
    nk_make_input(host, input);
    host->accept_keydown_latched = 0;
    if ((input[0].stat & NK_STAT_BUTTON_ONE) == 0U) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Smoke: Enter did not confirm player one in two-player select");
        return 0;
    }
    input[1].stat = NK_STAT_BUTTON_ONE;
    input[1].buttons = NK_BUTTON_ONE;
    if (!nk_tick(host, input)) return 0;
    input[0].stat = input[1].stat = 0;
    input[0].buttons = input[1].buttons = 0;
    for (int i = 0; i < 600 && host->flow.scene == NK_SCENE_SELECT; ++i)
        if (!nk_tick(host, input)) return 0;
    if (host->flow.scene != NK_SCENE_MATCH ||
        host->flow.control[1] != NK_CONTROL_HUMAN) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Smoke: two-player selection failed");
        return 0;
    }
    if (host->title_voice_events == 0 || host->cinema_sound_events == 0 ||
        host->select_scream_events == 0 || host->audio_asset_failures != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                     "Smoke audio failed: title=%u cinema=%u select=%u failures=%u",
                     host->title_voice_events, host->cinema_sound_events,
                     host->select_scream_events, host->audio_asset_failures);
        return 0;
    }
    SDL_Log("Smoke test passed: intro skip audio, prompt menu confirmation, 1P/2P matches, cancel and %u audio assets",
            host->title_voice_events + host->cinema_sound_events +
            host->select_scream_events);
    return 1;
}

int nk_frontend_main(int argc, char **argv)
{
    NkHost *host;
    const char *requested_assets = NULL;
    int skip_intro = 0;
    int smoke = 0;
    const char *smoke_prefix = "nk2-smoke";
    int result = 1;
    uint64_t last_counter;
    double accumulator = 0;
    double frequency;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) requested_assets = argv[++i];
        else if (strcmp(argv[i], "--skip-intro") == 0) skip_intro = 1;
        else if (strcmp(argv[i], "--smoke") == 0) smoke = 1;
        else if (strcmp(argv[i], "--smoke-output") == 0 && i + 1 < argc)
            smoke_prefix = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            puts("Usage: noggin2 [--assets DIRECTORY] [--skip-intro] [--smoke] [--smoke-output PREFIX]\n"
                 "Player 1: WASD + J/K (or arrows + Z/X in one-player mode).\n"
                 "Player 2: arrows + keypad 1/2. P pauses; Esc returns to title; Ctrl+B hides the background.");
            return 0;
        }
    }
    host = (NkHost *)calloc(1, sizeof(*host));
    if (!host) return 1;
    host->smoke = smoke;
    snprintf(host->smoke_prefix, sizeof(host->smoke_prefix), "%s", smoke_prefix);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        goto done;
    }
    if (!nk_find_asset_root(host->asset_root, sizeof(host->asset_root), requested_assets) ||
        !nk_load_image_index(host)) {
        fprintf(stderr, "Cannot find game assets. Pass --assets path/to/native/assets.\n");
        goto done;
    }
    nk_load_scene_palettes(host);
    host->window = SDL_CreateWindow("Noggin Knockers 2", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, 960, 720,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!host->window) goto done;
    host->renderer = SDL_CreateRenderer(host->window, -1,
                                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!host->renderer) host->renderer = SDL_CreateRenderer(host->window, -1,
                                                              SDL_RENDERER_SOFTWARE);
    if (!host->renderer) goto done;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_RenderSetLogicalSize(host->renderer, NK_HOST_WIDTH, 240) != 0) goto done;
    host->texture = SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_ABGR8888,
                                      SDL_TEXTUREACCESS_STREAMING, NK_HOST_WIDTH, NK_HOST_HEIGHT);
    if (!host->texture) goto done;
    for (int joystick = 0, slot = 0; joystick < SDL_NumJoysticks() && slot < 2; ++joystick)
        if (SDL_IsGameController(joystick))
            host->controllers[slot++] = SDL_GameControllerOpen(joystick);
    nk_host_audio_open(&host->audio, host->asset_root);
    nk_flow_init(&host->flow);
    if (smoke || !nk_load_options(&host->flow))
        host->flow.options.music_volume = 5;
    if (skip_intro) host->flow.scene = NK_SCENE_TITLE;
    nk_rng_seed(&host->rng, (u32)SDL_GetTicks());
    nk_dialogue_init(&host->dialogue);
    nk_input_filter_reset(&host->input_filters[0]);
    nk_input_filter_reset(&host->input_filters[1]);
    if (!nk_enter_scene(host)) {
        fprintf(stderr, "Cannot initialize game scene.\n");
        goto done;
    }
    if (smoke) {
        result = nk_run_smoke(host) ? 0 : 1;
        goto done;
    }
    frequency = (double)SDL_GetPerformanceFrequency();
    last_counter = SDL_GetPerformanceCounter();
    host->running = 1;
    while (host->running) {
        SDL_Event event;
        nk_input_sample input[2];
        uint64_t counter;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) host->running = 0;
            else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Scancode key = event.key.keysym.scancode;
                if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER ||
                    key == SDL_SCANCODE_SPACE)
                    host->accept_keydown_latched = 1;
                if (key == SDL_SCANCODE_F11) {
                    host->fullscreen = !host->fullscreen;
                    SDL_SetWindowFullscreen(host->window,
                                            host->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
            }
        }
        counter = SDL_GetPerformanceCounter();
        accumulator += (double)(counter - last_counter) / frequency;
        last_counter = counter;
        if (accumulator > 0.2) accumulator = 0.2;
        /* Advance button-edge filters only when a game step will consume them. */
        if (accumulator >= 0.01 && host->running) {
            nk_make_input(host, input);
            host->accept_keydown_latched = 0;
            for (int steps = 0; accumulator >= 0.01 && steps < 20 && host->running; ++steps) {
                if (!nk_tick(host, input)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game step failed in scene %d", host->flow.scene);
                    host->running = 0;
                    break;
                }
                nk_input_sample_consume_edges(&input[0]);
                nk_input_sample_consume_edges(&input[1]);
                accumulator -= 0.01;
            }
        }
        if (host->running) nk_render(host);
        SDL_Delay(1);
    }
    result = 0;
done:
    nk_close(host);
    free(host);
    return result;
}
