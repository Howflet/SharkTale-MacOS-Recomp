/*=============================================================================
	runtime.c: Core runtime for recompiled code: guest memory, the guest
	address -> host function table, calls, and failure reporting.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "recomp.h"
#include "runtime.h"


/*-----------------------------------------------------------------------------
	Guest memory: one 4 GB reservation; pages materialize on first touch.
-----------------------------------------------------------------------------*/

void rt_mem_init(void)
{
	void* want = (void*)GUEST_HOST_BASE;
	void* p = mmap(want, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED || p != want)
	{
		fprintf(stderr, "mmap guest space at %p failed (got %p)\n", want, p);
		exit(1);
	}
}

/*-----------------------------------------------------------------------------
	Dispatch: open-addressing hash of guest entry points.
-----------------------------------------------------------------------------*/

#define DISPATCH_BITS 20
#define DISPATCH_SIZE (1u << DISPATCH_BITS)
static RecompEntry Dispatch[DISPATCH_SIZE];
static unsigned DispatchCount;

static inline u32 hash_addr(u32 a) { return (a * 2654435761u) >> (32 - DISPATCH_BITS); }

void rt_register(u32 addr, RecompFn fn)
{
	u32 h = hash_addr(addr);
	while (Dispatch[h].addr && Dispatch[h].addr != addr)
		h = (h + 1) & (DISPATCH_SIZE - 1);
	if (!Dispatch[h].addr)
		DispatchCount++;
	Dispatch[h].addr = addr;
	Dispatch[h].fn = fn;
}

void rt_register_table(const RecompEntry* e, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		rt_register(e[i].addr, e[i].fn);
}

RecompFn rt_lookup(u32 addr)
{
	u32 h = hash_addr(addr);
	while (Dispatch[h].addr)
	{
		if (Dispatch[h].addr == addr)
			return Dispatch[h].fn;
		h = (h + 1) & (DISPATCH_SIZE - 1);
	}
	return NULL;
}

unsigned rt_dispatch_count(void) { return DispatchCount; }

/* Resolve a target that has no recompiled function: incremental-link stubs
   (E9 rel32 / FF 25 [abs]) are followed through the mapped image bytes. */
static RecompFn resolve_stub(u32* target)
{
	for (int hops = 0; hops < 4; hops++)
	{
		u32 t = *target;
		if (t < 0x10000 || t >= 0xf0000000u)
			return NULL;
		u8 op = LD1(t);
		if (op == 0xe9)
			*target = t + 5 + LD4(t + 1);
		else if (op == 0xff && LD1(t + 1) == 0x25)
			*target = LD4(LD4(t + 2));
		else
			return NULL;
		RecompFn fn = rt_lookup(*target);
		if (fn)
		{
			rt_register(t, fn);  /* cache the stub */
			return fn;
		}
	}
	return NULL;
}

void recomp_call(Ctx* c, u32 target)
{
	RecompFn fn = rt_lookup(target);
	if (!fn)
	{
		u32 t = target;
		fn = resolve_stub(&t);
		if (!fn)
			recomp_fail(c, target, "call/jump to address with no recompiled function");
		target = t;
	}
	R4(REG_EIP) = target;
	fn(c);
}

void recomp_tailcall(Ctx* c, u32 target)
{
	recomp_call(c, target);
}

void recomp_fail(Ctx* c, u32 where, const char* what)
{
	fprintf(stderr, "\n[recomp] FATAL: %s at %#010x\n", what, where);
	if (c)
		fprintf(stderr, "  EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
			R4(REG_EAX), R4(REG_ECX), R4(REG_EDX), R4(REG_EBX), R4(REG_ESP), R4(REG_EBP), R4(REG_ESI), R4(REG_EDI));
	rt_dump_guest_stack(c);
	abort();
}

void recomp_divide_error(void)
{
	fprintf(stderr, "[recomp] integer divide by zero\n");
	abort();
}

void rt_dump_guest_stack(Ctx* c)
{
	if (!c)
		return;
	u32 esp = R4(REG_ESP);
	fprintf(stderr, "  guest stack (words that are known function entries are marked *):\n");
	for (int i = 0; i < 48; i++)
	{
		u32 v = LD4(esp + 4 * i);
		fprintf(stderr, "    [esp+%3d] %08x\n", 4 * i, v);
	}
}

/*-----------------------------------------------------------------------------
	x87 80-bit extended <-> double.
-----------------------------------------------------------------------------*/

f80 recomp_ld80(u32 a)
{
	u64 mant = LD8(a);
	u16 se = LD2(a + 8);
	int sign = se >> 15, exp = se & 0x7fff;
	if (exp == 0 && mant == 0)
		return sign ? -0.0 : 0.0;
	if (exp == 0x7fff)
		return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
	double v = ldexp((double)mant, exp - 16383 - 63);
	return sign ? -v : v;
}

void recomp_st80(u32 a, f80 v)
{
	u64 mant = 0;
	u16 se = signbit(v) ? 0x8000 : 0;
	if (isnan(v))
	{
		mant = 0xc000000000000000ull;
		se |= 0x7fff;
	}
	else if (isinf(v))
	{
		mant = 0x8000000000000000ull;
		se |= 0x7fff;
	}
	else if (v != 0.0)
	{
		int e;
		double m = frexp(fabs(v), &e);  /* v = m * 2^e, m in [0.5, 1) */
		mant = (u64)ldexp(m, 64);
		se |= (u16)(e - 1 + 16383);
	}
	ST8(a, mant);
	ST2(a + 8, se);
}
