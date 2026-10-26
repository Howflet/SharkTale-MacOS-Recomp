/*=============================================================================
	hooks.c: Host-side observation hooks on guest exports (debug aids).

	FOutputDevice::Logf and appErrorf are wrapped so the engine's log lines
	are echoed to stderr (this build's own file log is disabled). Calls
	between modules go through the dispatch table and are seen; calls inside
	Core itself are direct and are not.
=============================================================================*/
#include <math.h>
#include <stdlib.h>

#include "hle.h"

static RecompFn OrigLogfEvent, OrigLogf, OrigErrorf;
static int Quiet = -1;

static void echo(Ctx* c, u32 fmt, u32 argp, int event)
{
	if (Quiet < 0)
		Quiet = getenv("SHARK_QUIET_LOG") != NULL;
	if (Quiet)
		return;
	char buf[4096];
	FmtOut o = { 0, sizeof buf, 0, 0, buf };
	hle_format(&o, fmt, 1, argp);
	if (event >= 0)
	{
		extern const char* hle_ename(int);
		fprintf(stderr, "[log] %s: %s\n", hle_ename(event), buf);
	}
	else
		fprintf(stderr, "[log] %s\n", buf);
}

/* ?Logf@FOutputDevice@@QAAXW4EName@@PBGZZ: cdecl (this, EName, fmt, ...) */
static void hook_logf_event(Ctx* c)
{
	u32 esp = R4(REG_ESP);
	echo(c, LD4(esp + 12), esp + 16, (int)LD4(esp + 8));
	OrigLogfEvent(c);
}

/* ?Logf@FOutputDevice@@QAAXPBGZZ: cdecl (this, fmt, ...) */
static void hook_logf(Ctx* c)
{
	u32 esp = R4(REG_ESP);
	echo(c, LD4(esp + 8), esp + 12, -1);
	OrigLogf(c);
}

/* ?appErrorf@@YAXPBGZZ: cdecl (fmt, ...) -- fatal error */
static void hook_errorf(Ctx* c)
{
	u32 esp = R4(REG_ESP);
	char buf[4096];
	FmtOut o = { 0, sizeof buf, 0, 0, buf };
	hle_format(&o, LD4(esp + 4), 1, esp + 8);
	fprintf(stderr, "[log] FATAL appErrorf: %s\n", buf);
	OrigErrorf(c);
}

/* Frame counter on UGameEngine::Tick (SHARK_FPS=1). */
#include <time.h>
static RecompFn OrigTick;
static void hook_tick(Ctx* c)
{
	static unsigned frames;
	static struct timespec last;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!last.tv_sec)
		last = now;
	frames++;
	double dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) * 1e-9;
	if (dt >= 5.0)
	{
		fprintf(stderr, "[fps] %.1f frames/s\n", frames / dt);
		frames = 0;
		last = now;
	}
	OrigTick(c);
}

static RecompFn hook(GuestModule* m, const char* name, RecompFn fn)
{
	u32 a = rt_module_export(m, name);
	RecompFn orig = a ? rt_lookup(a) : NULL;
	if (orig)
		rt_register(a, fn);
	return orig;
}

void rt_install_hooks(void)
{
	GuestModule* core = rt_find_module("Core.dll");
	if (!core)
		return;
	OrigLogfEvent = hook(core, "?Logf@FOutputDevice@@QAAXW4EName@@PBGZZ", hook_logf_event);
	OrigLogf = hook(core, "?Logf@FOutputDevice@@QAAXPBGZZ", hook_logf);
	OrigErrorf = hook(core, "?appErrorf@@YAXPBGZZ", hook_errorf);
	GuestModule* engine = rt_find_module("Engine.dll");
	if (engine && getenv("SHARK_FPS"))
		OrigTick = hook(engine, "?Tick@UGameEngine@@UAEXM@Z", hook_tick);
}

/*-----------------------------------------------------------------------------
	SHARK_TRACE_MATERIALS=1: log each material D3DDrv's SetMaterial sees (once)
	and the number of passes it produced. Installed once D3DDrv is loaded.
-----------------------------------------------------------------------------*/

static u32 NamesArray;  /* FName::Names (TArray<FNameEntry*>) */

