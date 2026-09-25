#include "movdec.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SampleToChunk {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} SampleToChunk;

typedef struct TrackState {
    MovTrackInfo info;
    uint32_t *offsets;
    uint32_t *sizes;
    uint32_t sample_size;
    uint32_t size_count;
    uint32_t *chunk_offsets;
    uint32_t chunk_count;
    SampleToChunk *stsc;
    uint32_t stsc_count;
    uint32_t *stts_count;
    uint32_t *stts_delta;
    uint32_t stts_runs;
    uint64_t stts_total;
    int saw_mdhd;
    int saw_hdlr;
    int saw_stsd;
    int saw_stts;
    int saw_stsz;
    int saw_stco;
    int saw_stsc;
} TrackState;

struct MovDecoder {
    uint8_t *data;
    size_t size;
    uint32_t movie_timescale;
    uint32_t movie_duration;
    TrackState video;
    TrackState audio;
    uint8_t *pixels;
    size_t pixel_bytes;
    uint8_t *audio_data;
    size_t audio_bytes;
    int32_t decoded_frame;
    uint8_t pair_table[512];
    uint8_t quad_table[1024];
    uint8_t octet_table[2048];
    uint8_t palette[256 * 3];
};

enum { MOVDEC_ERROR_MAX = 192 };
static char g_last_error[MOVDEC_ERROR_MAX];

static void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_last_error, sizeof(g_last_error), format, args);
    va_end(args);
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int has_atom(const uint8_t *p, const char *name) {
    return memcmp(p, name, 4) == 0;
}

static void free_track(TrackState *track) {
    free(track->offsets);
    free(track->sizes);
    free(track->chunk_offsets);
    free(track->stsc);
    free(track->stts_count);
    free(track->stts_delta);
    memset(track, 0, sizeof(*track));
}

static int ensure_count(void **ptr, size_t item_size, uint32_t count,
                        size_t available_bytes, size_t record_size) {
    size_t bytes;
    void *replacement;
    if (record_size && (uint64_t)count > available_bytes / record_size)
        return 0;
    if (count == 0) {
        free(*ptr);
        *ptr = NULL;
        return 1;
    }
    if ((uint64_t)count > (uint64_t)SIZE_MAX / item_size) return 0;
    bytes = item_size * (size_t)count;
    replacement = realloc(*ptr, bytes);
    if (!replacement) return 0;
    *ptr = replacement;
    return 1;
}

void movdec_make_default_palette(uint8_t *rgb, size_t rgb_size) {
    static const uint8_t cube[] = {255, 204, 153, 102, 51, 0};
    static const uint8_t ramp[] = {238, 221, 187, 170, 136, 119, 85, 68, 34, 17};
    size_t n = 0;
    int r, g, b;
    if (!rgb || rgb_size < 256 * 3) return;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                rgb[n++] = cube[r];
                rgb[n++] = cube[g];
                rgb[n++] = cube[b];
            }
        }
    }
    /* The final cube black is shared with the terminal grayscale entry in
     * the classic QuickTime 256-color palette. */
    n = 215 * 3;
    for (r = 0; r < 10; r++) {
        rgb[n++] = ramp[r]; rgb[n++] = 0; rgb[n++] = 0;
    }
    for (r = 0; r < 10; r++) {
        rgb[n++] = 0; rgb[n++] = ramp[r]; rgb[n++] = 0;
    }
    for (r = 0; r < 10; r++) {
        rgb[n++] = 0; rgb[n++] = 0; rgb[n++] = ramp[r];
    }
    for (r = 0; r < 10; r++) {
        rgb[n++] = ramp[r]; rgb[n++] = ramp[r]; rgb[n++] = ramp[r];
    }
    rgb[n++] = 0; rgb[n++] = 0; rgb[n++] = 0;
}

