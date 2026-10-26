/*=============================================================================
	hle_d3d8_device.c: IDirect3DDevice8 and its resources, on top of the
	Metal back end (gfx.h / gfx_metal.m).

	Resources keep their contents in guest memory (so Lock returns guest
	pointers exactly as D3D does) and are uploaded to the GPU when dirty.
	State mirrors D3D8 (render states, texture stage states, transforms,
	lights, material) and is snapshotted into a GfxState per draw.
=============================================================================*/
#include <stdlib.h>

#include "hle_com.h"
#include "d3d8_types.h"
#include "gfx.h"

/*-----------------------------------------------------------------------------
	Formats.
-----------------------------------------------------------------------------*/

int d3d8_format_supported(u32 fmt, u32 usage, u32 rtype)
{
	switch (fmt)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5:
	case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A8L8:
	case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
	case D3DFMT_D16: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_P8: case D3DFMT_V8U8:
		return 1;
	default:
		return 0;
	}
}

static int is_dxt(u32 f) { return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5; }
static int bytes_per_pixel(u32 f)
{
	switch (f)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: return 4;
	case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
	case D3DFMT_D16: case D3DFMT_V8U8: return 2;
	case D3DFMT_R8G8B8: return 3;
	default: return 1;
	}
}
/* Row pitch and total size of one mip level in D3D layout. */
static void level_layout(u32 fmt, u32 w, u32 h, u32* pitch, u32* size)
{
	if (is_dxt(fmt))
	{
		u32 bw = (w + 3) / 4, bh = (h + 3) / 4, block = fmt == D3DFMT_DXT1 ? 8 : 16;
		*pitch = bw * block;
		*size = bw * bh * block;
	}
	else
	{
		*pitch = w * (u32)bytes_per_pixel(fmt);
		*size = *pitch * h;
	}
}
static int gfx_format(u32 f)
{
	switch (f)
	{
	case D3DFMT_DXT1: return GFX_BC1;
	case D3DFMT_DXT2: case D3DFMT_DXT3: return GFX_BC2;
	case D3DFMT_DXT4: case D3DFMT_DXT5: return GFX_BC3;
	default: return GFX_BGRA8;
	}
}

/*-----------------------------------------------------------------------------
	Resources.
-----------------------------------------------------------------------------*/

enum { RT_SURFACE = 1, RT_VOLUME, RT_TEXTURE, RT_VOLUMETEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER };

/* Every host struct starts with its D3DRESOURCETYPE so Release can free it.
   Texture and static buffer contents live in host memory; the guest only
   sees a temporary copy while a level or range is locked (the guest heap is
   far too small to hold a level's worth of textures). Dynamic buffers keep a
   persistent guest copy since they are locked many times per frame. */
typedef struct Texture
{
	u32 rtype, guest, fmt, w, h, levels, usage, pool, faces;
	u8* data[6][16];
	u32 pitch[6][16], size[6][16];
	u32 locked[6][16];    /* guest copy while locked */
	u8 dirty[6][16];
	void* gpu;
	u32 surfaces[6][16];  /* cached guest IDirect3DSurface8 objects (refcounts forward to the texture) */
	int render_target;
} Texture;

typedef struct Surface
{
	u32 rtype, guest;
	Texture* tex;       /* NULL for standalone surfaces */
	u32 face, level;
	u32 fmt, w, h, mem, pitch;
	int kind;           /* 0 texture level, 1 back buffer, 2 depth, 3 image */
	int synced;         /* locked for writing: contents mirror the GPU back buffer */
} Surface;

typedef struct Buffer
{
	u32 rtype, guest, size, usage, fvf, fmt, pool;
	int index;
	u8* data;           /* host view of the contents */
	u32 mem;            /* dynamic buffers: persistent guest copy */
	u32 locked, lock_off, lock_size;
} Buffer;

static Texture* tex_of(u32 obj) { return obj ? (Texture*)com_host(obj) : NULL; }
static Surface* surf_of(u32 obj) { return obj ? (Surface*)com_host(obj) : NULL; }
static Buffer* buf_of(u32 obj) { return obj ? (Buffer*)com_host(obj) : NULL; }

/*-----------------------------------------------------------------------------
	Device state.
-----------------------------------------------------------------------------*/

typedef struct VShader { int used; GfxAttrib attrib[VS_COUNT]; int pretransformed; u32 fvf; } VShader;

static struct Device
{
	u32 guest, d3d;
	u32 width, height, windowed, auto_depth, depth_fmt;
	GfxState s;
	u32 vshader;  /* FVF code or handle */
	u32 streams[GFX_MAX_STREAMS], strides[GFX_MAX_STREAMS];
	u32 indices, base_vertex;
	u32 textures[GFX_MAX_STAGES];
	u8 light_enabled[64];
	float light_data[64][26];
	u8 light_set[64];
	u32 palette[256][256];
	u32 cur_palette;
	u32 backbuffer, depthbuffer, rendertarget, zstencil;
	u32 shader_handles;
	VShader vs[512];
	int in_scene;
	u32 frame;
} D;

static void set_default_state(void)
{
	u32* rs = D.s.rs;
	memset(&D.s, 0, sizeof D.s);
	rs[7] = D.auto_depth ? 1 : 0;  /* ZENABLE */
	rs[8] = 3;                     /* FILLMODE solid */
	rs[9] = 2;                     /* SHADEMODE gouraud */
	rs[14] = 1;                    /* ZWRITEENABLE */
	rs[19] = 2; rs[20] = 1;        /* SRC ONE, DEST ZERO */
	rs[22] = 3;                    /* CULL CCW */
	rs[23] = 4;                    /* ZFUNC LESSEQUAL */
	rs[25] = 8;                    /* ALPHAFUNC ALWAYS */
	rs[26] = 0;
	float one = 1.0f;
	memcpy(&rs[37], &one, 4);      /* FOGEND */
	memcpy(&rs[38], &one, 4);      /* FOGDENSITY */
	rs[56] = 8;                    /* STENCILFUNC ALWAYS */
	rs[53] = rs[54] = rs[55] = 1;  /* STENCIL ops KEEP */
	rs[58] = rs[59] = 0xffffffff;
	rs[60] = 0xffffffff;           /* TEXTUREFACTOR */
	rs[136] = 1;                   /* CLIPPING */
	rs[137] = 1;                   /* LIGHTING */
	rs[141] = 1;                   /* COLORVERTEX */
	rs[142] = 1;                   /* LOCALVIEWER */
	rs[145] = 1; rs[146] = 2;      /* DIFFUSE/SPECULAR MATERIALSOURCE */
	memcpy(&rs[154], &one, 4);     /* POINTSIZE */
	memcpy(&rs[158], &one, 4);     /* POINTSCALE_A */
	rs[162] = 0xffffffff;          /* MULTISAMPLEMASK */
	rs[168] = 0xf;                 /* COLORWRITEENABLE */
	rs[171] = 1;                   /* BLENDOP ADD */
	for (int t = 0; t < GFX_MAX_STAGES; t++)
	{
		u32* ts = D.s.tss[t];
		ts[1] = t == 0 ? 4 : 1;    /* COLOROP: MODULATE / DISABLE */
		ts[2] = 2; ts[3] = 1;      /* COLORARG1 TEXTURE, ARG2 CURRENT */
		ts[4] = t == 0 ? 2 : 1;    /* ALPHAOP: SELECTARG1 / DISABLE */
		ts[5] = 2; ts[6] = 1;
		ts[11] = (u32)t;           /* TEXCOORDINDEX */
		ts[13] = ts[14] = ts[25] = 1;  /* ADDRESS WRAP */
		ts[16] = ts[17] = 1;       /* MAG/MIN POINT */
		ts[18] = 0;                /* MIP NONE */
		ts[21] = 1;                /* MAXANISOTROPY */
		ts[26] = ts[27] = ts[28] = 1;
		for (int k = 0; k < 16; k += 5)
			D.s.texmat[t][k] = 1.0f;
	}
	for (int k = 0; k < 16; k += 5)
		D.s.world[k] = D.s.view[k] = D.s.proj[k] = 1.0f;
	D.s.material[0] = D.s.material[1] = D.s.material[2] = D.s.material[3] = 1.0f;
	D.s.viewport[2] = (float)D.width;
	D.s.viewport[3] = (float)D.height;
	D.s.viewport[5] = 1.0f;
	D.vshader = 0;
	for (int a = 0; a < VS_COUNT; a++)
		D.s.attrib[a].type = VT_NONE;
}

