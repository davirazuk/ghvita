// Replays: see replay.h. A file:
//   ReplayHead, then the Config the run was played with (cfg_size bytes), the frames, the singing as IMA ADPCM
//   (4 bits a sample, two a byte, low nibble first; the predictor starts at 0, step index 0)
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "player.h"
#include "plat.h"
#include "replay.h"
#include "vocals.h"
#include "voicefx.h"

#define MAGIC "GHR1"
#define VERSION 1

typedef struct {
    char magic[4];
    uint32_t version, cfg_size, frame_size;
    ReplayInfo info;
} ReplayHead;

// ------------------------------------------------------------------------------------------------- IMA ADPCM

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

typedef struct {
    int pred, index;
} Adpcm;

static int adpcm_decode(Adpcm * s, int code)
{
    int step = step_table[s->index], diff = step >> 3;
    if (code & 4)
        diff += step;
    if (code & 2)
        diff += step >> 1;
    if (code & 1)
        diff += step >> 2;
    s->pred += code & 8 ? -diff : diff;
    s->pred = s->pred > 32767 ? 32767 : s->pred < -32768 ? -32768 : s->pred;
    s->index += index_table[code];
    s->index = s->index < 0 ? 0 : s->index > 88 ? 88 : s->index;
    return s->pred;
}

static int adpcm_encode(Adpcm * s, int sample)
{
    int step = step_table[s->index], diff = sample - s->pred, code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    if (diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
    }
    if (diff >= step >> 2)
        code |= 1;
    adpcm_decode(s, code);          // the encoder follows what the decoder will make of it
    return code;
}

// --------------------------------------------------------------------------------------------------- recording

static struct {
    RFrame * f;
    int n, cap;
    bool ok;
} rec;

void replay_rec_begin(void)
{
    rec.n = 0;
    rec.ok = true;
}

void replay_rec_frame(const RFrame * f)
{
    if (!rec.ok)
        return;
    if (rec.n == rec.cap) {
        int cap = rec.cap ? rec.cap * 2 : 8192;
        RFrame * more = realloc(rec.f, (size_t)cap * sizeof(RFrame));
        if (!more) {
            rec.ok = false;
            return;
        }
        rec.f = more;
        rec.cap = cap;
    }
    rec.f[rec.n++] = *f;
}

void replay_rec_void(void)
{
    rec.ok = false;
}

bool replay_rec_ok(void)
{
    return rec.ok && rec.n > 0;
}

static uint32_t song_hash(const char * folder)
{
    uint32_t h = 2166136261u;
    for (const char * c = folder; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    return h;
}

static void replay_dir(char * out, int size)
{
    snprintf(out, size, "%sreplays", player_dir());
    mkdir(out, 0777);
}

static bool write_file(const char * path, const ReplayInfo * info, const RFrame * frames, const int16_t * voice)
{
    char tmp[420];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if (!f)
        return false;
    ReplayHead h = {{0}};
    memcpy(h.magic, MAGIC, 4);
    h.version = VERSION;
    h.cfg_size = sizeof(Config);
    h.frame_size = sizeof(RFrame);
    h.info = *info;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&cfg, sizeof(Config), 1, f) == 1 &&
              fwrite(frames, sizeof(RFrame), (size_t)info->frames, f) == (size_t)info->frames;
    if (ok && info->voice_n > 0) {
        Adpcm s = {0, 0};
        uint8_t buf[4096];
        int k = 0;
        for (int i = 0; ok && i < info->voice_n; i += 2) {
            int lo = adpcm_encode(&s, voice[i]), hi = i + 1 < info->voice_n ? adpcm_encode(&s, voice[i + 1]) : 0;
            buf[k++] = (uint8_t)(lo | hi << 4);
            if (k == (int)sizeof(buf)) {
                ok = fwrite(buf, 1, k, f) == (size_t)k;
                k = 0;
            }
        }
        if (ok && k)
            ok = fwrite(buf, 1, k, f) == (size_t)k;
    }
    ok &= fclose(f) == 0;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

bool replay_read_info(const char * path, ReplayInfo * info)
{
    FILE * f = fopen(path, "rb");
    if (!f)
        return false;
    ReplayHead h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, MAGIC, 4) == 0 && h.version == VERSION;
    fclose(f);
    if (ok)
        *info = h.info;
    return ok;
}