static void object_name(u32 obj, char* out, size_t cap)
{
	out[0] = 0;
	if (!obj || !NamesArray)
		return;
	u32 idx = LD4(obj + 0x20), data = LD4(NamesArray), num = LD4(NamesArray + 4);
	if (idx >= num)
		return;
	u32 entry = LD4(data + 4 * idx);
	size_t i = 0;
	for (; i + 1 < cap; i++)
	{
		u16 ch = LD2(entry + 12 + 2 * (u32)i);
		if (!ch)
			break;
		out[i] = (char)ch;
	}
	out[i] = 0;
}

static RecompFn SlotOrig[30];
static unsigned SlotCount[30];
static void slot_hook_0(Ctx* c) { SlotCount[0]++; SlotOrig[0](c); }
static void slot_hook_1(Ctx* c) { SlotCount[1]++; SlotOrig[1](c); }
static void slot_hook_2(Ctx* c) { SlotCount[2]++; SlotOrig[2](c); }
static void slot_hook_3(Ctx* c) { SlotCount[3]++; SlotOrig[3](c); }
static void slot_hook_4(Ctx* c) { SlotCount[4]++; SlotOrig[4](c); }
static void slot_hook_5(Ctx* c) { SlotCount[5]++; SlotOrig[5](c); }
static void slot_hook_6(Ctx* c) { SlotCount[6]++; SlotOrig[6](c); }
static void slot_hook_7(Ctx* c) { SlotCount[7]++; SlotOrig[7](c); }
static void slot_hook_8(Ctx* c) { SlotCount[8]++; SlotOrig[8](c); }
static void slot_hook_9(Ctx* c) { SlotCount[9]++; SlotOrig[9](c); }
static void slot_hook_10(Ctx* c) { SlotCount[10]++; SlotOrig[10](c); }
static void slot_hook_11(Ctx* c) { SlotCount[11]++; SlotOrig[11](c); }
static void slot_hook_12(Ctx* c) { SlotCount[12]++; SlotOrig[12](c); }
static void slot_hook_13(Ctx* c) { SlotCount[13]++; SlotOrig[13](c); }
static void slot_hook_14(Ctx* c) { SlotCount[14]++; SlotOrig[14](c); }
static void slot_hook_15(Ctx* c) { SlotCount[15]++; SlotOrig[15](c); }
static void slot_hook_16(Ctx* c) { SlotCount[16]++; SlotOrig[16](c); }
static void slot_hook_17(Ctx* c) { SlotCount[17]++; SlotOrig[17](c); }
static void slot_hook_18(Ctx* c) { SlotCount[18]++; SlotOrig[18](c); }
static void slot_hook_19(Ctx* c) { SlotCount[19]++; SlotOrig[19](c); }
static void slot_hook_20(Ctx* c) { SlotCount[20]++; SlotOrig[20](c); }
static void slot_hook_21(Ctx* c) { SlotCount[21]++; SlotOrig[21](c); }
static void slot_hook_22(Ctx* c) { SlotCount[22]++; SlotOrig[22](c); }
static void slot_hook_23(Ctx* c) { SlotCount[23]++; SlotOrig[23](c); }
static void slot_hook_24(Ctx* c) { SlotCount[24]++; SlotOrig[24](c); }
static void slot_hook_25(Ctx* c) { SlotCount[25]++; SlotOrig[25](c); }
static void slot_hook_26(Ctx* c) { SlotCount[26]++; SlotOrig[26](c); }
static void slot_hook_27(Ctx* c) { SlotCount[27]++; SlotOrig[27](c); }
static void slot_hook_28(Ctx* c) { SlotCount[28]++; SlotOrig[28](c); }
static void slot_hook_29(Ctx* c) { SlotCount[29]++; SlotOrig[29](c); }
static const RecompFn SlotHooks[30] = { slot_hook_0, slot_hook_1, slot_hook_2, slot_hook_3, slot_hook_4, slot_hook_5, slot_hook_6, slot_hook_7, slot_hook_8, slot_hook_9, slot_hook_10, slot_hook_11, slot_hook_12, slot_hook_13, slot_hook_14, slot_hook_15, slot_hook_16, slot_hook_17, slot_hook_18, slot_hook_19, slot_hook_20, slot_hook_21, slot_hook_22, slot_hook_23, slot_hook_24, slot_hook_25, slot_hook_26, slot_hook_27, slot_hook_28, slot_hook_29 };
void rt_dump_slot_counts(void)
{
	for (int i = 0; i < 30; i++)
		if (SlotOrig[i])
			fprintf(stderr, "[slots] %2d: %u\n", i, SlotCount[i]);
}