/*-----------------------------------------------------------------------------
	Vertex layouts: FVF codes and vertex shader declarations.
-----------------------------------------------------------------------------*/

static int vt_size(int t)
{
	static const int s[] = { 4, 8, 12, 16, 4, 4, 4, 8 };
	return t <= VT_SHORT4 ? s[t] : 0;
}

static void layout_from_fvf(u32 fvf, GfxAttrib* a, int* pretransformed)
{
	for (int i = 0; i < VS_COUNT; i++)
		a[i].type = VT_NONE;
	u16 off = 0;
	u32 pos = fvf & 0x00e;
	*pretransformed = pos == 0x004;
	if (pos)
	{
		a[VS_POSITION] = (GfxAttrib){ 0, (u8)(pos == 0x004 ? VT_FLOAT4 : VT_FLOAT3), off };
		off += pos == 0x004 ? 16 : 12;
		if (pos >= 0x006)
		{
			int betas = (int)(pos - 0x004) / 2;  /* XYZB1..B5 */
			int weights = (fvf & 0x1000) ? betas - 1 : betas;
			if (weights > 0)
				a[VS_BLENDWEIGHT] = (GfxAttrib){ 0, (u8)(VT_FLOAT1 + weights - 1), off };
			off += (u16)(4 * weights);
			if (fvf & 0x1000)
			{
				a[VS_BLENDINDICES] = (GfxAttrib){ 0, VT_UBYTE4, off };
				off += 4;
			}
		}
	}
	if (fvf & 0x010) { a[VS_NORMAL] = (GfxAttrib){ 0, VT_FLOAT3, off }; off += 12; }
	if (fvf & 0x020) { a[VS_PSIZE] = (GfxAttrib){ 0, VT_FLOAT1, off }; off += 4; }
	if (fvf & 0x040) { a[VS_DIFFUSE] = (GfxAttrib){ 0, VT_D3DCOLOR, off }; off += 4; }
	if (fvf & 0x080) { a[VS_SPECULAR] = (GfxAttrib){ 0, VT_D3DCOLOR, off }; off += 4; }
	int ntex = (int)((fvf >> 8) & 0xf);
	for (int t = 0; t < ntex && t < 8; t++)
	{
		static const int fmt[4] = { VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_FLOAT1 };
		int f = fmt[(fvf >> (16 + 2 * t)) & 3];
		a[VS_TEXCOORD0 + t] = (GfxAttrib){ 0, (u8)f, off };
		off += (u16)vt_size(f);
	}
}

static void layout_from_decl(u32 decl, VShader* v)
{
	for (int i = 0; i < VS_COUNT; i++)
		v->attrib[i].type = VT_NONE;
	v->pretransformed = 0;
	u32 stream = 0;
	u16 off[GFX_MAX_STREAMS] = { 0 };
	for (u32 p = decl;; p += 4)
	{
		u32 tok = LD4(p);
		if (tok == 0xffffffffu)
			break;
		u32 kind = tok >> 29;
		if (kind == 1)
		{
			stream = tok & 0xf;  /* D3DVSD_STREAM */
		}
		else if (kind == 2)
		{
			if (tok & 0x10000000u)
			{
				off[stream] += (u16)(4 * ((tok >> 16) & 0xf));  /* skip dwords */
			}
			else
			{
				u32 type = (tok >> 16) & 0xf, reg = tok & 0x1f;
				if (reg < VS_COUNT && stream < GFX_MAX_STREAMS)
					v->attrib[reg] = (GfxAttrib){ (u8)stream, (u8)type, off[stream] };
				off[stream] += (u16)vt_size((int)type);
			}
		}
		else if (kind == 4)
		{
			p += 4 * ((tok >> 25) & 0xf) * 4;  /* constant memory: count*4 dwords follow */
		}
		else if (kind == 5)
		{
			p += 4 * ((tok >> 24) & 0x1f);
		}
	}
}

/*-----------------------------------------------------------------------------
	Texture upload (format conversion to BGRA8 for non-DXT formats).
-----------------------------------------------------------------------------*/

static void upload_level(Texture* t, u32 face, u32 level)
{
	u32 w = t->w >> level, h = t->h >> level;
	if (!w) w = 1;
	if (!h) h = 1;
	const u8* src = t->data[face][level];
	if (is_dxt(t->fmt) || t->fmt == D3DFMT_A8R8G8B8)
	{
		gfx_texture_upload(t->gpu, (int)face, (int)level, (int)w, (int)h, src, (int)t->pitch[face][level]);
		return;
	}
	u32* tmp = malloc((size_t)w * h * 4);
	for (u32 y = 0; y < h; y++)
	{
		const u8* row = src + y * t->pitch[face][level];
		u32* out = tmp + y * w;
		for (u32 x = 0; x < w; x++)
		{
			u32 p = 0;
			switch (t->fmt)
			{
			case D3DFMT_X8R8G8B8: { u32 v; memcpy(&v, row + 4 * x, 4); p = v | 0xff000000u; break; }
			case D3DFMT_R5G6B5:
			{
				u16 v; memcpy(&v, row + 2 * x, 2);
				u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
				p = 0xff000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
				break;
			}
			case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
			{
				u16 v; memcpy(&v, row + 2 * x, 2);
				u32 a = t->fmt == D3DFMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
				u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
				p = (a << 24) | ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
				break;
			}
			case D3DFMT_A4R4G4B4:
			{
				u16 v; memcpy(&v, row + 2 * x, 2);
				p = ((u32)((v >> 12) & 15) * 17 << 24) | ((u32)((v >> 8) & 15) * 17 << 16) |
					((u32)((v >> 4) & 15) * 17 << 8) | (u32)(v & 15) * 17;
				break;
			}
			case D3DFMT_A8: p = (u32)row[x] << 24; break;
			case D3DFMT_L8: p = 0xff000000u | (u32)row[x] * 0x010101u; break;
			case D3DFMT_A8L8: p = ((u32)row[2 * x + 1] << 24) | (u32)row[2 * x] * 0x010101u; break;
			case D3DFMT_P8:
			{
				u32 e = D.palette[D.cur_palette & 255][row[x]];  /* PALETTEENTRY r,g,b,flags (alpha) */
				p = ((e >> 24) << 24) | ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
				break;
			}
			case D3DFMT_V8U8: p = 0xff000000u | ((u32)(u8)(row[2 * x] + 128) << 16) | ((u32)(u8)(row[2 * x + 1] + 128) << 8); break;
			default: p = 0xffff00ffu; break;
			}
			out[x] = p;
		}
	}
	gfx_texture_upload(t->gpu, (int)face, (int)level, (int)w, (int)h, tmp, (int)w * 4);
	free(tmp);
}

static void flush_texture(Texture* t)
{
	if (!t || !t->gpu)
		return;
	for (u32 f = 0; f < t->faces; f++)
		for (u32 l = 0; l < t->levels; l++)
			if (t->dirty[f][l])
			{
				upload_level(t, f, l);
				t->dirty[f][l] = 0;
			}
}

/*-----------------------------------------------------------------------------
	Resource COM objects.
-----------------------------------------------------------------------------*/

static u32 resource_vtable_tex(void);
static u32 resource_vtable_cube(void);
static u32 resource_vtable_vb(void);
static u32 resource_vtable_ib(void);
static u32 resource_vtable_surface(void);

static u32 new_texture(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool, int cube)
{
	Texture* t = calloc(1, sizeof *t);
	t->rtype = cube ? RT_CUBETEXTURE : RT_TEXTURE;
	t->w = w;
	t->h = cube ? w : h;
	t->fmt = fmt;
	t->usage = usage;
	t->pool = pool;
	t->faces = cube ? 6 : 1;
	if (!levels)
	{
		u32 m = w > t->h ? w : t->h;
		while (m)
		{
			levels++;
			m >>= 1;
		}
	}
	t->levels = levels > 16 ? 16 : levels;
	t->render_target = (usage & 1) != 0;  /* D3DUSAGE_RENDERTARGET */
	for (u32 f = 0; f < t->faces; f++)
		for (u32 l = 0; l < t->levels; l++)
		{
			u32 lw = t->w >> l, lh = t->h >> l;
			level_layout(fmt, lw ? lw : 1, lh ? lh : 1, &t->pitch[f][l], &t->size[f][l]);
			t->data[f][l] = calloc(1, t->size[f][l]);
		}
	t->gpu = gfx_texture_create(gfx_format(fmt), (int)t->w, (int)t->h, (int)t->levels, cube, t->render_target);
	t->guest = com_new(cube ? resource_vtable_cube() : resource_vtable_tex(), 16, t);
	return t->guest;
}