static int parse_mvhd(MovDecoder *movie, size_t start, size_t end) {
    const uint8_t *p = movie->data + start;
    size_t size = end - start;
    if (size < 4) return 0;
    if (p[0] == 0) {
        if (size < 20) return 0;
        movie->movie_timescale = be32(p + 12);
        movie->movie_duration = be32(p + 16);
    } else if (p[0] == 1 && end - start >= 32) {
        movie->movie_timescale = be32(p + 20);
        movie->movie_duration = be32(p + 28);
    } else return 0;
    return movie->movie_timescale != 0;
}

static int parse_hdlr(TrackState *track, const uint8_t *p, size_t size) {
    if (size < 12) return 0;
    if (memcmp(p + 8, "vide", 4) != 0 && memcmp(p + 8, "soun", 4) != 0)
        return 1; /* e.g. the nested data-reference handler (alis). */
    if (track->saw_hdlr) return 0;
    memcpy(track->info.handler, p + 8, 4);
    track->info.handler[4] = 0;
    track->saw_hdlr = 1;
    return 1;
}

static int parse_stsd(TrackState *track, const uint8_t *p, size_t size) {
    const uint8_t *entry;
    uint32_t entry_count;
    uint32_t entry_size;
    if (size < 8) return 0;
    entry_count = be32(p + 4);
    if (entry_count != 1) return 0;
    entry = p + 8;
    if (size - 8 < 8) return 0;
    entry_size = be32(entry);
    if (entry_size < 8 || entry_size > size - 8) return 0;
    memcpy(track->info.codec, entry + 4, 4);
    track->info.codec[4] = 0;
    /* Parse both standard sample-entry layouts before hdlr ordering is
     * known.  Some MOV writers place hdlr after stsd. */
    if (entry_size >= 36) {
        track->info.width = be16(entry + 24);
        track->info.height = be16(entry + 26);
        if (!track->info.width || !track->info.height) {
            track->info.width = be16(entry + 32);
            track->info.height = be16(entry + 34);
        }
        track->info.channels = be16(entry + 24);
        track->info.bits_per_sample = be16(entry + 26);
        track->info.sample_rate = be32(entry + 32) >> 16;
        if (!track->info.sample_rate) track->info.sample_rate = be32(entry + 32);
    }
    track->saw_stsd = 1;
    return 1;
}

static int parse_mdhd(TrackState *track, const uint8_t *p, size_t size) {
    if (size < 4) return 0;
    if (p[0] == 0) {
        if (size < 20) return 0;
        track->info.timescale = be32(p + 12);
        track->info.duration = be32(p + 16);
    } else if (p[0] == 1 && size >= 32) {
        track->info.timescale = be32(p + 20);
        track->info.duration = be32(p + 28);
    } else return 0;
    track->saw_mdhd = track->info.timescale != 0;
    return track->saw_mdhd;
}

static int parse_stts(TrackState *track, const uint8_t *p, size_t size) {
    uint32_t count, i;
    uint64_t total = 0;
    if (size < 8) return 0;
    count = be32(p + 4);
    if (!ensure_count((void **)&track->stts_count, sizeof(uint32_t), count,
                      size - 8, 8) ||
        !ensure_count((void **)&track->stts_delta, sizeof(uint32_t), count,
                      size - 8, 8)) return 0;
    track->stts_runs = count;
    for (i = 0; i < count; i++) {
        track->stts_count[i] = be32(p + 8 + i * 8);
        track->stts_delta[i] = be32(p + 12 + i * 8);
        total += track->stts_count[i];
        if (total > UINT32_MAX) return 0;
    }
    track->stts_total = total;
    track->saw_stts = 1;
    return 1;
}

static int parse_stsz(TrackState *track, const uint8_t *p, size_t size) {
    uint32_t i;
    if (size < 12) return 0;
    track->sample_size = be32(p + 4);
    track->size_count = be32(p + 8);
    if (!track->sample_size &&
        !ensure_count((void **)&track->sizes, sizeof(uint32_t),
                      track->size_count, size - 12, 4)) return 0;
    if (!track->sample_size) {
        for (i = 0; i < track->size_count; i++)
            track->sizes[i] = be32(p + 12 + i * 4);
    }
    track->saw_stsz = 1;
    return 1;
}

