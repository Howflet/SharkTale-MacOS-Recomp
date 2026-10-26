/*=============================================================================
	hle.c: Registry of native (HLE) implementations of the Windows/CRT
	functions the game imports. Each import gets a guest "thunk" address in
	GUEST_HLE_BASE; calling it dispatches to the host implementation.
	Unknown functions get a stub that stops with the function's name.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "hle.h"

typedef struct HleSlot { char dll[32]; char name[96]; HleFn fn; } HleSlot;
static HleSlot Slots[4096];
static int NumSlots;

extern const HleEntry hle_eh[];
extern const HleEntry hle_kernel32[], hle_msvcrt[], hle_user32[], hle_gdi32[], hle_advapi32[],
	hle_winmm[], hle_misc[], hle_wsock32[], hle_d3d8[], hle_dinput8[], hle_bink[], hle_ifc23[], hle_openal[];

static const struct { const char* dll; const HleEntry* table; } Dlls[] = {
	{ "KERNEL32.DLL", hle_kernel32 },
	{ "MSVCR70.DLL", hle_msvcrt },
	{ "MSVCR70.DLL", hle_eh },
	{ "USER32.DLL", hle_user32 },
	{ "GDI32.DLL", hle_gdi32 },
	{ "ADVAPI32.DLL", hle_advapi32 },
	{ "WINMM.DLL", hle_winmm },
	{ "WSOCK32.DLL", hle_wsock32 },
	{ "D3D8.DLL", hle_d3d8 },
	{ "DDRAW.DLL", hle_d3d8 },
	{ "DINPUT8.DLL", hle_dinput8 },
	{ "BINKW32.DLL", hle_bink },
	{ "IFC23.DLL", hle_ifc23 },
	{ "OPENAL32.DLL", hle_openal },
	{ "DEFOPENAL32.DLL", hle_openal },
	{ "SHELL32.DLL", hle_misc },
	{ "OLE32.DLL", hle_misc },
	{ "COMCTL32.DLL", hle_misc },
	{ "COMDLG32.DLL", hle_misc },
	{ "IMM32.DLL", hle_misc },
	{ "VERSION.DLL", hle_misc },
};

static void unimplemented(Ctx* c)
{
	u32 eip = R4(REG_EIP);
	int i = (int)((eip - GUEST_HLE_BASE) / 16);
	char what[256];
	snprintf(what, sizeof what, "unimplemented import %s!%s",
		(i >= 0 && i < NumSlots) ? Slots[i].dll : "?", (i >= 0 && i < NumSlots) ? Slots[i].name : "?");
	recomp_fail(c, eip, what);
}

static HleFn find_impl(const char* dll, const char* name)
{
	char upper[64];
	snprintf(upper, sizeof upper, "%s", dll);
	for (char* p = upper; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	if (!strstr(upper, ".DLL"))
		strncat(upper, ".DLL", sizeof upper - strlen(upper) - 1);
	for (unsigned d = 0; d < sizeof Dlls / sizeof Dlls[0]; d++)
	{
		if (strcmp(Dlls[d].dll, upper))
			continue;
		for (const HleEntry* e = Dlls[d].table; e->name; e++)
			if (!strcmp(e->name, name))
				return e->fn;
	}
	/* Functions shared across DLLs (e.g. OpenAL procs) live in hle_misc too. */
	for (const HleEntry* e = hle_misc; e->name; e++)
		if (!strcmp(e->name, name))
			return e->fn;
	return NULL;
}

/* SHARK_TRACE_HLE=1: log every native API call (name + first arguments). */
static int TraceHle = -1;
static void trace_wrapper(Ctx* c)
{
	u32 eip = R4(REG_EIP);
	int i = (int)((eip - GUEST_HLE_BASE) / 16);
	HleSlot* s = &Slots[i];
	u32 esp = R4(REG_ESP);
	fprintf(stderr, "[hle] %s!%s(%08x, %08x, %08x, %08x) ret=%08x\n", s->dll, s->name,
		LD4(esp + 4), LD4(esp + 8), LD4(esp + 12), LD4(esp + 16), LD4(esp));
	s->fn(c);
}

