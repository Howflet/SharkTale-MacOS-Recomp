/*=============================================================================
	SharkCore.h: Hand-written Core glue on top of the generated declarations.

	Rebuilt modules include this instead of Epic's Core.h. It provides the
	pieces UE2 headers would: TEXT(), object/class flags, the hardcoded names
	(EName), and the IMPLEMENT_PACKAGE / IMPLEMENT_CLASS registration macros.
	Values are taken from the shipped binaries (see SHARKTALE_NOTES.md).

	Build with /Zc:wchar_t- so L"" literals are unsigned short (TCHAR), which
	is how MSVC 7 mangled TCHAR (G) in every export.
=============================================================================*/
#pragma once

#include "UnNames.h"
#include "CoreClasses.h"

#define TEXT(s) L##s
#ifndef NULL
#define NULL 0
#endif
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Object flags (UObject::ObjectFlags). Registration passes 0x4084004. */
enum
{
	RF_Public     = 0x00000004,
	RF_Transient  = 0x00004000,
	RF_Standalone = 0x00080000,
	RF_Native     = 0x04000000,
};

/* Class flags (UClass::ClassFlags). */
enum
{
	CLASS_Config = 0x00000004,
};

/* Global allocation goes through GMalloc, as UE2's inline operators did. */
inline void* operator new(unsigned int Size) { return GMalloc->Malloc(Size, TEXT("new")); }
inline void* operator new[](unsigned int Size) { return GMalloc->Malloc(Size, TEXT("new")); }
inline void operator delete(void* Ptr) { GMalloc->Free(Ptr); }
inline void operator delete[](void* Ptr) { GMalloc->Free(Ptr); }

/* Diagnostic markers for rebuilt modules (build with -DSHARK_TRACE; seen in
   CrossOver logs via --debugmsg +debugstr). Compiled out otherwise. */
#ifdef SHARK_TRACE
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#define SHARK_MARK(msg) OutputDebugStringA("[shark] " msg "\n")
#else
#define SHARK_MARK(msg) ((void)0)
#endif

/* Checks. The shipped build kept check() (calls appFailAssert with the
   expression and source file); guard/unguard are omitted in rebuilt code. */
#define check(expr) do { if (!(expr)) appFailAssert(#expr, __FILE__, __LINE__); } while (0)

/*-----------------------------------------------------------------------------
	Package and class registration (UE2 IMPLEMENT_PACKAGE / IMPLEMENT_CLASS).
-----------------------------------------------------------------------------*/

/* Exports GPackage (TCHAR[]) and _DllMain@12, as every UE2 package DLL does. */
#define IMPLEMENT_PACKAGE(pkg) \
	extern "C" DLL_EXPORT TCHAR GPackage[]; \
	DLL_EXPORT TCHAR GPackage[] = TEXT(#pkg); \
	extern "C" DLL_EXPORT int __stdcall DllMain(void* hInstance, unsigned long Reason, void* Reserved) \
	{ \
		return 1; \
	}

/* Defines PrivateStaticClass (constructed at DLL load, which registers the
   class with Core) and the autoclass<Class> export. Arguments mirror the
   shipped registration calls: zero GUID, object flags 0x4084004. */
#define IMPLEMENT_CLASS_EX(TClass, TSuper, TWithin, ClassFlags) \
	UClass TClass::PrivateStaticClass( \
		(ENativeConstructor)0, sizeof(TClass), (ClassFlags), TSuper::StaticClass(), TWithin::StaticClass(), \
		FGuid(0, 0, 0, 0), &TEXT(#TClass)[1], GPackage, TClass::StaticConfigName(), \
		RF_Public | RF_Standalone | RF_Transient | RF_Native, \
		(void (__cdecl*)(void*))&TClass::InternalConstructor, \
		static_cast<void (UObject::*)(void)>(&TClass::StaticConstructor)); \
	extern "C" DLL_EXPORT UClass* autoclass##TClass; \
	DLL_EXPORT UClass* autoclass##TClass = TClass::StaticClass();

/* The per-class boilerplate DECLARE_CLASS put inline in the class body. */
#define DECLARE_CLASS_BOILERPLATE(TClass) \
public: \
	static UClass* __cdecl StaticClass(void) { return &PrivateStaticClass; } \
	static void* __cdecl operator new(unsigned int Size, UObject* Outer, FName Name, unsigned long Flags) \
	{ return UObject::StaticAllocateObject(StaticClass(), Outer, Name, Flags, 0, GError, 0, 0); } \
	static void* __cdecl operator new(unsigned int, EInternal* Mem) { return (void*)Mem; } \
	static void __cdecl InternalConstructor(void* X) { new((EInternal*)X) TClass(); } \
	virtual ~TClass() { ConditionalDestroy(); } \
private: \
	static UClass PrivateStaticClass; \
public:
