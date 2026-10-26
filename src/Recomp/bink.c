/*=============================================================================
	bink.c: Native Bink 1 container and audio decoder.

	Container: "BIK" + revision, frame count, size, fps, then per audio track
	(max decoded bytes, rate/flags, id), then a frame index whose bit 0 marks
	keyframes. Each frame holds, per audio track, a 32-bit size and a packet,
	followed by the video packet.

	Audio: each packet starts with the decoded byte count, then blocks of
	quantised transform coefficients (DCT-III or inverse real DFT) with a
	1/16-length linear crossfade between blocks. Algorithm as documented by
	the open Bink audio decoders (e.g. FFmpeg's binkaudio).
=============================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bink.h"

#define MAX_TRACKS 4
#define MAX_FRAME_LEN 4096

enum { AUD_16BIT = 0x4000, AUD_STEREO = 0x2000, AUD_DCT = 0x1000 };

/*-----------------------------------------------------------------------------
	Bit reader (little-endian bit order).
-----------------------------------------------------------------------------*/

typedef struct Bits { const uint8_t* p; uint32_t size_bits, pos; } Bits;

static uint32_t get_bits(Bits* b, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++, b->pos++)
		if (b->pos < b->size_bits && (b->p[b->pos >> 3] >> (b->pos & 7)) & 1)
			v |= 1u << i;
	return v;
}
static int bits_left(const Bits* b) { return (int)b->size_bits - (int)b->pos; }

/*-----------------------------------------------------------------------------
	Complex FFT (radix-2, in place). sign=+1 computes sum x e^{+i2pi kn/N}.
-----------------------------------------------------------------------------*/

static void fft(float* re, float* im, int n, int sign)
{
	for (int i = 1, j = 0; i < n; i++)
	{
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
		{
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1)
	{
		double ang = sign * 2 * M_PI / len;
		float wr = (float)cos(ang), wi = (float)sin(ang);
		for (int i = 0; i < n; i += len)
		{
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++)
			{
				int a = i + k, b = a + len / 2;
				float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = ncr;
			}
		}
	}
}

/* DCT-III: y_k = x0/2 + sum_{n>=1} x_n cos(pi n (2k+1) / 2N), via a 2N-point FFT. */
static void dct3(float* x, int n)
{
	static float re[2 * MAX_FRAME_LEN], im[2 * MAX_FRAME_LEN];
	for (int i = 0; i < 2 * n; i++)
	{
		re[i] = im[i] = 0;
		if (i < n)
		{
			float v = i ? x[i] : x[0] * 0.5f;
			double a = M_PI * i / (2.0 * n);
			re[i] = (float)(v * cos(a));
			im[i] = (float)(v * sin(a));
		}
	}
	fft(re, im, 2 * n, +1);
	for (int k = 0; k < n; k++)
		x[k] = re[k] * (2.0f / (float)n);  /* DCT-III normalised by 2/N */
}

/* Inverse real DFT from Bink's packing: [0]=DC, [1]=Nyquist, then (re, im) pairs. */
static void irdft(float* x, int n)
{
	static float re[MAX_FRAME_LEN], im[MAX_FRAME_LEN];
	re[0] = x[0];
	im[0] = 0;
	re[n / 2] = x[1];
	im[n / 2] = 0;
	for (int k = 1; k < n / 2; k++)
	{
		re[k] = x[2 * k];
		im[k] = x[2 * k + 1];
		re[n - k] = x[2 * k];
		im[n - k] = -x[2 * k + 1];
	}
	fft(re, im, n, +1);
	for (int i = 0; i < n; i++)
		x[i] = re[i] * 0.5f;
}

/*-----------------------------------------------------------------------------
	Audio decoder.
-----------------------------------------------------------------------------*/

static const uint16_t CriticalFreqs[25] = { 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000,
	2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500 };
static const uint8_t RleLengths[16] = { 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64 };

typedef struct AudioDec
{
	int dct, channels, out_channels, frame_len, overlap_len, num_bands, first;
	uint32_t bands[26];
	float root, quant_table[96];
	float previous[2][MAX_FRAME_LEN / 16];
	float coeffs[2][MAX_FRAME_LEN];
} AudioDec;

static void audio_init(AudioDec* a, uint32_t rate, uint32_t flags)
{
	memset(a, 0, sizeof *a);
	int ch = flags & AUD_STEREO ? 2 : 1;
	int bits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
	a->dct = (flags & AUD_DCT) != 0;
	a->out_channels = ch;
	if (!a->dct)
	{
		/* The RDFT variant codes interleaved samples as one channel. */
		rate *= (uint32_t)ch;
		a->channels = 1;
		if (ch == 2)
			bits++;
	}
	else
		a->channels = ch;
	a->frame_len = 1 << bits;
	a->overlap_len = a->frame_len / 16;
	uint32_t half = (rate + 1) / 2;
	a->root = a->dct ? (float)(a->frame_len / (sqrt(a->frame_len) * 32768.0))
		: (float)(2.0 / (sqrt(a->frame_len) * 32768.0));
	for (int i = 0; i < 96; i++)
		a->quant_table[i] = expf(i * 0.15289164787221953823f) * a->root;
	for (a->num_bands = 1; a->num_bands < 25; a->num_bands++)
		if (half <= CriticalFreqs[a->num_bands - 1])
			break;
	a->bands[0] = 2;
	for (int i = 1; i < a->num_bands; i++)
		a->bands[i] = (CriticalFreqs[i - 1] * (uint32_t)a->frame_len / half) & ~1u;
	a->bands[a->num_bands] = (uint32_t)a->frame_len;
	a->first = 1;
}

