// The song downloader: Chorus Encore (enchor.us) and RhythmVerse searches, and a queue that downloads, unpacks and
// converts the songs picked (songconv.c) into ux0:data/ghvita/songs/. Searches and downloads run on threads of their
// own; the screen only reads what they leave.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chart.h"

enum { DL_CHORUS, DL_RHYTHMVERSE, DL_SITES };

typedef enum { JOB_NONE, JOB_QUEUED, JOB_DOWNLOADING, JOB_INSTALLING, JOB_DONE, JOB_FAILED } JobState;

typedef struct {
    int site;
    char id[80];                    // "chorus:<md5>" or "rv:<file id>" (ghvita.ini keeps it: the library knows it)
    char name[96], artist[96], album[96], year[16], charter[64];
    int length_ms;
    int64_t size;                   // bytes, 0 when unknown
    char url[600];                  // what to fetch ("gdrive-folder:<id>", "mediafire:<page>", "mega:<link>" are looked up)
    char art[400];                  // the cover's address
    int8_t parts[PART_COUNT];       // what's charted and how hard, 0-6; -1 not there (the sites' numbers)
    bool any_parts, rock_band;      // the site told them; a Rock Band package
    char unplayable[64];            // why it can't be had ("drums and vocals only"), else empty
    bool have, have_name;           // (the screen's) in the library: this very chart / a song of the same name
} DlHit;

void dl_search(int site, const char * query);       // "" lists the newest charts
void dl_remove_tree(const char * path);
bool dl_extract_archive(const char * file, const char * dir, char * err, size_t err_size);
void dl_mkdirs(const char * path);
void dl_safe_part(char * s);                        // a name off the web as one the memory card takes
void dl_more(void);                                 // the next page of the last search
bool dl_searching(void);
bool dl_can_more(void);
// the results: lock while reading them (a search replaces them)
void dl_lock(void);
void dl_unlock(void);
int dl_count(void);
DlHit * dl_hit(int i);
int dl_version(void);                               // changes with the results
void dl_status(char * buf, size_t size);           // what the last search found (or why it failed)
// downloads, one at a time, in the order asked
void dl_queue(const DlHit * h);
void dl_cancel(const char * id);
JobState dl_job_state(const char * id, int * percent, char * msg, size_t msg_size);
int dl_jobs_active(void);                           // queued or under way
int dl_jobs_done(void);                             // songs added since the start
void dl_now(char * buf, size_t size);               // "Reptilia  42%   (2 more)", or "" when nothing is going
// a song folder installed since the last call (the main thread adds it to the library)
bool dl_take_installed(char * folder, size_t size);
// the cover of a hit, fetched in the background: the file once it's there, else NULL (asks for it)
const char * dl_cover(const DlHit * h);