static u32 surface_for(Texture* t, u32 face, u32 level)
{
	com_addref(t->guest);
	if (t->surfaces[face][level])
		return t->surfaces[face][level];
	Surface* s = calloc(1, sizeof *s);
	s->rtype = RT_SURFACE;
	s->tex = t;
	s->face = face;
	s->level = level;
	s->fmt = t->fmt;
	s->w = t->w >> level ? t->w >> level : 1;
	s->h = t->h >> level ? t->h >> level : 1;
	s->pitch = t->pitch[face][level];
	s->guest = com_new(resource_vtable_surface(), 16, s);
	t->surfaces[face][level] = s->guest;
	return s->guest;
}

static u32 new_surface(int kind, u32 w, u32 h, u32 fmt)
{
	Surface* s = calloc(1, sizeof *s);
	s->rtype = RT_SURFACE;
	s->kind = kind;
	s->w = w;
	s->h = h;
	s->fmt = fmt;
	u32 size;
	level_layout(fmt, w, h, &s->pitch, &size);
	s->mem = rt_heap_alloc(size);
	memset(GP(s->mem), 0, size);
	s->guest = com_new(resource_vtable_surface(), 16, s);
	return s->guest;
}

/* IUnknown + IDirect3DResource8 common methods. */
static u32 res_addref(u32 obj);
HLE(R_QueryInterface) { ST4(ARG(2), COM_THIS); res_addref(COM_THIS); HLE_RET(c, S_OK, 12); }
static void destroy_resource(u32 obj)
{
	u32 rtype = *(u32*)com_host(obj);
	if (rtype == RT_TEXTURE || rtype == RT_CUBETEXTURE)
	{
		Texture* t = tex_of(obj);
		for (u32 f = 0; f < t->faces; f++)
			for (u32 l = 0; l < t->levels; l++)
			{
				free(t->data[f][l]);
				if (t->locked[f][l])
					rt_heap_free(t->locked[f][l]);
				if (t->surfaces[f][l])
				{
					free(com_host(t->surfaces[f][l]));
					rt_heap_free(t->surfaces[f][l]);
				}
			}
		gfx_texture_destroy(t->gpu);
		free(t);
	}
	else if (rtype == RT_SURFACE)
	{
		Surface* s = surf_of(obj);
		rt_heap_free(s->mem);
		free(s);
	}
	else
	{
		Buffer* b = buf_of(obj);
		if (b->mem)
			rt_heap_free(b->mem);
		else
			free(b->data);
		if (b->locked)
			rt_heap_free(b->locked);
		free(b);
	}
	rt_heap_free(obj);
}

/* Texture level surfaces share their container's reference count, as in D3D. */
static u32 ref_target(u32 obj)
{
	Surface* s = surf_of(obj);
	return s && s->rtype == RT_SURFACE && s->tex ? s->tex->guest : obj;
}
static u32 res_addref(u32 obj) { return obj ? com_addref(ref_target(obj)) : 0; }
static u32 res_release(u32 obj)
{
	if (!obj)
		return 0;
	obj = ref_target(obj);
	u32 n = com_release(obj);
	if (!n)
		destroy_resource(obj);
	return n;
}
/* Device bindings hold a reference, as in D3D. */
static void rebind(u32* slot, u32 obj)
{
	if (*slot == obj)
		return;
	res_addref(obj);
	res_release(*slot);
	*slot = obj;
}

HLE(R_AddRef) { HLE_RET(c, res_addref(COM_THIS), 4); }
HLE(R_Release) { HLE_RET(c, res_release(COM_THIS), 4); }
HLE(R_GetDevice) { ST4(ARG(1), D.guest); com_addref(D.guest); HLE_RET(c, S_OK, 8); }
HLE(R_SetPrivateData) { HLE_RET(c, S_OK, 20); }
HLE(R_GetPrivateData) { HLE_RET(c, D3DERR_NOTAVAILABLE, 16); }
HLE(R_FreePrivateData) { HLE_RET(c, S_OK, 8); }
HLE(R_SetPriority) { HLE_RET(c, 0, 8); }
HLE(R_GetPriority) { HLE_RET(c, 0, 4); }
HLE(R_PreLoad) { HLE_RET(c, 0, 4); }
HLE(T_GetType) { HLE_RET(c, tex_of(COM_THIS)->rtype, 4); }
HLE(T_SetLOD) { HLE_RET(c, 0, 8); }
HLE(T_GetLOD) { HLE_RET(c, 0, 4); }
HLE(T_GetLevelCount) { HLE_RET(c, tex_of(COM_THIS)->levels, 4); }

static void fill_surface_desc(u32 d, u32 fmt, u32 type, u32 usage, u32 pool, u32 w, u32 h)
{
	u32 pitch, size;
	level_layout(fmt, w, h, &pitch, &size);
	u32 v[8] = { fmt, type, usage, pool, size, 0, w, h };
	for (int i = 0; i < 8; i++)
		ST4(d + 4 * (u32)i, v[i]);
}

HLE(Tex_GetLevelDesc)
{
	Texture* t = tex_of(COM_THIS);
	u32 l = ARG(1);
	fill_surface_desc(ARG(2), t->fmt, RT_SURFACE, t->usage, t->pool, t->w >> l ? t->w >> l : 1, t->h >> l ? t->h >> l : 1);
	HLE_RET(c, S_OK, 12);
}
HLE(Tex_GetSurfaceLevel) { ST4(ARG(2), surface_for(tex_of(COM_THIS), 0, ARG(1))); HLE_RET(c, S_OK, 12); }
static void lock_level(Texture* t, u32 face, u32 level, u32 out, u32 rect)
{
	if (face >= t->faces || level >= t->levels)
	{
		ST4(out, 0);
		ST4(out + 4, 0);
		return;
	}
	if (!t->locked[face][level])
	{
		t->locked[face][level] = rt_heap_alloc(t->size[face][level]);
		memcpy(GP(t->locked[face][level]), t->data[face][level], t->size[face][level]);
	}
	u32 base = t->locked[face][level], pitch = t->pitch[face][level];
	if (rect)
	{
		u32 x = LD4(rect), y = LD4(rect + 4);
		if (is_dxt(t->fmt))
			base += (y / 4) * pitch + (x / 4) * (t->fmt == D3DFMT_DXT1 ? 8 : 16);
		else
			base += y * pitch + x * (u32)bytes_per_pixel(t->fmt);
	}
	ST4(out, pitch);
	ST4(out + 4, base);
}
static void unlock_level(Texture* t, u32 face, u32 level)
{
	if (face >= t->faces || level >= t->levels || !t->locked[face][level])
		return;
	memcpy(t->data[face][level], GP(t->locked[face][level]), t->size[face][level]);
	rt_heap_free(t->locked[face][level]);
	t->locked[face][level] = 0;
	t->dirty[face][level] = 1;
}
HLE(Tex_LockRect) { lock_level(tex_of(COM_THIS), 0, ARG(1), ARG(2), ARG(3)); HLE_RET(c, S_OK, 20); }
HLE(Tex_UnlockRect) { unlock_level(tex_of(COM_THIS), 0, ARG(1)); HLE_RET(c, S_OK, 8); }
HLE(Tex_AddDirtyRect) { tex_of(COM_THIS)->dirty[0][0] = 1; HLE_RET(c, S_OK, 8); }
HLE(Cube_GetLevelDesc)
{
	Texture* t = tex_of(COM_THIS);
	u32 l = ARG(1);
	fill_surface_desc(ARG(2), t->fmt, RT_SURFACE, t->usage, t->pool, t->w >> l ? t->w >> l : 1, t->w >> l ? t->w >> l : 1);
	HLE_RET(c, S_OK, 12);
}
HLE(Cube_GetCubeMapSurface) { ST4(ARG(3), surface_for(tex_of(COM_THIS), ARG(1), ARG(2))); HLE_RET(c, S_OK, 16); }
HLE(Cube_LockRect) { lock_level(tex_of(COM_THIS), ARG(1), ARG(2), ARG(3), ARG(4)); HLE_RET(c, S_OK, 24); }
HLE(Cube_UnlockRect) { unlock_level(tex_of(COM_THIS), ARG(1), ARG(2)); HLE_RET(c, S_OK, 12); }
HLE(Cube_AddDirtyRect) { tex_of(COM_THIS)->dirty[ARG(1)][0] = 1; HLE_RET(c, S_OK, 12); }

