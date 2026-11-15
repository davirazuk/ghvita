#pragma once
#include <stdint.h>

#define AEC_RATE 16000

// Takes the song out of a mic recording made with the speakers on. ref: what the speakers played, the same rate and
// timeline as mic. In place. Returns how much quieter the song got, in dB (0: no song found in the mic, left as it was).
float aec_clean(int16_t * mic, const int16_t * ref, int n);
