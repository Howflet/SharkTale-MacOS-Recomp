/*=============================================================================
	hle_dinput8.c: DirectInput 8 (system keyboard and mouse) for WinDrv.

	Devices read a shared host input state that the platform layer (the
	macOS window) updates: DirectInput scan-code key states, accumulated
	mouse motion/wheel/buttons, and a buffered event queue per device.
=============================================================================*/
#include <pthread.h>
#include <stdlib.h>

#include "hle_com.h"
#include "input.h"

/*-----------------------------------------------------------------------------
	Shared input state (written by the platform layer).
-----------------------------------------------------------------------------*/

InputState g_input;
static pthread_mutex_t InputLock = PTHREAD_MUTEX_INITIALIZER;

static void push_event(InputQueue* q, u32 ofs, u32 data)
{
	int next = (q->tail + 1) % INPUT_QUEUE;
	if (next == q->head)
		q->head = (q->head + 1) % INPUT_QUEUE;  /* overflow: drop oldest */
	extern u32 hle_tick_count(void);
	q->ev[q->tail] = (InputEvent){ ofs, data, hle_tick_count(), g_input.sequence++ };
	q->tail = next;
}

void input_key(u8 dik, int down)
{
	pthread_mutex_lock(&InputLock);
	g_input.keys[dik] = down ? 0x80 : 0;
	push_event(&g_input.kbd_queue, dik, down ? 0x80 : 0);
	pthread_mutex_unlock(&InputLock);
}

void input_mouse_move(s32 dx, s32 dy)
{
	pthread_mutex_lock(&InputLock);
	g_input.mouse_dx += dx;
	g_input.mouse_dy += dy;
	if (dx)
		push_event(&g_input.mouse_queue, 0, (u32)dx);  /* DIMOFS_X */
	if (dy)
		push_event(&g_input.mouse_queue, 4, (u32)dy);  /* DIMOFS_Y */
	pthread_mutex_unlock(&InputLock);
}

void input_mouse_wheel(s32 dz)
{
	pthread_mutex_lock(&InputLock);
	g_input.mouse_dz += dz;
	push_event(&g_input.mouse_queue, 8, (u32)dz);  /* DIMOFS_Z */
	pthread_mutex_unlock(&InputLock);
}

void input_mouse_button(int button, int down)
{
	pthread_mutex_lock(&InputLock);
	g_input.buttons[button & 7] = down ? 0x80 : 0;
	push_event(&g_input.mouse_queue, 12 + (u32)(button & 7), down ? 0x80 : 0);  /* DIMOFS_BUTTONn */
	pthread_mutex_unlock(&InputLock);
}

/*-----------------------------------------------------------------------------
	IDirectInputDevice8W.  Guest object: vtbl, refs, host(8), kind @16.
-----------------------------------------------------------------------------*/

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };
static int dev_kind(Ctx* c) { return (int)LD4(COM_THIS + 16); }

