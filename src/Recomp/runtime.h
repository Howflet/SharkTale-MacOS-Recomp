/*=============================================================================
	runtime.h: Host-side runtime API (loader, dispatch, threads, HLE glue).
=============================================================================*/
#pragma once

#include "recomp.h"

/* Guest address space layout. */
#define GUEST_HEAP_BASE   0x20000000u
#define GUEST_HEAP_END    0x6f000000u
#define GUEST_STACK_BASE  0x70000000u   /* per-thread stacks grow down from here + n*size */
#define GUEST_STACK_SIZE  0x00400000u
#define GUEST_TEB_BASE    0x7e000000u   /* one 4 KB TEB per thread */
#define GUEST_SCRATCH     0x7e800000u   /* per-thread scratch (cpuid results etc.) */
#define GUEST_HLE_BASE    0x7f000000u   /* HLE thunk addresses: base + 16 * index */
#define GUEST_HLE_DATA    0x7f800000u   /* static data owned by HLE (strings, objects) */

void rt_mem_init(void);
void rt_register(u32 addr, RecompFn fn);
void rt_register_table(const RecompEntry* e, unsigned n);
RecompFn rt_lookup(u32 addr);
unsigned rt_dispatch_count(void);
void rt_dump_guest_stack(Ctx* c);

/* Guest heap (simple allocator over GUEST_HEAP_*). */
u32 rt_heap_alloc(u32 size);
u32 rt_pages_alloc(u32 size);   /* VirtualAlloc: 64 KB granules */
int rt_pages_free(u32 addr);
void rt_heap_free(u32 addr);
u32 rt_heap_size(u32 addr);
u32 rt_heap_realloc(u32 addr, u32 size);

/* Guest pointer helpers. */
static inline void* GP(u32 a) { return a ? (void*)(g_mem + a) : NULL; }
static inline u32 HP(const void* p) { return p ? (u32)((const u8*)p - g_mem) : 0; }

/* Stack argument access for HLE functions: at entry ESP -> return address. */
#define ARG(i) LD4(R4(REG_ESP) + 4 + 4 * (i))
#define ARGF(i) asf32(ARG(i))
/* Return from an HLE function: stdcall pops n argument bytes, cdecl pops none. */
#define HLE_RET(c, v, nbytes) do { R4(REG_EAX) = (u32)(v); R4(REG_ESP) += 4 + (nbytes); return; } while (0)
#define HLE_RET64(c, v, nbytes) do { u64 _v = (u64)(v); R4(REG_EAX) = (u32)_v; R4(REG_EDX) = (u32)(_v >> 32); R4(REG_ESP) += 4 + (nbytes); return; } while (0)
/* Return a float/double in ST0 (x87 convention). */
#define HLE_RETF(c, v, nbytes) do { R10(REG_ST0) = (f80)(v); R4(REG_ESP) += 4 + (nbytes); return; } while (0)

/* Calling guest code from the host (callbacks: WndProc, thread procs, atexit). */
u32 rt_call_guest(Ctx* c, u32 fn, int nargs, const u32* args, int stdcall);

/* Per-thread context. */
Ctx* rt_thread_ctx(void);
Ctx* rt_new_thread_ctx(void);

/* Loader. */
typedef struct GuestModule
{
	char name[64];        /* e.g. "Core.dll" */
	u32 base, size, entry;
	int recompiled;       /* code is recompiled (else: HLE-only) */
	int initialized;
} GuestModule;

GuestModule* rt_load_module(const char* name);       /* map + relocate + bind; NULL if unknown */
GuestModule* rt_find_module(const char* name);
GuestModule* rt_module_at(u32 addr);
u32 rt_module_export(GuestModule* m, const char* name);  /* by name; 0 if missing */
void rt_init_modules(Ctx* c);                         /* run DllMain of loaded, uninitialized modules */
extern const char* g_game_dir;                        /* host path of the game's System directory */

/* HLE registry. */
typedef void (*HleFn)(Ctx*);
u32 rt_hle_address(const char* dll, const char* name);  /* guest thunk address, creating a stub if unknown */
void rt_hle_init(void);
