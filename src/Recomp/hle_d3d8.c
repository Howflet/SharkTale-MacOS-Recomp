/*=============================================================================
	hle_d3d8.c: Direct3D 8 and DirectDraw for the recompiled game.

	IDirect3D8 (adapter enumeration and caps) and IDirectDraw/IDirectDraw7
	(video memory query, used by the launcher) are implemented here. The
	device (IDirect3DDevice8 and its resources) is in hle_d3d8_device.c,
	rendering through Metal.

	We present a fixed-function device with hardware T&L and no vertex or
	pixel shader versions, so D3DDrv uses its fixed-function paths.
=============================================================================*/
#include <stdlib.h>

#include "hle_com.h"
#include "d3d8_types.h"

/*-----------------------------------------------------------------------------
	Adapter description.
-----------------------------------------------------------------------------*/

typedef struct Mode { u32 w, h; } Mode;
static const Mode Modes[] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 },
	{ 1280, 1024 }, { 1440, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
};
#define NUM_MODES (sizeof Modes / sizeof Modes[0])
static const u32 ModeFormats[] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5 };

void d3d8_fill_caps(u32 out)
{
	D3DCAPS8 k;
	memset(&k, 0, sizeof k);
	k.DeviceType = 1;  /* HAL */
	k.Caps2 = 0x00020000 | 0x00080000 | 0x20000000;  /* fullscreen gamma, render windowed, dynamic textures */
	k.PresentationIntervals = 0x80000000u | 1;
	k.DevCaps = 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x2000 | 0x10000 | 0x80000;
	k.PrimitiveMiscCaps = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x800;
	k.RasterCaps = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x100000 | 0x200000;
	k.ZCmpCaps = k.AlphaCmpCaps = 0xff;
	k.SrcBlendCaps = k.DestBlendCaps = 0x1fff;
	k.ShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;
	k.TextureCaps = 0x1 | 0x4 | 0x400 | 0x800 | 0x4000 | 0x10000;
	k.TextureFilterCaps = k.CubeTextureFilterCaps = k.VolumeTextureFilterCaps =
		0x100 | 0x200 | 0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000 | 0x4000000;
	k.TextureAddressCaps = k.VolumeTextureAddressCaps = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20;
	k.LineCaps = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;
	k.MaxTextureWidth = k.MaxTextureHeight = 4096;
	k.MaxVolumeExtent = 256;
	k.MaxTextureRepeat = 8192;
	k.MaxTextureAspectRatio = 4096;
	k.MaxAnisotropy = 16;
	k.MaxVertexW = 1e10f;
	k.GuardBandLeft = k.GuardBandTop = -1e8f;
	k.GuardBandRight = k.GuardBandBottom = 1e8f;
	k.StencilCaps = 0xff;
	k.FVFCaps = 8 | 0x80000;
	k.TextureOpCaps = 0x03ffffff;
	k.MaxTextureBlendStages = 8;
	k.MaxSimultaneousTextures = 4;
	k.VertexProcessingCaps = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40;
	k.MaxActiveLights = 8;
	k.MaxUserClipPlanes = 6;
	k.MaxVertexBlendMatrices = 4;
	k.MaxPointSize = 64.0f;
	k.MaxPrimitiveCount = 0xfffff;
	k.MaxVertexIndex = 0xfffff;
	k.MaxStreams = 8;
	k.MaxStreamStride = 256;
	k.VertexShaderVersion = 0;
	k.PixelShaderVersion = 0;
	memcpy(GP(out), &k, sizeof k);
}

/*-----------------------------------------------------------------------------
	IDirect3D8.
-----------------------------------------------------------------------------*/

HLE(D3D_QueryInterface) { HLE_RET(c, E_NOINTERFACE, 12); }
HLE(D3D_AddRef) { HLE_RET(c, com_addref(COM_THIS), 4); }
HLE(D3D_Release) { HLE_RET(c, com_release(COM_THIS), 4); }
HLE(D3D_RegisterSoftwareDevice) { HLE_RET(c, E_FAIL, 8); }
HLE(D3D_GetAdapterCount) { HLE_RET(c, 1, 4); }
HLE(D3D_GetAdapterIdentifier)
{
	/* (this, adapter, flags, D3DADAPTER_IDENTIFIER8*) */
	u32 id = ARG(3);
	memset(GP(id), 0, 1068);
	snprintf((char*)GP(id), 512, "nv4_disp.dll");
	snprintf((char*)GP(id + 512), 512, "NVIDIA GeForce FX 5900 Ultra");
	ST8(id + 1024, 0x0006000E000A0D96ull);  /* driver 6.14.10.3478 */
	ST4(id + 1032, 0x10de);                  /* VendorId */
	ST4(id + 1036, 0x0330);                  /* DeviceId */
	ST4(id + 1040, 0);
	ST4(id + 1044, 0xa1);
	ST4(id + 1064, 1);                       /* WHQL */
	HLE_RET(c, S_OK, 16);
}
HLE(D3D_GetAdapterModeCount) { HLE_RET(c, NUM_MODES * 2, 8); }
HLE(D3D_EnumAdapterModes)
{
	/* (this, adapter, index, D3DDISPLAYMODE*) */
	u32 i = ARG(2), m = ARG(3);
	if (i >= NUM_MODES * 2)
		HLE_RET(c, D3DERR_INVALIDCALL, 16);
	ST4(m, Modes[i % NUM_MODES].w);
	ST4(m + 4, Modes[i % NUM_MODES].h);
	ST4(m + 8, 60);
	ST4(m + 12, ModeFormats[i / NUM_MODES]);
	HLE_RET(c, S_OK, 16);
}
HLE(D3D_GetAdapterDisplayMode)
{
	extern s32 g_screen_w, g_screen_h;
	u32 m = ARG(2);
	ST4(m, (u32)g_screen_w);
	ST4(m + 4, (u32)g_screen_h);
	ST4(m + 8, 60);
	ST4(m + 12, D3DFMT_X8R8G8B8);
	HLE_RET(c, S_OK, 12);
}
HLE(D3D_CheckDeviceType) { HLE_RET(c, S_OK, 24); }
HLE(D3D_CheckDeviceFormat)
{
	/* (this, adapter, devtype, adapterfmt, usage, rtype, checkfmt) */
	u32 f = ARG(6);
	int ok = d3d8_format_supported(f, ARG(4), ARG(5));
	HLE_RET(c, ok ? S_OK : D3DERR_NOTAVAILABLE, 28);
}
HLE(D3D_CheckDeviceMultiSampleType) { HLE_RET(c, ARG(5) <= 1 ? S_OK : D3DERR_NOTAVAILABLE, 24); }
HLE(D3D_CheckDepthStencilMatch) { HLE_RET(c, S_OK, 24); }
HLE(D3D_GetDeviceCaps) { d3d8_fill_caps(ARG(3)); HLE_RET(c, S_OK, 16); }
HLE(D3D_GetAdapterMonitor) { HLE_RET(c, 1, 8); }
HLE(D3D_CreateDevice)
{
	/* (this, adapter, devtype, hwnd, behavior, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**) */
	u32 r = d3d8_create_device(c, ARG(3), ARG(4), ARG(5), ARG(6));
	HLE_RET(c, r, 28);
}