/* Vertex/index buffers. */
HLE(B_GetType) { HLE_RET(c, buf_of(COM_THIS)->index ? RT_INDEXBUFFER : RT_VERTEXBUFFER, 4); }
static void unlock_buffer(Buffer* b)
{
	if (!b->locked)
		return;
	memcpy(b->data + b->lock_off, GP(b->locked), b->lock_size);
	rt_heap_free(b->locked);
	b->locked = 0;
}
HLE(B_Lock)
{
	/* (this, offset, size, ptr*, flags) */
	Buffer* b = buf_of(COM_THIS);
	u32 off = ARG(1) < b->size ? ARG(1) : 0, size = ARG(2);
	if (!size || off + size > b->size)
		size = b->size - off;
	if (b->mem)
	{
		ST4(ARG(3), b->mem + off);
		HLE_RET(c, S_OK, 20);
	}
	unlock_buffer(b);
	b->locked = rt_heap_alloc(size);
	b->lock_off = off;
	b->lock_size = size;
	memcpy(GP(b->locked), b->data + off, size);
	ST4(ARG(3), b->locked);
	HLE_RET(c, S_OK, 20);
}
HLE(B_Unlock) { unlock_buffer(buf_of(COM_THIS)); HLE_RET(c, S_OK, 4); }
HLE(VB_GetDesc)
{
	Buffer* b = buf_of(COM_THIS);
	u32 d = ARG(1);
	u32 v[6] = { D3DFMT_VERTEXDATA, RT_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf };
	for (int i = 0; i < 6; i++)
		ST4(d + 4 * (u32)i, v[i]);
	HLE_RET(c, S_OK, 8);
}
HLE(IB_GetDesc)
{
	Buffer* b = buf_of(COM_THIS);
	u32 d = ARG(1);
	u32 v[5] = { b->fmt, RT_INDEXBUFFER, b->usage, b->pool, b->size };
	for (int i = 0; i < 5; i++)
		ST4(d + 4 * (u32)i, v[i]);
	HLE_RET(c, S_OK, 8);
}

/* Surfaces. */
HLE(S_GetContainer)
{
	Surface* s = surf_of(COM_THIS);
	if (!s->tex)
		HLE_RET(c, E_NOINTERFACE, 12);
	ST4(ARG(2), s->tex->guest);
	res_addref(s->tex->guest);
	HLE_RET(c, S_OK, 12);
}
HLE(S_GetDesc)
{
	Surface* s = surf_of(COM_THIS);
	u32 usage = s->kind == 1 ? 1 : s->kind == 2 ? 2 : 0;
	fill_surface_desc(ARG(1), s->fmt, RT_SURFACE, usage, s->kind == 3 ? 2 : 0, s->w, s->h);
	HLE_RET(c, S_OK, 8);
}
HLE(S_LockRect)
{
	Surface* s = surf_of(COM_THIS);
	if (s->tex)
	{
		lock_level(s->tex, s->face, s->level, ARG(1), ARG(2));
		HLE_RET(c, S_OK, 16);
	}
	if (s->guest == D.backbuffer && !(ARG(3) & 0x10))  /* not D3DLOCK_READONLY: real CPU access */
	{
		gfx_backbuffer_read(GP(s->mem), (int)s->pitch);
		s->synced = 1;
	}
	u32 base = s->mem;
	if (ARG(2))
		base += LD4(ARG(2) + 4) * s->pitch + LD4(ARG(2)) * (u32)bytes_per_pixel(s->fmt);
	ST4(ARG(1), s->pitch);
	ST4(ARG(1) + 4, base);
	HLE_RET(c, S_OK, 16);
}
HLE(S_UnlockRect)
{
	Surface* s = surf_of(COM_THIS);
	if (s->tex)
		unlock_level(s->tex, s->face, s->level);
	if (s->synced)
	{
		gfx_backbuffer_write(GP(s->mem), (int)s->pitch);
		s->synced = 0;
	}
	HLE_RET(c, S_OK, 4);
}

#define RES_COMMON \
	{ "QueryInterface", hle_R_QueryInterface }, { "AddRef", hle_R_AddRef }, { "Release", hle_R_Release }, \
	{ "GetDevice", hle_R_GetDevice }, { "SetPrivateData", hle_R_SetPrivateData }, \
	{ "GetPrivateData", hle_R_GetPrivateData }, { "FreePrivateData", hle_R_FreePrivateData }

static u32 resource_vtable_tex(void)
{
	static const ComMethod m[] = { RES_COMMON, { "SetPriority", hle_R_SetPriority }, { "GetPriority", hle_R_GetPriority },
		{ "PreLoad", hle_R_PreLoad }, { "GetType", hle_T_GetType }, { "SetLOD", hle_T_SetLOD }, { "GetLOD", hle_T_GetLOD },
		{ "GetLevelCount", hle_T_GetLevelCount }, { "GetLevelDesc", hle_Tex_GetLevelDesc },
		{ "GetSurfaceLevel", hle_Tex_GetSurfaceLevel }, { "LockRect", hle_Tex_LockRect },
		{ "UnlockRect", hle_Tex_UnlockRect }, { "AddDirtyRect", hle_Tex_AddDirtyRect } };
	return com_vtable("IDirect3DTexture8", m, sizeof m / sizeof m[0]);
}
static u32 resource_vtable_cube(void)
{
	static const ComMethod m[] = { RES_COMMON, { "SetPriority", hle_R_SetPriority }, { "GetPriority", hle_R_GetPriority },
		{ "PreLoad", hle_R_PreLoad }, { "GetType", hle_T_GetType }, { "SetLOD", hle_T_SetLOD }, { "GetLOD", hle_T_GetLOD },
		{ "GetLevelCount", hle_T_GetLevelCount }, { "GetLevelDesc", hle_Cube_GetLevelDesc },
		{ "GetCubeMapSurface", hle_Cube_GetCubeMapSurface }, { "LockRect", hle_Cube_LockRect },
		{ "UnlockRect", hle_Cube_UnlockRect }, { "AddDirtyRect", hle_Cube_AddDirtyRect } };
	return com_vtable("IDirect3DCubeTexture8", m, sizeof m / sizeof m[0]);
}
static u32 resource_vtable_vb(void)
{
	static const ComMethod m[] = { RES_COMMON, { "SetPriority", hle_R_SetPriority }, { "GetPriority", hle_R_GetPriority },
		{ "PreLoad", hle_R_PreLoad }, { "GetType", hle_B_GetType }, { "Lock", hle_B_Lock }, { "Unlock", hle_B_Unlock },
		{ "GetDesc", hle_VB_GetDesc } };
	return com_vtable("IDirect3DVertexBuffer8", m, sizeof m / sizeof m[0]);
}
static u32 resource_vtable_ib(void)
{
	static const ComMethod m[] = { RES_COMMON, { "SetPriority", hle_R_SetPriority }, { "GetPriority", hle_R_GetPriority },
		{ "PreLoad", hle_R_PreLoad }, { "GetType", hle_B_GetType }, { "Lock", hle_B_Lock }, { "Unlock", hle_B_Unlock },
		{ "GetDesc", hle_IB_GetDesc } };
	return com_vtable("IDirect3DIndexBuffer8", m, sizeof m / sizeof m[0]);
}
static u32 resource_vtable_surface(void)
{
	static const ComMethod m[] = { RES_COMMON, { "GetContainer", hle_S_GetContainer }, { "GetDesc", hle_S_GetDesc },
		{ "LockRect", hle_S_LockRect }, { "UnlockRect", hle_S_UnlockRect } };
	return com_vtable("IDirect3DSurface8", m, sizeof m / sizeof m[0]);
}

/*-----------------------------------------------------------------------------
	Drawing.
-----------------------------------------------------------------------------*/

