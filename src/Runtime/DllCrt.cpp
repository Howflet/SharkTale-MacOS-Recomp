/*=============================================================================
	DllCrt.cpp: Minimal DLL startup for rebuilt modules.

	The originals link MSVCR70's DLL CRT. We have no MSVC import libraries, so
	this supplies the entry point instead: it runs C++ static initializers
	(which construct each PrivateStaticClass and so register native classes
	with Core), keeps an atexit list for static destructors, and forwards to
	the module's DllMain. Everything else comes from msvcr70.dll via its
	import library (build/implib/msvcr70.lib).
=============================================================================*/

typedef void (__cdecl* PVFV)(void);

// The linker sorts .CRT$XC* sections alphabetically; clang-cl puts each
// dynamic initializer pointer in .CRT$XCU, between these two markers.
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
extern "C" __declspec(allocate(".CRT$XCA")) PVFV __xc_a[] = { 0 };
extern "C" __declspec(allocate(".CRT$XCZ")) PVFV __xc_z[] = { 0 };
#pragma comment(linker, "/merge:.CRT=.rdata")

static PVFV AtExit[256];
static int AtExitCount = 0;

extern "C" int __cdecl atexit(PVFV Func)
{
	if (AtExitCount >= (int)(sizeof(AtExit) / sizeof(AtExit[0])))
		return -1;
	AtExit[AtExitCount++] = Func;
	return 0;
}

extern "C" int __stdcall DllMain(void* hInstance, unsigned long Reason, void* Reserved);

enum { DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1 };

extern "C" int __stdcall _DllMainCRTStartup(void* hInstance, unsigned long Reason, void* Reserved)
{
	if (Reason == DLL_PROCESS_ATTACH)
	{
		for (PVFV* Init = __xc_a; Init < __xc_z; ++Init)
			if (*Init)
				(*Init)();
	}
	int Result = DllMain(hInstance, Reason, Reserved);
	if (Reason == DLL_PROCESS_DETACH)
	{
		while (AtExitCount > 0)
			AtExit[--AtExitCount]();
	}
	return Result;
}

// Referenced by the compiler for floating point use when no CRT is linked.
extern "C" int _fltused = 0x9875;
