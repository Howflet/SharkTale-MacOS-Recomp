/*=============================================================================
	hle_com.c: Guest-resident COM objects with native methods.
=============================================================================*/
#include <stdlib.h>

#include "hle_com.h"

typedef struct VtCache { char name[64]; u32 vt; } VtCache;
static VtCache Vtables[64];
static int NumVtables;

u32 com_vtable(const char* iface, const ComMethod* methods, int count)
{
	for (int i = 0; i < NumVtables; i++)
		if (!strcmp(Vtables[i].name, iface))
			return Vtables[i].vt;
	u32 vt = hle_static_alloc(4 * (u32)count);
	for (int i = 0; i < count; i++)
	{
		char name[160];
		snprintf(name, sizeof name, "%s::%s", iface, methods[i].name);
		ST4(vt + 4 * (u32)i, rt_hle_bind("COM", name, methods[i].fn));
	}
	snprintf(Vtables[NumVtables].name, sizeof Vtables[0].name, "%s", iface);
	Vtables[NumVtables++].vt = vt;
	return vt;
}

u32 com_new(u32 vtable, u32 size, void* host)
{
	u32 o = rt_heap_alloc(size < 16 ? 16 : size);
	memset(GP(o), 0, size < 16 ? 16 : size);
	ST4(o, vtable);
	ST4(o + 4, 1);
	u64 h = (u64)(uintptr_t)host;
	ST4(o + 8, (u32)h);
	ST4(o + 12, (u32)(h >> 32));
	return o;
}

u32 com_addref(u32 obj)
{
	u32 n = LD4(obj + 4) + 1;
	ST4(obj + 4, n);
	return n;
}

u32 com_release(u32 obj)
{
	u32 n = LD4(obj + 4);
	if (n)
		ST4(obj + 4, --n);
	return n;
}