static u32 vertex_count(u32 prim, u32 n)
{
	switch (prim)
	{
	case 1: return n;          /* POINTLIST */
	case 2: return 2 * n;      /* LINELIST */
	case 3: return n + 1;      /* LINESTRIP */
	case 4: return 3 * n;      /* TRIANGLELIST */
	default: return n + 2;     /* STRIP, FAN */
	}
}

static void current_layout(GfxAttrib* a, int* pre)
{
	if (D.vshader & 1)
	{
		VShader* v = &D.vs[(D.vshader >> 1) & 511];
		memcpy(a, v->attrib, sizeof v->attrib);
		*pre = v->pretransformed;
	}
	else
		layout_from_fvf(D.vshader, a, pre);
}

static void snapshot(void)
{
	current_layout(D.s.attrib, &D.s.pretransformed);
	int n = 0;
	for (int i = 0; i < 64 && n < GFX_MAX_LIGHTS; i++)
		if (D.light_enabled[i] && D.light_set[i])
		{
			GfxLight* L = &D.s.lights[n++];
			const float* d = D.light_data[i];  /* D3DLIGHT8 as floats (type first as int) */
			u32 type;
			memcpy(&type, &d[0], 4);
			L->type = type;
			memcpy(L->diffuse, &d[1], 16);
			memcpy(L->specular, &d[5], 16);
			memcpy(L->ambient, &d[9], 16);
			memcpy(L->position, &d[13], 12);
			memcpy(L->direction, &d[16], 12);
			L->range = d[19];
			L->falloff = d[20];
			L->att0 = d[21];
			L->att1 = d[22];
			L->att2 = d[23];
			L->theta = d[24];
			L->phi = d[25];
		}
	for (; n < GFX_MAX_LIGHTS; n++)
		D.s.lights[n].type = 0;
	for (int t = 0; t < GFX_MAX_STAGES; t++)
	{
		Texture* tex = tex_of(D.textures[t]);
		flush_texture(tex);
		D.s.textures[t] = tex ? tex->gpu : NULL;
		D.s.texture_formats[t] = tex ? tex->fmt : 0;
	}
}

/* Copy the referenced vertex range of every used stream into host memory. */
static void gather_streams(u32 first, u32 count, const void* ptrs[], u32 sizes[])
{
	u8 used[GFX_MAX_STREAMS] = { 0 };
	for (int a = 0; a < VS_COUNT; a++)
		if (D.s.attrib[a].type != VT_NONE)
			used[D.s.attrib[a].stream] = 1;
	for (int s = 0; s < GFX_MAX_STREAMS; s++)
	{
		ptrs[s] = NULL;
		sizes[s] = 0;
		Buffer* b = buf_of(D.streams[s]);
		if (!used[s] || !b)
			continue;
		ptrs[s] = b->data + first * D.strides[s];
		sizes[s] = count * D.strides[s];
	}
}

/* SHARK_TRACE_DRAWS=<frame> (or the script's 't' action): describe every draw of that frame. */
static int TraceFrame = -2;
void d3d8_trace_next_frame(void) { TraceFrame = (int)D.frame + 1; }
static void trace_draw(Ctx* c, u32 prim, u32 nverts, const void* const* streams, const u32* strides, u32 icount)
{
	if (TraceFrame == -2)
		TraceFrame = getenv("SHARK_TRACE_DRAWS") ? atoi(getenv("SHARK_TRACE_DRAWS")) : -1;
	if (TraceFrame < 0 || D.frame != (u32)TraceFrame)
		return;
	fprintf(stderr, "[draw] caller ebp %#x pass %u numpasses? esp %#x\n", R4(REG_EBP), LD4(R4(REG_EBP) - 0x1c), R4(REG_ESP));
	fprintf(stderr, "[draw] prim %u verts %u idx %u vs %#x pre %d light %u z %u/%u cull %u blend %u %u/%u atest %u fog %u\n",
		prim, nverts, icount, D.vshader, D.s.pretransformed, D.s.rs[137], D.s.rs[7], D.s.rs[14], D.s.rs[22], D.s.rs[27],
		D.s.rs[19], D.s.rs[20], D.s.rs[15], D.s.rs[28]);
	for (int a = 0; a < VS_COUNT; a++)
		if (D.s.attrib[a].type != VT_NONE)
			fprintf(stderr, "    attr %d: stream %u type %u off %u\n", a, D.s.attrib[a].stream, D.s.attrib[a].type,
				D.s.attrib[a].offset);
	for (int v = 0; v < 3 && v < (int)nverts && streams[0]; v++)
	{
		const float* f = (const float*)((const u8*)streams[0] + v * strides[0]);
		fprintf(stderr, "    v%d: %g %g %g %g | %08x %g %g\n", v, f[0], f[1], f[2], f[3], ((const u32*)f)[3], f[4], f[5]);
	}
	for (int t = 0; t < 2; t++)
	{
		Texture* tx = tex_of(D.textures[t]);
		fprintf(stderr, "    stage %d: tex %ux%u fmt %#x  op %u %u/%u alpha %u %u/%u tci %#x ttf %#x\n", t, tx ? tx->w : 0,
			tx ? tx->h : 0, tx ? tx->fmt : 0, D.s.tss[t][1], D.s.tss[t][2], D.s.tss[t][3], D.s.tss[t][4], D.s.tss[t][5],
			D.s.tss[t][6], D.s.tss[t][11], D.s.tss[t][24]);
	}
	const float* m[3] = { D.s.world, D.s.view, D.s.proj };
	for (int i = 0; i < 3; i++)
		fprintf(stderr, "    %s: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n", i == 0 ? "world" : i == 1 ? "view" : "proj",
			m[i][0], m[i][1], m[i][2], m[i][3], m[i][4], m[i][5], m[i][6], m[i][7], m[i][8], m[i][9], m[i][10], m[i][11],
			m[i][12], m[i][13], m[i][14], m[i][15]);
	fprintf(stderr, "    viewport %g %g %g %g %g %g\n", D.s.viewport[0], D.s.viewport[1], D.s.viewport[2], D.s.viewport[3],
		D.s.viewport[4], D.s.viewport[5]);
}

static void draw(Ctx* c, u32 prim, u32 nverts, const void* const* streams, const u32* strides, const u32* sizes,
	const void* idx, int isize, u32 icount)
{
	trace_draw(c, prim, nverts, streams, strides, icount);
	if (prim == 6)
	{
		/* Triangle fan -> list. */
		u32 tris = icount ? icount - 2 : nverts - 2;
		u32* list = malloc(sizeof(u32) * 3 * tris);
		for (u32 i = 0; i < tris; i++)
		{
			u32 a = 0, b = i + 1, cc = i + 2;
			if (idx)
			{
				a = isize == 2 ? ((const u16*)idx)[0] : ((const u32*)idx)[0];
				b = isize == 2 ? ((const u16*)idx)[i + 1] : ((const u32*)idx)[i + 1];
				cc = isize == 2 ? ((const u16*)idx)[i + 2] : ((const u32*)idx)[i + 2];
			}
			list[3 * i] = a;
			list[3 * i + 1] = b;
			list[3 * i + 2] = cc;
		}
		gfx_draw(&D.s, 4, (int)nverts, streams, strides, sizes, list, 4, (int)(3 * tris), 0);
		free(list);
		return;
	}
	gfx_draw(&D.s, (int)prim, (int)nverts, streams, strides, sizes, idx, isize, (int)icount, 0);
}

HLE(Dev_DrawPrimitive)
{
	u32 prim = ARG(1), start = ARG(2), n = vertex_count(prim, ARG(3));
	snapshot();
	const void* ptrs[GFX_MAX_STREAMS];
	u32 sizes[GFX_MAX_STREAMS];
	gather_streams(start, n, ptrs, sizes);
	draw(c, prim, n, ptrs, D.strides, sizes, NULL, 0, 0);
	HLE_RET(c, S_OK, 16);
}

