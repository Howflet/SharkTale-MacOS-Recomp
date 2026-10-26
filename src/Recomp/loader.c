/*=============================================================================
	loader.c: Map the original PE images into guest memory at the layout in
	tools/recomp_layout.txt, relocate, bind imports, and run DLL entry points.

	Code never executes from these images (it is recompiled); they provide
	data sections, vtables, string tables, import address tables and export
	directories exactly as the original program expects them.
=============================================================================*/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "recomp.h"
#include "runtime.h"

/* Recompiled modules: dispatch tables produced by EmitC.java. */
#define DECLARE_TABLE(file, mod, base) \
	extern const RecompEntry mod##_entries[]; extern const unsigned mod##_entry_count;
RECOMP_MODULE_LIST(DECLARE_TABLE)

typedef struct ModuleSpec { const char* file; u32 base; const RecompEntry* entries; const unsigned* count; } ModuleSpec;
#define SPEC(file, mod, base) { file, base, mod##_entries, &mod##_entry_count },
static const ModuleSpec Specs[] = { RECOMP_MODULE_LIST(SPEC) };
#define NUM_SPECS (sizeof(Specs) / sizeof(Specs[0]))

static GuestModule Modules[64];
static int NumModules;
static int LoadOrder[64];
static int NumLoaded;

const char* g_game_dir;

static const ModuleSpec* find_spec(const char* name)
{
	for (unsigned i = 0; i < NUM_SPECS; i++)
		if (!strcasecmp(Specs[i].file, name))
			return &Specs[i];
	return NULL;
}

GuestModule* rt_find_module(const char* name)
{
	for (int i = 0; i < NumModules; i++)
		if (!strcasecmp(Modules[i].name, name))
			return &Modules[i];
	return NULL;
}

GuestModule* rt_module_at(u32 addr)
{
	for (int i = 0; i < NumModules; i++)
		if (addr >= Modules[i].base && addr < Modules[i].base + Modules[i].size)
			return &Modules[i];
	return NULL;
}