static RecompFn OrigSetMaterial;
static void hook_set_material(Ctx* c)
{
	static u32 seen[8192];
	u32 self = R4(REG_ECX), mat = LD4(R4(REG_ESP) + 4);
	OrigSetMaterial(c);
	u32 h = (mat >> 3) & 8191;
	for (int probe = 0; probe < 8192; probe++, h = (h + 1) & 8191)
	{
		if (seen[h] == mat)
			return;
		if (!seen[h])
			break;
	}
	seen[h] = mat;
	u32 state = LD4(self + 0x2aa0c);
	char name[128], cls[128];
	object_name(mat, name, sizeof name);
	object_name(mat ? LD4(mat + 0x24) : 0, cls, sizeof cls);
	fprintf(stderr, "[material] %s %s -> %d passes\n", cls, name, state ? (int)LD4(state + 0x54c) : -1);
}

void rt_install_d3d_hooks(void)
{
	static int done;
	if (done || !getenv("SHARK_TRACE_MATERIALS"))
		return;
	done = 1;
	GuestModule* core = rt_find_module("Core.dll");
	NamesArray = core ? rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A") : 0;
	GuestModule* d3d = rt_find_module("D3DDrv.dll");
	if (!d3d)
		return;
	u32 delta = d3d->base - 0x10000000u;
	if (getenv("SHARK_TRACE_SLOTS"))
	{
		/* FD3DRenderInterface vtable at 0x10078100 (entries are jump thunks). */
		for (int i = 0; i < 30; i++)
		{
			u32 thunk = LD4(0x10078100u + delta + 4 * (u32)i);
			RecompFn f = rt_lookup(thunk);
			if (!f)
			{
				u32 t = thunk;
				if (LD1(t) == 0xe9)
					t = t + 5 + LD4(t + 1);
				f = rt_lookup(t);
			}
			if (!f)
				continue;
			SlotOrig[i] = f;
			rt_register(thunk, SlotHooks[i]);
		}
		atexit(rt_dump_slot_counts);
	}
	u32 target = 0x1001e420u + delta;  /* FD3DRenderInterface::SetMaterial (vtable slot 15) */
	OrigSetMaterial = rt_lookup(target);
	if (OrigSetMaterial)
	{
		rt_register(target, hook_set_material);
		rt_register(0x10001532u + delta, hook_set_material);  /* its jump thunk */
	}
	fprintf(stderr, "[hooks] material trace %s\n", OrigSetMaterial ? "on" : "unavailable");
}

/*-----------------------------------------------------------------------------
	Host console: run an engine console command (UGameEngine::Exec) from the
	host, e.g. from the SHARK_SCRIPT 'x' action. Called with a live Ctx.
-----------------------------------------------------------------------------*/

static char PendingExec[8][256];
static int NumPendingExec;

void rt_console_queue(const char* cmd)
{
	if (NumPendingExec < 8)
		snprintf(PendingExec[NumPendingExec++], sizeof PendingExec[0], "%s", cmd);
}

static int name_is(u32 obj, const char* want)
{
	char n[128];
	object_name(obj, n, sizeof n);
	return !strcmp(n, want);
}