static int parse_stco(TrackState *track, const uint8_t *p, size_t size) {
    uint32_t count, i;
    if (size < 8) return 0;
    count = be32(p + 4);
    if (!ensure_count((void **)&track->chunk_offsets, sizeof(uint32_t), count,
                      size - 8, 4)) return 0;
    track->chunk_count = count;
    for (i = 0; i < count; i++)
        track->chunk_offsets[i] = be32(p + 8 + i * 4);
    track->saw_stco = 1;
    return 1;
}

static int parse_stsc(TrackState *track, const uint8_t *p, size_t size) {
    uint32_t count, i;
    if (size < 8) return 0;
    count = be32(p + 4);
    if (!ensure_count((void **)&track->stsc, sizeof(SampleToChunk), count,
                      size - 8, 12)) return 0;
    track->stsc_count = count;
    for (i = 0; i < count; i++) {
        track->stsc[i].first_chunk = be32(p + 8 + i * 12);
        track->stsc[i].samples_per_chunk = be32(p + 12 + i * 12);
        if (!track->stsc[i].first_chunk || !track->stsc[i].samples_per_chunk ||
            (i && track->stsc[i].first_chunk <= track->stsc[i - 1].first_chunk))
            return 0;
    }
    track->saw_stsc = 1;
    return 1;
}

static int atom_bounds(const uint8_t *data, size_t pos, size_t end,
                       size_t *header, size_t *atom_end, const uint8_t **kind) {
    uint64_t declared;
    if (pos > end || end - pos < 8) return 0;
    declared = be32(data + pos);
    *header = 8;
    if (declared == 1) {
        if (end - pos < 16) return 0;
        declared = ((uint64_t)be32(data + pos + 8) << 32) |
                   be32(data + pos + 12);
        *header = 16;
    } else if (declared == 0) {
        declared = end - pos;
    }
    if (declared < *header || declared > end - pos ||
        declared > (uint64_t)SIZE_MAX) return 0;
    *atom_end = pos + (size_t)declared;
    *kind = data + pos + 4;
    return 1;
}

