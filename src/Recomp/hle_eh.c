/*=============================================================================
	hle_eh.c: MSVC x86 exception handling for recompiled code.

	Frames register SEH records at FS:[0] exactly as on Windows; each
	recompiled function remembers the record it registered (eh_rec).
	_CxxThrowException walks the guest record chain calling each frame's
	handler; the C++ handler (__CxxFrameHandler, reached through each
	function's __ehhandler thunk with EAX = FuncInfo) and the SEH handler
	(_except_handler3) are implemented here from the binary's own tables.
	When a frame catches, the throw records the target (Ctx::unwind_record,
	plus the continuation and its ESP/EBP) and returns. Every recompiled call
	site checks unwind_record afterwards (UNWIND_CHECK) and returns, until the
	function that registered the record resumes at its continuation label.
	No setjmp: it made clang treat every EH function as returns_twice.
=============================================================================*/
#include <stdlib.h>

#include "hle.h"

#define EH_EXCEPTION_NUMBER 0xE06D7363u
#define EH_UNWINDING 2u
#define EH_EXIT_UNWIND 4u
#define ExceptionContinueSearch 1u

/*-----------------------------------------------------------------------------
	Guest calls with registers preserved (funclets clobber freely).
-----------------------------------------------------------------------------*/

typedef struct Regs { u32 r[8]; u32 fs; } Regs;
static void save(Ctx* c, Regs* s) { for (int i = 0; i < 8; i++) s->r[i] = R4(4 * i); }
static void restore(Ctx* c, const Regs* s) { for (int i = 0; i < 8; i++) R4(4 * i) = s->r[i]; }

/* Call a funclet that addresses its parent frame through EBP. */
static u32 call_with_ebp(Ctx* c, u32 fn, u32 ebp)
{
	Regs s;
	save(c, &s);
	R4(REG_EBP) = ebp;
	u32 r = rt_call_guest(c, fn, 0, NULL, 0);
	restore(c, &s);
	return r;
}

static u32 call_handler(Ctx* c, u32 handler, u32 rec, u32 frame)
{
	Regs s;
	save(c, &s);
	u32 args[4] = { rec, frame, 0, 0 };
	u32 r = rt_call_guest(c, handler, 4, args, 0);
	restore(c, &s);
	return r;
}

/* Start unwinding to the frame that registered `record`: callers return until it resumes. */
static void resume_frame(Ctx* c, u32 record, u32 ebp, u32 esp, u32 continuation)
{
	ST4(c->teb, record);  /* frames above it are gone */
	c->unwind_ebp = ebp;
	c->unwind_esp = esp;
	c->eh_resume = continuation;
	c->unwind_record = record;
}

/*-----------------------------------------------------------------------------
	Current exception (for rethrow: throw;).
-----------------------------------------------------------------------------*/

static __thread u32 CurObj, CurInfo;

static u32 make_record(u32 obj, u32 info)
{
	u32 r = rt_heap_alloc(80);
	memset(GP(r), 0, 80);
	ST4(r + 0, EH_EXCEPTION_NUMBER);
	ST4(r + 4, 1);           /* EXCEPTION_NONCONTINUABLE */
	ST4(r + 16, 3);          /* NumberParameters */
	ST4(r + 20, 0x19930520);
	ST4(r + 24, obj);
	ST4(r + 28, info);
	return r;
}

/* Global unwind: let every frame above target clean up, then unlink them. */
static void global_unwind(Ctx* c, u32 rec, u32 target)
{
	ST4(rec + 4, LD4(rec + 4) | EH_UNWINDING);
	for (u32 f = LD4(c->teb); f != 0xffffffffu && f != target; f = LD4(c->teb))
	{
		ST4(c->teb, LD4(f));  /* unlink before calling, as RtlUnwind does */
		call_handler(c, LD4(f + 4), rec, f);
	}
	ST4(rec + 4, LD4(rec + 4) & ~EH_UNWINDING);
}

static void describe_unhandled(Ctx* c, u32 obj, u32 info)
{
	char msg[1024] = "";
	if (info && obj)
	{
		u32 cta = LD4(info + 12);
		u32 ct = LD4(cta + 4);
		const char* name = (const char*)GP(LD4(ct + 4) + 8);
		if (!strcmp(name, ".PAG") || !strcmp(name, ".PBG"))
			g_wstr_utf8(LD4(obj), msg, sizeof msg);
		else
			snprintf(msg, sizeof msg, "(object of type %s)", name);
	}
	fprintf(stderr, "[eh] unhandled C++ exception: %s\n", msg);
}