static const ComMethod D3D8Methods[] = {
	{ "QueryInterface", hle_D3D_QueryInterface }, { "AddRef", hle_D3D_AddRef }, { "Release", hle_D3D_Release },
	{ "RegisterSoftwareDevice", hle_D3D_RegisterSoftwareDevice }, { "GetAdapterCount", hle_D3D_GetAdapterCount },
	{ "GetAdapterIdentifier", hle_D3D_GetAdapterIdentifier }, { "GetAdapterModeCount", hle_D3D_GetAdapterModeCount },
	{ "EnumAdapterModes", hle_D3D_EnumAdapterModes }, { "GetAdapterDisplayMode", hle_D3D_GetAdapterDisplayMode },
	{ "CheckDeviceType", hle_D3D_CheckDeviceType }, { "CheckDeviceFormat", hle_D3D_CheckDeviceFormat },
	{ "CheckDeviceMultiSampleType", hle_D3D_CheckDeviceMultiSampleType },
	{ "CheckDepthStencilMatch", hle_D3D_CheckDepthStencilMatch }, { "GetDeviceCaps", hle_D3D_GetDeviceCaps },
	{ "GetAdapterMonitor", hle_D3D_GetAdapterMonitor }, { "CreateDevice", hle_D3D_CreateDevice },
};

HLE(Direct3DCreate8)
{
	u32 vt = com_vtable("IDirect3D8", D3D8Methods, sizeof D3D8Methods / sizeof D3D8Methods[0]);
	HLE_RET(c, com_new(vt, 16, NULL), 4);
}

/*-----------------------------------------------------------------------------
	IDirectDraw / IDirectDraw7: only what the launcher needs (video memory).
-----------------------------------------------------------------------------*/

static u32 dd7_new(void);
HLE(DD_QueryInterface)
{
	ST4(ARG(2), dd7_new());
	HLE_RET(c, S_OK, 12);
}
HLE(DD_AddRef) { HLE_RET(c, com_addref(COM_THIS), 4); }
HLE(DD_Release) { HLE_RET(c, com_release(COM_THIS), 4); }
HLE(DD_Unsupported) { recomp_fail(c, R4(REG_EIP), "IDirectDraw method not implemented"); }
HLE(DD7_GetAvailableVidMem)
{
	/* (this, DDSCAPS2*, total*, free*) */
	if (ARG(2))
		ST4(ARG(2), 256u << 20);
	if (ARG(3))
		ST4(ARG(3), 240u << 20);
	HLE_RET(c, S_OK, 16);
}

static u32 dd_vtable(int seven)
{
	ComMethod m[30];
	m[0] = (ComMethod){ "QueryInterface", hle_DD_QueryInterface };
	m[1] = (ComMethod){ "AddRef", hle_DD_AddRef };
	m[2] = (ComMethod){ "Release", hle_DD_Release };
	for (int i = 3; i < 30; i++)
		m[i] = (ComMethod){ "Unsupported", hle_DD_Unsupported };
	if (seven)
		m[23] = (ComMethod){ "GetAvailableVidMem", hle_DD7_GetAvailableVidMem };
	return com_vtable(seven ? "IDirectDraw7" : "IDirectDraw", m, 30);
}
static u32 dd7_new(void) { return com_new(dd_vtable(1), 16, NULL); }

HLE(DirectDrawCreate)
{
	/* (GUID*, LPDIRECTDRAW*, IUnknown*) */
	ST4(ARG(1), com_new(dd_vtable(0), 16, NULL));
	HLE_RET(c, S_OK, 12);
}

const HleEntry hle_d3d8[] = { ENTRY(Direct3DCreate8), ENTRY(DirectDrawCreate), { NULL, NULL } };
