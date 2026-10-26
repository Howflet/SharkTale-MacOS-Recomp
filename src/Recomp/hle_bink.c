/*=============================================================================
	hle_bink.c: BINKW32 (stdcall) over the native decoder in bink.c.

	Guest-visible structs (the fields the game reads):
	  BINK:      Width 0, Height 4, Frames 8, FrameNum 0xc (1-based),
	             LastFrameNum 0x10, FrameRate 0x14, FrameRateDiv 0x18,
	             Paused 0x1c, NumTracks 0xf0
	  BINKTRACK: Frequency 0, Bits 4, Channels 8, MaxSize 0xc
	  BINKSUMMARY (GetSummary): Width 0, Height 4, FileFrameRate 0xc,
	             FileFrameRateDiv 0x10, FrameRate 0x14, FrameRateDiv 0x18,
	             TotalFrames 0x20

	Sound data (BINKFROMMEMORY opens) is decoded by bink.c. Movie files are
	decoded through FFmpeg (bink_video.c) when it is installed; otherwise they
	are reported as one silent frame, so the intros end at once.
=============================================================================*/
#include <math.h>
#include <stdlib.h>

#include "bink.h"
#include "bink_video.h"
#include "hle.h"

#define BINK_SIZE 0x200
#define TRACK_SIZE 0x20

typedef struct Open { u32 guest; BinkFile* file; BinkVideo* video; } Open;
static Open Opens[256];

static Open* open_of(u32 bink)
{
	for (int i = 0; i < 256; i++)
		if (Opens[i].guest == bink && bink)
			return &Opens[i];
	return NULL;
}

static BinkFile* file_of(u32 bink)
{
	for (int i = 0; i < 256; i++)
		if (Opens[i].guest == bink && bink)
			return Opens[i].file;
	return NULL;
}

static u32 new_bink(BinkFile* f, u32 width, u32 height, u32 frames, u32 rate, u32 div, u32 tracks)
{
	u32 b = rt_heap_alloc(BINK_SIZE);
	memset(GP(b), 0, BINK_SIZE);
	ST4(b + 0x00, width);
	ST4(b + 0x04, height);
	ST4(b + 0x08, frames);
	ST4(b + 0x0c, 1);
	ST4(b + 0x14, rate);
	ST4(b + 0x18, div);
	ST4(b + 0xf0, tracks);
	for (int i = 0; i < 256; i++)
		if (!Opens[i].guest)
		{
			Opens[i] = (Open){ b, f };
			break;
		}
	return b;
}

HLE(BinkOpen)
{
	u32 name = ARG(0), flags = ARG(1);
	if (flags & 0x04000000u)  /* BINKFROMMEMORY: a whole .bik in guest memory */
	{
		u32 size = LD4(name + 4) + 8;
		BinkFile* f = !memcmp(GP(name), "BIK", 3) ? bink_open_memory(GP(name), size) : NULL;
		if (!f)
			HLE_RET(c, 0, 8);
		const BinkInfo* in = bink_info(f);
		HLE_RET(c, new_bink(f, in->width, in->height, in->frames, in->fps_num, in->fps_den, in->num_tracks), 8);
	}
	/* A movie file: decode it if FFmpeg's Bink video decoder is available. */
	char host[2048];
	hle_host_path(g_str(name), host, sizeof host);
	BinkFile* f = bink_video_available() ? bink_open_path(host) : NULL;
	BinkVideo* v = f ? bink_video_open(f) : NULL;
	if (!v)
	{
		bink_close(f);
		fprintf(stderr, "[bink] %s: no video decoder, skipping\n", g_str(name));
		HLE_RET(c, new_bink(NULL, 640, 480, 1, 30, 1, 0), 8);
	}
	const BinkInfo* in = bink_info(f);
	u32 b = new_bink(f, in->width, in->height, in->frames, in->fps_num, in->fps_den, in->num_tracks);
	open_of(b)->video = v;
	fprintf(stderr, "[bink] %s: %ux%u, %u frames at %.2f fps\n", g_str(name), in->width, in->height, in->frames,
		(double)in->fps_num / in->fps_den);
	HLE_RET(c, b, 8);
}

HLE(BinkClose)
{
	u32 b = ARG(0);
	for (int i = 0; i < 256; i++)
		if (Opens[i].guest == b && b)
		{
			bink_video_close(Opens[i].video);
			bink_close(Opens[i].file);
			Opens[i].video = NULL;
			Opens[i].guest = 0;
			Opens[i].file = NULL;
			rt_heap_free(b);
			break;
		}
	HLE_RET(c, 0, 4);
}