static void dispatch(Ctx* c, u32 rec)
{
	for (u32 f = LD4(c->teb); f != 0xffffffffu; f = LD4(f))
	{
		u32 r = call_handler(c, LD4(f + 4), rec, f);
		if (c->unwind_record)
			return;  /* caught: unwinding to the catching frame */
		if (r != ExceptionContinueSearch)
			recomp_fail(c, f, "exception handler returned ContinueExecution");
	}
	describe_unhandled(c, LD4(rec + 24), LD4(rec + 28));
	recomp_fail(c, 0, "unhandled exception");
}

HLE(_CxxThrowException)
{
	u32 obj = ARG(0), info = ARG(1);
	if (!obj && !info)
	{
		obj = CurObj;  /* throw; */
		info = CurInfo;
	}
	if (getenv("SHARK_TRACE_EH"))
	{
		char m[512] = "";
		if (obj && info)
		{
			u32 ct = LD4(LD4(info + 12) + 4);
			const char* n = (const char*)GP(LD4(ct + 4) + 8);
			if (!strcmp(n, ".PAG") || !strcmp(n, ".PBG"))
				g_wstr_utf8(LD4(obj), m, sizeof m);
		}
		fprintf(stderr, "[eh] throw %s\n", m);
	}
	dispatch(c, make_record(obj, info));
	HLE_RET(c, 0, 8);  /* unwinding: the caller's UNWIND_CHECK returns */
}

/*-----------------------------------------------------------------------------
	C++ frames: __CxxFrameHandler (EAX = FuncInfo).
	FuncInfo: magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap, ...
	UnwindMapEntry: toState, action.  TryBlockMapEntry: tryLow, tryHigh,
	catchHigh, nCatches, pHandlerArray.  HandlerType: adjectives, pType,
	dispCatchObj, addressOfHandler.
-----------------------------------------------------------------------------*/

static void cxx_local_unwind(Ctx* c, u32 frame, u32 fi, s32 to)
{
	u32 ebp = frame + 0xc, map = LD4(fi + 8);
	s32 state = (s32)LD4(frame + 8);
	while (state != -1 && state != to && state >= 0 && state < (s32)LD4(fi + 4))
	{
		u32 e = map + 8 * (u32)state;
		s32 next = (s32)LD4(e);
		u32 action = LD4(e + 4);
		ST4(frame + 8, (u32)next);
		if (action)
			call_with_ebp(c, action, ebp);
		state = next;
	}
}

static int type_matches(u32 ht, u32 info, u32* match_ct)
{
	u32 ptype = LD4(ht + 4);
	if (!ptype || !LD1(ptype + 8))
		return 1;  /* catch (...) */
	const char* want = (const char*)GP(ptype + 8);
	u32 cta = LD4(info + 12), n = LD4(cta);
	for (u32 i = 0; i < n; i++)
	{
		u32 ct = LD4(cta + 4 + 4 * i);
		const char* have = (const char*)GP(LD4(ct + 4) + 8);
		/* ".PAG" (TCHAR*) vs ".PBG" (const TCHAR*): constness differs only in the letter. */
		if (!strcmp(want, have) || (want[0] == '.' && want[1] == 'P' && !strcmp(want + 3, have + 3)))
		{
			*match_ct = ct;
			return 1;
		}
	}
	return 0;
}

