/*=============================================================================
	uops.c: P-code user-defined ops (CALLOTHER) used by the game's code.

	Float ops take and return double bit patterns (x87 values are doubles).
	The CPU reported through CPUID is a GenuineIntel family-6 part with
	MMX/SSE/SSE2 and no 3DNow!, so the 3DNow! paths stay unused.
=============================================================================*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach_time.h>

#include "recomp.h"
#include "runtime.h"

#define ARGS1 va_list ap; va_start(ap, c); u64 a0 = va_arg(ap, u64)
#define ARGS2 ARGS1; u64 a1 = va_arg(ap, u64)
#define ARGS4 ARGS2; u64 a2 = va_arg(ap, u64); u64 a3 = va_arg(ap, u64)
static inline u128 mk128(u64 lo, u64 hi) { return ((u128)hi << 64) | lo; }
static inline float lane(u64 lo, u64 hi, int i) { return asf32((u32)((i < 2 ? lo : hi) >> (32 * (i & 1)))); }

/* CPUID: results go to per-thread scratch memory as EAX, EBX, EDX, ECX. */
static u64 cpuid_common(Ctx* c, u32 leaf)
{
	u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
	switch (leaf)
	{
	case 0:
		eax = 2;
		ebx = 0x756e6547; edx = 0x49656e69; ecx = 0x6c65746e;  /* "GenuineIntel" */
		break;
	case 1:
		eax = 0x00000686;                                      /* family 6, model 8 */
		edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26);
		break;
	case 0x80000000u:
		eax = 0x80000004u;
		break;
	case 0x80000001u:
		break;                                                 /* no 3DNow!, no extensions */
	case 0x80000002u: memcpy(&eax, "Shar", 4); memcpy(&ebx, "k Ta", 4); memcpy(&ecx, "le R", 4); memcpy(&edx, "ecom", 4); break;
	case 0x80000003u: memcpy(&eax, "p CP", 4); memcpy(&ebx, "U   ", 4); break;
	default:
		break;
	}
	u32 out = GUEST_SCRATCH + c->thread_id * 0x100;
	ST4(out + 0, eax);
	ST4(out + 4, ebx);
	ST4(out + 8, edx);
	ST4(out + 12, ecx);
	return out;
}

#define CPUID_OP(name) u128 uop_##name(Ctx* c, ...) { ARGS1; va_end(ap); return cpuid_common(c, (u32)a0); }
CPUID_OP(cpuid)
CPUID_OP(cpuid_basic_info)
CPUID_OP(cpuid_Version_info)
CPUID_OP(cpuid_cache_tlb_info)
CPUID_OP(cpuid_serial_info)
CPUID_OP(cpuid_Deterministic_Cache_Parameters_info)
CPUID_OP(cpuid_MONITOR_MWAIT_Features_info)
CPUID_OP(cpuid_Thermal_Power_Management_info)
CPUID_OP(cpuid_Extended_Feature_Enumeration_info)
CPUID_OP(cpuid_Direct_Cache_Access_info)
CPUID_OP(cpuid_Architectural_Performance_Monitoring_info)
CPUID_OP(cpuid_Extended_Topology_info)
CPUID_OP(cpuid_Processor_Extended_States_info)
CPUID_OP(cpuid_Quality_of_Service_info)
CPUID_OP(cpuid_brand_part1_info)
CPUID_OP(cpuid_brand_part2_info)
CPUID_OP(cpuid_brand_part3_info)

/* RDTSC: a monotonic counter (UE2 calibrates GSecondsPerCycle against it). */
u128 uop_rdtsc(Ctx* c, ...)
{
	return mach_absolute_time();
}

/* x87 transcendentals (operands/results are double bit patterns). */
u128 uop_fsin(Ctx* c, ...) { ARGS1; va_end(ap); return bitsf64(sin(asf64(a0))); }
u128 uop_fcos(Ctx* c, ...) { ARGS1; va_end(ap); return bitsf64(cos(asf64(a0))); }
u128 uop_fptan(Ctx* c, ...) { ARGS1; va_end(ap); return bitsf64(tan(asf64(a0))); }
u128 uop_fpatan(Ctx* c, ...) { ARGS2; va_end(ap); return bitsf64(atan2(asf64(a0), asf64(a1))); }
u128 uop_f2xm1(Ctx* c, ...) { ARGS1; va_end(ap); return bitsf64(exp2(asf64(a0)) - 1.0); }
u128 uop_fscale(Ctx* c, ...) { ARGS2; va_end(ap); return bitsf64(ldexp(asf64(a0), (int)trunc(asf64(a1)))); }
u128 uop_log2(Ctx* c, ...) { ARGS1; va_end(ap); return bitsf64(log2(asf64(a0))); }
u128 uop_ffree(Ctx* c, ...) { return 0; }

/* SSE reciprocal estimates (exact values are within the hardware tolerance). */
u128 uop_rsqrtss(Ctx* c, ...) { ARGS4; va_end(ap); return mk128((a0 & ~0xffffffffull) | bitsf32(1.0f / sqrtf(asf32((u32)a2))), a1); }
u128 uop_rcpss(Ctx* c, ...) { ARGS4; va_end(ap); return mk128((a0 & ~0xffffffffull) | bitsf32(1.0f / asf32((u32)a2)), a1); }

