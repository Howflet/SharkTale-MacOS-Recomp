/*=============================================================================
	d3d8_types.h: Direct3D 8 constants and structures (guest layouts).
=============================================================================*/
#pragma once

#include "recomp.h"

enum
{
	D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
	D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_P8 = 41,
	D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62, D3DFMT_Q8W8V8U8 = 63,
	D3DFMT_V16U16 = 64, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75,
	D3DFMT_D16 = 80, D3DFMT_D24X8 = 77, D3DFMT_D24X4S4 = 79, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
	D3DFMT_VERTEXDATA = 100,
	D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844, D3DFMT_DXT3 = 0x33545844,
	D3DFMT_DXT4 = 0x34545844, D3DFMT_DXT5 = 0x35545844,
};

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086cu
#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017cu
#define D3DERR_DEVICELOST 0x88760868u
#define D3DERR_DEVICENOTRESET 0x88760869u

typedef struct D3DCAPS8
{
	u32 DeviceType, AdapterOrdinal, Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps;
	u32 PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps,
		TextureCaps, TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps,
		VolumeTextureAddressCaps, LineCaps;
	u32 MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
	float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
	u32 StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps,
		MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
	float MaxPointSize;
	u32 MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion, MaxVertexShaderConst,
		PixelShaderVersion;
	float MaxPixelShaderValue;
} D3DCAPS8;
_Static_assert(sizeof(D3DCAPS8) == 212, "D3DCAPS8 layout");

int d3d8_format_supported(u32 fmt, u32 usage, u32 rtype);
u32 d3d8_create_device(Ctx* c, u32 hwnd, u32 behavior, u32 params, u32 out_device);
void d3d8_fill_caps(u32 out);