static int parse_track_atoms(MovDecoder *movie, TrackState *track,
                             size_t start, size_t end) {
    size_t pos = start;
    while (pos < end) {
        size_t header = 8;
        size_t atom_end;
        const uint8_t *kind;
        if (!atom_bounds(movie->data, pos, end, &header, &atom_end, &kind)) {
            set_error("malformed track atom at %lu", (unsigned long)pos);
            return 0;
        }
        if (has_atom(kind, "mdhd")) {
            if (track->saw_mdhd || !parse_mdhd(track, movie->data + pos + header,
                                                atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "hdlr")) {
            if (!parse_hdlr(track, movie->data + pos + header,
                            atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "stsd")) {
            if (track->saw_stsd || !parse_stsd(track, movie->data + pos + header,
                                               atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "stts")) {
            if (track->saw_stts || !parse_stts(track, movie->data + pos + header,
                                               atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "stsz")) {
            if (track->saw_stsz || !parse_stsz(track, movie->data + pos + header,
                                               atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "stco")) {
            if (track->saw_stco || !parse_stco(track, movie->data + pos + header,
                                               atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "co64")) {
            set_error("unsupported co64 chunk offsets");
            return 0;
        } else if (has_atom(kind, "stsc")) {
            if (track->saw_stsc || !parse_stsc(track, movie->data + pos + header,
                                               atom_end - pos - header)) return 0;
        } else if (has_atom(kind, "mdia") || has_atom(kind, "minf") ||
                   has_atom(kind, "stbl")) {
            if (!parse_track_atoms(movie, track, pos + header, atom_end)) return 0;
        }
        pos = atom_end;
    }
    return pos == end;
}

static int finalize_track(MovDecoder *movie, TrackState *track) {
    uint32_t sample_count;
    uint32_t chunk, entry = 0, sample = 0;
    if (!track->saw_mdhd || !track->saw_hdlr || !track->saw_stsd ||
        !track->saw_stts || !track->saw_stsz || !track->saw_stco ||
        !track->saw_stsc || !track->stts_total || !track->chunk_count ||
        !track->stsc_count) return 0;
    if (track->stts_total > UINT32_MAX || track->size_count != track->stts_total)
        return 0;
    sample_count = (uint32_t)track->stts_total;
    track->info.sample_count = sample_count;
    if (!ensure_count((void **)&track->offsets, sizeof(uint32_t), sample_count,
                      SIZE_MAX, 0) || (!track->sample_size && !track->sizes)) return 0;
    if (track->stsc[0].first_chunk != 1) return 0;
    for (chunk = 1; chunk <= track->chunk_count; chunk++) {
        uint32_t samples_per_chunk;
        uint32_t j;
        while (entry + 1 < track->stsc_count &&
               track->stsc[entry + 1].first_chunk <= chunk) entry++;
        if (track->stsc[entry].first_chunk > chunk) return 0;
        samples_per_chunk = track->stsc[entry].samples_per_chunk;
        if ((uint64_t)sample + samples_per_chunk > sample_count) return 0;
        for (j = 0; j < samples_per_chunk; j++) {
            uint64_t offset = track->chunk_offsets[chunk - 1];
            uint32_t k;
            for (k = 0; k < j; k++) {
                uint32_t prior = track->sample_size ? track->sample_size :
                    track->sizes[sample - j + k];
                offset += prior;
            }
            {
                uint32_t length = track->sample_size ? track->sample_size :
                    track->sizes[sample];
                if (offset > movie->size || length > movie->size - (size_t)offset ||
                    offset > UINT32_MAX) return 0;
                track->offsets[sample] = (uint32_t)offset;
            }
            sample++;
        }
    }
    return sample == sample_count;
}

static int parse_movie_atoms(MovDecoder *movie, size_t start, size_t end) {
    size_t pos = start;
    while (pos < end) {
        size_t header = 8;
        size_t atom_end;
        const uint8_t *kind;
        if (!atom_bounds(movie->data, pos, end, &header, &atom_end, &kind)) {
            set_error("malformed movie atom at %lu", (unsigned long)pos);
            return 0;
        }
        if (has_atom(kind, "mvhd")) {
            if (!parse_mvhd(movie, pos + header, atom_end)) return 0;
        } else if (has_atom(kind, "trak")) {
            TrackState candidate;
            memset(&candidate, 0, sizeof(candidate));
            if (!parse_track_atoms(movie, &candidate, pos + header, atom_end)) {
                if (!g_last_error[0]) set_error("malformed track atom table");
                free_track(&candidate);
                return 0;
            }
            if (strcmp(candidate.info.handler, "vide") == 0 &&
                !movie->video.info.handler[0]) {
                movie->video = candidate;
                memset(&candidate, 0, sizeof(candidate));
            } else if (strcmp(candidate.info.handler, "soun") == 0 &&
                       !movie->audio.info.handler[0]) {
                movie->audio = candidate;
                memset(&candidate, 0, sizeof(candidate));
            }
            free_track(&candidate);
        } else if (has_atom(kind, "moov")) {
            if (!parse_movie_atoms(movie, pos + header, atom_end)) return 0;
        }
        pos = atom_end;
    }
    return pos == end;
}

static int prepare_samples(MovDecoder *movie) {
    TrackState *tracks[2] = {&movie->video, &movie->audio};
    int i;
    for (i = 0; i < 2; i++) {
        TrackState *track = tracks[i];
        if (!track->info.handler[0]) continue;
        if (!finalize_track(movie, track)) {
            set_error("invalid %s sample tables", track->info.handler);
            fprintf(stderr, "finalize failed handler=%s codec=%s samples=%lu sizes=%lu chunks=%lu stsc=%lu\\n",
                    track->info.handler, track->info.codec,
                    (unsigned long)track->stts_total,
                    (unsigned long)track->size_count,
                    (unsigned long)track->chunk_count,
                    (unsigned long)track->stsc_count);
            return 0;
        }
    }
    if (movie->video.info.width == 0 || movie->video.info.height == 0 ||
        (strcmp(movie->video.info.codec, "smc ") != 0 &&
         strcmp(movie->video.info.codec, "cvid") != 0)) {
        set_error("unsupported video handler=%s codec=%s", movie->video.info.handler,
                  movie->video.info.codec);
        fprintf(stderr, "unsupported video handler=%s codec=%s size=%lux%lu\\n",
                movie->video.info.handler, movie->video.info.codec,
                (unsigned long)movie->video.info.width,
                (unsigned long)movie->video.info.height);
        return 0;
    }
    movie->pixel_bytes = (size_t)movie->video.info.width * movie->video.info.height;
    movie->pixels = (uint8_t *)calloc(1, movie->pixel_bytes);
    if (!movie->pixels) return 0;
    movdec_make_default_palette(movie->palette, sizeof(movie->palette));

    if (movie->audio.info.handler[0]) {
        uint64_t total = movie->audio.sample_size ?
            (uint64_t)movie->audio.sample_size * movie->audio.info.sample_count : 0;
        uint32_t i;
        if (!movie->audio.sample_size) {
            for (i = 0; i < movie->audio.info.sample_count; i++)
                total += movie->audio.sizes[i];
        }
        if (total > SIZE_MAX || total > movie->size) return 0;
        movie->audio_data = (uint8_t *)malloc((size_t)total);
        if (!movie->audio_data && total) return 0;
        movie->audio_bytes = (size_t)total;
        for (i = 0; i < movie->audio.info.sample_count; i++) {
            uint32_t length = movie->audio.sample_size ? movie->audio.sample_size :
                movie->audio.sizes[i];
            uint32_t offset = movie->audio.offsets[i];
            size_t dst = movie->audio.sample_size ?
                (size_t)i * movie->audio.sample_size : 0;
            if (!movie->audio.sample_size) {
                uint32_t prior;
                for (prior = 0; prior < i; prior++) dst += movie->audio.sizes[prior];
            }
            memcpy(movie->audio_data + dst, movie->data + offset, length);
        }
    }
    movie->decoded_frame = -1;
    return 1;
}

MovDecoder *movdec_open(const char *path) {
    FILE *file;
    long length;
    MovDecoder *movie;
    g_last_error[0] = 0;
    file = fopen(path, "rb");
    if (!file) { set_error("cannot open movie"); return NULL; }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file); set_error("cannot seek movie"); return NULL;
    }
    length = ftell(file);
    if (length <= 0 || (uint64_t)length > SIZE_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); set_error("invalid movie length"); return NULL;
    }
    movie = (MovDecoder *)calloc(1, sizeof(*movie));
    if (!movie) { fclose(file); set_error("out of memory"); return NULL; }
    movie->size = (size_t)length;
    movie->data = (uint8_t *)malloc(movie->size);
    if (!movie->data || fread(movie->data, 1, movie->size, file) != movie->size) {
        fclose(file); movdec_close(movie); set_error("cannot read movie"); return NULL;
    }
    fclose(file);
    if (!parse_movie_atoms(movie, 0, movie->size) || !prepare_samples(movie)) {
        if (!g_last_error[0]) set_error("malformed or unsupported movie tables");
        movdec_close(movie); return NULL;
    }
    return movie;
}

void movdec_close(MovDecoder *movie) {
    if (!movie) return;
    free_track(&movie->video);
    free_track(&movie->audio);
    free(movie->pixels);
    free(movie->audio_data);
    free(movie->data);
    free(movie);
}

const MovTrackInfo *movdec_video_info(const MovDecoder *movie) { return movie ? &movie->video.info : NULL; }
const MovTrackInfo *movdec_audio_info(const MovDecoder *movie) { return movie ? &movie->audio.info : NULL; }
uint32_t movdec_movie_timescale(const MovDecoder *movie) { return movie ? movie->movie_timescale : 0; }
uint32_t movdec_movie_duration(const MovDecoder *movie) { return movie ? movie->movie_duration : 0; }
size_t movdec_video_bytes(const MovDecoder *movie) { return movie ? movie->pixel_bytes : 0; }
const uint8_t *movdec_palette_rgb(const MovDecoder *movie) { return movie ? movie->palette : NULL; }
const char *movdec_codec(const MovDecoder *movie) { return movie ? movie->video.info.codec : ""; }
const char *movdec_last_error(void) {
    return g_last_error[0] ? g_last_error : "malformed or unsupported movie";
}

static uint32_t frame_for_time(const TrackState *track, int32_t movie_time,
                               uint32_t movie_scale) {
    uint64_t target, elapsed = 0;
    uint32_t frame = 0, run;
    uint32_t i;
    if (!track || !track->info.timescale || !movie_scale || movie_time <= 0)
        return 0;
    target = (uint64_t)movie_time * track->info.timescale / movie_scale;
    for (i = 0; i < track->stts_runs; i++) {
        run = track->stts_count[i];
        if (target < elapsed + (uint64_t)run * track->stts_delta[i]) {
            uint32_t delta = track->stts_delta[i] ? track->stts_delta[i] : 1;
            frame += (uint32_t)((target - elapsed) / delta);
            return frame < track->info.sample_count ? frame : track->info.sample_count - 1;
        }
        elapsed += (uint64_t)run * track->stts_delta[i];
        frame += run;
    }
    return track->info.sample_count ? track->info.sample_count - 1 : 0;
}

uint32_t movdec_frame_index_for_time(const MovDecoder *movie, int32_t movie_time) {
    if (!movie) return 0;
    return frame_for_time(&movie->video, movie_time, movie->movie_timescale);
}

static void copy_block(MovDecoder *movie, uint32_t dst, uint32_t src) {
    uint32_t w = movie->video.info.width;
    uint32_t blocks_w = (w + 3) / 4;
    uint32_t dx = (dst % blocks_w) * 4, dy = (dst / blocks_w) * 4;
    uint32_t sx = (src % blocks_w) * 4, sy = (src / blocks_w) * 4;
    uint32_t y, x;
    for (y = 0; y < 4 && dy + y < movie->video.info.height; y++)
        for (x = 0; x < 4 && dx + x < w; x++)
            movie->pixels[(dy + y) * w + dx + x] = movie->pixels[(sy + y) * w + sx + x];
}

static void fill_block(MovDecoder *movie, uint32_t block, uint8_t value) {
    uint32_t w = movie->video.info.width, blocks_w = (w + 3) / 4;
    uint32_t x = (block % blocks_w) * 4, y = (block / blocks_w) * 4;
    uint32_t yy, xx;
    for (yy = 0; yy < 4 && y + yy < movie->video.info.height; yy++)
        for (xx = 0; xx < 4 && x + xx < w; xx++) movie->pixels[(y + yy) * w + x + xx] = value;
}

static int smc_decode(MovDecoder *movie, const uint8_t *sample, uint32_t size) {
    size_t pos = 0;
    uint32_t total = ((movie->video.info.width + 3) / 4) * ((movie->video.info.height + 3) / 4);
    uint32_t block = 0, pair = 0, quad = 0, octet = 0;
    if (size < 4) return 0;
    pos = 1;
    if (pos + 3 > size) return 0;
    pos += 3; /* SMC header byte and 24-bit encoded-size field. */
    while (block < total && pos < size) {
        uint8_t opcode = sample[pos++];
        uint32_t n, i;
        switch (opcode & 0xF0) {
        case 0x00: case 0x10:
            n = (opcode & 0x10) ? (pos < size ? 1 + sample[pos++] : 0) : 1 + (opcode & 0x0F);
            block += n;
            break;
        case 0x20: case 0x30:
            n = (opcode & 0x10) ? (pos < size ? 1 + sample[pos++] : 0) : 1 + (opcode & 0x0F);
            if (block == 0) return 0;
            for (i = 0; i < n && block < total; i++, block++) copy_block(movie, block, block - 1);
            break;
        case 0x40: case 0x50:
            n = (opcode & 0x10) ? (pos < size ? 1 + sample[pos++] : 0) : 1 + (opcode & 0x0F);
            n *= 2;
            if (block < 2) return 0;
            {
                uint32_t first = block;
                for (i = 0; i < n && block < total; i++, block++)
                    copy_block(movie, block, first - 2 + (i & 1));
            }
            break;
        case 0x60: case 0x70:
            n = (opcode & 0x10) ? (pos < size ? 1 + sample[pos++] : 0) : 1 + (opcode & 0x0F);
            if (pos >= size) return 0;
            for (i = 0; i < n && block < total; i++) fill_block(movie, block++ , sample[pos]);
            pos++;
            break;
        case 0x80: case 0x90: {
            uint32_t base;
            n = 1 + (opcode & 0x0F);
            if ((opcode & 0xF0) == 0x80) {
                if (pos + 2 > size) return 0;
                movie->pair_table[pair * 2] = sample[pos++];
                movie->pair_table[pair * 2 + 1] = sample[pos++];
                base = pair++ * 2;
                pair %= 256;
            } else { if (pos >= size) return 0; base = sample[pos++] * 2; }
            for (i = 0; i < n && block < total; i++) {
                uint16_t flags;
                uint32_t y, x;
                if (pos + 2 > size) return 0;
                flags = be16(sample + pos);
                pos += 2;
                for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
                    uint32_t bx = (block % ((movie->video.info.width + 3) / 4)) * 4 + x;
                    uint32_t by = (block / ((movie->video.info.width + 3) / 4)) * 4 + y;
                    if (bx < movie->video.info.width && by < movie->video.info.height)
                        movie->pixels[by * movie->video.info.width + bx] = movie->pair_table[base + ((flags & (0x8000 >> (y * 4 + x))) ? 1 : 0)];
                }
                block++;
            }
            break;
        }
        case 0xA0: case 0xB0: {
            uint32_t base;
            n = 1 + (opcode & 0x0F);
            if ((opcode & 0xF0) == 0xA0) {
                if (pos + 4 > size) return 0;
                for (i = 0; i < 4; i++)
                    movie->quad_table[quad * 4 + i] = sample[pos++];
                base = quad++ * 4; quad %= 256;
            } else { if (pos >= size) return 0; base = sample[pos++] * 4; }
            for (i = 0; i < n && block < total; i++) { uint32_t flags, y, x; if (pos + 4 > size) return 0; flags = be32(sample + pos); pos += 4;
                for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) { uint32_t q = (flags >> (30 - 2 * (y * 4 + x))) & 3; uint32_t bx = (block % ((movie->video.info.width + 3) / 4)) * 4 + x, by = (block / ((movie->video.info.width + 3) / 4)) * 4 + y; if (bx < movie->video.info.width && by < movie->video.info.height) movie->pixels[by * movie->video.info.width + bx] = movie->quad_table[base + q]; }
                block++;
            } break;
        }
        case 0xC0: case 0xD0: {
            uint32_t base;
            n = 1 + (opcode & 0x0F);
            if ((opcode & 0xF0) == 0xC0) { if (pos + 8 > size) return 0; for (i = 0; i < 8; i++) movie->octet_table[octet * 8 + i] = sample[pos++]; base = octet++ * 8; octet %= 256; }
            else { if (pos >= size) return 0; base = sample[pos++] * 8; }
            for (i = 0; i < n && block < total; i++) {
                uint16_t v1, v2, v3;
                uint32_t fa, fb, y, x;
                if (pos + 6 > size) return 0;
                v1 = be16(sample + pos);
                v2 = be16(sample + pos + 2);
                v3 = be16(sample + pos + 4);
                pos += 6;
                fa = ((v1 & 0xFFF0) << 8) | (v2 >> 4);
                fb = ((v3 & 0xFFF0) << 8) | ((v1 & 0xF) << 8) |
                     ((v2 & 0xF) << 4) | (v3 & 0xF);
                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 4; x++) {
                        uint32_t flags = y < 2 ? fa : fb;
                        uint32_t idx = (flags >> (21 - 3 * (x + 4 * (y & 1)))) & 7;
                        uint32_t bx = (block % ((movie->video.info.width + 3) / 4)) * 4 + x;
                        uint32_t by = (block / ((movie->video.info.width + 3) / 4)) * 4 + y;
                        if (bx < movie->video.info.width && by < movie->video.info.height)
                            movie->pixels[by * movie->video.info.width + bx] =
                                movie->octet_table[base + idx];
                    }
                }
                block++;
            }
            break;
        }
        case 0xE0: case 0xF0:
            n = 1 + (opcode & 0x0F);
            for (i = 0; i < n && block < total; i++) { if (pos + 16 > size) return 0; uint32_t y, x; for (y=0;y<4;y++) for(x=0;x<4;x++){uint32_t bx=(block%((movie->video.info.width+3)/4))*4+x,by=(block/((movie->video.info.width+3)/4))*4+y;if(bx<movie->video.info.width&&by<movie->video.info.height)movie->pixels[by*movie->video.info.width+bx]=sample[pos];pos++;} block++; }
            break;
        default: return 0;
        }
    }
    return block >= total;
}