HLE(BinkOpenTrack)
{
	u32 b = ARG(0);
	BinkFile* f = file_of(b);
	BinkTrackInfo ti;
	if (!f || !bink_track_info(f, (int)ARG(1), &ti))
		HLE_RET(c, 0, 8);
	/* MaxSize must cover every frame's decoded audio: check each packet's reported size. */
	u32 max = bink_track_max_reported(f, (int)ARG(1));
	if (max < ti.max_bytes)
		max = ti.max_bytes;
	u32 t = rt_heap_alloc(TRACK_SIZE);
	memset(GP(t), 0, TRACK_SIZE);
	ST4(t + 0x00, ti.rate);
	ST4(t + 0x04, 16);
	ST4(t + 0x08, ti.channels);
	ST4(t + 0x0c, max);
	ST4(t + 0x10, b);
	ST4(t + 0x14, ARG(1));
	HLE_RET(c, t, 8);
}
HLE(BinkCloseTrack) { if (ARG(0)) rt_heap_free(ARG(0)); HLE_RET(c, 0, 4); }

HLE(BinkGetTrackData)
{
	/* (track, dest): decoded audio of the bink's current frame. */
	u32 t = ARG(0), dest = ARG(1);
	u32 b = LD4(t + 0x10);
	BinkFile* f = file_of(b);
	u32 frame = LD4(b + 0x0c);
	u32 n = f && frame ? bink_track_decode(f, (int)LD4(t + 0x14), frame - 1, GP(dest), LD4(t + 0x0c)) : 0;
	HLE_RET(c, n, 8);
}

HLE(BinkGetSummary)
{
	u32 b = ARG(0), s = ARG(1);
	memset(GP(s), 0, 0x40);
	ST4(s + 0x00, LD4(b + 0x00));
	ST4(s + 0x04, LD4(b + 0x04));
	ST4(s + 0x0c, LD4(b + 0x14));
	ST4(s + 0x10, LD4(b + 0x18));
	ST4(s + 0x14, LD4(b + 0x14));
	ST4(s + 0x18, LD4(b + 0x18));
	ST4(s + 0x20, LD4(b + 0x08));
	HLE_RET(c, 0, 8);
}

HLE(BinkNextFrame)
{
	u32 b = ARG(0);
	ST4(b + 0x10, LD4(b + 0x0c));
	ST4(b + 0x0c, LD4(b + 0x0c) + 1);
	HLE_RET(c, 0, 4);
}
HLE(BinkGoto) { ST4(ARG(0) + 0x0c, ARG(1) ? ARG(1) : 1); HLE_RET(c, 0, 12); }
HLE(BinkDoFrame)
{
	Open* o = open_of(ARG(0));
	if (o && o->video && LD4(ARG(0) + 0x0c))
		bink_video_decode(o->video, LD4(ARG(0) + 0x0c) - 1);
	HLE_RET(c, 0, 4);
}
HLE(BinkPause) { ST4(ARG(0) + 0x1c, ARG(1)); HLE_RET(c, 0, 8); }
HLE(BinkCopyToBuffer)
{
	/* (bink, dest, pitch, height, x, y, flags): the game asks for 32-bit BGRA (BINKSURFACE32A). */
	u32 b = ARG(0);
	Open* o = open_of(b);
	if (o && o->video)
	{
		if ((ARG(6) & 0xf) != 5 && (ARG(6) & 0xf) != 3)
			fprintf(stderr, "[bink] CopyToBuffer surface type %u treated as BGRA\n", ARG(6) & 0xf);
		/* The height argument is the movie's; the destination is the whole back buffer. */
		extern void d3d8_backbuffer_size(u32* w, u32* h);
		u32 dest_w, dest_h;
		d3d8_backbuffer_size(&dest_w, &dest_h);
		if (dest_w != ARG(2) / 4)
			dest_w = ARG(2) / 4, dest_h = ARG(3);
		float scale = fminf((float)dest_w / 800.0f, (float)dest_h / 600.0f);
		if (scale > 1.25f)
		{
			/* The movies are authored for 800x600: at higher resolutions scale them up as the game
			   would have shown them full-screen, instead of a small centred picture. */
			bink_video_copy_scaled_bgra(o->video, GP(ARG(1)), ARG(2), dest_w, dest_h, scale);
		}
		else
		{
			u32 h = ARG(3) < LD4(b + 4) ? ARG(3) : LD4(b + 4);
			bink_video_copy_bgra(o->video, GP(ARG(1) + ARG(5) * ARG(2) + 4 * ARG(4)), ARG(2), LD4(b), h);
		}
	}
	HLE_RET(c, 0, 28);
}

const HleEntry hle_bink[] = {
	ENTRY_AS("_BinkOpen@8", BinkOpen), ENTRY_AS("_BinkClose@4", BinkClose),
	ENTRY_AS("_BinkOpenTrack@8", BinkOpenTrack), ENTRY_AS("_BinkCloseTrack@4", BinkCloseTrack),
	ENTRY_AS("_BinkGetTrackData@8", BinkGetTrackData), ENTRY_AS("_BinkGetSummary@8", BinkGetSummary),
	ENTRY_AS("_BinkNextFrame@4", BinkNextFrame), ENTRY_AS("_BinkGoto@12", BinkGoto),
	ENTRY_AS("_BinkDoFrame@4", BinkDoFrame), ENTRY_AS("_BinkPause@8", BinkPause),
	ENTRY_AS("_BinkCopyToBuffer@28", BinkCopyToBuffer),
	{ NULL, NULL } };
