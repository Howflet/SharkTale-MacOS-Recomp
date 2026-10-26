/*=============================================================================
	host.c: Threads (guest contexts, stacks, TEBs), host->guest calls, and
	the guest heap.
=============================================================================*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"
#include "runtime.h"

/*-----------------------------------------------------------------------------
	Thread contexts. Each guest thread gets a Ctx, a 4 MB guest stack and a
	TEB (FS points at it; [0] is the SEH chain head).
-----------------------------------------------------------------------------*/

static pthread_key_t CtxKey;
static pthread_once_t CtxOnce = PTHREAD_ONCE_INIT;
static pthread_mutex_t ThreadLock = PTHREAD_MUTEX_INITIALIZER;
static u32 NextThread;

static void make_key(void) { pthread_key_create(&CtxKey, NULL); }

Ctx* rt_new_thread_ctx(void)
{
	pthread_once(&CtxOnce, make_key);
	Ctx* c = calloc(1, sizeof(Ctx));
	pthread_mutex_lock(&ThreadLock);
	c->thread_id = NextThread++;
	pthread_mutex_unlock(&ThreadLock);
	u32 stack_top = GUEST_STACK_BASE + (c->thread_id + 1) * GUEST_STACK_SIZE - 16;
	c->teb = GUEST_TEB_BASE + c->thread_id * 0x1000;
	ST4(c->teb + 0x00, 0xffffffffu);                        /* ExceptionList: end of chain */
	ST4(c->teb + 0x04, stack_top);                          /* StackBase */
	ST4(c->teb + 0x08, stack_top - GUEST_STACK_SIZE + 16);  /* StackLimit */
	ST4(c->teb + 0x18, c->teb);                             /* Self */
	ST4(c->teb + 0x24, 0x100 + c->thread_id);               /* ClientId.UniqueThread */
	ST4(c->teb + 0x2c, c->teb + 0x800);                     /* ThreadLocalStoragePointer */
	R4(REG_ESP) = stack_top;
	R4(REG_EBP) = 0;
	R4(REG_FS_OFFSET) = c->teb;
	R2(REG_FPUControlWord) = 0x027f;
	pthread_setspecific(CtxKey, c);
	return c;
}

Ctx* rt_thread_ctx(void)
{
	pthread_once(&CtxOnce, make_key);
	Ctx* c = pthread_getspecific(CtxKey);
	return c ? c : rt_new_thread_ctx();
}

/*-----------------------------------------------------------------------------
	Host -> guest calls. Arguments are pushed right to left, then a return
	address; the recompiled callee's RET pops it (and its arguments if it is
	stdcall). Callee-saved registers follow the x86 convention anyway.
-----------------------------------------------------------------------------*/

#define GUEST_RETURN_MAGIC 0x7effff00u

u32 rt_call_guest(Ctx* c, u32 fn, int nargs, const u32* args, int stdcall)
{
	if (c->unwind_record)
		return 0;  /* an exception is unwinding through this host frame: run nothing more */
	u32 esp = R4(REG_ESP);
	for (int i = nargs - 1; i >= 0; i--)
	{
		esp -= 4;
		ST4(esp, args[i]);
	}
	esp -= 4;
	ST4(esp, GUEST_RETURN_MAGIC);
	R4(REG_ESP) = esp;
	recomp_call(c, fn);
	if (!stdcall)
		R4(REG_ESP) += 4 * nargs;
	return R4(REG_EAX);
}

/*-----------------------------------------------------------------------------
	Guest heap: size-class free lists over [GUEST_HEAP_BASE, GUEST_HEAP_END).
	Blocks carry an 8-byte header (size class, requested size); returned
	pointers are 8-byte aligned. Large blocks (> 1 MB) get their own pages.
-----------------------------------------------------------------------------*/

static pthread_mutex_t HeapLock = PTHREAD_MUTEX_INITIALIZER;
static u32 HeapTop = GUEST_HEAP_BASE;
static u32 PagesLow = GUEST_HEAP_END;  /* lowest address ever handed out by the page allocator */
static u32 FreeLists[40];  /* by power of two of the block size */

static int size_class(u32 n)
{
	int k = 4;
	while ((1u << k) < n)
		k++;
	return k;
}

