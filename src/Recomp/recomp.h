/*=============================================================================
	recomp.h: Runtime interface for statically recompiled x86 code.

	Generated code (tools/ghidra_scripts/EmitC.java) is one C function per
	guest function, F_<guestaddr>(Ctx*). Registers live in Ctx::r, laid out
	exactly like Ghidra's x86 register space (x86regs.h), so raw P-code
	register varnodes map to fixed offsets. Guest memory is a flat 4 GB
	reservation at the fixed host address GUEST_HOST_BASE (so every guest
	access is a constant-base load); guest pointers stay 32-bit.

	x87 80-bit values are kept as host doubles (f80) in their register slots.
=============================================================================*/
#pragma once

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "x86regs.h"
#include "recomp_modules.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef __uint128_t u128;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef __int128 s128;
typedef double f80;

typedef u16 u16u __attribute__((aligned(1)));
typedef u32 u32u __attribute__((aligned(1)));
typedef u64 u64u __attribute__((aligned(1)));
typedef f80 f80u __attribute__((aligned(1)));
typedef u128 u128u __attribute__((aligned(1)));

typedef struct Ctx
{
	_Alignas(16) u8 r[REG_FILE_SIZE];
	u32 thread_id;
	u32 teb;           /* guest address of this thread's TEB (FS base) */
	void* host;        /* per-thread host runtime data */
	u32 eh_resume;     /* continuation address when an exception handler resumes a frame */
	u32 unwind_record; /* nonzero while unwinding to the frame that registered this SEH record */
	u32 unwind_esp, unwind_ebp;  /* registers for that frame's continuation */
} Ctx;

typedef void (*RecompFn)(Ctx*);
typedef struct RecompEntry { u32 addr; RecompFn fn; } RecompEntry;

#define GUEST_HOST_BASE 0x10000000000ull  /* 1 TB: macOS honours mmap hints here */
#define g_mem ((u8*)GUEST_HOST_BASE)

/* Registers. */
#define R1(o)  (*(u8*)&c->r[o])
#define R2(o)  (*(u16u*)&c->r[o])
#define R4(o)  (*(u32u*)&c->r[o])
#define R8(o)  (*(u64u*)&c->r[o])
#define R10(o) (*(f80u*)&c->r[o])
#define R16(o) (*(u128u*)&c->r[o])

/* Guest memory. */
static inline u8 LD1(u32 a) { return g_mem[a]; }
static inline u16 LD2(u32 a) { u16 v; memcpy(&v, g_mem + a, 2); return v; }
static inline u32 LD4(u32 a) { u32 v; memcpy(&v, g_mem + a, 4); return v; }
static inline u64 LD8(u32 a) { u64 v; memcpy(&v, g_mem + a, 8); return v; }
static inline u128 LD16(u32 a) { u128 v; memcpy(&v, g_mem + a, 16); return v; }
static inline void ST1(u32 a, u8 v) { g_mem[a] = v; }
static inline void ST2(u32 a, u16 v) { memcpy(g_mem + a, &v, 2); }
static inline void ST4(u32 a, u32 v) { memcpy(g_mem + a, &v, 4); }
static inline void ST8(u32 a, u64 v) { memcpy(g_mem + a, &v, 8); }
static inline void ST16(u32 a, u128 v) { memcpy(g_mem + a, &v, 16); }
f80 recomp_ld80(u32 a);           /* x87 80-bit extended -> double */
void recomp_st80(u32 a, f80 v);  /* double -> x87 80-bit extended */
#define LD10(a) recomp_ld80(a)
#define ST10(a, v) recomp_st80((a), (v))

