/*=============================================================================
	bink_video.h: Bink video decoding (libavcodec, loaded at run time).
=============================================================================*/
#pragma once

#include <stdint.h>

#include "bink.h"

typedef struct BinkVideo BinkVideo;

int bink_video_available(void);
BinkVideo* bink_video_open(BinkFile* file);  /* NULL if FFmpeg is unavailable */
void bink_video_close(BinkVideo* v);

/* Decode frame `index` (0-based); sequential calls are cheap, jumps restart at a keyframe. */
int bink_video_decode(BinkVideo* v, uint32_t index);

/* Copy the last decoded frame as BGRA (alpha 255 unless the movie has alpha). */
void bink_video_copy_bgra(BinkVideo* v, uint8_t* dest, uint32_t pitch, uint32_t width, uint32_t height);

/* The last decoded frame's Y, U and V planes, valid until the next decode. 0 if there is none. */
int bink_video_planes(BinkVideo* v, const uint8_t* planes[3], int pitches[3], int* width, int* height);

/* Copy the last frame scaled by `scale` (bilinear), centred in a dest_w x dest_h surface. */
void bink_video_copy_scaled_bgra(BinkVideo* v, uint8_t* dest, uint32_t pitch, uint32_t dest_w, uint32_t dest_h, float scale);
