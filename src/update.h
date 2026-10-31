// The game updating itself from its GitHub releases
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { UP_OFF, UP_CHECKING, UP_NONE, UP_AVAILABLE, UP_DOWNLOADING, UP_UNPACKING, UP_READY, UP_FAILED } UpState;

// what became of the last update (update_news says). Call before anything's loaded: an update cut short carries on from
// here, once. If that was cut short too the game starts as it is, so it can never get stuck starting
void update_boot(void);
void update_check(void);                // once, on a thread; does nothing with no update.txt
UpState update_state(void);
int update_version(void);               // the build on offer
int64_t update_size(void);
void update_progress(int64_t * done, int64_t * total);
const char * update_error(void);
void update_get(void);                  // fetch it, unpack it, check every file
void update_cancel(void);
void update_install(void);              // hands over to the GH Vita Updater app; if that can't start, says so in update_state
const char * update_news(void);         // once: "Updated to build N", or what went wrong, or NULL