HLE(Dev_DrawIndexedPrimitive)
{
	/* (this, type, minIndex, numVertices, startIndex, primCount) */
	u32 prim = ARG(1), minIndex = ARG(2), numVerts = ARG(3), startIndex = ARG(4);
	u32 icount = vertex_count(prim, ARG(5));
	Buffer* ib = buf_of(D.indices);
	if (!ib)
		HLE_RET(c, D3DERR_INVALIDCALL, 24);
	snapshot();
	const void* ptrs[GFX_MAX_STREAMS];
	u32 sizes[GFX_MAX_STREAMS];
	gather_streams(D.base_vertex + minIndex, numVerts, ptrs, sizes);
	int isize = ib->fmt == D3DFMT_INDEX32 ? 4 : 2;
	/* Rebase indices to the copied vertex range. */
	u32* idx = malloc(4 * icount);
	const u8* src = ib->data + startIndex * (u32)isize;
	for (u32 i = 0; i < icount; i++)
	{
		u32 v = isize == 2 ? ((const u16*)src)[i] : ((const u32*)src)[i];
		idx[i] = v - minIndex;
	}
	draw(c, prim, numVerts, ptrs, D.strides, sizes, idx, 4, icount);
	free(idx);
	HLE_RET(c, S_OK, 24);
}

HLE(Dev_DrawPrimitiveUP)
{
	/* (this, type, primCount, data, stride) */
	u32 prim = ARG(1), n = vertex_count(prim, ARG(2)), stride = ARG(4);
	snapshot();
	const void* ptrs[GFX_MAX_STREAMS] = { GP(ARG(3)) };
	u32 strides[GFX_MAX_STREAMS] = { stride }, sizes[GFX_MAX_STREAMS] = { n * stride };
	draw(c, prim, n, ptrs, strides, sizes, NULL, 0, 0);
	rebind(&D.streams[0], 0);  /* D3D clears stream 0 after *UP draws */
	HLE_RET(c, S_OK, 20);
}

HLE(Dev_DrawIndexedPrimitiveUP)
{
	/* (this, type, minIndex, numVertices, primCount, indexData, indexFormat, vertexData, stride) */
	u32 prim = ARG(1), minIndex = ARG(2), numVerts = ARG(3), icount = vertex_count(prim, ARG(4));
	u32 stride = ARG(8);
	int isize = ARG(6) == D3DFMT_INDEX32 ? 4 : 2;
	snapshot();
	const void* ptrs[GFX_MAX_STREAMS] = { GP(ARG(7) + minIndex * stride) };
	u32 strides[GFX_MAX_STREAMS] = { stride }, sizes[GFX_MAX_STREAMS] = { numVerts * stride };
	u32* idx = malloc(4 * icount);
	const u8* src = GP(ARG(5));
	for (u32 i = 0; i < icount; i++)
		idx[i] = (isize == 2 ? ((const u16*)src)[i] : ((const u32*)src)[i]) - minIndex;
	draw(c, prim, numVerts, ptrs, strides, sizes, idx, 4, icount);
	free(idx);
	rebind(&D.streams[0], 0);
	rebind(&D.indices, 0);
	HLE_RET(c, S_OK, 36);
}

/*-----------------------------------------------------------------------------
	Device methods.
-----------------------------------------------------------------------------*/

