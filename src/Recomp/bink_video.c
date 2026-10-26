/*=============================================================================
	bink_video.c: Bink video frames through FFmpeg's libavcodec ("binkvideo").

	libavcodec is loaded at run time (dlopen), so the game runs without
	FFmpeg installed; movies are then skipped. Packets come from bink.c's
	container parser; frames are converted from YUV 4:2:0 (BT.601, limited
	range) to BGRA for BinkCopyToBuffer.
=============================================================================*/
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include "bink.h"
#include "bink_video.h"

static struct
{
	int tried, ok;
	const AVCodec* (*find_decoder)(enum AVCodecID);
	AVCodecContext* (*alloc_context3)(const AVCodec*);
	int (*open2)(AVCodecContext*, const AVCodec*, AVDictionary**);
	void (*free_context)(AVCodecContext**);
	int (*send_packet)(AVCodecContext*, const AVPacket*);
	int (*receive_frame)(AVCodecContext*, AVFrame*);
	void (*flush_buffers)(AVCodecContext*);
	AVPacket* (*packet_alloc)(void);
	void (*packet_free)(AVPacket**);
	AVFrame* (*frame_alloc)(void);
	void (*frame_free)(AVFrame**);
	void* (*mallocz)(size_t);
} F;

static int load(void)
{
	if (F.tried)
		return F.ok;
	F.tried = 1;
	/* The app bundle's own copy first (Contents/Frameworks), then a system FFmpeg. */
	static const char* codec_paths[] = { "@executable_path/../Frameworks/libavcodec.dylib", "libavcodec.dylib",
		"/opt/homebrew/lib/libavcodec.dylib", "/usr/local/lib/libavcodec.dylib", NULL };
	static const char* util_paths[] = { "@executable_path/../Frameworks/libavutil.dylib", "libavutil.dylib",
		"/opt/homebrew/lib/libavutil.dylib", "/usr/local/lib/libavutil.dylib", NULL };
	void* codec = NULL;
	void* util = NULL;
	for (int i = 0; !codec && codec_paths[i]; i++)
		codec = dlopen(codec_paths[i], RTLD_NOW);
	for (int i = 0; !util && util_paths[i]; i++)
		util = dlopen(util_paths[i], RTLD_NOW);
	if (codec && util)
	{
		Dl_info info;
		if (dladdr(dlsym(codec, "avcodec_find_decoder"), &info))
			fprintf(stderr, "[bink] using %s\n", info.dli_fname);
	}
	if (!codec || !util)
	{
		fprintf(stderr, "[bink] FFmpeg (libavcodec) not found: movies will be skipped\n");
		return 0;
	}
#define SYM(lib, field, name) if (!(*(void**)&F.field = dlsym(lib, name))) { fprintf(stderr, "[bink] missing %s\n", name); return 0; }
	SYM(codec, find_decoder, "avcodec_find_decoder");
	SYM(codec, alloc_context3, "avcodec_alloc_context3");
	SYM(codec, open2, "avcodec_open2");
	SYM(codec, free_context, "avcodec_free_context");
	SYM(codec, send_packet, "avcodec_send_packet");
	SYM(codec, receive_frame, "avcodec_receive_frame");
	SYM(codec, flush_buffers, "avcodec_flush_buffers");
	SYM(codec, packet_alloc, "av_packet_alloc");
	SYM(codec, packet_free, "av_packet_free");
	SYM(util, frame_alloc, "av_frame_alloc");
	SYM(util, frame_free, "av_frame_free");
	SYM(util, mallocz, "av_mallocz");
#undef SYM
	F.ok = F.find_decoder(AV_CODEC_ID_BINKVIDEO) != NULL;
	if (!F.ok)
		fprintf(stderr, "[bink] this FFmpeg has no Bink video decoder: movies will be skipped\n");
	return F.ok;
}

struct BinkVideo
{
	BinkFile* file;
	AVCodecContext* ctx;
	AVPacket* pkt;
	AVFrame* frame;
	int64_t next;      /* next frame index the decoder expects */
	int have_frame;
};

int bink_video_available(void) { return load(); }

BinkVideo* bink_video_open(BinkFile* file)
{
	if (!load())
		return NULL;
	const BinkInfo* in = bink_info(file);
	const AVCodec* codec = F.find_decoder(AV_CODEC_ID_BINKVIDEO);
	AVCodecContext* ctx = F.alloc_context3(codec);
	ctx->width = (int)in->width;
	ctx->height = (int)in->height;
	/* As FFmpeg's demuxer does: the signature as codec tag, the video flags as extradata. */
	ctx->codec_tag = (uint32_t)'B' | (uint32_t)'I' << 8 | (uint32_t)'K' << 16 | (uint32_t)(uint8_t)in->revision << 24;
	ctx->extradata = F.mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(ctx->extradata, &in->video_flags, 4);
	ctx->extradata_size = 4;
	if (F.open2(ctx, codec, NULL) < 0)
	{
		F.free_context(&ctx);
		return NULL;
	}
	BinkVideo* v = calloc(1, sizeof *v);
	v->file = file;
	v->ctx = ctx;
	v->pkt = F.packet_alloc();
	v->frame = F.frame_alloc();
	return v;
}

void bink_video_close(BinkVideo* v)
{
	if (!v)
		return;
	F.frame_free(&v->frame);
	F.packet_free(&v->pkt);
	F.free_context(&v->ctx);
	free(v);
}