HLE(Dev_QueryInterface) { ST4(ARG(2), COM_THIS); com_addref(COM_THIS); HLE_RET(c, S_OK, 12); }
HLE(Dev_AddRef) { HLE_RET(c, com_addref(COM_THIS), 4); }
HLE(Dev_Release) { HLE_RET(c, com_release(COM_THIS), 4); }
HLE(Dev_GetCapabilities)
{
	u32 caps = ARG(1);
	u32 size = LD4(caps);
	memset(GP(caps + 4), 0, size > 4 ? size - 4 : 0);
	ST4(caps + 4, 0x5);  /* DIDC_ATTACHED | DIDC_EMULATED */
	ST4(caps + 8, dev_kind(c) == DEV_KEYBOARD ? 0x13 : 0x12);  /* DI8DEVTYPE_KEYBOARD / MOUSE */
	ST4(caps + 12, dev_kind(c) == DEV_KEYBOARD ? 0 : 3);       /* axes */
	ST4(caps + 16, dev_kind(c) == DEV_KEYBOARD ? 128 : 5);     /* buttons */
	HLE_RET(c, S_OK, 8);
}
HLE(Dev_EnumObjects) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetProperty) { HLE_RET(c, E_FAIL, 12); }
HLE(Dev_SetProperty) { HLE_RET(c, S_OK, 12); }
HLE(Dev_Acquire) { HLE_RET(c, S_OK, 4); }
HLE(Dev_Unacquire) { HLE_RET(c, S_OK, 4); }
HLE(Dev_GetDeviceState)
{
	u32 size = ARG(1), out = ARG(2);
	pthread_mutex_lock(&InputLock);
	if (dev_kind(c) == DEV_KEYBOARD)
	{
		memcpy(GP(out), g_input.keys, size < 256 ? size : 256);
	}
	else
	{
		/* DIMOUSESTATE(2): lX, lY, lZ, rgbButtons[4 or 8]; relative since last read. */
		memset(GP(out), 0, size);
		ST4(out, (u32)g_input.mouse_dx);
		ST4(out + 4, (u32)g_input.mouse_dy);
		ST4(out + 8, (u32)g_input.mouse_dz);
		for (u32 b = 0; b < 8 && 12 + b < size; b++)
			ST1(out + 12 + b, g_input.buttons[b]);
		g_input.mouse_dx = g_input.mouse_dy = g_input.mouse_dz = 0;
	}
	pthread_mutex_unlock(&InputLock);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetDeviceData)
{
	/* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
	u32 sz = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
	InputQueue* q = dev_kind(c) == DEV_KEYBOARD ? &g_input.kbd_queue : &g_input.mouse_queue;
	u32 want = LD4(inout), n = 0;
	pthread_mutex_lock(&InputLock);
	int h = q->head;
	while (h != q->tail && n < want)
	{
		if (out)
		{
			u32 o = out + n * sz;
			memset(GP(o), 0, sz);
			ST4(o, q->ev[h].ofs);
			ST4(o + 4, q->ev[h].data);
			ST4(o + 8, q->ev[h].time);
			ST4(o + 12, q->ev[h].seq);
		}
		h = (h + 1) % INPUT_QUEUE;
		n++;
	}
	if (!(flags & 1))  /* DIGDD_PEEK */
		q->head = h;
	pthread_mutex_unlock(&InputLock);
	ST4(inout, n);
	HLE_RET(c, S_OK, 20);
}
HLE(Dev_SetDataFormat) { HLE_RET(c, S_OK, 8); }
HLE(Dev_SetEventNotification) { HLE_RET(c, S_OK, 8); }
HLE(Dev_SetCooperativeLevel) { HLE_RET(c, S_OK, 12); }
HLE(Dev_GetObjectInfo) { HLE_RET(c, E_FAIL, 16); }
HLE(Dev_GetDeviceInfo) { HLE_RET(c, E_FAIL, 8); }
HLE(Dev_RunControlPanel) { HLE_RET(c, S_OK, 12); }
HLE(Dev_Initialize) { HLE_RET(c, S_OK, 16); }
HLE(Dev_CreateEffect) { HLE_RET(c, E_FAIL, 20); }
HLE(Dev_EnumEffects) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetEffectInfo) { HLE_RET(c, E_FAIL, 12); }
HLE(Dev_GetForceFeedbackState) { HLE_RET(c, E_FAIL, 8); }
HLE(Dev_SendForceFeedbackCommand) { HLE_RET(c, E_FAIL, 8); }
HLE(Dev_EnumCreatedEffectObjects) { HLE_RET(c, S_OK, 16); }
HLE(Dev_Escape) { HLE_RET(c, E_FAIL, 8); }
HLE(Dev_Poll) { HLE_RET(c, 1, 4); }  /* DI_NOEFFECT */
HLE(Dev_SendDeviceData) { HLE_RET(c, E_FAIL, 20); }
HLE(Dev_EnumEffectsInFile) { HLE_RET(c, E_FAIL, 20); }
HLE(Dev_WriteEffectToFile) { HLE_RET(c, E_FAIL, 20); }
HLE(Dev_BuildActionMap) { HLE_RET(c, E_FAIL, 16); }
HLE(Dev_SetActionMap) { HLE_RET(c, E_FAIL, 16); }
HLE(Dev_GetImageInfo) { HLE_RET(c, E_FAIL, 8); }