HLE(Dev_QueryInterface) { HLE_RET(c, E_NOINTERFACE, 12); }
HLE(Dev_AddRef) { HLE_RET(c, com_addref(COM_THIS), 4); }
HLE(Dev_Release) { HLE_RET(c, com_release(COM_THIS), 4); }
HLE(Dev_TestCooperativeLevel) { HLE_RET(c, S_OK, 4); }
HLE(Dev_GetAvailableTextureMem) { HLE_RET(c, 256u << 20, 4); }
HLE(Dev_ResourceManagerDiscardBytes) { HLE_RET(c, S_OK, 8); }
HLE(Dev_GetDirect3D) { ST4(ARG(1), D.d3d); com_addref(D.d3d); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetDeviceCaps) { d3d8_fill_caps(ARG(1)); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetDisplayMode)
{
	u32 m = ARG(1);
	ST4(m, D.width);
	ST4(m + 4, D.height);
	ST4(m + 8, 60);
	ST4(m + 12, D3DFMT_X8R8G8B8);
	HLE_RET(c, S_OK, 8);
}
HLE(Dev_GetCreationParameters) { memset(GP(ARG(1)), 0, 16); ST4(ARG(1) + 4, 1); HLE_RET(c, S_OK, 8); }
HLE(Dev_SetCursorProperties) { HLE_RET(c, S_OK, 16); }
HLE(Dev_SetCursorPosition) { HLE_RET(c, 0, 16); }
HLE(Dev_ShowCursor) { HLE_RET(c, 0, 8); }
HLE(Dev_CreateAdditionalSwapChain) { HLE_RET(c, D3DERR_NOTAVAILABLE, 12); }
HLE(Dev_Reset)
{
	u32 pp = ARG(1);
	D.width = LD4(pp) ? LD4(pp) : D.width;
	D.height = LD4(pp + 4) ? LD4(pp + 4) : D.height;
	gfx_resize((int)D.width, (int)D.height);
	set_default_state();
	HLE_RET(c, S_OK, 8);
}
HLE(Dev_Present)
{
	extern void rt_console_run_pending(Ctx* c);
	rt_console_run_pending(c);
	gfx_present();
	D.frame++;
	if (D.frame % 20000 == 0 && getenv("SHARK_TRACE_SLOTS"))
	{
		extern void rt_dump_slot_counts(void);
		fprintf(stderr, "[slots] frame %u\n", D.frame);
		rt_dump_slot_counts();
	}
	HLE_RET(c, S_OK, 20);
}
HLE(Dev_GetBackBuffer) { ST4(ARG(3), D.backbuffer); res_addref(D.backbuffer); HLE_RET(c, S_OK, 16); }
HLE(Dev_GetRasterStatus) { memset(GP(ARG(1)), 0, 8); HLE_RET(c, S_OK, 8); }
HLE(Dev_SetGammaRamp) { HLE_RET(c, 0, 12); }
HLE(Dev_GetGammaRamp)
{
	for (u32 i = 0; i < 256; i++)
		for (u32 ch = 0; ch < 3; ch++)
			ST2(ARG(1) + 512 * ch + 2 * i, (u16)(i * 257));
	HLE_RET(c, 0, 8);
}
HLE(Dev_CreateTexture)
{
	/* (this, w, h, levels, usage, fmt, pool, out) */
	ST4(ARG(7), new_texture(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0));
	HLE_RET(c, S_OK, 32);
}
HLE(Dev_CreateVolumeTexture) { HLE_RET(c, D3DERR_NOTAVAILABLE, 36); }
HLE(Dev_CreateCubeTexture)
{
	/* (this, edge, levels, usage, fmt, pool, out) */
	ST4(ARG(6), new_texture(ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1));
	HLE_RET(c, S_OK, 28);
}
static u32 new_buffer(int index, u32 len, u32 usage, u32 fvf_or_fmt, u32 pool)
{
	Buffer* b = calloc(1, sizeof *b);
	b->index = index;
	b->size = len;
	b->usage = usage;
	b->pool = pool;
	if (index)
		b->fmt = fvf_or_fmt;
	else
		b->fvf = fvf_or_fmt;
	b->rtype = index ? RT_INDEXBUFFER : RT_VERTEXBUFFER;
	if (usage & 0x200)  /* D3DUSAGE_DYNAMIC */
	{
		b->mem = rt_heap_alloc(len);
		memset(GP(b->mem), 0, len);
		b->data = GP(b->mem);
	}
	else
		b->data = calloc(1, len ? len : 1);
	b->guest = com_new(index ? resource_vtable_ib() : resource_vtable_vb(), 16, b);
	return b->guest;
}
HLE(Dev_CreateVertexBuffer) { ST4(ARG(5), new_buffer(0, ARG(1), ARG(2), ARG(3), ARG(4))); HLE_RET(c, S_OK, 24); }
HLE(Dev_CreateIndexBuffer) { ST4(ARG(5), new_buffer(1, ARG(1), ARG(2), ARG(3), ARG(4))); HLE_RET(c, S_OK, 24); }
HLE(Dev_CreateRenderTarget) { ST4(ARG(6), new_surface(1, ARG(1), ARG(2), ARG(3))); HLE_RET(c, S_OK, 28); }
HLE(Dev_CreateDepthStencilSurface) { ST4(ARG(5), new_surface(2, ARG(1), ARG(2), ARG(3))); HLE_RET(c, S_OK, 24); }
HLE(Dev_CreateImageSurface) { ST4(ARG(4), new_surface(3, ARG(1), ARG(2), ARG(3))); HLE_RET(c, S_OK, 20); }
HLE(Dev_CopyRects)
{
	/* (this, src, rects, n, dst, points): whole-surface copies between same-format surfaces. */
	Surface* s = surf_of(ARG(1));
	Surface* d = surf_of(ARG(4));
	if (s && d && s->fmt == d->fmt && !ARG(2))
	{
		u32 pitch, size;
		level_layout(s->fmt, s->w, s->h, &pitch, &size);
		u8* from = s->tex ? s->tex->data[s->face][s->level] : GP(s->mem);
		u8* to = d->tex ? d->tex->data[d->face][d->level] : GP(d->mem);
		memcpy(to, from, size);
		if (d->tex)
			d->tex->dirty[d->face][d->level] = 1;
	}
	HLE_RET(c, S_OK, 24);
}
HLE(Dev_UpdateTexture)
{
	Texture* s = tex_of(ARG(1));
	Texture* d = tex_of(ARG(2));
	if (s && d)
		for (u32 f = 0; f < d->faces && f < s->faces; f++)
			for (u32 l = 0; l < d->levels && l < s->levels; l++)
			{
				memcpy(d->data[f][l], s->data[f][l], d->size[f][l]);
				d->dirty[f][l] = 1;
			}
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetFrontBuffer) { HLE_RET(c, D3DERR_NOTAVAILABLE, 8); }
HLE(Dev_SetRenderTarget)
{
	if (ARG(1))
		rebind(&D.rendertarget, ARG(1));
	rebind(&D.zstencil, ARG(2));
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetRenderTarget) { ST4(ARG(1), D.rendertarget); res_addref(D.rendertarget); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetDepthStencilSurface)
{
	ST4(ARG(1), D.zstencil);
	if (D.zstencil)
		res_addref(D.zstencil);
	HLE_RET(c, D.zstencil ? S_OK : D3DERR_NOTAVAILABLE, 8);
}
HLE(Dev_BeginScene) { D.in_scene = 1; HLE_RET(c, S_OK, 4); }
HLE(Dev_EndScene) { D.in_scene = 0; HLE_RET(c, S_OK, 4); }
HLE(Dev_Clear)
{
	/* (this, count, rects, flags, color, z, stencil) */
	int rects[64 * 4];
	int n = (int)(ARG(1) < 64 ? ARG(1) : 64);
	for (int i = 0; i < n * 4 && ARG(2); i++)
		rects[i] = (int)LD4(ARG(2) + 4 * (u32)i);
	gfx_clear(ARG(3), ARG(4), ARGF(5), ARG(6), ARG(2) ? rects : NULL, ARG(2) ? n : 0);
	HLE_RET(c, S_OK, 28);
}
static float* transform_slot(u32 t)
{
	if (t == 2) return D.s.view;
	if (t == 3) return D.s.proj;
	if (t >= 16 && t < 24) return D.s.texmat[t - 16];
	if (t == 256) return D.s.world;
	return NULL;
}
HLE(Dev_SetTransform)
{
	float* m = transform_slot(ARG(1));
	if (m)
		memcpy(m, GP(ARG(2)), 64);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetTransform)
{
	float* m = transform_slot(ARG(1));
	if (m)
		memcpy(GP(ARG(2)), m, 64);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_MultiplyTransform)
{
	float* m = transform_slot(ARG(1));
	if (m)
	{
		float a[16], b[16], r[16];
		memcpy(a, m, 64);
		memcpy(b, GP(ARG(2)), 64);
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				r[i * 4 + j] = b[i * 4 + 0] * a[0 * 4 + j] + b[i * 4 + 1] * a[1 * 4 + j] +
					b[i * 4 + 2] * a[2 * 4 + j] + b[i * 4 + 3] * a[3 * 4 + j];
		memcpy(m, r, 64);
	}
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_SetViewport)
{
	u32 v = ARG(1);
	D.s.viewport[0] = (float)LD4(v);
	D.s.viewport[1] = (float)LD4(v + 4);
	D.s.viewport[2] = (float)LD4(v + 8);
	D.s.viewport[3] = (float)LD4(v + 12);
	D.s.viewport[4] = asf32(LD4(v + 16));
	D.s.viewport[5] = asf32(LD4(v + 20));
	HLE_RET(c, S_OK, 8);
}
HLE(Dev_GetViewport)
{
	u32 v = ARG(1);
	for (int i = 0; i < 4; i++)
		ST4(v + 4 * (u32)i, (u32)D.s.viewport[i]);
	ST4(v + 16, bitsf32(D.s.viewport[4]));
	ST4(v + 20, bitsf32(D.s.viewport[5]));
	HLE_RET(c, S_OK, 8);
}
HLE(Dev_SetMaterial) { memcpy(D.s.material, GP(ARG(1)), 68); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetMaterial) { memcpy(GP(ARG(1)), D.s.material, 68); HLE_RET(c, S_OK, 8); }
HLE(Dev_SetLight)
{
	u32 i = ARG(1);
	if (i < 64)
	{
		memcpy(D.light_data[i], GP(ARG(2)), 104);
		D.light_set[i] = 1;
	}
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetLight) { if (ARG(1) < 64) memcpy(GP(ARG(2)), D.light_data[ARG(1)], 104); HLE_RET(c, S_OK, 12); }
HLE(Dev_LightEnable) { if (ARG(1) < 64) D.light_enabled[ARG(1)] = ARG(2) != 0; HLE_RET(c, S_OK, 12); }
HLE(Dev_GetLightEnable) { ST4(ARG(2), ARG(1) < 64 ? D.light_enabled[ARG(1)] : 0); HLE_RET(c, S_OK, 12); }
HLE(Dev_SetClipPlane) { HLE_RET(c, S_OK, 12); }
HLE(Dev_GetClipPlane) { memset(GP(ARG(2)), 0, 16); HLE_RET(c, S_OK, 12); }
HLE(Dev_SetRenderState) { if (ARG(1) < 256) D.s.rs[ARG(1)] = ARG(2); HLE_RET(c, S_OK, 12); }
HLE(Dev_GetRenderState) { ST4(ARG(2), ARG(1) < 256 ? D.s.rs[ARG(1)] : 0); HLE_RET(c, S_OK, 12); }
/* State blocks: recorded blocks are not replayed (the engine re-sets state each frame). */
static u32 NextBlock = 1;
HLE(Dev_BeginStateBlock) { HLE_RET(c, S_OK, 4); }
HLE(Dev_EndStateBlock) { ST4(ARG(1), NextBlock++); HLE_RET(c, S_OK, 8); }
HLE(Dev_ApplyStateBlock) { HLE_RET(c, S_OK, 8); }
HLE(Dev_CaptureStateBlock) { HLE_RET(c, S_OK, 8); }
HLE(Dev_DeleteStateBlock) { HLE_RET(c, S_OK, 8); }
HLE(Dev_CreateStateBlock) { ST4(ARG(2), NextBlock++); HLE_RET(c, S_OK, 12); }
HLE(Dev_SetClipStatus) { HLE_RET(c, S_OK, 8); }
HLE(Dev_GetClipStatus) { memset(GP(ARG(1)), 0, 24); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetTexture)
{
	u32 t = ARG(1) < GFX_MAX_STAGES ? D.textures[ARG(1)] : 0;
	ST4(ARG(2), t);
	if (t)
		res_addref(t);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_SetTexture) { if (ARG(1) < GFX_MAX_STAGES) rebind(&D.textures[ARG(1)], ARG(2)); HLE_RET(c, S_OK, 12); }
HLE(Dev_GetTextureStageState)
{
	ST4(ARG(3), ARG(1) < GFX_MAX_STAGES && ARG(2) < 32 ? D.s.tss[ARG(1)][ARG(2)] : 0);
	HLE_RET(c, S_OK, 16);
}
HLE(Dev_SetTextureStageState)
{
	if (ARG(1) < GFX_MAX_STAGES && ARG(2) < 32)
		D.s.tss[ARG(1)][ARG(2)] = ARG(3);
	HLE_RET(c, S_OK, 16);
}
HLE(Dev_ValidateDevice) { ST4(ARG(1), 1); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetInfo) { HLE_RET(c, E_FAIL, 16); }
HLE(Dev_SetPaletteEntries)
{
	u32 p = ARG(1);
	if (p < 256)
		memcpy(D.palette[p], GP(ARG(2)), 1024);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_GetPaletteEntries) { if (ARG(1) < 256) memcpy(GP(ARG(2)), D.palette[ARG(1)], 1024); HLE_RET(c, S_OK, 12); }