const uint8_t *movdec_frame_index(MovDecoder *movie, uint32_t target,
                                  uint32_t *frame_index) {
    uint32_t i, size;
    const uint8_t *sample;
    if (!movie || strcmp(movie->video.info.codec, "smc ") != 0 ||
        !movie->video.offsets || !movie->video.info.sample_count ||
        target >= movie->video.info.sample_count) return NULL;
    if ((int32_t)target < movie->decoded_frame) {
        memset(movie->pixels, 0, movie->pixel_bytes);
        memset(movie->pair_table, 0, sizeof(movie->pair_table));
        memset(movie->quad_table, 0, sizeof(movie->quad_table));
        memset(movie->octet_table, 0, sizeof(movie->octet_table));
        movie->decoded_frame = -1;
    }
    for (i = (uint32_t)(movie->decoded_frame + 1); i <= target; i++) {
        sample = movdec_video_sample(movie, i, &size);
        if (!sample || !smc_decode(movie, sample, size)) return NULL;
        movie->decoded_frame = (int32_t)i;
    }
    if (frame_index) *frame_index = target;
    return movie->pixels;
}

const uint8_t *movdec_frame(MovDecoder *movie, int32_t movie_time,
                            uint32_t *frame_index) {
    uint32_t target;
    if (!movie) return NULL;
    target = frame_for_time(&movie->video, movie_time, movie->movie_timescale);
    return movdec_frame_index(movie, target, frame_index);
}

const uint8_t *movdec_video_sample(const MovDecoder *movie, uint32_t index, uint32_t *size) {
    uint32_t offset, length;
    if (!movie || index >= movie->video.info.sample_count) return NULL;
    offset = movie->video.offsets[index];
    length = movie->video.sample_size ? movie->video.sample_size : movie->video.sizes[index];
    if ((uint64_t)offset + length > movie->size) return NULL;
    if (size) *size = length;
    return movie->data + offset;
}

const uint8_t *movdec_audio_bytes(const MovDecoder *movie, uint32_t *size) {
    if (!movie || !movie->audio.info.sample_count || !movie->audio_data ||
        movie->audio_bytes > UINT32_MAX) return NULL;
    if (size) *size = (uint32_t)movie->audio_bytes;
    return movie->audio_data;
}