/* Float bit casts. */
static inline float asf32(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static inline double asf64(u64 b) { double d; memcpy(&d, &b, 8); return d; }
static inline u32 bitsf32(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static inline u64 bitsf64(double d) { u64 b; memcpy(&b, &d, 8); return b; }
static inline u64 f80bits(f80 d) { return bitsf64(d); }

/* x86 float->int conversions: out-of-range gives the "integer indefinite". */
static inline u64 f2i_trunc16(double d) { return (d >= -32768.0 && d < 32768.0) ? (u64)(s64)d : 0x8000; }
static inline u64 f2i_trunc32(double d) { return (d >= -2147483648.0 && d < 2147483648.0) ? (u64)(s64)d : 0x80000000u; }
static inline u64 f2i_trunc64(double d) { return (d >= -9223372036854775808.0 && d < 9223372036854775808.0) ? (u64)(s64)d : 0x8000000000000000ull; }
static inline u64 f2i_trunc8(double d) { return (u64)(s64)d; }

/* Flags. */
#define DEF_FLAGS(N, T, ST) \
	static inline u8 carry##N(T a, T b) { return (u8)((T)(a + b) < a); } \
	static inline u8 scarry##N(T a, T b) { T r = (T)(a + b); return (u8)((((ST)(a ^ r)) & ((ST)(b ^ r))) < 0); } \
	static inline u8 sborrow##N(T a, T b) { T r = (T)(a - b); return (u8)((((ST)(a ^ b)) & ((ST)(a ^ r))) < 0); }
DEF_FLAGS(8, u8, s8)
DEF_FLAGS(16, u16, s16)
DEF_FLAGS(32, u32, s32)
DEF_FLAGS(64, u64, s64)

/* Shifts with P-code semantics (shift >= width gives 0 / sign fill). */
#define DEF_SHIFTS(N, T, ST) \
	static inline T shl##N(T a, u64 s) { return s >= N ? 0 : (T)(a << s); } \
	static inline T shr##N(T a, u64 s) { return s >= N ? 0 : (T)(a >> s); } \
	static inline T sar##N(T a, u64 s) { return (T)((ST)a >> (s >= N ? N - 1 : s)); }
DEF_SHIFTS(8, u8, s8)
DEF_SHIFTS(16, u16, s16)
DEF_SHIFTS(32, u32, s32)
DEF_SHIFTS(64, u64, s64)
DEF_SHIFTS(128, u128, s128)

/* Division (x86 #DE on zero is reported, not emulated). */
void recomp_divide_error(void);
#define DEF_DIV(N, T, ST) \
	static inline T udiv##N(T a, T b) { if (!b) recomp_divide_error(); return b ? a / b : 0; } \
	static inline T urem##N(T a, T b) { if (!b) recomp_divide_error(); return b ? a % b : 0; } \
	static inline T sdiv##N(T a, T b) { if (!b) recomp_divide_error(); return b ? (T)((ST)a / (ST)b) : 0; } \
	static inline T srem##N(T a, T b) { if (!b) recomp_divide_error(); return b ? (T)((ST)a % (ST)b) : 0; }
DEF_DIV(8, u8, s8)
DEF_DIV(16, u16, s16)
DEF_DIV(32, u32, s32)
DEF_DIV(64, u64, s64)

static inline u64 popcnt(u64 v) { return (u64)__builtin_popcountll(v); }
static inline u64 lzcnt32(u64 v) { return (u32)v ? (u64)__builtin_clz((u32)v) : 32; }
static inline u64 lzcnt16(u64 v) { return (u16)v ? (u64)__builtin_clz((u32)(u16)v) - 16 : 16; }

/* Control flow. */
void recomp_call(Ctx* c, u32 target);
void recomp_tailcall(Ctx* c, u32 target);
void recomp_fail(Ctx* c, u32 where, const char* what);
/* After every call: if an exception is unwinding, return, unless this
   function registered the catching record (then resume at its catch). */
#define UNWIND_CHECK() \
	do { if (__builtin_expect(c->unwind_record != 0, 0)) { if (c->unwind_record == eh_rec) goto eh_catch; return; } } while (0)

/* User-defined P-code ops (CALLOTHER), implemented in uops.c. */
#include "uops.h"
