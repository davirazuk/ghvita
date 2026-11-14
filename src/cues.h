// a chart's own light show (Rock Band's VENUE track, Clone Hero's lighting events)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "chart.h"

// "[lighting (strobe_fast)]", "lighting (verse)", "[bonusfx]", "[next]" as "strobe fast", "verse", "bonusfx", "next"
bool cue_name(const char * text, char * out, size_t size);
// the cues of the song in folder (its notes.mid or notes.chart), timed by c's tempo map: a malloc'd array, its count
int cues_load(const char * folder, const Chart * c, Section ** out);
