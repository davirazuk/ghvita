#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#include <opusfile.h>
#include <mpg123.h>
#include "decoder.h"
#include "resamp.h"
#include "plat.h"

#define SRC_FRAMES 4096                 // the resampler's window of source frames

typedef enum { K_VORBIS, K_OPUS, K_MP3, K_WAV } Kind;

struct Decoder {
    Kind kind;
    int rate, channels;                 // the file's
    int mix[64][2];                     // past stereo (a Rock Band .mogg): each track's share of left, right
    bool have_mix;
    int64_t length;                     // in the file's frames (0 unknown)
    OggVorbis_File vf;
    OggOpusFile * of;
    mpg123_handle * mh;
    FILE * wav;
    long wav_data;                      // where the samples start
    int wav_bits, wav_float, wav_block;
    // resampling (rate != 48000): source frames as stereo, the next output frame's place among them
    int16_t * src;
    int src_count, src_real;            // frames in src, and how many of them are the file's (the rest silence)
    double pos, step;
    bool src_eof;
    RsKernel kern;
};

// ------------------------------------------------------------------------------------------- the file's own frames

static int16_t clamp16(int v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

// stereo frames at the file's rate
static int raw_read(Decoder * d, int16_t * out, int frames)
{
    switch (d->kind) {
    case K_OPUS: {
        int n = op_read_stereo(d->of, out, frames * 2);
        return n == OP_HOLE ? raw_read(d, out, frames) : n;
    }
    case K_VORBIS: {
        int got = 0;
        int16_t tmp[4096];
        while (got < frames) {
            int want = frames - got;
            if (want * d->channels > 4096)
                want = 4096 / d->channels;
            int bitstream;
            long n = ov_read(&d->vf, (char *)tmp, want * d->channels * 2, 0, 2, 1, &bitstream);
            if (n == OV_HOLE)
                continue;
            if (n <= 0)
                return got ? got : (int)n;
            int f = (int)(n / 2 / d->channels);
            for (int i = 0; i < f; i++) {
                if (d->channels > 2) {
                    // a Rock Band .mogg: a pair of tracks to each instrument, all of it the song
                    int sum[2] = {0, 0};
                    for (int c = 0; c < d->channels && c < 64; c++) {
                        int s = tmp[i * d->channels + c];
                        if (d->have_mix) {
                            sum[0] += s * d->mix[c][0];
                            sum[1] += s * d->mix[c][1];
                        } else
                            sum[c & 1] += s * 3072;
                    }
                    for (int c = 0; c < 2; c++) {
                        int v = sum[c] >> 12;
                        out[(got + i) * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                    continue;
                }
                out[(got + i) * 2] = tmp[i * d->channels];
                out[(got + i) * 2 + 1] = tmp[i * d->channels + (d->channels > 1)];
            }
            got += f;
            if (bitstream != 0 && ov_info(&d->vf, bitstream)->channels != d->channels)
                d->channels = ov_info(&d->vf, bitstream)->channels;
        }
        return got;
    }
    case K_MP3: {
        size_t done = 0;
        int r = mpg123_read(d->mh, (unsigned char *)out, frames * 4, &done);
        if (r == MPG123_NEW_FORMAT && done == 0)
            return raw_read(d, out, frames);
        if (done == 0)
            return r == MPG123_DONE ? 0 : r == MPG123_OK ? 0 : -1;
        return (int)(done / 4);
    }
    case K_WAV: {
        uint8_t buf[8192];
        int want = frames;
        if (want * d->wav_block > (int)sizeof(buf))
            want = (int)sizeof(buf) / d->wav_block;
        int f = (int)fread(buf, d->wav_block, want, d->wav);
        int bytes = d->wav_bits / 8;
        for (int i = 0; i < f; i++)
            for (int c = 0; c < 2; c++) {
                const uint8_t * p = buf + i * d->wav_block + (c < d->channels ? c : 0) * bytes;
                int v;
                if (d->wav_float) {
                    float x;
                    memcpy(&x, p, 4);
                    v = (int)(x * 32767);
                } else if (bytes == 1)
                    v = (p[0] - 128) << 8;
                else if (bytes == 2)
                    v = (int16_t)(p[0] | p[1] << 8);
                else if (bytes == 3)
                    v = (int16_t)(p[1] | p[2] << 8);
                else
                    v = (int16_t)(p[2] | p[3] << 8);
                out[i * 2 + c] = clamp16(v);
            }
        return f;
    }
    }
    return -1;
}

static bool raw_seek(Decoder * d, int64_t frame)
{
    switch (d->kind) {
    case K_OPUS: return op_pcm_seek(d->of, frame) == 0;
    case K_VORBIS: return ov_pcm_seek(&d->vf, frame) == 0;
    case K_MP3: return mpg123_seek(d->mh, (off_t)frame, SEEK_SET) >= 0;
    case K_WAV:
        if (d->length && frame > d->length)
            frame = d->length;
        return fseek(d->wav, d->wav_data + (long)frame * d->wav_block, SEEK_SET) == 0;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------------- opening

static uint32_t le32(const uint8_t * p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// song.mix beside song.ogg: "left right" for each track, 4096 being all of it
static void read_mix(Decoder * d, const char * path)
{
    char name[700];
    snprintf(name, sizeof(name), "%s", path);
    char * dot = strrchr(name, '.');
    if (!dot || strlen(dot) < 4)
        return;
    strcpy(dot, ".mix");
    FILE * f = fopen(name, "r");
    if (!f)
        return;
    int n = 0;
    while (n < 64 && fscanf(f, "%d %d", &d->mix[n][0], &d->mix[n][1]) == 2)
        n++;
    fclose(f);
    d->have_mix = n >= d->channels;
}

static bool open_wav(Decoder * d, const char * path)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return false;
    }
    bool fmt = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8)
            break;
        uint32_t size = le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && size >= 16) {
            uint8_t b[40];
            size_t n = size < sizeof(b) ? size : sizeof(b);
            if (fread(b, 1, n, f) != n)
                break;
            int format = b[0] | b[1] << 8;
            if (format == 0xFFFE && n >= 26)
                format = b[24] | b[25] << 8;
            d->channels = b[2] | b[3] << 8;
            d->rate = (int)le32(b + 4);
            d->wav_block = b[12] | b[13] << 8;
            d->wav_bits = b[14] | b[15] << 8;
            d->wav_float = format == 3;
            fmt = (format == 1 || (format == 3 && d->wav_bits == 32)) && d->channels > 0 && d->wav_bits >= 8 &&
                  d->wav_bits <= 32 && d->wav_block >= d->channels * d->wav_bits / 8 && d->rate > 0;
            fseek(f, (long)(size - n + (size & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt)
                break;
            d->wav_data = ftell(f);
            d->length = size / d->wav_block;
            d->wav = f;
            d->kind = K_WAV;
            return true;
        } else
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
    fclose(f);
    return false;
}

static bool open_mp3(Decoder * d, const char * path)
{
    static bool init;
    if (!init) {
        mpg123_init();
        init = true;
    }
    int err;
    d->mh = mpg123_new(NULL, &err);
    if (!d->mh)
        return false;
    mpg123_param(d->mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET | MPG123_GAPLESS, 0);
    long rate;
    int channels, enc;
    if (mpg123_open(d->mh, path) != MPG123_OK || mpg123_getformat(d->mh, &rate, &channels, &enc) != MPG123_OK) {
        mpg123_delete(d->mh);
        d->mh = NULL;
        return false;
    }
    mpg123_format_none(d->mh);
    mpg123_format(d->mh, rate, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    d->rate = (int)rate;
    d->channels = 2;
    off_t len = mpg123_length(d->mh);
    d->length = len > 0 ? len : 0;
    d->kind = K_MP3;
    return true;
}

Decoder * dec_open(const char * path)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t magic[12] = {0};
    size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    Decoder * d = calloc(1, sizeof(Decoder));
    bool ok = false;
    if (got >= 4 && !memcmp(magic, "OggS", 4)) {
        int err;
        d->of = op_open_file(path, &err);
        if (d->of) {
            d->kind = K_OPUS;
            d->rate = 48000;
            d->channels = 2;
            ogg_int64_t len = op_pcm_total(d->of, -1);
            d->length = len > 0 ? len : 0;
            ok = true;
        } else if (ov_fopen(path, &d->vf) == 0) {
            vorbis_info * vi = ov_info(&d->vf, -1);
            d->kind = K_VORBIS;
            d->rate = (int)vi->rate;
            d->channels = vi->channels;
            ogg_int64_t len = ov_pcm_total(&d->vf, -1);
            d->length = len > 0 ? len : 0;
            ok = true;
        }
    } else if (got >= 12 && !memcmp(magic, "RIFF", 4))
        ok = open_wav(d, path);
    else if (got >= 3 && (!memcmp(magic, "ID3", 3) || (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0)))
        ok = open_mp3(d, path);
    if (!ok) {
        plat_log("audio: can't read %s", path);
        free(d);
        return NULL;
    }
    if (d->channels > 2)
        read_mix(d, path);
    if (d->rate != 48000) {
        d->step = d->rate / 48000.0;
        d->src = malloc(SRC_FRAMES * 2 * sizeof(int16_t));
        if (d->src && !rs_kernel(&d->kern, d->rate)) {
            free(d->src);
            d->src = NULL;
        }
    }
    dec_seek(d, 0);
    return d;
}

void dec_close(Decoder * d)
{
    if (!d)
        return;
    switch (d->kind) {
    case K_OPUS: op_free(d->of); break;
    case K_VORBIS: ov_clear(&d->vf); break;
    case K_MP3: mpg123_close(d->mh); mpg123_delete(d->mh); break;
    case K_WAV: fclose(d->wav); break;
    }
    free(d->src);
    rs_kernel_free(&d->kern);
    free(d);
}

// ---------------------------------------------------------------------------------------------------- 48 kHz out

// the window: frames pos - (RS_HALF - 1) .. pos + RS_HALF of src; before the start it's silence
bool dec_seek(Decoder * d, int64_t frame)
{
    if (!d->src)
        return raw_seek(d, frame);
    double at = frame * d->step;
    int64_t whole = (int64_t)at;
    int64_t first = whole - (RS_HALF - 1);
    int pad = first < 0 ? (int)-first : 0;
    if (first < 0)
        first = 0;
    bool ok = raw_seek(d, first);
    memset(d->src, 0, (size_t)pad * 2 * sizeof(int16_t));
    d->src_count = pad;
    d->src_real = pad;
    d->src_eof = false;
    d->pos = (RS_HALF - 1) + (at - whole);
    return ok;
}

static int resample(Decoder * d, int16_t * out, int frames)
{
    int made = 0;
    while (made < frames) {
        int i = (int)d->pos;
        if (d->src_eof && i >= d->src_real)
            break;                      // past the last real frame
        if (i + RS_HALF >= d->src_count) {
            // slide the window down to what's still needed, then fill: the file, or silence after its end
            int keep = d->src_count - (i - (RS_HALF - 1));
            if (keep < 0)
                keep = 0;
            int drop = d->src_count - keep;
            memmove(d->src, d->src + drop * 2, (size_t)keep * 2 * sizeof(int16_t));
            d->pos -= drop;
            d->src_real -= drop;
            d->src_count = keep;
            if (!d->src_eof) {
                int n = raw_read(d, d->src + keep * 2, SRC_FRAMES - keep);
                if (n <= 0)
                    d->src_eof = true;
                else {
                    d->src_count += n;
                    d->src_real = d->src_count;
                }
            }
            if (d->src_eof) {
                int n = RS_HALF * 2;
                memset(d->src + d->src_count * 2, 0, (size_t)n * 2 * sizeof(int16_t));
                d->src_count += n;
            }
            continue;
        }
        float v[2];
        rs_point(&d->kern, d->src + (i - (RS_HALF - 1)) * 2, (float)(d->pos - i), v);
        out[made * 2] = clamp16((int)lrintf(v[0]));
        out[made * 2 + 1] = clamp16((int)lrintf(v[1]));
        made++;
        d->pos += d->step;
    }
    return made;
}

int dec_read(Decoder * d, int16_t * out, int frames)
{
    return d->src ? resample(d, out, frames) : raw_read(d, out, frames);
}

int64_t dec_length(const Decoder * d)
{
    if (!d->length)
        return 0;
    return d->rate == 48000 ? d->length : (int64_t)(d->length * 48000.0 / d->rate);
}

int dec_file_rate(const Decoder * d)
{
    return d->rate;
}

double dec_file_seconds(const char * path)
{
    Decoder * d = dec_open(path);
    if (!d)
        return 0;
    double s = d->rate > 0 ? (double)d->length / d->rate : 0;
    dec_close(d);
    return s;
}
