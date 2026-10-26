/*=============================================================================
	hle_com.h: COM objects implemented natively but living in guest memory.

	A guest COM object is { vtable*, refcount, host pointer (64-bit, split) }
	followed by interface-specific guest data. Its vtable is an array of HLE
	thunk addresses; methods are stdcall with `this` as the first argument.
=============================================================================*/
#pragma once

#include "hle.h"

typedef struct ComMethod { const char* name; HleFn fn; } ComMethod;

/* Build (once) a guest vtable for an interface from its methods in slot order. */
u32 com_vtable(const char* iface, const ComMethod* methods, int count);

/* Allocate a guest object of `size` bytes (>= 16) with the given vtable. */
u32 com_new(u32 vtable, u32 size, void* host);
static inline void* com_host(u32 obj) { return (void*)(uintptr_t)(((u64)LD4(obj + 12) << 32) | LD4(obj + 8)); }

/* Shared IUnknown handling: AddRef/Release on the guest refcount. */
u32 com_addref(u32 obj);
u32 com_release(u32 obj);

/* Register a native function under a name and return its guest thunk address. */
u32 rt_hle_bind(const char* dll, const char* name, HleFn fn);

#define COM_THIS ARG(0)
#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
