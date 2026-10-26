/*=============================================================================
	gfx.h: Interface between the Direct3D 8 front end (d3d8_device.c, C) and
	the Metal back end + macOS window/input (gfx_metal.m, Objective-C).

	The front end owns D3D semantics (COM objects, state, guest-memory
	resources); the back end owns the window, GPU resources and drawing.
	Formats crossing this interface are already converted to the back end's
	(BGRA8, BC1-3, R8, RG8).
=============================================================================*/
#pragma once

#include <stdint.h>

/* Texture formats the back end understands. */
enum { GFX_BGRA8 = 1, GFX_BC1, GFX_BC2, GFX_BC3, GFX_R8, GFX_RG8 };

/* Vertex attribute types (D3DVSDT_*). */
enum { VT_FLOAT1 = 0, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_D3DCOLOR, VT_UBYTE4, VT_SHORT2, VT_SHORT4, VT_NONE = 0xff };

/* Fixed-function semantic slots (D3DVSDE_* registers). */
enum { VS_POSITION = 0, VS_BLENDWEIGHT, VS_BLENDINDICES, VS_NORMAL, VS_PSIZE, VS_DIFFUSE, VS_SPECULAR,
	VS_TEXCOORD0, VS_TEXCOORD7 = VS_TEXCOORD0 + 7, VS_COUNT };

#define GFX_MAX_STREAMS 8
#define GFX_MAX_STAGES 8
#define GFX_MAX_LIGHTS 8

typedef struct GfxAttrib { uint8_t stream, type; uint16_t offset; } GfxAttrib;

typedef struct GfxLight
{
	uint32_t type;  /* 0 = off, 1 point, 2 spot, 3 directional */
	float diffuse[4], specular[4], ambient[4];
	float position[3], range;
	float direction[3], falloff;
	float att0, att1, att2, theta, phi;
} GfxLight;

/* Everything a draw needs, snapshotted by the front end. Values are raw
   D3D8 render/texture-stage states so the back end mirrors D3D exactly. */
typedef struct GfxState
{
	uint32_t rs[256];                  /* render states (D3DRS_*) */
	uint32_t tss[GFX_MAX_STAGES][32];  /* texture stage states (D3DTSS_*) */
	float world[16], view[16], proj[16];
	float texmat[GFX_MAX_STAGES][16];
	GfxAttrib attrib[VS_COUNT];        /* VT_NONE = absent */
	int pretransformed;                /* XYZRHW positions */
	float viewport[6];                 /* x, y, w, h, minz, maxz */
	float material[17];                /* diffuse, ambient, specular, emissive (rgba), power */
	GfxLight lights[GFX_MAX_LIGHTS];
	void* textures[GFX_MAX_STAGES];    /* back end texture handles */
	uint32_t texture_formats[GFX_MAX_STAGES];
} GfxState;

/* Window / frame. */
int gfx_init(int width, int height, int fullscreen);
void gfx_resize(int width, int height);
void gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const int* rects, int nrects);
void gfx_present(void);
void gfx_pump_events(void);

/* Textures. */
void* gfx_texture_create(int format, int width, int height, int levels, int is_cube, int render_target);
void gfx_texture_upload(void* tex, int face, int level, int width, int height, const void* data, int pitch);
void gfx_texture_destroy(void* tex);

/* Drawing: prim = D3DPRIMITIVETYPE; streams point at host memory copies. */
void gfx_draw(const GfxState* s, int prim, int vertex_count, const void* const* streams, const uint32_t* strides,
	const uint32_t* stream_sizes, const void* indices, int index_size, int index_count, int base_vertex);

/* Back buffer CPU access (LockRect on the back buffer): BGRA, game resolution. */
void gfx_backbuffer_read(void* dst, int pitch);
void gfx_backbuffer_write(const void* src, int pitch);

/* Debug: save the next presented frame to a PNG. */
void gfx_dump_next_frame(const char* path);
