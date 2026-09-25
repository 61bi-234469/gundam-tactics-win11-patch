#include "../qtim_proxy/movdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, unsigned long value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static int write_bmp(const char *path, const MovDecoder *movie,
                     const unsigned char *pixels) {
    const MovTrackInfo *video = movdec_video_info(movie);
    const unsigned char *rgb = movdec_palette_rgb(movie);
    unsigned width = video->width, height = video->height;
    unsigned stride = (width + 3) & ~3u;
    unsigned image_size = stride * height;
    unsigned dib_size = 40 + 256 * 4 + image_size;
    unsigned file_size = 14 + dib_size;
    unsigned char *buffer = (unsigned char *)calloc(1, file_size);
    FILE *file;
    unsigned y, x;
    if (!buffer) return 0;
    buffer[0] = 'B'; buffer[1] = 'M';
    put_u32(buffer + 2, file_size);
    put_u32(buffer + 10, 14 + 40 + 256 * 4);
    put_u32(buffer + 14, 40);
    put_u32(buffer + 18, width);
    put_u32(buffer + 22, height);
    put_u16(buffer + 26, 1);
    put_u16(buffer + 28, 8);
    put_u32(buffer + 34, image_size);
    /* The reference QuickTime/ffmpeg BMP leaves biClrUsed at zero. */
    put_u32(buffer + 46, 0);
    for (x = 0; x < 256; x++) {
        buffer[54 + x * 4 + 0] = rgb[x * 3 + 2];
        buffer[54 + x * 4 + 1] = rgb[x * 3 + 1];
        buffer[54 + x * 4 + 2] = rgb[x * 3 + 0];
    }
    for (y = 0; y < height; y++) {
        memcpy(buffer + 14 + 40 + 256 * 4 + (height - y - 1) * stride,
               pixels + y * width, width);
    }
    file = fopen(path, "wb");
    if (!file || fwrite(buffer, 1, file_size, file) != file_size) {
        if (file) fclose(file);
        free(buffer);
        return 0;
    }
    fclose(file);
    free(buffer);
    return 1;
}

static void json_path(char *dst, size_t cap, const char *src) {
    size_t i, n = 0;
    for (i = 0; src[i] && n + 2 < cap; i++) {
        if (src[i] == '\\' || src[i] == '"') dst[n++] = '\\';
        dst[n++] = src[i];
    }
    dst[n] = 0;
}

int main(int argc, char **argv) {
    MovDecoder *movie;
    const MovTrackInfo *video, *audio;
    const char *input, *outdir;
    uint32_t frame_count, i;
    uint32_t movie_timescale, movie_duration;
    char output[1024], manifest[1024], escaped[1024];
    FILE *mf;
    if (argc != 3) {
        fprintf(stderr, "usage: movdec.exe <movie.mov> <output-dir>\n");
        return 2;
    }
    input = argv[1]; outdir = argv[2];
    movie = movdec_open(input);
    if (!movie) {
        fprintf(stderr, "cannot parse movie: %s (%s)\n", input,
                movdec_last_error());
        return 1;
    }
    video = movdec_video_info(movie);
    audio = movdec_audio_info(movie);
    movie_timescale = movdec_movie_timescale(movie);
    movie_duration = movdec_movie_duration(movie);
    frame_count = video->sample_count;
    for (i = 0; i < frame_count; i++) {
        uint32_t decoded;
        const unsigned char *pixels = movdec_frame_index(movie, i, &decoded);
        if (!pixels) { fprintf(stderr, "decode failed at frame %lu\n", (unsigned long)i); movdec_close(movie); return 1; }
        snprintf(output, sizeof(output), "%s\\frame_%04lu.bmp", outdir, (unsigned long)(i + 1));
        if (!write_bmp(output, movie, pixels)) { fprintf(stderr, "write failed: %s\n", output); movdec_close(movie); return 1; }
    }
    snprintf(manifest, sizeof(manifest), "%s\\manifest.json", outdir);
    json_path(escaped, sizeof(escaped), input);
    mf = fopen(manifest, "w");
    if (!mf) { movdec_close(movie); return 1; }
    fprintf(mf, "{\n  \"input\": \"%s\",\n  \"width\": %lu,\n  \"height\": %lu,\n  \"timescale\": %lu,\n  \"duration\": %lu,\n  \"frame_count\": %lu,\n  \"fps\": %.9f,\n  \"video_timescale\": %lu,\n  \"video_duration\": %lu,\n  \"video_codec\": \"%s\",\n  \"audio\": {\"codec\": \"%s\", \"rate\": %lu, \"channels\": %u, \"bits\": %u}\n}\n",
            escaped, (unsigned long)video->width, (unsigned long)video->height,
            (unsigned long)movie_timescale, (unsigned long)movie_duration,
            (unsigned long)video->sample_count,
            movie_duration ? (double)video->sample_count * movie_timescale / movie_duration : 0.0,
            (unsigned long)video->timescale, (unsigned long)video->duration,
            video->codec, audio->codec, (unsigned long)audio->sample_rate,
            audio->channels, audio->bits_per_sample);
    fclose(mf);
    printf("decoded %lu frames %lux%lu codec=%s -> %s\n",
           (unsigned long)frame_count, (unsigned long)video->width,
           (unsigned long)video->height, video->codec, outdir);
    movdec_close(movie);
    return 0;
}
