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
    char genre[32];                 // what the site says it is
    char comment[160];              // the chart author's note about it
    int length_ms;
    int64_t size;                   // bytes, 0 when unknown
    char url[600];                  // what to fetch ("gdrive-folder:<id>", "mediafire:<page>", "mega:<link>" are looked up)
    char art[400];                  // the cover's address
    int8_t parts[PART_COUNT];       // what's charted and how hard, 0-6; -1 not there (the sites' numbers)
    bool any_parts, rock_band;      // the site told them; a Rock Band package
    char unplayable[64];            // why it can't be had ("drums and vocals only"), else empty
    bool have, have_name;           // (the screen's) in the library: this very chart / a song of the same name
    int order;                      // the hits' own order: the sorts' tiebreak (not saved, not shown)
} DlHit;

void dl_search(int site, const char * query);       // "" lists the newest charts

// The filters a search carries ("by charter", genre, ...). RhythmVerse needs the charter and artist as the site's own
// ids (as the lookup pickers return them), Chorus takes what's typed.
typedef struct {
    char charter[72], charter_name[96];     // the id and what the screen shows
    char artist[72], artist_name[96];
    int genre;                              // which of dl_genres(), -1 all
    int instrument;                         // which of dl_instru(), -1 all
    int difficulty;                         // 0 expert 1 hard 2 medium 3 easy, -1 all
    int decade, year;                       // 1990, 1974 or 0 all
    bool stems;                             // only the full multitracks (RhythmVerse takes it)
    int sort;                               // which of the sorts (dl_sort*), 0 the sites' best match
} DlFilter;
void dl_get_filter(int site, DlFilter * f);
void dl_set_filter(int site, const DlFilter * f);
bool dl_quit_filters(void);                         // anything on either site?
void dl_filter_text(int site, char * buf, size_t size);   // the filters and the sort, as one line ("" all off)

int dl_sort_count(void);
const char * dl_sort_label(int i);                  // "Best match", "Title A to Z", ...

// the genres RhythmVerse knows (both sites search them), fetched when first asked. -1 while it's loading, 0 when the
// site didn't answer
int dl_genre_count(void);
const char * dl_genre_slug(int i);
const char * dl_genre_name(int i);
int dl_diff_count(void);                            // 0 expert .. 3 easy
// the instruments the two sites share, one name for each: dl_instru's two slugs (theirs and Chorus's)
int dl_instru_count(void);
void dl_instru(int i, const char ** rv, const char ** chorus, const char ** label);

typedef struct { char id[80], name[96]; int releases; } DlLookup;
// "who is that?" for the pickers: 0 charter, 1 artist, both RhythmVerse's live lookups. Returns how many were found
int dl_lookup(int kind, const char * text, DlLookup * out, int max);
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
// downloads, one at a time, in the order asked. The queue is saved with the game closed: waiting and failed jobs come
// back at the next start, the waiting ones carrying on by themselves
bool dl_queue(const DlHit * h);                    // false: every slot is busy, try again when one's done
void dl_forget(const char * id);                   // a failed or finished job leaves the list
void dl_cancel(const char * id);
void dl_prioritize(const char * id);               // a waiting one downloads before the rest of its kind
void dl_clear_done(void);                          // the finished ones leave the list
JobState dl_job_state(const char * id, int * percent, char * msg, size_t msg_size);
int dl_jobs_active(void);                           // queued or under way
int dl_jobs_done(void);                             // songs added since the start
void dl_now(char * buf, size_t size);               // "Reptilia  42%   (2 more)", or "" when nothing is going

typedef struct {
    DlHit hit;
    JobState state;
    int percent;                    // -1 unknown
    int64_t done, total;            // bytes of the download under way (total 0 unknown)
    char msg[96];
} DlJob;
int dl_jobs_list(DlJob * out, int max);             // the whole queue, waiting and finished, for the queue screen
// a song folder installed since the last call (the main thread adds it to the library)
bool dl_take_installed(char * folder, size_t size);
// the cover of a hit, fetched in the background: the file once it's there, else NULL (asks for it)
const char * dl_cover(const DlHit * h);
