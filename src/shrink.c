#include <dirent.h>
#include <ogg/ogg.h>
#include <opus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vorbis/vorbisfile.h>
#include "audio.h"
#include "decoder.h"
#include "plat.h"
#include "shrink.h"

#define FRAME 960                       // 20 ms
#define ONE_STEM 128000
#define STEM 96000
#define INSTRUMENT 64000                // a Rock Band stem, one instrument
#define MAX_GROUPS 6
#define SILENT 3400                     // the average power of a stem with nothing in it (-55 dB; a blip doesn't count)

typedef struct {
    ogg_stream_state os;
    OpusEncoder * enc;
    FILE * f;
    int16_t pcm[FRAME * 2];
    int fill, skip;
    int64_t pushed, coded, packet;
    bool bad;
    char path[720];
} Out;

static void write_pages(Out * o, bool flush)
{
    ogg_page pg;
    while (flush ? ogg_stream_flush(&o->os, &pg) : ogg_stream_pageout(&o->os, &pg))
        if (fwrite(pg.header, 1, pg.header_len, o->f) != (size_t)pg.header_len ||
            fwrite(pg.body, 1, pg.body_len, o->f) != (size_t)pg.body_len)
            o->bad = true;
}

static void add_packet(Out * o, unsigned char * data, long size, int64_t granule, bool first, bool last)
{
    ogg_packet p = {data, size, first, last, granule, o->packet++};
    ogg_stream_packetin(&o->os, &p);
}