u32 rt_hle_address(const char* dll, const char* name)
{
	if (TraceHle < 0)
		TraceHle = getenv("SHARK_TRACE_HLE") != NULL;
	for (int i = 0; i < NumSlots; i++)
		if (!strcasecmp(Slots[i].dll, dll) && !strcmp(Slots[i].name, name))
			return GUEST_HLE_BASE + 16 * i;
	if (NumSlots >= (int)(sizeof Slots / sizeof Slots[0]))
	{
		fprintf(stderr, "[hle] too many imports\n");
		exit(1);
	}
	extern u32 hle_data_address(const char* name);
	u32 data = hle_data_address(name);
	if (data)
		return data;  /* exported variables (_iob, _acmdln...) are addresses of data */
	HleSlot* s = &Slots[NumSlots];
	snprintf(s->dll, sizeof s->dll, "%s", dll);
	snprintf(s->name, sizeof s->name, "%s", name);
	s->fn = find_impl(dll, name);
	u32 addr = GUEST_HLE_BASE + 16 * NumSlots++;
	rt_register(addr, s->fn ? (TraceHle ? trace_wrapper : s->fn) : unimplemented);
	return addr;
}

u32 rt_hle_bind(const char* dll, const char* name, HleFn fn)
{
	u32 addr = rt_hle_address(dll, name);
	int i = (int)((addr - GUEST_HLE_BASE) / 16);
	Slots[i].fn = fn;
	rt_register(addr, TraceHle ? trace_wrapper : fn);
	return addr;
}

const char* hle_slot_name(u32 addr)
{
	int i = (int)((addr - GUEST_HLE_BASE) / 16);
	return (addr >= GUEST_HLE_BASE && i < NumSlots) ? Slots[i].name : NULL;
}

/*-----------------------------------------------------------------------------
	Static guest data owned by HLE (CRT globals such as _iob, _pctype).
-----------------------------------------------------------------------------*/

static u32 HleDataTop = GUEST_HLE_DATA;

u32 hle_static_alloc(u32 size)
{
	u32 a = (HleDataTop + 15) & ~15u;
	HleDataTop = a + size;
	return a;
}

/*-----------------------------------------------------------------------------
	Guest string helpers.
-----------------------------------------------------------------------------*/

u32 g_wcslen(u32 s)
{
	u32 n = 0;
	while (LD2(s + 2 * n))
		n++;
	return n;
}

/* Guest UTF-16 -> host UTF-8 (BMP only, which is all the game uses). */
char* g_wstr_utf8(u32 s, char* out, size_t cap)
{
	size_t o = 0;
	if (!s)
	{
		out[0] = 0;
		return out;
	}
	for (u32 i = 0;; i++)
	{
		u16 ch = LD2(s + 2 * i);
		if (!ch || o + 4 >= cap)
			break;
		if (ch < 0x80)
			out[o++] = (char)ch;
		else if (ch < 0x800)
		{
			out[o++] = (char)(0xc0 | (ch >> 6));
			out[o++] = (char)(0x80 | (ch & 0x3f));
		}
		else
		{
			out[o++] = (char)(0xe0 | (ch >> 12));
			out[o++] = (char)(0x80 | ((ch >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (ch & 0x3f));
		}
	}
	out[o] = 0;
	return out;
}

/* Host UTF-8/ASCII -> guest UTF-16 at d (cap in characters, includes NUL). */
u32 g_put_wstr(u32 d, const char* s, u32 cap)
{
	u32 n = 0;
	for (; *s && n + 1 < cap; s++)
		ST2(d + 2 * n++, (u8)*s);
	if (cap)
		ST2(d + 2 * n, 0);
	return n;
}

/*-----------------------------------------------------------------------------
	x87 stack helpers for CRT functions that use ST0/ST1 (_ftol, _CIpow...).
	Ghidra models the x87 stack as fixed ST0..ST7 with explicit shifts.
-----------------------------------------------------------------------------*/

void x87_pop(Ctx* c)
{
	for (int i = 0; i < 7; i++)
		R10(REG_ST0 + 0x10 * i) = R10(REG_ST0 + 0x10 * (i + 1));
}

void x87_push(Ctx* c, f80 v)
{
	for (int i = 7; i > 0; i--)
		R10(REG_ST0 + 0x10 * i) = R10(REG_ST0 + 0x10 * (i - 1));
	R10(REG_ST0) = v;
}
