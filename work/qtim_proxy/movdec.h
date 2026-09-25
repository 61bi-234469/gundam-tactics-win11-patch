#ifndef GT_MOVDEC_H
#define GT_MOVDEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct MovDecoder MovDecoder;

typedef struct MovTrackInfo {
    char handler[5];
    char codec[5];
    uint32_t timescale;
    uint32_t duration;
    uint32_t sample_count;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} MovTrackInfo;

MovDecoder *movdec_open(const char *path);
void movdec_close(MovDecoder *movie);
const MovTrackInfo *movdec_video_info(const MovDecoder *movie);
const MovTrackInfo *movdec_audio_info(const MovDecoder *movie);
uint32_t movdec_movie_timescale(const MovDecoder *movie);
uint32_t movdec_movie_duration(const MovDecoder *movie);

/* Decode the frame selected by a signed movie-clock timestamp. The returned
 * pixels are an internal top-down 8bpp buffer with a QuickTime-compatible
 * palette. Negative/preroll timestamps are clamped to the first frame. */
const uint8_t *movdec_frame(MovDecoder *movie, int32_t movie_time,
                            uint32_t *frame_index);
/* Decode by exact sample index. This is used by the CLI so validation does
 * not invent timestamps from an average frame duration. */
const uint8_t *movdec_frame_index(MovDecoder *movie, uint32_t index,
                                   uint32_t *frame_index);
uint32_t movdec_frame_index_for_time(const MovDecoder *movie,
                                     int32_t movie_time);
size_t movdec_video_bytes(const MovDecoder *movie);
const uint8_t *movdec_video_sample(const MovDecoder *movie, uint32_t index,
                                   uint32_t *size);
const uint8_t *movdec_audio_bytes(const MovDecoder *movie, uint32_t *size);
const uint8_t *movdec_palette_rgb(const MovDecoder *movie);
const char *movdec_codec(const MovDecoder *movie);
const char *movdec_last_error(void);

/* Exposed for the CLI and proxy DIB builder. */
void movdec_make_default_palette(uint8_t *rgb, size_t rgb_size);

#endif