HLE(Dev_SetCurrentTexturePalette) { D.cur_palette = ARG(1); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetCurrentTexturePalette) { ST4(ARG(1), D.cur_palette); HLE_RET(c, S_OK, 8); }
HLE(Dev_ProcessVertices) { HLE_RET(c, D3DERR_INVALIDCALL, 24); }
HLE(Dev_CreateVertexShader)
{
	/* (this, decl, function, handle*, usage): fixed-function declarations only. */
	if (ARG(2))
		fprintf(stderr, "[d3d8] programmable vertex shader requested; using its declaration with fixed function\n");
	u32 i = (++D.shader_handles) & 511;
	layout_from_decl(ARG(1), &D.vs[i]);
	D.vs[i].used = 1;
	ST4(ARG(3), (i << 1) | 1 | 0x10000);
	HLE_RET(c, S_OK, 20);
}
HLE(Dev_SetVertexShader) { D.vshader = ARG(1); HLE_RET(c, S_OK, 8); }
HLE(Dev_GetVertexShader) { ST4(ARG(1), D.vshader); HLE_RET(c, S_OK, 8); }
HLE(Dev_DeleteVertexShader) { HLE_RET(c, S_OK, 8); }
HLE(Dev_SetVertexShaderConstant) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetVertexShaderConstant) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetVertexShaderDeclaration) { HLE_RET(c, D3DERR_INVALIDCALL, 16); }
HLE(Dev_GetVertexShaderFunction) { HLE_RET(c, D3DERR_INVALIDCALL, 16); }
HLE(Dev_SetStreamSource)
{
	if (ARG(1) < GFX_MAX_STREAMS)
	{
		rebind(&D.streams[ARG(1)], ARG(2));
		D.strides[ARG(1)] = ARG(3);
	}
	HLE_RET(c, S_OK, 16);
}
HLE(Dev_GetStreamSource)
{
	u32 s = ARG(1) < GFX_MAX_STREAMS ? ARG(1) : 0;
	ST4(ARG(2), D.streams[s]);
	if (D.streams[s])
		res_addref(D.streams[s]);
	ST4(ARG(3), D.strides[s]);
	HLE_RET(c, S_OK, 16);
}
HLE(Dev_SetIndices) { rebind(&D.indices, ARG(1)); D.base_vertex = ARG(2); HLE_RET(c, S_OK, 12); }
HLE(Dev_GetIndices)
{
	ST4(ARG(1), D.indices);
	if (D.indices)
		res_addref(D.indices);
	ST4(ARG(2), D.base_vertex);
	HLE_RET(c, S_OK, 12);
}
HLE(Dev_CreatePixelShader) { HLE_RET(c, D3DERR_NOTAVAILABLE, 12); }
HLE(Dev_SetPixelShader) { HLE_RET(c, S_OK, 8); }
HLE(Dev_GetPixelShader) { ST4(ARG(1), 0); HLE_RET(c, S_OK, 8); }
HLE(Dev_DeletePixelShader) { HLE_RET(c, S_OK, 8); }
HLE(Dev_SetPixelShaderConstant) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetPixelShaderConstant) { HLE_RET(c, S_OK, 16); }
HLE(Dev_GetPixelShaderFunction) { HLE_RET(c, D3DERR_INVALIDCALL, 16); }
HLE(Dev_DrawRectPatch) { HLE_RET(c, D3DERR_INVALIDCALL, 16); }
HLE(Dev_DrawTriPatch) { HLE_RET(c, D3DERR_INVALIDCALL, 16); }
HLE(Dev_DeletePatch) { HLE_RET(c, S_OK, 8); }

#define M(n) { #n, hle_Dev_##n }
static const ComMethod DeviceMethods[] = {
	M(QueryInterface), M(AddRef), M(Release), M(TestCooperativeLevel), M(GetAvailableTextureMem),
	M(ResourceManagerDiscardBytes), M(GetDirect3D), M(GetDeviceCaps), M(GetDisplayMode), M(GetCreationParameters),
	M(SetCursorProperties), M(SetCursorPosition), M(ShowCursor), M(CreateAdditionalSwapChain), M(Reset), M(Present),
	M(GetBackBuffer), M(GetRasterStatus), M(SetGammaRamp), M(GetGammaRamp), M(CreateTexture), M(CreateVolumeTexture),
	M(CreateCubeTexture), M(CreateVertexBuffer), M(CreateIndexBuffer), M(CreateRenderTarget),
	M(CreateDepthStencilSurface), M(CreateImageSurface), M(CopyRects), M(UpdateTexture), M(GetFrontBuffer),
	M(SetRenderTarget), M(GetRenderTarget), M(GetDepthStencilSurface), M(BeginScene), M(EndScene), M(Clear),
	M(SetTransform), M(GetTransform), M(MultiplyTransform), M(SetViewport), M(GetViewport), M(SetMaterial),
	M(GetMaterial), M(SetLight), M(GetLight), M(LightEnable), M(GetLightEnable), M(SetClipPlane), M(GetClipPlane),
	M(SetRenderState), M(GetRenderState), M(BeginStateBlock), M(EndStateBlock), M(ApplyStateBlock),
	M(CaptureStateBlock), M(DeleteStateBlock), M(CreateStateBlock), M(SetClipStatus), M(GetClipStatus),
	M(GetTexture), M(SetTexture), M(GetTextureStageState), M(SetTextureStageState), M(ValidateDevice), M(GetInfo),
	M(SetPaletteEntries), M(GetPaletteEntries), M(SetCurrentTexturePalette), M(GetCurrentTexturePalette),
	M(DrawPrimitive), M(DrawIndexedPrimitive), M(DrawPrimitiveUP), M(DrawIndexedPrimitiveUP), M(ProcessVertices),
	M(CreateVertexShader), M(SetVertexShader), M(GetVertexShader), M(DeleteVertexShader),
	M(SetVertexShaderConstant), M(GetVertexShaderConstant), M(GetVertexShaderDeclaration),
	M(GetVertexShaderFunction), M(SetStreamSource), M(GetStreamSource), M(SetIndices), M(GetIndices),
	M(CreatePixelShader), M(SetPixelShader), M(GetPixelShader), M(DeletePixelShader), M(SetPixelShaderConstant),
	M(GetPixelShaderConstant), M(GetPixelShaderFunction), M(DrawRectPatch), M(DrawTriPatch), M(DeletePatch),
};
#undef M
_Static_assert(sizeof DeviceMethods / sizeof DeviceMethods[0] == 97, "IDirect3DDevice8 has 97 methods");

void d3d8_backbuffer_size(u32* w, u32* h) { *w = D.width; *h = D.height; }

u32 d3d8_create_device(Ctx* c, u32 hwnd, u32 behavior, u32 pp, u32 out_device)
{
	/* D3DPRESENT_PARAMETERS: w, h, fmt, count, ms, swap, hwnd, windowed, autodepth, depthfmt, flags, refresh, interval */
	memset(&D, 0, sizeof D);
	D.width = LD4(pp) ? LD4(pp) : 1024;
	D.height = LD4(pp + 4) ? LD4(pp + 4) : 768;
	D.windowed = LD4(pp + 28);
	D.auto_depth = LD4(pp + 32);
	D.depth_fmt = LD4(pp + 36);
	if (!gfx_init((int)D.width, (int)D.height, !D.windowed))
		return D3DERR_NOTAVAILABLE;
	D.d3d = ARG(0);  /* the IDirect3D8 this was called on */
	D.guest = com_new(com_vtable("IDirect3DDevice8", DeviceMethods, 97), 16, NULL);
	D.backbuffer = new_surface(1, D.width, D.height, D3DFMT_X8R8G8B8);
	D.depthbuffer = D.auto_depth ? new_surface(2, D.width, D.height, D.depth_fmt) : 0;
	D.rendertarget = D.backbuffer;
	D.zstencil = D.depthbuffer;
	set_default_state();
	ST4(out_device, D.guest);
	extern void rt_install_d3d_hooks(void);
	rt_install_d3d_hooks();
	fprintf(stderr, "[d3d8] device %ux%u %s, depth %s\n", D.width, D.height, D.windowed ? "windowed" : "fullscreen",
		D.auto_depth ? "on" : "off");
	return D3D_OK;
}
