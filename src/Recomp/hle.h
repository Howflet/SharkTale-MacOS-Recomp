/*=============================================================================
	hle.h: Shared declarations for native API implementations.
=============================================================================*/
#pragma once

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "recomp.h"
#include "runtime.h"

typedef struct HleEntry { const char* name; HleFn fn; } HleEntry;

/* Define an HLE function body: HLE(Name) { ... HLE_RET(c, value, argbytes); } */
#define HLE(name) static void hle_##name(Ctx* c)
#define ENTRY(name) { #name, hle_##name }
#define ENTRY_AS(export_name, fn) { export_name, hle_##fn }

/* stdcall functions that just succeed/fail. */
#define HLE_CONST(name, value, argbytes) HLE(name) { HLE_RET(c, value, argbytes); }

u32 hle_static_alloc(u32 size);
const char* hle_slot_name(u32 addr);

u32 g_wcslen(u32 s);
char* g_wstr_utf8(u32 s, char* out, size_t cap);
u32 g_put_wstr(u32 d, const char* s, u32 cap);
static inline const char* g_str(u32 s) { return s ? (const char*)GP(s) : ""; }

void x87_pop(Ctx* c);
void x87_push(Ctx* c, f80 v);

/* Paths: guest (Windows) path -> host path. Relative paths resolve against the
   guest current directory (initially the game's System directory). */
void hle_host_path(const char* guest, char* out, size_t cap);
void hle_host_wpath(u32 gwpath, char* out, size_t cap);
extern char g_guest_cwd[1024];  /* Windows-style, e.g. C:\Shark Tale\System */

/* Win32 error code for GetLastError. */
void hle_set_last_error(u32 e);

/* Formatting engine (printf family) over guest memory. */
typedef struct FmtOut { u32 dst; u32 cap; u32 len; int wide; char* host; } FmtOut;
u32 hle_format(FmtOut* out, u32 fmt, int wide_fmt, u32 argp);

/* Handles (files, find, mappings, threads, events, modules, windows). */
u32 hle_handle_new(int kind, void* host);
void* hle_handle_get(u32 h, int kind);
void hle_handle_close(u32 h);
enum { H_FILE = 1, H_FIND, H_MAPPING, H_THREAD, H_EVENT, H_MUTEX, H_PROCESS };
