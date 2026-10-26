/*=============================================================================
	bink.h: Native Bink 1 container and audio decoder (host side).
=============================================================================*/
#pragma once

#include <stdint.h>

typedef struct BinkFile BinkFile;

/* Open from a complete in-memory file (copied) or from a host path. */
BinkFile* bink_open_memory(const void* data, uint32_t size);
BinkFile* bink_open_path(const char* path);
void bink_close(BinkFile* b);

typedef struct BinkInfo
{
	uint32_t width, height, frames, fps_num, fps_den, num_tracks, video_flags;
	char revision;
} BinkInfo;
const BinkInfo* bink_info(const BinkFile* b);

typedef struct BinkTrackInfo { uint32_t rate, channels, max_bytes; } BinkTrackInfo;
int bink_track_info(const BinkFile* b, int track, BinkTrackInfo* out);

/* Decode track `track`'s audio for `frame` as interleaved 16-bit PCM into
   out (up to cap bytes); returns the byte count. Frames are expected in
   order; a jump restarts the decoder at that frame. */
uint32_t bink_track_decode(BinkFile* b, int track, uint32_t frame, void* out, uint32_t cap);

/* Largest decoded byte count any frame of the track reports. */
uint32_t bink_track_max_reported(const BinkFile* b, int track);

int bink_video_is_keyframe(const BinkFile* b, uint32_t frame);

/* Raw video packet of a frame (after the audio packets). */
const uint8_t* bink_video_packet(const BinkFile* b, uint32_t frame, uint32_t* size, int* keyframe);
