/* binkdec: decode a Bink file's first audio track to a WAV (test tool for src/Recomp/bink.c).
     binkdec in.bik out.wav */
#include <stdio.h>
#include <stdlib.h>
#include "bink.h"

int main(int argc, char** argv)
{
	if (argc < 3)
		return 2;
	BinkFile* b = bink_open_path(argv[1]);
	BinkTrackInfo t;
	if (!b || !bink_track_info(b, 0, &t))
	{
		fprintf(stderr, "no audio\n");
		return 1;
	}
	const BinkInfo* in = bink_info(b);
	FILE* f = fopen(argv[2], "wb");
	fseek(f, 44, SEEK_SET);
	uint32_t total = 0, cap = t.max_bytes * 4 + 65536;
	char* buf = malloc(cap);
	for (uint32_t fr = 0; fr < in->frames; fr++)
	{
		uint32_t n = bink_track_decode(b, 0, fr, buf, cap);
		fwrite(buf, 1, n, f);
		total += n;
	}
	uint32_t ch = t.channels, rate = t.rate;
	uint32_t hdr[11] = { 0x46464952u, 36 + total, 0x45564157u, 0x20746d66u, 16, 1u | ch << 16, rate, rate * ch * 2,
		(ch * 2) | 16u << 16, 0x61746164u, total };
	fseek(f, 0, SEEK_SET);
	fwrite(hdr, 4, 11, f);
	fclose(f);
	printf("%c %ux%u %u frames, audio %u Hz %u ch, %u bytes\n", in->revision, in->width, in->height, in->frames, rate, ch, total);
	return 0;
}
