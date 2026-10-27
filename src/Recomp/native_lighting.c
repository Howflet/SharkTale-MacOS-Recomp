/*=============================================================================
	native_lighting.c: Native replacement for Engine's static-mesh vertex
	lighting (FUN_104b48a0), the hot spot of geometry precaching.

	FStaticMeshBatchVertexStream::GetStreamData calls it for each batch
	element. It relights all of the mesh's vertices into the actor's
	UStaticMeshInstance colour cache, once per light in the instance's light
	list. The recompiled version took about 3 s per LowerCity precache, and a
	save precaches twice. It transforms every vertex by LocalToWorld for every
	light, even where the light's visibility bit is clear and the result is
	thrown away.

	This version does the same work in the same order and precision (x87
	values as doubles, as the recompiled code keeps them), but transforms only
	the vertices a light reaches. FVector::SafeNormal, SampleIntensity and the
	FColor conversions still run as guest code, so the colours come out
	bit-identical.

	tools/native_overrides.txt renames the generated F_113b48a0 to
	F_113b48a0_recomp, so this definition links in its place.
	  SHARK_RECOMP_LIGHTING=1   use the recompiled version instead
	  SHARK_SELFTEST=1          run both on every call and count differences
=============================================================================*/
#pragma STDC FP_CONTRACT OFF
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

void F_113b48a0_recomp(Ctx* restrict c);

enum { MODE_NATIVE, MODE_RECOMP, MODE_CHECK };

static struct
{
	int init, mode;
	u32 get_light;     /* AActor::GetLightRenderData() */
	u32 safe_normal;   /* FVector::SafeNormal() const (Core, through Engine's import) */
	u32 sample;        /* FDynamicLight::SampleIntensity(FVector, FVector) */
	u32 color_ctor;    /* FColor::FColor(const FPlane&) */
	u32 color_add;     /* FColor::operator+=(FColor) */
	u32 scale;         /* float constant multiplied with the actor's brightness */
	unsigned calls, bad;
} E;

static void report(void)
{
	fprintf(stderr, "[selftest] static mesh lighting: %u/%u calls differ\n", E.bad, E.calls);
}

static void init(void)
{
	E.init = 1;
	GuestModule* engine = rt_find_module("Engine.dll");
	u32 delta = engine ? engine->base - 0x10300000u : 0;
	E.get_light = 0x1030ca77u + delta;
	E.safe_normal = LD4(0x1096e5a0u + delta);
	E.sample = 0x10306b09u + delta;
	E.color_ctor = 0x1030564bu + delta;
	E.color_add = 0x10308dbeu + delta;
	E.scale = 0x1065b418u + delta;
	E.mode = getenv("SHARK_RECOMP_LIGHTING") ? MODE_RECOMP : getenv("SHARK_SELFTEST") ? MODE_CHECK : MODE_NATIVE;
	if (E.mode == MODE_CHECK)
		atexit(report);
}

static u32 call_this(Ctx* c, u32 fn, u32 self, int nargs, const u32* args)
{
	R4(REG_ECX) = self;
	return rt_call_guest(c, fn, nargs, args, 1);  /* thiscall: the callee pops */
}

/* out = (x, y, z, w) * M, summed in the x87 code's order. */
static void transform(u32 m, float x, float y, float z, float w, float out[4])
{
	for (int i = 0; i < 4; i++)
	{
		double s = (double)x * asf32(LD4(m + 4 * (u32)i));
		s += (double)y * asf32(LD4(m + 16 + 4 * (u32)i));
		s += (double)z * asf32(LD4(m + 32 + 4 * (u32)i));
		s += (double)w * asf32(LD4(m + 48 + 4 * (u32)i));
		out[i] = (float)s;
	}
}

/* The function body. cdecl (UStaticMesh* mesh, UStaticMeshInstance* inst,
   FDynamicActor* actor); does not pop the return address. */