bool replay_save(const Song * song, const ReplayInfo * in)
{
    if (!replay_rec_ok() || !song)
        return false;
    ReplayInfo info = *in;
    snprintf(info.song, sizeof(info.song), "%s", song->folder);
    snprintf(info.name, sizeof(info.name), "%s", song->name);
    snprintf(info.artist, sizeof(info.artist), "%s", song->artist);
    info.frames = rec.n;
    info.when = (int64_t)time(NULL);
#ifdef GHV_BUILD
    info.build = GHV_BUILD;
#endif
    const int16_t * voice = NULL;
    int vn = 0;
    float t0 = 0;
    voice = vox_record_get(&vn, &t0);
    info.voice_n = voice ? vn : 0;
    info.voice_t0 = t0;
    char dir[400], path[420], best[420];
    replay_dir(dir, sizeof(dir));
    uint32_t h = song_hash(song->folder);
    snprintf(path, sizeof(path), "%s/%08x_%d%d_last.ghr", dir, (unsigned)h, info.part, info.diff);
    snprintf(best, sizeof(best), "%s/%08x_%d%d_best.ghr", dir, (unsigned)h, info.part, info.diff);
    bool ok = write_file(path, &info, rec.f, voice);
    ReplayInfo old;
    if (ok && !info.failed && (!replay_read_info(best, &old) || info.score > old.score))
        write_file(best, &info, rec.f, voice);
    plat_log("replay: %s %d frames, %d s of singing", ok ? "kept" : "couldn't keep", rec.n, info.voice_n / VOICE_RATE);
    return ok;
}

bool replay_last_path(const Song * song, int part, int diff, char * out, int size)
{
    char dir[400];
    replay_dir(dir, sizeof(dir));
    snprintf(out, size, "%s/%08x_%d%d_last.ghr", dir, (unsigned)song_hash(song->folder), part, diff);
    ReplayInfo in;
    return replay_read_info(out, &in);
}

int replay_list(const Song * song, char paths[][400], ReplayInfo * infos, int max)
{
    char dir[400], pre[16];
    replay_dir(dir, sizeof(dir));
    snprintf(pre, sizeof(pre), "%08x_", (unsigned)song_hash(song->folder));
    DIR * d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent * e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, pre, strlen(pre)) || len < 5 || strcmp(e->d_name + len - 4, ".ghr"))
            continue;
        snprintf(paths[n], 400, "%s/%s", dir, e->d_name);
        if (replay_read_info(paths[n], &infos[n]) && !strcmp(infos[n].song, song->folder))
            n++;
    }
    closedir(d);
    // newest first
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && infos[j].when > infos[j - 1].when; j--) {
            ReplayInfo ti = infos[j];
            infos[j] = infos[j - 1];
            infos[j - 1] = ti;
            char tp[400];
            memcpy(tp, paths[j], 400);
            memcpy(paths[j], paths[j - 1], 400);
            memcpy(paths[j - 1], tp, 400);
        }
    return n;
}

// ---------------------------------------------------------------------------------------------------- playback

static struct {
    bool on;
    ReplayInfo info;
    Config cfg;
    RFrame * f;
    int at;
    int16_t * voice;
    int channels;
    float voice_t0;
    int16_t * raw;              // the singing as recorded (mono), kept to mix again in another style
    int style;                  // what it's mixed as, -1 not yet
} play;