u32 rt_heap_alloc(u32 size)
{
	if (size == 0)
		size = 1;
	int k = size_class(size + 8);
	pthread_mutex_lock(&HeapLock);
	u32 block = FreeLists[k];
	if (block)
	{
		FreeLists[k] = LD4(block + 8);
	}
	else
	{
		u32 bytes = 1u << k;
		if (HeapTop + bytes > PagesLow)
		{
			pthread_mutex_unlock(&HeapLock);
			fprintf(stderr, "[heap] out of guest memory (request %u)\n", size);
			return 0;
		}
		block = HeapTop;
		HeapTop += bytes;
	}
	pthread_mutex_unlock(&HeapLock);
	ST4(block, (u32)k);
	ST4(block + 4, size);
	return block + 8;
}

void rt_heap_free(u32 addr)
{
	if (!addr)
		return;
	u32 block = addr - 8;
	int k = (int)LD4(block);
	if (k < 4 || k >= 40)
	{
		fprintf(stderr, "[heap] free of non-heap pointer %#x\n", addr);
		return;
	}
	pthread_mutex_lock(&HeapLock);
	ST4(block + 8, FreeLists[k]);
	FreeLists[k] = block;
	pthread_mutex_unlock(&HeapLock);
}

/*-----------------------------------------------------------------------------
	Page allocator (VirtualAlloc): 64 KB granules allocated first-fit from the
	top of the heap range downwards, so the small-block heap above and this
	one grow towards each other. Freed runs are reused.
-----------------------------------------------------------------------------*/

#define PAGE_SHIFT 16
#define NUM_GRANULES ((GUEST_HEAP_END - GUEST_HEAP_BASE) >> PAGE_SHIFT)
static u32 RunLength[NUM_GRANULES];  /* at a run's first granule: its length; 0 = free */
static u8 Used[NUM_GRANULES];

u32 rt_pages_alloc(u32 size)
{
	u32 n = (size + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
	if (!n)
		n = 1;
	pthread_mutex_lock(&HeapLock);
	u32 floor = (HeapTop - GUEST_HEAP_BASE + (1u << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
	u32 run = 0;
	for (u32 g = NUM_GRANULES; g-- > floor;)
	{
		run = Used[g] ? 0 : run + 1;
		if (run == n)
		{
			for (u32 i = 0; i < n; i++)
				Used[g + i] = 1;
			RunLength[g] = n;
			u32 addr = GUEST_HEAP_BASE + (g << PAGE_SHIFT);
			if (addr < PagesLow)
				PagesLow = addr;
			pthread_mutex_unlock(&HeapLock);
			memset(GP(addr), 0, (size_t)n << PAGE_SHIFT);
			return addr;
		}
	}
	pthread_mutex_unlock(&HeapLock);
	fprintf(stderr, "[heap] out of guest pages (request %u; heap top %#x, pages low %#x)\n", size, HeapTop, PagesLow);
	return 0;
}

int rt_pages_free(u32 addr)
{
	if (addr < GUEST_HEAP_BASE || addr >= GUEST_HEAP_END || (addr & ((1u << PAGE_SHIFT) - 1)))
		return 0;
	u32 g = (addr - GUEST_HEAP_BASE) >> PAGE_SHIFT;
	pthread_mutex_lock(&HeapLock);
	u32 n = RunLength[g];
	for (u32 i = 0; i < n; i++)
		Used[g + i] = 0;
	RunLength[g] = 0;
	pthread_mutex_unlock(&HeapLock);
	return n != 0;
}

u32 rt_heap_size(u32 addr)
{
	return addr ? LD4(addr - 4) : 0;
}

u32 rt_heap_realloc(u32 addr, u32 size)
{
	if (!addr)
		return rt_heap_alloc(size);
	if (!size)
	{
		rt_heap_free(addr);
		return 0;
	}
	u32 block = addr - 8;
	int k = (int)LD4(block);
	if (size + 8 <= (1u << k))
	{
		ST4(block + 4, size);
		return addr;
	}
	u32 n = rt_heap_alloc(size);
	u32 old = LD4(block + 4);
	memcpy(GP(n), GP(addr), old < size ? old : size);
	rt_heap_free(addr);
	return n;
}
