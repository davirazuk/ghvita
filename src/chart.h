// notes.vhc, as tools/pack.py writes it: read whole into memory, the arrays point into it
#pragma once
#include <stdint.h>

enum { PART_GUITAR, PART_BASS, PART_RHYTHM, PART_COOP, PART_KEYS, PART_DRUMS, PART_VOCALS, PART_COUNT };
enum { DIFF_EASY, DIFF_MEDIUM, DIFF_HARD, DIFF_EXPERT, DIFF_COUNT };

#define FRET_OPEN 0x20
#define NOTE_HOPO 1
#define NOTE_TAP 2
#define NOTE_SP 4
#define NOTE_SP_END 8
#define NOTE_TOM_Y 0x10     // drums: these pads are toms here, not cymbals (the next two follow on)
#define NOTE_TOM_B 0x20
#define NOTE_TOM_G 0x40

typedef struct {
    int32_t time;           // microseconds
    uint32_t len;           // sustain, microseconds (0: none)
    uint8_t frets;          // bit 0-4 green..orange, FRET_OPEN
    uint8_t flags;
    uint16_t sustain_beats; // the sustain in quarter notes x 100
} Note;

typedef struct {
    int32_t start, end;
} Range;

typedef struct {
    int32_t time;
    uint32_t us_per_quarter;
} TempoPoint;

typedef struct {
    int32_t time;
    uint8_t kind;           // 0 measure, 1 beat
    uint8_t pad[3];
} BeatLine;

typedef struct {
    int32_t time;
    char name[60];
} Section;

typedef struct {
    int part, diff;
    int note_count, sp_count, solo_count;
    const Note * notes;
    const Range * sp;
    const Range * solos;
} Part;

typedef struct {
    int version;            // 2: drum gems tell cymbals from toms, and disco flips are done
    int length_ms;
    int tempo_count, beat_count, section_count, part_count;
    const TempoPoint * tempo;
    const BeatLine * beats;
    const Section * sections;
    Part * parts;
    void * blob;
} Chart;

Chart * chart_load(const char * path);
void chart_free(Chart * c);
const Part * chart_part(const Chart * c, int part, int diff);
// where a time falls in quarter notes (for Star Power's drain)
double chart_quarters(const Chart * c, double seconds);