#define M(n) { #n, hle_Dev_##n }
static const ComMethod DevMethods[] = {
	M(QueryInterface), M(AddRef), M(Release), M(GetCapabilities), M(EnumObjects), M(GetProperty), M(SetProperty),
	M(Acquire), M(Unacquire), M(GetDeviceState), M(GetDeviceData), M(SetDataFormat), M(SetEventNotification),
	M(SetCooperativeLevel), M(GetObjectInfo), M(GetDeviceInfo), M(RunControlPanel), M(Initialize), M(CreateEffect),
	M(EnumEffects), M(GetEffectInfo), M(GetForceFeedbackState), M(SendForceFeedbackCommand),
	M(EnumCreatedEffectObjects), M(Escape), M(Poll), M(SendDeviceData), M(EnumEffectsInFile), M(WriteEffectToFile),
	M(BuildActionMap), M(SetActionMap), M(GetImageInfo),
};
#undef M

/*-----------------------------------------------------------------------------
	IDirectInput8W.
-----------------------------------------------------------------------------*/

HLE(DI_QueryInterface) { HLE_RET(c, E_NOINTERFACE, 12); }
HLE(DI_AddRef) { HLE_RET(c, com_addref(COM_THIS), 4); }
HLE(DI_Release) { HLE_RET(c, com_release(COM_THIS), 4); }
HLE(DI_CreateDevice)
{
	/* (this, REFGUID, LPDIRECTINPUTDEVICE8W*, outer): GUID_SysMouse 6F1D2B60, GUID_SysKeyboard 6F1D2B61 */
	u32 d1 = LD4(ARG(1));
	int kind = d1 == 0x6F1D2B61u ? DEV_KEYBOARD : d1 == 0x6F1D2B60u ? DEV_MOUSE : 0;
	if (!kind)
		HLE_RET(c, E_FAIL, 16);
	u32 vt = com_vtable("IDirectInputDevice8W", DevMethods, sizeof DevMethods / sizeof DevMethods[0]);
	u32 dev = com_new(vt, 32, NULL);
	ST4(dev + 16, (u32)kind);
	ST4(ARG(2), dev);
	HLE_RET(c, S_OK, 16);
}
HLE(DI_EnumDevices) { HLE_RET(c, S_OK, 20); }  /* no joysticks */
HLE(DI_GetDeviceStatus) { HLE_RET(c, S_OK, 8); }
HLE(DI_RunControlPanel) { HLE_RET(c, S_OK, 12); }
HLE(DI_Initialize) { HLE_RET(c, S_OK, 12); }
HLE(DI_FindDevice) { HLE_RET(c, E_FAIL, 16); }
HLE(DI_EnumDevicesBySemantics) { HLE_RET(c, S_OK, 24); }
HLE(DI_ConfigureDevices) { HLE_RET(c, E_FAIL, 20); }

static const ComMethod DIMethods[] = {
	{ "QueryInterface", hle_DI_QueryInterface }, { "AddRef", hle_DI_AddRef }, { "Release", hle_DI_Release },
	{ "CreateDevice", hle_DI_CreateDevice }, { "EnumDevices", hle_DI_EnumDevices },
	{ "GetDeviceStatus", hle_DI_GetDeviceStatus }, { "RunControlPanel", hle_DI_RunControlPanel },
	{ "Initialize", hle_DI_Initialize }, { "FindDevice", hle_DI_FindDevice },
	{ "EnumDevicesBySemantics", hle_DI_EnumDevicesBySemantics }, { "ConfigureDevices", hle_DI_ConfigureDevices },
};

HLE(DirectInput8Create)
{
	/* (hinst, version, REFIID, LPVOID*, outer) */
	u32 vt = com_vtable("IDirectInput8W", DIMethods, sizeof DIMethods / sizeof DIMethods[0]);
	ST4(ARG(3), com_new(vt, 16, NULL));
	HLE_RET(c, S_OK, 20);
}

const HleEntry hle_dinput8[] = { ENTRY(DirectInput8Create), { NULL, NULL } };
