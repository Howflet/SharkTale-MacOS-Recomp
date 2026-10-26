/*=============================================================================
	hle_msvcrt.c: MSVCR70 for the recompiled game (CDECL: callers pop args).
	Strings are guest memory; "wide" means UTF-16 (TCHAR).
=============================================================================*/
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"

#define CRET(c, v) HLE_RET(c, v, 0)
#define CRETF(c, v) HLE_RETF(c, v, 0)

/*-----------------------------------------------------------------------------
	Formatting (printf family). argp points at the first variadic argument
	on the guest stack (x86: ints 4 bytes, doubles 8 bytes, 64-bit ints 8).
-----------------------------------------------------------------------------*/

static void out_ch(FmtOut* o, u32 ch)
{
	if (o->host)
	{
		if (o->len + 1 < o->cap)
			o->host[o->len] = (char)ch;
	}
	else if (o->len + 1 < o->cap || !o->cap)
	{
		if (o->wide)
			ST2(o->dst + 2 * o->len, (u16)ch);
		else
			ST1(o->dst + o->len, (u8)ch);
	}
	o->len++;
}

static void out_str(FmtOut* o, const char* s)
{
	while (*s)
		out_ch(o, (u8)*s++);
}

u32 hle_format(FmtOut* o, u32 fmt, int wide_fmt, u32 argp)
{
	int fw = wide_fmt ? 2 : 1;
	#define FCH(i) (wide_fmt ? LD2(fmt + 2 * (i)) : LD1(fmt + (i)))
	u32 i = 0;
	for (;;)
	{
		u32 ch = FCH(i++);
		if (!ch)
			break;
		if (ch != '%')
		{
			out_ch(o, ch);
			continue;
		}
		char spec[32] = "%";
		int sl = 1, width = -1, prec = -1, longlong = 0, hsize = 0, lsize = 0;
		/* flags */
		for (;;)
		{
			ch = FCH(i);
			if (ch == '-' || ch == '+' || ch == ' ' || ch == '#' || ch == '0')
			{
				spec[sl++] = (char)ch;
				i++;
			}
			else
				break;
		}
		/* width */
		if (FCH(i) == '*')
		{
			width = (s32)LD4(argp);
			argp += 4;
			i++;
		}
		else
			while (FCH(i) >= '0' && FCH(i) <= '9')
				width = (width < 0 ? 0 : width * 10) + (int)(FCH(i++) - '0');
		/* precision */
		if (FCH(i) == '.')
		{
			i++;
			prec = 0;
			if (FCH(i) == '*')
			{
				prec = (s32)LD4(argp);
				argp += 4;
				i++;
			}
			else
				while (FCH(i) >= '0' && FCH(i) <= '9')
					prec = prec * 10 + (int)(FCH(i++) - '0');
		}
		/* size */
		for (;;)
		{
			ch = FCH(i);
			if (ch == 'l') { lsize = 1; i++; }
			else if (ch == 'h') { hsize = 1; i++; }
			else if (ch == 'L') { i++; }
			else if (ch == 'I' && FCH(i + 1) == '6' && FCH(i + 2) == '4') { longlong = 1; i += 3; }
			else if (ch == 'I' && FCH(i + 1) == '3' && FCH(i + 2) == '2') { i += 3; }
			else break;
		}
		ch = FCH(i++);
		if (width >= 0)
			sl += snprintf(spec + sl, sizeof spec - sl, "%d", width);
		if (prec >= 0)
			sl += snprintf(spec + sl, sizeof spec - sl, ".%d", prec);
		char buf[512];
		switch (ch)
		{
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
		{
			s64 v;
			if (longlong)
			{
				v = (s64)LD8(argp);
				argp += 8;
			}
			else
			{
				u32 raw = LD4(argp);
				argp += 4;
				v = (ch == 'd' || ch == 'i') ? (s64)(s32)raw : (s64)raw;
				if (hsize)
					v = (ch == 'd' || ch == 'i') ? (s64)(s16)raw : (s64)(u16)raw;
			}
			snprintf(spec + sl, sizeof spec - sl, "ll%c", (char)ch);
			snprintf(buf, sizeof buf, spec, (long long)v);
			out_str(o, buf);
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
		{
			double d = asf64(LD8(argp));
			argp += 8;
			snprintf(spec + sl, sizeof spec - sl, "%c", (char)ch);
			snprintf(buf, sizeof buf, spec, d);
			out_str(o, buf);
			break;
		}
		case 'c': case 'C':
		{
			u32 v = LD4(argp);
			argp += 4;
			out_ch(o, wide_fmt ? (u16)v : (u8)v);
			break;
		}
		case 'p':
		{
			snprintf(buf, sizeof buf, "%08X", LD4(argp));
			argp += 4;
			out_str(o, buf);
			break;
		}
		case 's': case 'S':
		{
			u32 s = LD4(argp);
			argp += 4;
			/* MSVC: %s is the format's own width, %S the other; h/l force narrow/wide. */
			int wide_arg = wide_fmt ? (ch == 's') : (ch == 'S');
			if (hsize) wide_arg = 0;
			if (lsize) wide_arg = 1;
			if (!s)
			{
				out_str(o, "(null)");
				break;
			}
			u32 len = 0;
			while ((wide_arg ? LD2(s + 2 * len) : LD1(s + len)) && (prec < 0 || (int)len < prec))
				len++;
			int pad = width > (int)len ? width - (int)len : 0;
			int left = strchr(spec, '-') != NULL;
			if (!left)
				while (pad-- > 0) out_ch(o, ' ');
			for (u32 k = 0; k < len; k++)
				out_ch(o, wide_arg ? LD2(s + 2 * k) : LD1(s + k));
			if (left)
				while (pad-- > 0) out_ch(o, ' ');
			break;
		}
		case '%':
			out_ch(o, '%');
			break;
		case 'n':
			ST4(LD4(argp), o->len);
			argp += 4;
			break;
		default:
			out_ch(o, '%');
			if (ch)
				out_ch(o, ch);
			break;
		}
		(void)fw;
	}
	/* NUL terminate within capacity. */
	if (o->host)
	{
		if (o->cap)
			o->host[o->len < o->cap ? o->len : o->cap - 1] = 0;
	}
	else if (o->cap)
	{
		u32 end = o->len < o->cap ? o->len : o->cap - 1;
		if (o->wide) ST2(o->dst + 2 * end, 0); else ST1(o->dst + end, 0);
	}
	return o->len;
	#undef FCH
}

static u32 fmt_to(u32 dst, u32 cap, int wide, u32 fmt, u32 argp)
{
	FmtOut o = { dst, cap, 0, wide, NULL };
	return hle_format(&o, fmt, wide, argp);
}

HLE(sprintf) { CRET(c, fmt_to(ARG(0), 0x7fffffff, 0, ARG(1), R4(REG_ESP) + 12)); }
HLE(swprintf) { CRET(c, fmt_to(ARG(0), 0x7fffffff, 1, ARG(1), R4(REG_ESP) + 12)); }
HLE(vsprintf) { CRET(c, fmt_to(ARG(0), 0x7fffffff, 0, ARG(1), ARG(2))); }
HLE(_snprintf)
{
	u32 n = fmt_to(ARG(0), ARG(1) + 1, 0, ARG(2), R4(REG_ESP) + 16);
	CRET(c, n > ARG(1) ? 0xffffffffu : n);
}
HLE(_vsnprintf)
{
	u32 n = fmt_to(ARG(0), ARG(1) + 1, 0, ARG(2), ARG(3));
	CRET(c, n > ARG(1) ? 0xffffffffu : n);
}
HLE(_vsnwprintf)
{
	u32 n = fmt_to(ARG(0), ARG(1) + 1, 1, ARG(2), ARG(3));
	CRET(c, n > ARG(1) ? 0xffffffffu : n);
}
static void host_print(FILE* f, int wide, u32 fmt, u32 argp)
{
	char buf[8192];
	FmtOut o = { 0, sizeof buf, 0, 0, buf };
	hle_format(&o, fmt, wide, argp);
	fputs(buf, f);
}
HLE(printf) { host_print(stdout, 0, ARG(0), R4(REG_ESP) + 8); CRET(c, 0); }

/*-----------------------------------------------------------------------------
	Memory and strings.
-----------------------------------------------------------------------------*/

HLE(malloc) { CRET(c, rt_heap_alloc(ARG(0))); }
HLE(calloc)
{
	u32 n = ARG(0) * ARG(1);
	u32 p = rt_heap_alloc(n);
	memset(GP(p), 0, n);
	CRET(c, p);
}
HLE(realloc) { CRET(c, rt_heap_realloc(ARG(0), ARG(1))); }
HLE(free) { rt_heap_free(ARG(0)); CRET(c, 0); }
HLE(memcpy) { memcpy(GP(ARG(0)), GP(ARG(1)), ARG(2)); CRET(c, ARG(0)); }
HLE(memmove) { memmove(GP(ARG(0)), GP(ARG(1)), ARG(2)); CRET(c, ARG(0)); }
HLE(memchr)
{
	void* r = memchr(GP(ARG(0)), (int)ARG(1), ARG(2));
	CRET(c, r ? HP(r) : 0);
}
HLE(strlen) { CRET(c, strlen(g_str(ARG(0)))); }
HLE(strchr) { char* r = strchr((char*)GP(ARG(0)), (int)ARG(1)); CRET(c, r ? HP(r) : 0); }
HLE(strrchr) { char* r = strrchr((char*)GP(ARG(0)), (int)ARG(1)); CRET(c, r ? HP(r) : 0); }
HLE(strstr) { char* r = strstr((char*)GP(ARG(0)), g_str(ARG(1))); CRET(c, r ? HP(r) : 0); }
HLE(strncmp) { CRET(c, (u32)strncmp(g_str(ARG(0)), g_str(ARG(1)), ARG(2))); }
HLE(_stricmp) { CRET(c, (u32)strcasecmp(g_str(ARG(0)), g_str(ARG(1)))); }
HLE(strncpy) { strncpy((char*)GP(ARG(0)), g_str(ARG(1)), ARG(2)); CRET(c, ARG(0)); }
HLE(strncat) { strncat((char*)GP(ARG(0)), g_str(ARG(1)), ARG(2)); CRET(c, ARG(0)); }
HLE(_strdup)
{
	u32 n = (u32)strlen(g_str(ARG(0))) + 1;
	u32 p = rt_heap_alloc(n);
	memcpy(GP(p), GP(ARG(0)), n);
	CRET(c, p);
}
HLE(_strlwr) { for (char* p = (char*)GP(ARG(0)); *p; p++) *p = (char)tolower((unsigned char)*p); CRET(c, ARG(0)); }
static u32 StrtokSave;
HLE(strtok)
{
	u32 s = ARG(0) ? ARG(0) : StrtokSave;
	const char* delim = g_str(ARG(1));
	if (!s)
		CRET(c, 0);
	while (LD1(s) && strchr(delim, LD1(s)))
		s++;
	if (!LD1(s))
	{
		StrtokSave = 0;
		CRET(c, 0);
	}
	u32 e = s;
	while (LD1(e) && !strchr(delim, LD1(e)))
		e++;
	if (LD1(e))
	{
		ST1(e, 0);
		StrtokSave = e + 1;
	}
	else
		StrtokSave = 0;
	CRET(c, s);
}
HLE(atoi) { CRET(c, (u32)atoi(g_str(ARG(0)))); }
HLE(strtol)
{
	char* end;
	long v = strtol(g_str(ARG(0)), &end, (int)ARG(2));
	if (ARG(1))
		ST4(ARG(1), ARG(0) + (u32)(end - g_str(ARG(0))));
	CRET(c, (u32)(s32)v);
}
HLE(atof) { CRETF(c, atof(g_str(ARG(0)))); }
HLE(strtod)
{
	char* end;
	double v = strtod(g_str(ARG(0)), &end);
	if (ARG(1))
		ST4(ARG(1), ARG(0) + (u32)(end - g_str(ARG(0))));
	CRETF(c, v);
}
HLE(toupper) { CRET(c, (u32)toupper((int)ARG(0))); }
HLE(isalnum) { CRET(c, isalnum((int)(u8)ARG(0)) != 0); }
HLE(isalpha) { CRET(c, isalpha((int)(u8)ARG(0)) != 0); }
HLE(isdigit) { CRET(c, isdigit((int)(u8)ARG(0)) != 0); }
HLE(isspace) { CRET(c, isspace((int)(u8)ARG(0)) != 0); }
HLE(iswspace) { u32 ch = ARG(0) & 0xffff; CRET(c, ch == ' ' || (ch >= 9 && ch <= 13) || ch == 0xa0); }
HLE(_isctype) { CRET(c, 0); }

/* Wide strings (UTF-16). */
HLE(wcslen) { CRET(c, g_wcslen(ARG(0))); }
HLE(wcscpy)
{
	u32 d = ARG(0), s = ARG(1), i = 0;
	u16 ch;
	do { ch = LD2(s + 2 * i); ST2(d + 2 * i, ch); i++; } while (ch);
	CRET(c, d);
}
HLE(wcsncpy)
{
	u32 d = ARG(0), s = ARG(1), n = ARG(2), i = 0;
	for (; i < n && LD2(s + 2 * i); i++)
		ST2(d + 2 * i, LD2(s + 2 * i));
	for (; i < n; i++)
		ST2(d + 2 * i, 0);
	CRET(c, d);
}
HLE(wcscat)
{
	u32 d = ARG(0), s = ARG(1), dl = g_wcslen(d), i = 0;
	u16 ch;
	do { ch = LD2(s + 2 * i); ST2(d + 2 * (dl + i), ch); i++; } while (ch);
	CRET(c, d);
}
static int wcmp(u32 a, u32 b, u32 n, int ci)
{
	for (u32 i = 0; i < n; i++)
	{
		u32 x = LD2(a + 2 * i), y = LD2(b + 2 * i);
		if (ci)
		{
			if (x < 128) x = (u32)toupper((int)x);
			if (y < 128) y = (u32)toupper((int)y);
		}
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
	}
	return 0;
}
HLE(wcscmp) { CRET(c, (u32)wcmp(ARG(0), ARG(1), 0xffffffffu, 0)); }
HLE(wcsncmp) { CRET(c, (u32)wcmp(ARG(0), ARG(1), ARG(2), 0)); }
HLE(_wcsicmp) { CRET(c, (u32)wcmp(ARG(0), ARG(1), 0xffffffffu, 1)); }
HLE(_wcsnicmp) { CRET(c, (u32)wcmp(ARG(0), ARG(1), ARG(2), 1)); }
HLE(wcschr)
{
	u32 s = ARG(0), ch = ARG(1) & 0xffff;
	for (;; s += 2)
	{
		u16 v = LD2(s);
		if (v == ch)
			CRET(c, s);
		if (!v)
			CRET(c, 0);
	}
}
HLE(wcsstr)
{
	u32 h = ARG(0), n = ARG(1), nl = g_wcslen(n);
	if (!nl)
		CRET(c, h);
	for (; LD2(h); h += 2)
		if (!memcmp(GP(h), GP(n), 2 * nl))
			CRET(c, h);
	CRET(c, 0);
}
HLE(_wcsupr)
{
	for (u32 s = ARG(0); LD2(s); s += 2)
	{
		u16 v = LD2(s);
		if (v >= 'a' && v <= 'z')
			ST2(s, (u16)(v - 32));
	}
	CRET(c, ARG(0));
}
HLE(_wtoi)
{
	char tmp[64];
	g_wstr_utf8(ARG(0), tmp, sizeof tmp);
	CRET(c, (u32)atoi(tmp));
}
HLE(wcstoul)
{
	char tmp[128];
	g_wstr_utf8(ARG(0), tmp, sizeof tmp);
	char* end;
	unsigned long v = strtoul(tmp, &end, (int)ARG(2));
	if (ARG(1))
		ST4(ARG(1), ARG(0) + 2 * (u32)(end - tmp));
	CRET(c, (u32)v);
}
HLE(_itow)
{
	char tmp[40];
	int radix = (int)ARG(2);
	s32 v = (s32)ARG(0);
	if (radix == 10)
		snprintf(tmp, sizeof tmp, "%d", v);
	else if (radix == 16)
		snprintf(tmp, sizeof tmp, "%x", (u32)v);
	else
		snprintf(tmp, sizeof tmp, "%d", v);
	g_put_wstr(ARG(1), tmp, 40);
	CRET(c, ARG(1));
}
HLE(wcstombs)
{
	u32 d = ARG(0), s = ARG(1), n = ARG(2), i = 0;
	for (; i < n; i++)
	{
		u16 ch = LD2(s + 2 * i);
		if (d)
			ST1(d + i, ch < 256 ? (u8)ch : '?');
		if (!ch)
			break;
	}
	CRET(c, i);
}
HLE(swscanf)
{
	/* Only the patterns the game uses with numbers: %i/%d/%f/%x via host sscanf. */
	char in[1024], fmt[256];
	g_wstr_utf8(ARG(0), in, sizeof in);
	g_wstr_utf8(ARG(1), fmt, sizeof fmt);
	void* out[8] = { 0 };
	for (int k = 0; k < 8; k++)
		out[k] = GP(ARG(2 + k));
	int r = sscanf(in, fmt, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
	CRET(c, (u32)r);
}
HLE(sscanf)
{
	void* out[8] = { 0 };
	for (int k = 0; k < 8; k++)
		out[k] = GP(ARG(2 + k));
	int r = sscanf(g_str(ARG(0)), g_str(ARG(1)), out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
	CRET(c, (u32)r);
}

/*-----------------------------------------------------------------------------
	Math (doubles on the stack; results in ST0).
-----------------------------------------------------------------------------*/

#define ARGD(i) asf64(((u64)ARG((i) + 1) << 32) | ARG(i))
HLE(sin) { CRETF(c, sin(ARGD(0))); }
HLE(cos) { CRETF(c, cos(ARGD(0))); }
HLE(acos) { CRETF(c, acos(ARGD(0))); }
HLE(atan) { CRETF(c, atan(ARGD(0))); }
HLE(exp) { CRETF(c, exp(ARGD(0))); }
HLE(log) { CRETF(c, log(ARGD(0))); }
HLE(sqrt) { CRETF(c, sqrt(ARGD(0))); }
HLE(fabs) { CRETF(c, fabs(ARGD(0))); }
HLE(floor) { CRETF(c, floor(ARGD(0))); }
HLE(ceil) { CRETF(c, ceil(ARGD(0))); }
HLE(pow) { CRETF(c, pow(ARGD(0), ARGD(2))); }
HLE(ldexp) { CRETF(c, ldexp(ARGD(0), (int)ARG(2))); }
HLE(_finite) { CRET(c, isfinite(ARGD(0)) != 0); }
HLE(_isnan) { CRET(c, isnan(ARGD(0)) != 0); }
/* _CI* intrinsics: operands in ST0 (and ST1), result replaces them in ST0. */
HLE(_CIpow) { f80 y = R10(REG_ST0); x87_pop(c); R10(REG_ST0) = pow(R10(REG_ST0), y); CRET(c, R4(REG_EAX)); }
HLE(_CIfmod) { f80 y = R10(REG_ST0); x87_pop(c); R10(REG_ST0) = fmod(R10(REG_ST0), y); CRET(c, R4(REG_EAX)); }
HLE(_CIacos) { R10(REG_ST0) = acos(R10(REG_ST0)); CRET(c, R4(REG_EAX)); }
HLE(_CIasin) { R10(REG_ST0) = asin(R10(REG_ST0)); CRET(c, R4(REG_EAX)); }
/* _ftol: ST0 -> EDX:EAX (truncate), pops ST0. */
HLE(_ftol)
{
	double d = R10(REG_ST0);
	x87_pop(c);
	HLE_RET64(c, (u64)(s64)d, 0);
}
HLE(_controlfp) { CRET(c, 0x0009001f); }
static u32 RandState = 1;
HLE(rand) { RandState = RandState * 214013u + 2531011u; CRET(c, (RandState >> 16) & 0x7fff); }
HLE(srand) { RandState = ARG(0); CRET(c, 0); }
HLE(_adjust_fdiv) { CRET(c, 0); }

/*-----------------------------------------------------------------------------
	Files (stdio). Guest FILE* points at a 32-byte guest struct whose first
	word indexes the host FILE table.
-----------------------------------------------------------------------------*/

static FILE* HostFiles[512];
static u32 Iob;

static u32 iob(void)
{
	if (!Iob)
	{
		Iob = hle_static_alloc(32 * 20);
		HostFiles[1] = stdin;
		HostFiles[2] = stdout;
		HostFiles[3] = stderr;
		for (int i = 0; i < 3; i++)
			ST4(Iob + 32 * i, (u32)i + 1);
	}
	return Iob;
}

static FILE* guest_file(u32 f)
{
	if (!f)
		return NULL;
	u32 i = LD4(f);
	return i < 512 ? HostFiles[i] : NULL;
}

static u32 wrap_file(FILE* f)
{
	if (!f)
		return 0;
	for (u32 i = 4; i < 512; i++)
		if (!HostFiles[i])
		{
			HostFiles[i] = f;
			u32 g = rt_heap_alloc(32);
			memset(GP(g), 0, 32);
			ST4(g, i);
			return g;
		}
	return 0;
}

static void fix_mode(const char* m, char* out)
{
	snprintf(out, 8, "%s", m);
	char* t = strchr(out, 't');
	if (t)
		*t = 'b';
}

HLE(fopen)
{
	char host[2048], mode[8];
	hle_host_path(g_str(ARG(0)), host, sizeof host);
	fix_mode(g_str(ARG(1)), mode);
	CRET(c, wrap_file(fopen(host, mode)));
}
HLE(_wfopen)
{
	char host[2048], mode[16];
	hle_host_wpath(ARG(0), host, sizeof host);
	g_wstr_utf8(ARG(1), mode, sizeof mode);
	char m2[8];
	fix_mode(mode, m2);
	CRET(c, wrap_file(fopen(host, m2)));
}
HLE(freopen) { CRET(c, ARG(2)); }
HLE(fclose)
{
	FILE* f = guest_file(ARG(0));
	if (f && f != stdout && f != stderr && f != stdin)
	{
		fclose(f);
		HostFiles[LD4(ARG(0))] = NULL;
		rt_heap_free(ARG(0));
	}
	CRET(c, 0);
}
HLE(fread) { FILE* f = guest_file(ARG(3)); CRET(c, f ? (u32)fread(GP(ARG(0)), ARG(1), ARG(2), f) : 0); }
HLE(fwrite) { FILE* f = guest_file(ARG(3)); CRET(c, f ? (u32)fwrite(GP(ARG(0)), ARG(1), ARG(2), f) : 0); }
HLE(fseek) { FILE* f = guest_file(ARG(0)); CRET(c, f ? (u32)fseek(f, (s32)ARG(1), (int)ARG(2)) : 0xffffffffu); }
HLE(ftell) { FILE* f = guest_file(ARG(0)); CRET(c, f ? (u32)ftell(f) : 0xffffffffu); }
HLE(fgetc) { FILE* f = guest_file(ARG(0)); CRET(c, f ? (u32)fgetc(f) : 0xffffffffu); }
HLE(fgets)
{
	FILE* f = guest_file(ARG(2));
	CRET(c, f && fgets((char*)GP(ARG(0)), (int)ARG(1), f) ? ARG(0) : 0);
}
HLE(fputs) { FILE* f = guest_file(ARG(1)); if (f) fputs(g_str(ARG(0)), f); CRET(c, 0); }
HLE(fprintf)
{
	FILE* f = guest_file(ARG(0));
	if (f)
		host_print(f, 0, ARG(1), R4(REG_ESP) + 12);
	CRET(c, 0);
}

/* Low-level I/O. */
HLE(_open)
{
	char host[2048];
	hle_host_path(g_str(ARG(0)), host, sizeof host);
	u32 fl = ARG(1);
	int flags = (fl & 3) == 1 ? O_WRONLY : (fl & 3) == 2 ? O_RDWR : O_RDONLY;
	if (fl & 0x100) flags |= O_CREAT;
	if (fl & 0x200) flags |= O_TRUNC;
	if (fl & 0x8) flags |= O_APPEND;
	CRET(c, (u32)open(host, flags, 0644));
}
HLE(_close) { CRET(c, (u32)close((int)ARG(0))); }
HLE(_read) { CRET(c, (u32)read((int)ARG(0), GP(ARG(1)), ARG(2))); }
HLE(_write) { CRET(c, (u32)write((int)ARG(0), GP(ARG(1)), ARG(2))); }
HLE(_lseek) { CRET(c, (u32)lseek((int)ARG(0), (s32)ARG(1), (int)ARG(2))); }

static void fill_stat(const struct stat* st, u32 out)
{
	/* struct _stat (32-bit time): dev, ino(2), mode(2), nlink(2), uid(2), gid(2), rdev, size, atime, mtime, ctime */
	memset(GP(out), 0, 36);
	ST2(out + 6, (u16)(S_ISDIR(st->st_mode) ? 0x4000 | 0755 : 0x8000 | 0644));
	ST2(out + 8, 1);
	ST4(out + 20, (u32)st->st_size);
	ST4(out + 24, (u32)st->st_atime);
	ST4(out + 28, (u32)st->st_mtime);
	ST4(out + 32, (u32)st->st_ctime);
}
HLE(_stat)
{
	char host[2048];
	hle_host_path(g_str(ARG(0)), host, sizeof host);
	struct stat st;
	if (stat(host, &st))
		CRET(c, 0xffffffffu);
	fill_stat(&st, ARG(1));
	CRET(c, 0);
}
HLE(_wstat)
{
	char host[2048];
	hle_host_wpath(ARG(0), host, sizeof host);
	struct stat st;
	if (stat(host, &st))
		CRET(c, 0xffffffffu);
	fill_stat(&st, ARG(1));
	CRET(c, 0);
}

/*-----------------------------------------------------------------------------
	Time.
-----------------------------------------------------------------------------*/

HLE(time)
{
	u32 t = (u32)time(NULL);
	if (ARG(0))
		ST4(ARG(0), t);
	CRET(c, t);
}
HLE(clock) { CRET(c, (u32)(clock() / (CLOCKS_PER_SEC / 1000))); }
HLE(difftime) { CRETF(c, (double)(s32)ARG(0) - (double)(s32)ARG(1)); }
static u32 TmBuf;
static u32 put_tm(const struct tm* t)
{
	if (!TmBuf)
		TmBuf = hle_static_alloc(36);
	int f[9] = { t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst };
	for (int i = 0; i < 9; i++)
		ST4(TmBuf + 4 * i, (u32)f[i]);
	return TmBuf;
}
HLE(localtime) { time_t t = (time_t)(s32)LD4(ARG(0)); struct tm tm; localtime_r(&t, &tm); CRET(c, put_tm(&tm)); }
HLE(gmtime) { time_t t = (time_t)(s32)LD4(ARG(0)); struct tm tm; gmtime_r(&t, &tm); CRET(c, put_tm(&tm)); }
HLE(mktime)
{
	struct tm t = { 0 };
	u32 g = ARG(0);
	t.tm_sec = (int)LD4(g); t.tm_min = (int)LD4(g + 4); t.tm_hour = (int)LD4(g + 8);
	t.tm_mday = (int)LD4(g + 12); t.tm_mon = (int)LD4(g + 16); t.tm_year = (int)LD4(g + 20);
	t.tm_isdst = -1;
	CRET(c, (u32)mktime(&t));
}
static void now_fmt(const char* f, char* out, size_t n)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, f, &tm);
}
HLE(_strdate) { char b[16]; now_fmt("%m/%d/%y", b, sizeof b); strcpy((char*)GP(ARG(0)), b); CRET(c, ARG(0)); }
HLE(_strtime) { char b[16]; now_fmt("%H:%M:%S", b, sizeof b); strcpy((char*)GP(ARG(0)), b); CRET(c, ARG(0)); }
HLE(_wstrdate) { char b[16]; now_fmt("%m/%d/%y", b, sizeof b); g_put_wstr(ARG(0), b, 16); CRET(c, ARG(0)); }
HLE(_wstrtime) { char b[16]; now_fmt("%H:%M:%S", b, sizeof b); g_put_wstr(ARG(0), b, 16); CRET(c, ARG(0)); }

/*-----------------------------------------------------------------------------
	Startup, exit, misc.
-----------------------------------------------------------------------------*/

/* _initterm(begin, end): call each non-null function pointer. */
HLE(_initterm)
{
	u32 b = ARG(0), e = ARG(1);
	u32 esp = R4(REG_ESP);
	for (u32 p = b; p < e; p += 4)
	{
		u32 fn = LD4(p);
		if (fn)
			rt_call_guest(c, fn, 0, NULL, 0);
	}
	R4(REG_ESP) = esp;
	CRET(c, 0);
}

static u32 AtExit[512];
static int NumAtExit;
static void add_atexit(u32 fn) { if (NumAtExit < 512) AtExit[NumAtExit++] = fn; }
HLE(_onexit) { add_atexit(ARG(0)); CRET(c, ARG(0)); }
HLE(__dllonexit) { add_atexit(ARG(0)); CRET(c, ARG(0)); }
void hle_run_atexit(Ctx* c)
{
	while (NumAtExit > 0)
		rt_call_guest(c, AtExit[--NumAtExit], 0, NULL, 0);
}
HLE(exit) { fprintf(stderr, "[msvcrt] exit(%d)\n", (int)ARG(0)); exit((int)ARG(0)); }
HLE(_exit) { exit((int)ARG(0)); }
HLE(_c_exit) { CRET(c, 0); }
HLE(_cexit) { CRET(c, 0); }
HLE(abort) { recomp_fail(c, 0, "abort() called by the game"); }
HLE(_amsg_exit) { recomp_fail(c, ARG(0), "_amsg_exit (CRT runtime error)"); }
HLE(_purecall) { recomp_fail(c, 0, "pure virtual function call"); }
HLE(terminate) { recomp_fail(c, 0, "terminate()"); }

extern u32 hle_command_line_a(void);
static u32 AcmdlnVar, ErrnoVar, MbCurMax, FmodeVar, CommodeVar, Pctype;
HLE(_errno) { if (!ErrnoVar) ErrnoVar = hle_static_alloc(4); CRET(c, ErrnoVar); }
HLE(__p__fmode) { if (!FmodeVar) FmodeVar = hle_static_alloc(4); CRET(c, FmodeVar); }
HLE(__p__commode) { if (!CommodeVar) CommodeVar = hle_static_alloc(4); CRET(c, CommodeVar); }
HLE(__set_app_type) { CRET(c, 0); }
HLE(__setusermatherr) { CRET(c, 0); }
HLE(__getmainargs)
{
	/* (int* argc, char*** argv, char*** env, int wildcard, startinfo*) */
	u32 argv = hle_static_alloc(8), env = hle_static_alloc(4);
	ST4(argv, hle_command_line_a());
	ST4(argv + 4, 0);
	ST4(env, 0);
	ST4(ARG(0), 1);
	ST4(ARG(1), argv);
	ST4(ARG(2), env);
	CRET(c, 0);
}
HLE(setlocale)
{
	static u32 C;
	if (!C) { C = hle_static_alloc(2); ST1(C, 'C'); }
	CRET(c, C);
}
HLE(qsort)
{
	/* Insertion sort calling the guest comparator (cdecl cmp(a, b)). */
	u32 base = ARG(0), n = ARG(1), w = ARG(2), cmp = ARG(3);
	u32 tmp = rt_heap_alloc(w);
	for (u32 i = 1; i < n; i++)
	{
		memcpy(GP(tmp), GP(base + i * w), w);
		s32 j = (s32)i - 1;
		while (j >= 0)
		{
			u32 args[2] = { base + (u32)j * w, tmp };
			if ((s32)rt_call_guest(c, cmp, 2, args, 0) <= 0)
				break;
			memcpy(GP(base + (u32)(j + 1) * w), GP(base + (u32)j * w), w);
			j--;
		}
		memcpy(GP(base + (u32)(j + 1) * w), GP(tmp), w);
	}
	rt_heap_free(tmp);
	CRET(c, 0);
}

/* exception / type_info objects: vptr + what-string pointer. */
HLE(exception_ctor) { u32 t = R4(REG_ECX); ST4(t, 0); ST4(t + 4, 0); HLE_RET(c, t, 0); }
HLE(exception_copy) { u32 t = R4(REG_ECX); ST4(t, LD4(ARG(0))); ST4(t + 4, LD4(ARG(0) + 4)); HLE_RET(c, t, 4); }
HLE(thiscall_nop) { HLE_RET(c, 0, 0); }

/* Exported data symbols are reached through the IAT as addresses. They are
   resolved here to static guest storage by name (see hle_data_address). */
u32 hle_data_address(const char* name)
{
	if (!strcmp(name, "_iob")) return iob();
	if (!strcmp(name, "_acmdln")) { if (!AcmdlnVar) { AcmdlnVar = hle_static_alloc(4); ST4(AcmdlnVar, hle_command_line_a()); } return AcmdlnVar; }
	if (!strcmp(name, "__mb_cur_max")) { if (!MbCurMax) { MbCurMax = hle_static_alloc(4); ST4(MbCurMax, 1); } return MbCurMax; }
	if (!strcmp(name, "_pctype")) { if (!Pctype) { Pctype = hle_static_alloc(4 + 2 * 257); ST4(Pctype, Pctype + 4 + 2); } return Pctype; }
	if (!strcmp(name, "_adjust_fdiv")) return hle_static_alloc(4);
	return 0;
}

const HleEntry hle_msvcrt[] = {
	ENTRY(sprintf), ENTRY(swprintf), ENTRY(vsprintf), ENTRY(_snprintf), ENTRY(_vsnprintf), ENTRY(_vsnwprintf),
	ENTRY(printf), ENTRY(malloc), ENTRY(calloc), ENTRY(realloc), ENTRY(free), ENTRY(memcpy), ENTRY(memmove),
	ENTRY(memchr), ENTRY(strlen), ENTRY(strchr), ENTRY(strrchr), ENTRY(strstr), ENTRY(strncmp), ENTRY(_stricmp),
	ENTRY(strncpy), ENTRY(strncat), ENTRY(_strdup), ENTRY(_strlwr), ENTRY(strtok), ENTRY(atoi), ENTRY(strtol),
	ENTRY(atof), ENTRY(strtod), ENTRY(toupper), ENTRY(isalnum), ENTRY(isalpha), ENTRY(isdigit), ENTRY(isspace),
	ENTRY(iswspace), ENTRY(_isctype), ENTRY(wcslen), ENTRY(wcscpy), ENTRY(wcsncpy), ENTRY(wcscat), ENTRY(wcscmp),
	ENTRY(wcsncmp), ENTRY(_wcsicmp), ENTRY(_wcsnicmp), ENTRY(wcschr), ENTRY(wcsstr), ENTRY(_wcsupr), ENTRY(_wtoi),
	ENTRY(wcstoul), ENTRY(_itow), ENTRY(wcstombs), ENTRY(swscanf), ENTRY(sscanf), ENTRY(sin), ENTRY(cos),
	ENTRY(acos), ENTRY(atan), ENTRY(exp), ENTRY(log), ENTRY(sqrt), ENTRY(fabs), ENTRY(floor), ENTRY(ceil),
	ENTRY(pow), ENTRY(ldexp), ENTRY(_finite), ENTRY(_isnan), ENTRY(_CIpow), ENTRY(_CIfmod), ENTRY(_CIacos),
	ENTRY(_CIasin), ENTRY(_ftol), ENTRY(_controlfp), ENTRY(rand), ENTRY(srand), ENTRY(fopen), ENTRY(_wfopen),
	ENTRY(freopen), ENTRY(fclose), ENTRY(fread), ENTRY(fwrite), ENTRY(fseek), ENTRY(ftell), ENTRY(fgetc),
	ENTRY(fgets), ENTRY(fputs), ENTRY(fprintf), ENTRY(_open), ENTRY(_close), ENTRY(_read), ENTRY(_write),
	ENTRY(_lseek), ENTRY(_stat), ENTRY(_wstat), ENTRY(time), ENTRY(clock), ENTRY(difftime), ENTRY(localtime),
	ENTRY(gmtime), ENTRY(mktime), ENTRY(_strdate), ENTRY(_strtime), ENTRY(_wstrdate), ENTRY(_wstrtime),
	ENTRY(_initterm), ENTRY(_onexit), ENTRY(__dllonexit), ENTRY(exit), ENTRY(_exit), ENTRY(_c_exit),
	ENTRY(_cexit), ENTRY(abort), ENTRY(_amsg_exit), ENTRY(_purecall), ENTRY_AS("?terminate@@YAXXZ", terminate),
	ENTRY(_errno), ENTRY(__p__fmode), ENTRY(__p__commode), ENTRY(__set_app_type), ENTRY(__setusermatherr),
	ENTRY(__getmainargs), ENTRY(setlocale), ENTRY(qsort),
	ENTRY_AS("??0exception@@QAE@XZ", exception_ctor), ENTRY_AS("??0exception@@QAE@ABV0@@Z", exception_copy),
	ENTRY_AS("??1exception@@UAE@XZ", thiscall_nop), ENTRY_AS("??1type_info@@UAE@XZ", thiscall_nop),
	{ NULL, NULL }
};