void rt_console_run_pending(Ctx* c)
{
	if (!NumPendingExec)
		return;
	GuestModule* core = rt_find_module("Core.dll");
	GuestModule* engine = rt_find_module("Engine.dll");
	if (!NamesArray && core)
		NamesArray = rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A");
	u32 objects = core ? rt_module_export(core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A") : 0;
	u32 glog = core ? rt_module_export(core, "?GLog@@3PAVFOutputDevice@@A") : 0;
	u32 exec = engine ? rt_module_export(engine, "?Exec@UGameEngine@@UAEHPBGAAVFOutputDevice@@@Z") : 0;
	u32 game_engine = 0;
	if (objects)
		for (u32 i = 0, n = LD4(objects + 4); i < n && !game_engine; i++)
		{
			u32 o = LD4(LD4(objects) + 4 * i);
			if (o && name_is(LD4(o + 0x24), "GameEngine"))
				game_engine = o;
		}
	for (int k = 0; k < NumPendingExec; k++)
	{
		if (!game_engine || !exec || !glog)
		{
			fprintf(stderr, "[console] engine not found\n");
			break;
		}
		const char* cmd = PendingExec[k];
		size_t len = strlen(cmd);
		u32 w = rt_heap_alloc(2 * (u32)len + 2);
		for (size_t i = 0; i <= len; i++)
			ST2(w + 2 * (u32)i, (u16)(unsigned char)cmd[i]);
		u32 saved[8];
		for (int r = 0; r < 8; r++)
			saved[r] = R4(4 * r);
		R4(REG_ECX) = game_engine;
		u32 args[2] = { w, LD4(glog) };
		u32 ok = rt_call_guest(c, exec, 2, args, 1);
		for (int r = 0; r < 8; r++)
			R4(4 * r) = saved[r];
		fprintf(stderr, "[console] %s -> %u\n", cmd, ok);
		rt_heap_free(w);
	}
	NumPendingExec = 0;
}

/*-----------------------------------------------------------------------------
	SHARK_SELFTEST=1: check recompiled math kernels against C references
	before the game starts.
-----------------------------------------------------------------------------*/

static float frand(unsigned* seed, float lo, float hi)
{
	*seed = *seed * 1103515245u + 12345u;
	return lo + (hi - lo) * (float)((*seed >> 8) & 0xffff) / 65535.0f;
}

static void selftest_spherecheck(Ctx* c)
{
	GuestModule* engine = rt_find_module("Engine.dll");
	if (!engine)
		return;
	u32 delta = engine->base - 0x10300000u;
	u32 fn = 0x104b7c60u + delta;          /* FConvexVolume::SphereCheck(const FSphere&) */
	u32 sse_flag = LD4(0x1096ed80u + delta); /* imported int checked by the SSE path */
	u32 vol = rt_heap_alloc(0x210), sph = rt_heap_alloc(16);
	unsigned seed = 1;
	int bad[2] = { 0, 0 };
	for (int mode = 0; mode < 2; mode++)
	{
		u32 old_flag = LD4(sse_flag);
		ST4(sse_flag, (u32)mode);
		for (int t = 0; t < 2000; t++)
		{
			int n = 4 + (int)(frand(&seed, 0, 8.99f));
			float planes[32][4];
			for (int i = 0; i < n; i++)
			{
				float x = frand(&seed, -1, 1), y = frand(&seed, -1, 1), z = frand(&seed, -1, 1);
				float l = sqrtf(x * x + y * y + z * z) + 1e-6f;
				planes[i][0] = x / l;
				planes[i][1] = y / l;
				planes[i][2] = z / l;
				planes[i][3] = frand(&seed, -100, 100);
				for (int k = 0; k < 4; k++)
					ST4(vol + 16 * (u32)i + 4 * (u32)k, bitsf32(planes[i][k]));
			}
			ST4(vol + 0x200, (u32)n);
			float S[4] = { frand(&seed, -200, 200), frand(&seed, -200, 200), frand(&seed, -200, 200), frand(&seed, 0, 80) };
			for (int k = 0; k < 4; k++)
				ST4(sph + 4 * (u32)k, bitsf32(S[k]));
			int ref = 1;
			for (int i = 0; i < n; i++)
			{
				float d = planes[i][0] * S[0] + planes[i][1] * S[1] + planes[i][2] * S[2] - planes[i][3];
				if (d > -S[3])
					ref |= 2;
				if (d > S[3])
				{
					ref &= ~1;
					break;
				}
			}
			u32 saved[8];
			for (int r = 0; r < 8; r++)
				saved[r] = R4(4 * r);
			R4(REG_ECX) = vol;
			u32 args[1] = { sph };
			u32 got = rt_call_guest(c, fn, 1, args, 1) & 0xff;
			for (int r = 0; r < 8; r++)
				R4(4 * r) = saved[r];
			/* Visibility (bit 0) must match; the SSE path may report intersection differently near edges. */
			if ((got & 1) != (u32)(ref & 1))
			{
				if (bad[mode]++ < 5)
					fprintf(stderr, "[selftest] SphereCheck %s: planes %d sphere (%g %g %g r %g) got %u want %d\n",
						mode ? "SSE" : "x87", n, S[0], S[1], S[2], S[3], got, ref);
			}
		}
		ST4(sse_flag, old_flag);
	}
	fprintf(stderr, "[selftest] SphereCheck: x87 %d/2000 wrong, SSE %d/2000 wrong\n", bad[0], bad[1]);
	rt_heap_free(vol);
	rt_heap_free(sph);
}

void rt_selftest(Ctx* c)
{
	if (!getenv("SHARK_SELFTEST"))
		return;
	selftest_spherecheck(c);
}