static u8* read_file(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* d = malloc(*size);
	if (fread(d, 1, *size, f) != *size)
	{
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

static inline u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

/* PE header offsets inside a mapped image. */
static u32 nt_headers(u32 base) { return base + LD4(base + 0x3c); }
static u32 opt_header(u32 base) { return nt_headers(base) + 24; }
static u32 data_dir(u32 base, int i, u32* size)
{
	u32 o = opt_header(base) + 96 + 8 * i;
	if (size)
		*size = LD4(o + 4);
	return LD4(o);
}

u32 rt_module_export(GuestModule* m, const char* name)
{
	u32 dsize;
	u32 rva = data_dir(m->base, 0, &dsize);
	if (!rva)
		return 0;
	u32 e = m->base + rva;
	u32 ordinal_base = LD4(e + 16), nfuncs = LD4(e + 20), nnames = LD4(e + 24);
	u32 funcs = m->base + LD4(e + 28), names = m->base + LD4(e + 32), ords = m->base + LD4(e + 36);
	u32 index = 0xffffffffu;
	if ((uintptr_t)name < 0x10000)
	{
		index = (u32)(uintptr_t)name - ordinal_base;  /* by ordinal */
	}
	else
	{
		for (u32 i = 0; i < nnames; i++)
			if (!strcmp((const char*)GP(m->base + LD4(names + 4 * i)), name))
			{
				index = LD2(ords + 2 * i);
				break;
			}
	}
	if (index >= nfuncs)
		return 0;
	u32 f = LD4(funcs + 4 * index);
	if (f >= rva && f < rva + dsize)
	{
		/* Forwarder "DLL.Func". */
		char fwd[128];
		snprintf(fwd, sizeof fwd, "%s", (const char*)GP(m->base + f));
		char* dot = strchr(fwd, '.');
		if (!dot)
			return 0;
		*dot = 0;
		char dll[140];
		snprintf(dll, sizeof dll, "%s.dll", fwd);
		GuestModule* t = rt_load_module(dll);
		return t ? rt_module_export(t, dot + 1) : rt_hle_address(dll, dot + 1);
	}
	return f ? m->base + f : 0;
}

static void bind_imports(GuestModule* m)
{
	u32 rva = data_dir(m->base, 1, NULL);
	if (!rva)
		return;
	for (u32 d = m->base + rva; LD4(d + 12); d += 20)
	{
		const char* dll = (const char*)GP(m->base + LD4(d + 12));
		u32 names = LD4(d) ? m->base + LD4(d) : m->base + LD4(d + 16);
		u32 iat = m->base + LD4(d + 16);
		GuestModule* target = rt_load_module(dll);
		for (u32 i = 0;; i++)
		{
			u32 v = LD4(names + 4 * i);
			if (!v)
				break;
			const char* fn;
			char ordname[16];
			if (v & 0x80000000u)
			{
				snprintf(ordname, sizeof ordname, "#%u", v & 0xffff);
				fn = target ? (const char*)(uintptr_t)(v & 0xffff) : ordname;
			}
			else
			{
				fn = (const char*)GP(m->base + v + 2);
			}
			u32 addr = target ? rt_module_export(target, fn) : rt_hle_address(dll, fn);
			if (!addr)
			{
				fprintf(stderr, "[loader] %s: unresolved import %s!%s\n", m->name, dll,
					(v & 0x80000000u) ? ordname : fn);
				addr = rt_hle_address(dll, (v & 0x80000000u) ? ordname : fn);
			}
			ST4(iat + 4 * i, addr);
		}
	}
}

GuestModule* rt_load_module(const char* name)
{
	/* Accept paths and names without extension. */
	const char* base_name = name;
	for (const char* p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base_name = p + 1;
	char file[128];
	snprintf(file, sizeof file, "%s", base_name);
	if (!strchr(file, '.'))
		strncat(file, ".dll", sizeof file - strlen(file) - 1);

	GuestModule* m = rt_find_module(file);
	if (m)
		return m;
	const ModuleSpec* spec = find_spec(file);
	if (!spec)
		return NULL;  /* not recompiled: the caller falls back to HLE */

	char path[1024];
	snprintf(path, sizeof path, "%s/%s", g_game_dir, spec->file);
	size_t size;
	u8* pe = read_file(path, &size);
	if (!pe)
	{
		fprintf(stderr, "[loader] cannot read %s\n", path);
		exit(1);
	}
	u32 nt = rd32(pe + 0x3c);
	u16 nsec = rd16(pe + nt + 6), optsize = rd16(pe + nt + 20);
	const u8* opt = pe + nt + 24;
	u32 pref = rd32(opt + 28), image_size = rd32(opt + 56), headers = rd32(opt + 60);
	u32 base = spec->base, delta = base - pref;

	m = &Modules[NumModules++];
	snprintf(m->name, sizeof m->name, "%s", spec->file);
	m->base = base;
	m->size = image_size;
	m->entry = rd32(opt + 16) ? base + rd32(opt + 16) : 0;
	m->recompiled = 1;

	memcpy(GP(base), pe, headers);
	for (u16 i = 0; i < nsec; i++)
	{
		const u8* s = pe + nt + 24 + optsize + 40 * i;
		u32 vsize = rd32(s + 8), va = rd32(s + 12), rsize = rd32(s + 16), roff = rd32(s + 20);
		memset(GP(base + va), 0, vsize > rsize ? vsize : rsize);
		if (rsize)
			memcpy(GP(base + va), pe + roff, rsize < vsize || !vsize ? rsize : vsize);
	}
	free(pe);

	/* Base relocations (IMAGE_REL_BASED_HIGHLOW only, as in these binaries). */
	u32 rsz;
	u32 rrva = data_dir(base, 5, &rsz);
	if (delta && rrva)
	{
		for (u32 blk = base + rrva; blk < base + rrva + rsz;)
		{
			u32 page = LD4(blk), bsize = LD4(blk + 4);
			if (!bsize)
				break;
			for (u32 e = 8; e < bsize; e += 2)
			{
				u16 ent = LD2(blk + e);
				if ((ent >> 12) == 3)
				{
					u32 a = base + page + (ent & 0xfff);
					ST4(a, LD4(a) + delta);
				}
			}
			blk += bsize;
		}
	}
	/* Keep the in-image ImageBase coherent with where we put it. */
	ST4(opt_header(base) + 28, base);

	rt_register_table(spec->entries, *spec->count);
	bind_imports(m);
	LoadOrder[NumLoaded++] = (int)(m - Modules);  /* after dependencies */
	fprintf(stderr, "[loader] %-16s at %#010x (+%#x) %u functions\n", m->name, base, delta, *spec->count);
	return m;
}

void rt_init_modules(Ctx* c)
{
	for (int i = 0; i < NumLoaded; i++)
	{
		GuestModule* m = &Modules[LoadOrder[i]];
		if (m->initialized)
			continue;
		m->initialized = 1;
		u16 characteristics = LD2(nt_headers(m->base) + 4 + 18);
		if (!(characteristics & 0x2000) || !m->entry)
			continue;  /* not a DLL (the EXE entry runs last, from main) */
		u32 args[3] = { m->base, 1 /* DLL_PROCESS_ATTACH */, 0 };
		fprintf(stderr, "[loader] DllMain %s\n", m->name);
		u32 ok = rt_call_guest(c, m->entry, 3, args, 1);
		if (!ok)
			fprintf(stderr, "[loader] DllMain of %s returned FALSE\n", m->name);
	}
}