static Out * out_open(const char * path, int bitrate)
{
    Out * o = calloc(1, sizeof(Out));
    if (!o)
        return NULL;
    int err;
    snprintf(o->path, sizeof(o->path), "%s", path);
    o->enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    o->f = o->enc ? fopen(path, "wb") : NULL;
    if (!o->f) {
        if (o->enc)
            opus_encoder_destroy(o->enc);
        free(o);
        return NULL;
    }
    opus_encoder_ctl(o->enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(o->enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(o->enc, OPUS_GET_LOOKAHEAD(&o->skip));
    ogg_stream_init(&o->os, rand());
    unsigned char head[19] = "OpusHead";
    head[8] = 1;
    head[9] = 2;
    head[10] = (unsigned char)o->skip;
    head[11] = (unsigned char)(o->skip >> 8);
    head[12] = 48000 & 255;
    head[13] = 48000 >> 8;
    add_packet(o, head, sizeof(head), 0, true, false);
    write_pages(o, true);
    unsigned char tags[22] = "OpusTags\6\0\0\0ghvita";
    add_packet(o, tags, sizeof(tags), 0, false, false);
    write_pages(o, true);
    return o;
}

static void encode(Out * o, bool last)
{
    unsigned char data[1500];
    int n = opus_encode(o->enc, o->pcm, FRAME, data, sizeof(data));
    if (n < 0) {
        o->bad = true;
        return;
    }
    o->coded += FRAME;
    int64_t end = o->pushed + o->skip;
    add_packet(o, data, n, o->coded < end && !last ? o->coded : end, false, last);
    write_pages(o, false);
}

static inline void out_push(Out * o, int l, int r)
{
    o->pcm[o->fill * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    o->pcm[o->fill * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    o->pushed++;
    if (++o->fill == FRAME) {
        o->fill = 0;
        encode(o, false);
    }
}

// false when it went wrong; the file stays for the caller to remove
static bool out_close(Out * o)
{
    if (!o)
        return false;
    // the rest of the last frame, and enough after it for the encoder's delay
    while (o->coded < o->pushed + o->skip) {
        memset(o->pcm + o->fill * 2, 0, (FRAME - o->fill) * 4);
        o->fill = 0;
        encode(o, o->coded + FRAME >= o->pushed + o->skip);
    }
    write_pages(o, true);
    ogg_stream_clear(&o->os);
    opus_encoder_destroy(o->enc);
    bool ok = !o->bad && o->pushed > 0;
    if (fclose(o->f))
        ok = false;
    free(o);
    return ok;
}

// a song being played comes first
static void wait_for_game(void)
{
    while (audio_song_active())
        plat_sleep_us(200000);
}

bool shrink_mogg(const char * folder)
{
    char path[700], name[720];
    int gain[64][2], group[64], count = 0, groups = 1;
    char names[MAX_GROUPS][12] = {"song"};
    snprintf(path, sizeof(path), "%s/song.mix", folder);
    FILE * mix = fopen(path, "r");
    if (!mix)
        return false;
    memset(group, 0, sizeof(group));
    char line[300];
    while (fgets(line, sizeof(line), mix)) {
        if (strncmp(line, "part ", 5)) {
            if (count < 64 && sscanf(line, "%d %d", &gain[count][0], &gain[count][1]) == 2)
                count++;
            continue;
        }
        char * p = strtok(line + 5, " \n");
        if (!p || groups == MAX_GROUPS)
            continue;
        snprintf(names[groups], sizeof(names[0]), "%s", p);
        while ((p = strtok(NULL, " \n")))
            if (atoi(p) >= 0 && atoi(p) < 64)
                group[atoi(p)] = groups;
        groups++;
    }
    fclose(mix);
    snprintf(path, sizeof(path), "%s/song.ogg", folder);
    OggVorbis_File vf;
    if (ov_fopen(path, &vf))
        return false;
    int channels = ov_info(&vf, -1)->channels, rate = ov_info(&vf, -1)->rate;
    if (channels <= 2 || channels > 64 || count < channels) {
        ov_clear(&vf);
        return false;
    }
    Out * out[MAX_GROUPS] = {NULL};
    bool ok = true;
    for (int g = 0; g < groups; g++) {
        bool used = false;
        for (int c = 0; c < channels; c++)
            used |= group[c] == g;
        if (!used)
            continue;
        snprintf(name, sizeof(name), "%s/%s.tmp", folder, names[g]);
        out[g] = out_open(name, INSTRUMENT);
        ok &= out[g] != NULL;
    }
    static int16_t buf[8192];
    int cur[MAX_GROUPS][2] = {{0}}, prev[MAX_GROUPS][2] = {{0}};
    int64_t power[MAX_GROUPS] = {0}, heard_frames = 0;
    double phase = 0, step = rate / 48000.0;
    int bitstream;
    while (ok) {
        wait_for_game();
        long n = ov_read(&vf, (char *)buf, sizeof(buf) - sizeof(buf) % (channels * 2), 0, 2, 1, &bitstream);
        if (n == OV_HOLE)
            continue;
        if (n < 0)
            ok = false;
        if (n <= 0)
            break;
        int frames = (int)(n / 2 / channels);
        for (int f = 0; f < frames; f++) {
            memset(cur, 0, sizeof(cur));
            const int16_t * s = buf + f * channels;
            for (int c = 0; c < channels; c++) {
                cur[group[c]][0] += s[c] * gain[c][0];
                cur[group[c]][1] += s[c] * gain[c][1];
            }
            for (int g = 0; g < groups; g++) {
                cur[g][0] >>= 12;
                cur[g][1] >>= 12;
                power[g] += (int64_t)cur[g][0] * cur[g][0] + (int64_t)cur[g][1] * cur[g][1];
            }
            // to 48 kHz
            for (; phase < 1; phase += step)
                for (int g = 0; g < groups; g++)
                    if (out[g])
                        out_push(out[g], prev[g][0] + (int)((cur[g][0] - prev[g][0]) * phase),
                                 prev[g][1] + (int)((cur[g][1] - prev[g][1]) * phase));
            phase -= 1;
            memcpy(prev, cur, sizeof(cur));
        }
        heard_frames += frames;
    }
    ov_clear(&vf);
    for (int g = 0; g < groups; g++)
        if (out[g])
            ok &= out_close(out[g]);
    if (ok)
        remove(path);
    // a custom often has all its audio in one track: an empty stem would cost a decoder all song long
    bool heard = false;
    for (int g = 0; g < groups; g++) {
        power[g] /= heard_frames * 2 + 1;
        heard |= power[g] >= SILENT;
    }
    for (int g = 0; g < groups; g++) {
        snprintf(name, sizeof(name), "%s/%s.tmp", folder, names[g]);
        if (ok && (power[g] >= SILENT || !heard)) {
            char final[720];
            snprintf(final, sizeof(final), "%s/%s.ogg", folder, names[g]);
            rename(name, final);
        } else
            remove(name);
    }
    if (ok) {
        snprintf(path, sizeof(path), "%s/song.mix", folder);
        remove(path);
    }
    plat_log("shrink: %s %s", folder, ok ? "split to Opus stems" : "left as it was");
    return ok;
}

static bool is_opus(const char * path)
{
    char head[64];
    FILE * f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    for (size_t i = 0; i + 8 <= n; i++)
        if (!memcmp(head + i, "OpusHead", 8))
            return true;
    return false;
}

static bool is_audio(const char * name)
{
    static const char * const ext[] = {".ogg", ".opus", ".mp3", ".wav"};
    size_t n = strlen(name);
    for (int i = 0; i < 4; i++)
        if (n > strlen(ext[i]) && !strcasecmp(name + n - strlen(ext[i]), ext[i]))
            return true;
    return false;
}

static void shrink_stem(const char * path, int bitrate)
{
    char tmp[720];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    Decoder * d = dec_open(path);
    if (!d)
        return;
    int64_t length = dec_length(d);
    Out * o = out_open(tmp, bitrate);
    bool ok = o != NULL;
    static int16_t pcm[FRAME * 2];
    while (ok) {
        wait_for_game();
        int n = dec_read(d, pcm, FRAME);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++)
            out_push(o, pcm[i * 2], pcm[i * 2 + 1]);
    }
    int64_t pushed = o ? o->pushed : 0;
    ok &= out_close(o);
    dec_close(d);
    if (length > 0 && llabs(pushed - length) > 4800)
        ok = false;
    FILE * f = ok ? fopen(tmp, "rb") : NULL, * g = f ? fopen(path, "rb") : NULL;
    if (g) {
        fseek(f, 0, SEEK_END);
        fseek(g, 0, SEEK_END);
        ok = ftell(f) < ftell(g);
    }
    if (f)
        fclose(f);
    if (g)
        fclose(g);
    if (ok)
        remove(path);
    if (!ok || rename(tmp, path))
        remove(tmp);
    plat_log("shrink: %s %s", path, ok ? "to Opus" : "left as it was");
}

void shrink_song(const char * folder)
{
    char names[16][256];
    int n = 0;
    DIR * d = opendir(folder);
    if (!d)
        return;
    for (struct dirent * e; (e = readdir(d));)
        if (n < 16 && is_audio(e->d_name))
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", folder, names[i]);
        if (!is_opus(path))
            shrink_stem(path, n == 1 ? ONE_STEM : STEM);
    }
}