static int decode_one(BinkVideo* v, uint32_t index)
{
	uint32_t size;
	int key;
	const uint8_t* p = bink_video_packet(v->file, index, &size, &key);
	if (!p)
		return 0;
	/* libavcodec needs padded input. */
	static uint8_t* buf;
	static uint32_t cap;
	if (size + AV_INPUT_BUFFER_PADDING_SIZE > cap)
	{
		cap = size + AV_INPUT_BUFFER_PADDING_SIZE;
		buf = realloc(buf, cap);
	}
	memcpy(buf, p, size);
	memset(buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	v->pkt->data = buf;
	v->pkt->size = (int)size;
	v->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;
	if (F.send_packet(v->ctx, v->pkt) < 0)
		return 0;
	v->pkt->data = NULL;
	v->pkt->size = 0;
	if (F.receive_frame(v->ctx, v->frame) == 0)
		v->have_frame = 1;
	return 1;
}

int bink_video_decode(BinkVideo* v, uint32_t index)
{
	if (!v)
		return 0;
	if ((int64_t)index == v->next - 1 && v->have_frame)
		return 1;
	if ((int64_t)index != v->next)
	{
		/* Seek: restart at the nearest keyframe at or before index. */
		uint32_t k = index;
		while (k > 0 && !bink_video_is_keyframe(v->file, k))
			k--;
		F.flush_buffers(v->ctx);
		v->next = k;
	}
	while (v->next <= (int64_t)index)
	{
		decode_one(v, (uint32_t)v->next);
		v->next++;
	}
	return v->have_frame;
}

static inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

void bink_video_copy_bgra(BinkVideo* v, uint8_t* dest, uint32_t pitch, uint32_t width, uint32_t height)
{
	if (!v || !v->have_frame)
		return;
	AVFrame* f = v->frame;
	uint32_t w = (uint32_t)f->width < width ? (uint32_t)f->width : width;
	uint32_t h = (uint32_t)f->height < height ? (uint32_t)f->height : height;
	int alpha = f->format == AV_PIX_FMT_YUVA420P;
	for (uint32_t y = 0; y < h; y++)
	{
		const uint8_t* Y = f->data[0] + (size_t)y * (size_t)f->linesize[0];
		const uint8_t* U = f->data[1] + (size_t)(y / 2) * (size_t)f->linesize[1];
		const uint8_t* V = f->data[2] + (size_t)(y / 2) * (size_t)f->linesize[2];
		const uint8_t* A = alpha ? f->data[3] + (size_t)y * (size_t)f->linesize[3] : NULL;
		uint8_t* out = dest + (size_t)y * pitch;
		for (uint32_t x = 0; x < w; x++)
		{
			int c = 298 * (Y[x] - 16), d = U[x / 2] - 128, e = V[x / 2] - 128;
			out[4 * x + 0] = clamp8((c + 516 * d + 128) >> 8);
			out[4 * x + 1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
			out[4 * x + 2] = clamp8((c + 409 * e + 128) >> 8);
			out[4 * x + 3] = A ? A[x] : 255;
		}
	}
}

void bink_video_copy_scaled_bgra(BinkVideo* v, uint8_t* dest, uint32_t pitch, uint32_t dest_w, uint32_t dest_h, float scale)
{
	if (!v || !v->have_frame)
		return;
	AVFrame* f = v->frame;
	uint32_t sw = (uint32_t)f->width, sh = (uint32_t)f->height;
	/* Convert once at source size, then sample. */
	static uint8_t* src;
	static size_t cap;
	if ((size_t)sw * sh * 4 > cap)
	{
		cap = (size_t)sw * sh * 4;
		src = realloc(src, cap);
	}
	bink_video_copy_bgra(v, src, sw * 4, sw, sh);
	uint32_t w = (uint32_t)(sw * scale), h = (uint32_t)(sh * scale);
	if (w > dest_w) w = dest_w;
	if (h > dest_h) h = dest_h;
	uint32_t x0 = (dest_w - w) / 2, y0 = (dest_h - h) / 2;
	for (uint32_t y = 0; y < dest_h; y++)
		memset(dest + (size_t)y * pitch, 0, (size_t)dest_w * 4);
	for (uint32_t y = 0; y < h; y++)
	{
		float fy = ((float)y + 0.5f) / scale - 0.5f;
		int iy = fy < 0 ? 0 : (int)fy;
		float ty = fy - (float)iy;
		if (ty < 0) ty = 0;
		int iy1 = iy + 1 < (int)sh ? iy + 1 : (int)sh - 1;
		const uint8_t* r0 = src + (size_t)iy * sw * 4;
		const uint8_t* r1 = src + (size_t)iy1 * sw * 4;
		uint8_t* out = dest + (size_t)(y0 + y) * pitch + (size_t)x0 * 4;
		for (uint32_t x = 0; x < w; x++)
		{
			float fx = ((float)x + 0.5f) / scale - 0.5f;
			int ix = fx < 0 ? 0 : (int)fx;
			float tx = fx - (float)ix;
			if (tx < 0) tx = 0;
			int ix1 = ix + 1 < (int)sw ? ix + 1 : (int)sw - 1;
			for (int ch = 0; ch < 4; ch++)
			{
				float a = r0[4 * ix + ch] + (r0[4 * ix1 + ch] - r0[4 * ix + ch]) * tx;
				float b = r1[4 * ix + ch] + (r1[4 * ix1 + ch] - r1[4 * ix + ch]) * tx;
				out[4 * x + ch] = (uint8_t)(a + (b - a) * ty + 0.5f);
			}
		}
	}
}