static float get_float(Bits* b)
{
	int power = (int)get_bits(b, 5);
	float f = ldexpf((float)get_bits(b, 23), power - 23);
	return get_bits(b, 1) ? -f : f;
}

static int audio_block(AudioDec* a, Bits* b)
{
	float quant[25];
	if (a->dct)
		get_bits(b, 2);
	for (int ch = 0; ch < a->channels; ch++)
	{
		float* c = a->coeffs[ch];
		if (bits_left(b) < 58 + a->num_bands * 8)
			return 0;
		c[0] = get_float(b) * a->root;
		c[1] = get_float(b) * a->root;
		for (int i = 0; i < a->num_bands; i++)
		{
			uint32_t v = get_bits(b, 8);
			quant[i] = a->quant_table[v > 95 ? 95 : v];
		}
		int k = 0;
		float q = quant[0];
		int i = 2;
		while (i < a->frame_len)
		{
			int j;
			if (get_bits(b, 1))
				j = i + RleLengths[get_bits(b, 4)] * 8;
			else
				j = i + 8;
			if (j > a->frame_len)
				j = a->frame_len;
			int width = (int)get_bits(b, 4);
			if (!width)
			{
				memset(c + i, 0, sizeof(float) * (size_t)(j - i));
				i = j;
				while (a->bands[k] < (uint32_t)i)
					q = quant[k++];
			}
			else
			{
				while (i < j)
				{
					if (a->bands[k] == (uint32_t)i)
						q = quant[k++];
					uint32_t coeff = get_bits(b, width);
					c[i] = coeff ? (get_bits(b, 1) ? -q * (float)coeff : q * (float)coeff) : 0.0f;
					i++;
				}
			}
		}
		if (a->dct)
		{
			c[0] /= 0.5f;
			dct3(c, a->frame_len);
		}
		else
			irdft(c, a->frame_len);
	}
	for (int ch = 0; ch < a->channels; ch++)
	{
		int count = a->overlap_len * a->channels;
		float* out = a->coeffs[ch];
		if (!a->first)
			for (int i = 0, j = ch; i < a->overlap_len; i++, j += a->channels)
				out[i] = (a->previous[ch][i] * (float)(count - j) + out[i] * (float)j) / (float)count;
		memcpy(a->previous[ch], &out[a->frame_len - a->overlap_len], sizeof(float) * (size_t)a->overlap_len);
	}
	a->first = 0;
	return 1;
}

static int16_t to_s16(float v)
{
	float s = v * 32768.0f;
	return s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : (int16_t)lrintf(s);
}

/* Decode one packet; returns bytes written. */
static uint32_t audio_packet(AudioDec* a, const uint8_t* p, uint32_t size, int16_t* out, uint32_t cap)
{
	if (size < 4)
		return 0;
	uint32_t reported;
	memcpy(&reported, p, 4);
	Bits b = { p + 4, (size - 4) * 8, 0 };
	uint32_t written = 0, limit = reported < cap ? reported : cap;
	int per_ch = a->frame_len - a->overlap_len;
	while (written < limit && bits_left(&b) > 0)
	{
		if (!audio_block(a, &b))
			break;
		for (int i = 0; i < per_ch && written + 2 * (uint32_t)a->channels <= limit; i++)
			for (int ch = 0; ch < a->channels; ch++)
			{
				out[written / 2] = to_s16(a->coeffs[ch][i]);
				written += 2;
			}
		uint32_t n = (uint32_t)(-(int32_t)b.pos) & 31;
		b.pos += n;
	}
	return written;
}

/*-----------------------------------------------------------------------------
	Container.
-----------------------------------------------------------------------------*/

struct BinkFile
{
	uint8_t* data;
	uint32_t size;
	BinkInfo info;
	uint32_t* index;  /* frames + 1 entries */
	struct
	{
		uint32_t rate, flags, max_bytes, id;
		AudioDec dec;
		int64_t next_frame;  /* frame the decoder state expects next */
	} tracks[MAX_TRACKS];
};