/* Anything else reaching here is unexpected at runtime: report it clearly. */
#define UNSUPPORTED(name) u128 uop_##name(Ctx* c, ...) { recomp_fail(c, R4(REG_EIP), "unsupported instruction: " #name); return 0; }
UNSUPPORTED(swi)
/* 3DNow!: unreachable because CPUID reports no 3DNow!. */
UNSUPPORTED(FastExitMediaState)
UNSUPPORTED(FloatingReciprocalAprox)
UNSUPPORTED(PackedFloatingADD)
UNSUPPORTED(PackedFloatingAccumulate)
UNSUPPORTED(PackedFloatingCompareEQ)
UNSUPPORTED(PackedFloatingCompareGE)
UNSUPPORTED(PackedFloatingCompareGT)
UNSUPPORTED(PackedFloatingMIN)
UNSUPPORTED(PackedFloatingMUL)
UNSUPPORTED(PackedFloatingNegAccumulate)
UNSUPPORTED(PackedFloatingPosNegAccumulate)
UNSUPPORTED(PackedFloatingReciprocalIter1)
UNSUPPORTED(PackedFloatingReciprocalIter2)
UNSUPPORTED(PackedFloatingReciprocalSQRAprox)
UNSUPPORTED(PackedFloatingReciprocalSQRIter1)
UNSUPPORTED(PackedFloatingSUB)
UNSUPPORTED(PackedFloatingSUBR)
UNSUPPORTED(PackedFloatingToIntDwordConv)
UNSUPPORTED(PackedIntToFloatingDwordConv)
UNSUPPORTED(PackedSwapDWords)
UNSUPPORTED(PackedFloatingMAX)

/* SSE packed (dst lo, dst hi, src lo, src hi). */
u128 uop_rcpps(Ctx* c, ...)
{
	ARGS4;
	va_end(ap);
	u32 r[4];
	for (int i = 0; i < 4; i++)
		r[i] = bitsf32(1.0f / lane(a2, a3, i));
	return mk128(r[0] | ((u64)r[1] << 32), r[2] | ((u64)r[3] << 32));
}
u128 uop_rsqrtps(Ctx* c, ...)
{
	ARGS4;
	va_end(ap);
	u32 r[4];
	for (int i = 0; i < 4; i++)
		r[i] = bitsf32(1.0f / sqrtf(lane(a2, a3, i)));
	return mk128(r[0] | ((u64)r[1] << 32), r[2] | ((u64)r[3] << 32));
}
/* MOVMSKPS (dst, src lo, src hi): sign bits of the four floats. */
u128 uop_movmskps(Ctx* c, ...)
{
	va_list ap;
	va_start(ap, c);
	(void)va_arg(ap, u64);
	u64 lo = va_arg(ap, u64), hi = va_arg(ap, u64);
	va_end(ap);
	return ((lo >> 31) & 1) | ((lo >> 62) & 2) | (((hi >> 31) & 1) << 2) | (((hi >> 63) & 1) << 3);
}

/* MMX (dst, src), 64-bit. */
static inline s16 w16(u64 v, int i) { return (s16)(v >> (16 * i)); }
static inline s32 d32(u64 v, int i) { return (s32)(v >> (32 * i)); }
static inline s16 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
static inline s8 sat8(s32 v) { return v > 127 ? 127 : v < -128 ? -128 : (s8)v; }
u128 uop_pmaddwd(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	s32 lo = w16(a0, 0) * w16(a1, 0) + w16(a0, 1) * w16(a1, 1);
	s32 hi = w16(a0, 2) * w16(a1, 2) + w16(a0, 3) * w16(a1, 3);
	return (u32)lo | ((u64)(u32)hi << 32);
}
u128 uop_packssdw(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	u16 r[4] = { (u16)sat16(d32(a0, 0)), (u16)sat16(d32(a0, 1)), (u16)sat16(d32(a1, 0)), (u16)sat16(d32(a1, 1)) };
	return r[0] | ((u64)r[1] << 16) | ((u64)r[2] << 32) | ((u64)r[3] << 48);
}
u128 uop_packsswb(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	u64 r = 0;
	for (int i = 0; i < 4; i++)
		r |= (u64)(u8)sat8(w16(a0, i)) << (8 * i);
	for (int i = 0; i < 4; i++)
		r |= (u64)(u8)sat8(w16(a1, i)) << (8 * (i + 4));
	return r;
}
u128 uop_psubsw(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	u64 r = 0;
	for (int i = 0; i < 4; i++)
		r |= (u64)(u16)sat16((s32)w16(a0, i) - w16(a1, i)) << (16 * i);
	return r;
}
u128 uop_psllw(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	if (a1 > 15)
		return 0;
	u64 r = 0;
	for (int i = 0; i < 4; i++)
		r |= (u64)(u16)((u16)w16(a0, i) << a1) << (16 * i);
	return r;
}
u128 uop_psraw(Ctx* c, ...)
{
	ARGS2;
	va_end(ap);
	int s = a1 > 15 ? 15 : (int)a1;
	u64 r = 0;
	for (int i = 0; i < 4; i++)
		r |= (u64)(u16)(w16(a0, i) >> s) << (16 * i);
	return r;
}