bool replay_load(const char * path)
{
    replay_unload();
    FILE * f = fopen(path, "rb");
    if (!f)
        return false;
    ReplayHead h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, MAGIC, 4) == 0 && h.version == VERSION &&
              h.cfg_size > 0 && h.cfg_size < 65536 && h.frame_size == sizeof(RFrame) && h.info.frames > 0 &&
              h.info.frames < 10000000 && h.info.voice_n >= 0 && h.info.voice_n < VOICE_RATE * 3600;
    if (!ok)
        plat_log("replay: %s is from another version, or damaged", path);
    if (ok) {
        // settings added since are yours; ones it has that this version doesn't are skipped
        play.cfg = cfg;
        size_t take = h.cfg_size < sizeof(Config) ? h.cfg_size : sizeof(Config);
        ok = fread(&play.cfg, take, 1, f) == 1 && fseek(f, (long)(h.cfg_size - take), SEEK_CUR) == 0;
    }
    if (ok) {
        play.info = h.info;
        play.f = malloc((size_t)h.info.frames * sizeof(RFrame));
        ok = play.f && fread(play.f, sizeof(RFrame), (size_t)h.info.frames, f) == (size_t)h.info.frames;
    }
    if (ok && h.info.voice_n > 0) {
        size_t bytes = (size_t)(h.info.voice_n + 1) / 2;
        uint8_t * packed = malloc(bytes);
        play.voice = malloc((size_t)h.info.voice_n * sizeof(int16_t));
        if (packed && play.voice && fread(packed, 1, bytes, f) == bytes) {
            Adpcm s = {0, 0};
            for (int i = 0; i < h.info.voice_n; i++)
                play.voice[i] = (int16_t)adpcm_decode(&s, i & 1 ? packed[i / 2] >> 4 : packed[i / 2] & 15);
        } else {
            free(play.voice);
            play.voice = NULL;
            play.info.voice_n = 0;
        }
        free(packed);
    }
    fclose(f);
    if (!ok) {
        replay_unload();
        return false;
    }
    play.on = true;
    play.at = 0;
    play.channels = 1;
    play.voice_t0 = play.info.voice_t0;
    play.style = -1;
    return true;
}

void replay_unload(void)
{
    free(play.f);
    if (play.voice != play.raw)
        free(play.voice);
    free(play.raw);
    memset(&play, 0, sizeof(play));
}

bool replay_playing(void)
{
    return play.on;
}

const ReplayInfo * replay_info(void)
{
    return play.on ? &play.info : NULL;
}

const Config * replay_config(void)
{
    return play.on ? &play.cfg : NULL;
}

const RFrame * replay_next(double t)
{
    if (!play.on || play.at >= play.info.frames || play.f[play.at].t > t)
        return NULL;
    return &play.f[play.at++];
}

void replay_rewind(void)
{
    play.at = 0;
}

const int16_t * replay_voice(int * n, float * t0, int * channels)
{
    *n = play.voice ? play.info.voice_n : 0;
    *t0 = play.voice_t0;
    *channels = play.channels;
    return play.voice;
}

bool replay_mix_voice(int style)
{
    if (!play.voice)
        return false;
    if (play.style == style)
        return true;
    if (!play.raw) {
        play.raw = play.voice;              // the first mix: what was decoded is the recording
    } else {
        free(play.voice);                   // the old mix (the mixer's let go of it: audio_voice NULL first)
        play.voice = play.raw;
        play.channels = 1;
        play.voice_t0 = play.info.voice_t0;
        play.style = -1;
    }
    int n = play.info.voice_n;
    int16_t * work = malloc((size_t)n * sizeof(int16_t));
    if (!work)
        return false;
    memcpy(work, play.raw, (size_t)n * sizeof(int16_t));
    float t0;
    int16_t * st = voicefx_mix(work, n, play.info.voice_t0, play.f, play.info.frames, style, &t0);
    free(work);
    if (!st)
        return false;
    play.voice = st;
    play.channels = 2;
    play.voice_t0 = t0;
    play.style = style;
    return true;
}