HLE(__CxxFrameHandler)
{
	u32 fi = R4(REG_EAX);
	u32 rec = ARG(0), frame = ARG(1);
	u32 flags = LD4(rec + 4);
	if (flags & (EH_UNWINDING | EH_EXIT_UNWIND))
	{
		cxx_local_unwind(c, frame, fi, -1);
		HLE_RET(c, ExceptionContinueSearch, 0);
	}
	if (LD4(rec) != EH_EXCEPTION_NUMBER)
		HLE_RET(c, ExceptionContinueSearch, 0);
	u32 obj = LD4(rec + 24), info = LD4(rec + 28);
	s32 state = (s32)LD4(frame + 8);
	u32 ntry = LD4(fi + 12), tmap = LD4(fi + 16);
	for (u32 t = 0; t < ntry; t++)
	{
		u32 tb = tmap + 20 * t;
		s32 lo = (s32)LD4(tb), hi = (s32)LD4(tb + 4);
		if (state < lo || state > hi)
			continue;
		u32 nc = LD4(tb + 12), handlers = LD4(tb + 16);
		for (u32 h = 0; h < nc; h++)
		{
			u32 ht = handlers + 16 * h, ct = 0;
			if (!type_matches(ht, info, &ct))
				continue;
			u32 ebp = frame + 0xc;
			global_unwind(c, rec, frame);
			cxx_local_unwind(c, frame, fi, lo);
			/* Catch object. */
			s32 disp = (s32)LD4(ht + 8);
			if (disp && LD4(ht + 4) && ct)
			{
				u32 dest = ebp + (u32)disp;
				if (LD4(ht) & 8)
					ST4(dest, obj);                         /* by reference */
				else if ((LD4(ct) & 1) || !LD4(ct + 24))
					memcpy(GP(dest), GP(obj), LD4(ct + 20)); /* simple type */
				else
				{
					Regs s;
					save(c, &s);
					R4(REG_ECX) = dest;
					u32 a[1] = { obj };
					rt_call_guest(c, LD4(ct + 24), 1, a, 1);  /* copy ctor (thiscall) */
					restore(c, &s);
				}
			}
			ST4(frame + 8, (u32)(hi + 1));
			u32 saved_obj = CurObj, saved_info = CurInfo;
			CurObj = obj;
			CurInfo = info;
			u32 cont = call_with_ebp(c, LD4(ht + 12), ebp);
			CurObj = saved_obj;
			CurInfo = saved_info;
			if (c->unwind_record)
				HLE_RET(c, 0, 0);  /* the catch block threw (or rethrew): that unwind wins */
			if (LD4(info + 4) && obj)
			{
				Regs s;
				save(c, &s);
				R4(REG_ECX) = obj;
				rt_call_guest(c, LD4(info + 4), 0, NULL, 1);  /* destroy the exception object */
				restore(c, &s);
			}
			resume_frame(c, frame, ebp, LD4(ebp - 0x10), cont);
			HLE_RET(c, 0, 0);
		}
	}
	HLE_RET(c, ExceptionContinueSearch, 0);
}

/*-----------------------------------------------------------------------------
	SEH frames: _except_handler3. Record at EBP-0x10: next, handler,
	scopetable, trylevel; [EBP-0x14] = EXCEPTION_POINTERS*, [EBP-0x18] = ESP.
	Scope entry: enclosing level, filter (NULL = __finally), handler.
-----------------------------------------------------------------------------*/

static void seh_local_unwind(Ctx* c, u32 frame, s32 to)
{
	u32 ebp = frame + 0x10, table = LD4(frame + 8);
	s32 tl = (s32)LD4(frame + 12);
	while (tl != -1 && tl != to)
	{
		u32 e = table + 12 * (u32)tl;
		s32 enclosing = (s32)LD4(e);
		ST4(frame + 12, (u32)enclosing);
		if (!LD4(e + 4) && LD4(e + 8))
			call_with_ebp(c, LD4(e + 8), ebp);  /* __finally */
		tl = enclosing;
	}
}

HLE(_except_handler3)
{
	u32 rec = ARG(0), frame = ARG(1);
	u32 ebp = frame + 0x10;
	if (LD4(rec + 4) & (EH_UNWINDING | EH_EXIT_UNWIND))
	{
		seh_local_unwind(c, frame, -1);
		HLE_RET(c, ExceptionContinueSearch, 0);
	}
	u32 ptrs = rt_heap_alloc(8);
	ST4(ptrs, rec);
	ST4(ptrs + 4, 0);
	ST4(ebp - 0x14, ptrs);
	u32 table = LD4(frame + 8);
	for (s32 tl = (s32)LD4(frame + 12); tl != -1;)
	{
		u32 e = table + 12 * (u32)tl;
		u32 filter = LD4(e + 4);
		if (filter)
		{
			s32 r = (s32)call_with_ebp(c, filter, ebp);
			if (r > 0)
			{
				global_unwind(c, rec, frame);
				seh_local_unwind(c, frame, tl);
				ST4(frame + 12, LD4(e));
				resume_frame(c, frame, ebp, LD4(ebp - 0x18), LD4(e + 8));
				HLE_RET(c, 0, 0);
			}
			if (r < 0)
				HLE_RET(c, 0, 0);  /* ContinueExecution */
		}
		tl = (s32)LD4(e);
	}
	HLE_RET(c, ExceptionContinueSearch, 0);
}

HLE(_XcptFilter) { HLE_RET(c, 0, 0); }

/* setjmp/longjmp from guest code are not supported (not used on the game's paths). */
HLE(_setjmp3) { HLE_RET(c, 0, 0); }
HLE(longjmp) { recomp_fail(c, 0, "guest longjmp"); }

const HleEntry hle_eh[] = {
	ENTRY(_CxxThrowException), ENTRY(__CxxFrameHandler), ENTRY(_except_handler3), ENTRY(_XcptFilter),
	ENTRY(_setjmp3), ENTRY(longjmp),
	{ NULL, NULL }
};