static void light_vertices(Ctx* c)
{
	u32 esp = R4(REG_ESP);
	u32 mesh = LD4(esp + 4), inst = LD4(esp + 8), actor = LD4(esp + 12);
	/* Guest scratch below the return address: normal in (16), normal out (12),
	   FPlane (16), FColor (4). */
	u32 nin = esp - 0x40, nout = nin + 16, plane = nout + 16, color = plane + 16;
	R4(REG_ESP) = nin - 16;

	u32 colors = LD4(inst + 0x4c);
	memset(GP(colors), 0, (size_t)LD4(inst + 0x50) * 4);
	ST4(inst + 0x60, LD4(inst + 0x60) + 1);
	u32 matrix = actor + 8;  /* FDynamicActor::LocalToWorld */
	for (s32 i = 0; i < (s32)LD4(inst + 0x3c); i++)
	{
		u32 entry = LD4(inst + 0x38) + 0x14 * (u32)i;  /* light, visibility bits (TArray), lit flag */
		u32 light = LD4(entry);
		ST4(entry + 0x10, 0);
		if (!light)
			continue;
		u32 dl = call_this(c, E.get_light, light, 0, NULL);
		if (c->unwind_record)
			return;
		if (!dl || LD4(dl + 0x38) || LD4(dl + 0x34))
			continue;
		u32 vert = LD4(mesh + 0x78), out = colors;
		u32 bits = LD4(entry + 8) ? LD4(entry + 4) : 0;
		u8 mask = 1;
		for (s32 v = 0; v < (s32)LD4(mesh + 0x7c); v++)
		{
			if (LD1(bits) & mask)
			{
				float p[4], n[4];
				transform(matrix, asf32(LD4(vert)), asf32(LD4(vert + 4)), asf32(LD4(vert + 8)), 1.0f, p);
				transform(matrix, asf32(LD4(vert + 12)), asf32(LD4(vert + 16)), asf32(LD4(vert + 20)), 0.0f, n);
				for (int k = 0; k < 4; k++)
					ST4(nin + 4 * (u32)k, bitsf32(n[k]));
				u32 a1[1] = { nout };
				call_this(c, E.safe_normal, nin, 1, a1);
				if (c->unwind_record)
					return;

				double f = (double)asf32(LD4(LD4(actor) + 0x2c0)) * asf32(LD4(E.scale));
				float sb = (float)(f * asf32(LD4(dl + 0x10)));
				float sg = (float)(f * asf32(LD4(dl + 0x0c)));
				float sr = (float)(f * asf32(LD4(dl + 0x08)));
				u32 a6[6] = { bitsf32(p[0]), bitsf32(p[1]), bitsf32(p[2]), LD4(nout), LD4(nout + 4), LD4(nout + 8) };
				call_this(c, E.sample, dl, 6, a6);
				if (c->unwind_record)
					return;
				float intensity = (float)R10(REG_ST0);
				for (int k = 0; k < 7; k++)  /* FSTP: pop the x87 stack */
					R10(REG_ST0 + 16 * k) = R10(REG_ST0 + 16 * (k + 1));

				ST4(plane, bitsf32((float)((double)sr * intensity)));
				ST4(plane + 4, bitsf32((float)((double)sg * intensity)));
				ST4(plane + 8, bitsf32((float)((double)sb * intensity)));
				ST4(plane + 12, 0);
				u32 a2[1] = { plane };
				u32 made = call_this(c, E.color_ctor, color, 1, a2);
				if (c->unwind_record)
					return;
				u32 a3[1] = { LD4(made) };
				call_this(c, E.color_add, out, 1, a3);
				if (c->unwind_record)
					return;
			}
			mask = (u8)(mask << 1);
			out += 4;
			vert += 0x18;
			if (!mask)
			{
				bits++;
				mask = 1;
			}
		}
		ST4(entry + 0x10, 1);
	}

	/* Modulate by the mesh's vertex colours (if it has them) and take their alpha. */
	u8* src = GP(LD4(mesh + 0x98));
	u8* dst = GP(colors);
	for (s32 v = 0; v < (s32)LD4(mesh + 0x7c); v++, src += 4, dst += 4)
	{
		if (LD4(mesh + 0x154))
		{
			dst[2] = (u8)((u32)src[2] * dst[2] / 255);
			dst[1] = (u8)((u32)src[1] * dst[1] / 255);
			dst[0] = (u8)((u32)dst[0] * src[0] / 255);
		}
		dst[3] = src[3];
	}
	R4(REG_ESP) = esp;
}

/* Runs the native version, then the recompiled one on the same input, and
   compares the colour cache, the revision counter and the lights' lit flags. */
static void check(Ctx* c)
{
	u32 esp = R4(REG_ESP);
	u32 inst = LD4(esp + 8);
	u32 colors = LD4(inst + 0x4c), n = LD4(inst + 0x50), lights = LD4(inst + 0x3c), list = LD4(inst + 0x38);
	u32 rev = LD4(inst + 0x60);
	light_vertices(c);
	if (c->unwind_record)
		return;
	u8* mine = malloc((size_t)n * 4 + (size_t)lights * 4 + 4);
	memcpy(mine, GP(colors), (size_t)n * 4);
	for (u32 i = 0; i < lights; i++)
		memcpy(mine + 4 * n + 4 * i, GP(list + 0x14 * i + 0x10), 4);
	u32 my_rev = LD4(inst + 0x60);
	ST4(inst + 0x60, rev);
	F_113b48a0_recomp(c);
	if (c->unwind_record)
	{
		free(mine);
		return;
	}
	int diff = memcmp(mine, GP(colors), (size_t)n * 4) != 0 || my_rev != LD4(inst + 0x60);
	for (u32 i = 0; i < lights; i++)
		diff |= memcmp(mine + 4 * n + 4 * i, GP(list + 0x14 * i + 0x10), 4) != 0;
	E.calls++;
	if (diff && E.bad++ < 5)
	{
		u32 first = 0;
		while (first < n && !memcmp(mine + 4 * first, GP(colors + 4 * first), 4))
			first++;
		fprintf(stderr, "[selftest] static mesh lighting differs: instance %#x, %u vertices, %u lights, first vertex %u (native %08x, recompiled %08x)\n",
			inst, n, lights, first, first < n ? *(u32*)(mine + 4 * first) : 0, first < n ? LD4(colors + 4 * first) : 0);
	}
	free(mine);
}

void F_113b48a0(Ctx* restrict c)
{
	if (!E.init)
		init();
	if (E.mode == MODE_RECOMP)
	{
		F_113b48a0_recomp(c);
		return;
	}
	if (E.mode == MODE_CHECK)
	{
		check(c);  /* the recompiled run pops the return address */
		return;
	}
	light_vertices(c);
	if (!c->unwind_record)
		R4(REG_ESP) += 4;  /* ret */
}