static BinkFile* parse(uint8_t* data, uint32_t size)
{
	if (size < 44 || memcmp(data, "BIK", 3))
	{
		free(data);
		return NULL;
	}
	BinkFile* b = calloc(1, sizeof *b);
	b->data = data;
	b->size = size;
	uint32_t h[10];
	memcpy(h, data + 4, sizeof h);
	b->info.revision = (char)data[3];
	b->info.frames = h[1];
	b->info.width = h[4];
	b->info.height = h[5];
	b->info.fps_num = h[6];
	b->info.fps_den = h[7] ? h[7] : 1;
	b->info.video_flags = h[8];
	b->info.num_tracks = h[9] > MAX_TRACKS ? MAX_TRACKS : h[9];
	uint32_t o = 44, nt = h[9];
	for (uint32_t t = 0; t < nt && t < MAX_TRACKS; t++)
		memcpy(&b->tracks[t].max_bytes, data + o + 4 * t, 4);
	o += 4 * nt;
	for (uint32_t t = 0; t < nt && t < MAX_TRACKS; t++)
	{
		uint16_t rf[2];
		memcpy(rf, data + o + 4 * t, 4);
		b->tracks[t].rate = rf[0];
		b->tracks[t].flags = rf[1];
		audio_init(&b->tracks[t].dec, rf[0], rf[1]);
		b->tracks[t].next_frame = 0;
	}
	o += 4 * nt;
	for (uint32_t t = 0; t < nt && t < MAX_TRACKS; t++)
		memcpy(&b->tracks[t].id, data + o + 4 * t, 4);
	o += 4 * nt;
	if ((uint64_t)o + 4ull * (b->info.frames + 1) > size)
	{
		bink_close(b);
		return NULL;
	}
	b->index = malloc(4 * (size_t)(b->info.frames + 1));
	memcpy(b->index, data + o, 4 * (size_t)(b->info.frames + 1));
	return b;
}

BinkFile* bink_open_memory(const void* data, uint32_t size)
{
	uint8_t* copy = malloc(size);
	memcpy(copy, data, size);
	return parse(copy, size);
}

BinkFile* bink_open_path(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = malloc((size_t)n);
	if (fread(data, 1, (size_t)n, f) != (size_t)n)
	{
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	return parse(data, (uint32_t)n);
}

void bink_close(BinkFile* b)
{
	if (!b)
		return;
	free(b->data);
	free(b->index);
	free(b);
}

const BinkInfo* bink_info(const BinkFile* b) { return &b->info; }

int bink_track_info(const BinkFile* b, int track, BinkTrackInfo* out)
{
	if (track < 0 || (uint32_t)track >= b->info.num_tracks)
		return 0;
	out->rate = b->tracks[track].rate;
	out->channels = b->tracks[track].flags & AUD_STEREO ? 2 : 1;
	out->max_bytes = b->tracks[track].max_bytes;
	return 1;
}

static const uint8_t* frame_data(const BinkFile* b, uint32_t frame, uint32_t* size)
{
	if (frame >= b->info.frames)
		return NULL;
	uint32_t s = b->index[frame] & ~1u, e = b->index[frame + 1] & ~1u;
	if (e > b->size || s >= e)
		return NULL;
	*size = e - s;
	return b->data + s;
}

uint32_t bink_track_decode(BinkFile* b, int track, uint32_t frame, void* out, uint32_t cap)
{
	if (track < 0 || (uint32_t)track >= b->info.num_tracks)
		return 0;
	if (b->tracks[track].next_frame != frame)
	{
		/* Out of order: restart the decoder (the crossfade state belongs to the previous frame). */
		audio_init(&b->tracks[track].dec, b->tracks[track].rate, b->tracks[track].flags);
	}
	b->tracks[track].next_frame = (int64_t)frame + 1;
	uint32_t size;
	const uint8_t* p = frame_data(b, frame, &size);
	if (!p)
		return 0;
	for (int t = 0; t < (int)b->info.num_tracks; t++)
	{
		uint32_t n;
		if (size < 4)
			return 0;
		memcpy(&n, p, 4);
		p += 4;
		size -= 4;
		if (n > size)
			return 0;
		if (t == track)
			return audio_packet(&b->tracks[t].dec, p, n, out, cap);
		p += n;
		size -= n;
	}
	return 0;
}

uint32_t bink_track_max_reported(const BinkFile* b, int track)
{
	uint32_t max = 0;
	for (uint32_t fr = 0; fr < b->info.frames; fr++)
	{
		uint32_t size;
		const uint8_t* p = frame_data(b, fr, &size);
		for (int t = 0; p && t < (int)b->info.num_tracks && size >= 4; t++)
		{
			uint32_t n;
			memcpy(&n, p, 4);
			if (n > size - 4)
				break;
			if (t == track && n >= 4)
			{
				uint32_t r;
				memcpy(&r, p + 4, 4);
				if (r > max)
					max = r;
				break;
			}
			p += 4 + n;
			size -= 4 + n;
		}
	}
	return max;
}

int bink_video_is_keyframe(const BinkFile* b, uint32_t frame)
{
	return frame < b->info.frames && (b->index[frame] & 1);
}

const uint8_t* bink_video_packet(const BinkFile* b, uint32_t frame, uint32_t* size, int* keyframe)
{
	const uint8_t* p = frame_data(b, frame, size);
	if (!p)
		return NULL;
	*keyframe = b->index[frame] & 1;
	for (uint32_t t = 0; t < b->info.num_tracks; t++)
	{
		uint32_t n;
		if (*size < 4)
			return NULL;
		memcpy(&n, p, 4);
		p += 4 + n;
		*size -= 4 + n;
	}
	return p;
}
