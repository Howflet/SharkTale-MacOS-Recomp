/*=============================================================================
	gfx_shaders.metal: Direct3D 8 fixed-function pipeline as one uber-shader.
	Compiled at runtime (embedded into gfx_metal.m by the build).
	Uniform layouts must match VSU/FSU in gfx_metal.m.
=============================================================================*/
#include <metal_stdlib>
using namespace metal;

struct Light
{
	float4 diffuse, specular, ambient;
	float4 position;   /* view space xyz, range */
	float4 direction;  /* view space xyz, falloff */
	float4 atten;      /* att0, att1, att2, type */
	float4 spot;       /* cos(theta/2), cos(phi/2) */
};

struct VSU
{
	float4x4 wv, proj;
	float4x4 texmat[8];
	float4 vp, vpz;
	float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
	float4 global_ambient;
	float4 fog;        /* start, end, density, mode (0 none 1 exp 2 exp2 3 linear) */
	Light lights[8];
	uint4 attr[16];    /* stream, type, offset */
	uint4 streams[8];  /* byte offset, stride */
	uint4 tci[8];      /* texcoordindex, transform flags, coord type of the source set */
	uint flags, matsrc, nlights;
	float point_size;
	float mat_power, pad0, pad1, pad2;
};

enum { F_PRE = 1, F_LIGHTING = 2, F_NORMALIZE = 4, F_SPECULAR = 8, F_LOCALVIEWER = 16, F_COLORVERTEX = 32,
	F_FOGSPECALPHA = 64, F_RANGEFOG = 128 };
enum { VT_FLOAT1 = 0, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4, VT_D3DCOLOR, VT_UBYTE4, VT_SHORT2, VT_SHORT4, VT_NONE = 255 };
enum { VS_POSITION = 0, VS_BLENDWEIGHT, VS_BLENDINDICES, VS_NORMAL, VS_PSIZE, VS_DIFFUSE, VS_SPECULAR, VS_TEXCOORD0 };

struct VOut
{
	float4 pos [[position]];
	float4 diffuse;
	float4 specular;
	float fog;
	float4 t0, t1, t2, t3, t4, t5, t6, t7;
	float psize [[point_size]];
};

static float4 fetch(device const uint* buf, constant VSU& u, uint semantic, uint vid, float4 def)
{
	uint4 a = u.attr[semantic];
	if (a.y == VT_NONE)
		return def;
	uint4 st = u.streams[a.x];
	uint w = (st.x + vid * st.y + a.z) >> 2;
	uint c = buf[w];
	switch (a.y)
	{
	case VT_FLOAT1: return float4(as_type<float>(c), 0, 0, 1);
	case VT_FLOAT2: return float4(as_type<float>(c), as_type<float>(buf[w + 1]), 0, 1);
	case VT_FLOAT3: return float4(as_type<float>(c), as_type<float>(buf[w + 1]), as_type<float>(buf[w + 2]), 1);
	case VT_FLOAT4:
		return float4(as_type<float>(c), as_type<float>(buf[w + 1]), as_type<float>(buf[w + 2]), as_type<float>(buf[w + 3]));
	case VT_D3DCOLOR: return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;
	case VT_UBYTE4: return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24);
	case VT_SHORT2: return float4(float(short(c & 0xffff)), float(short(c >> 16)), 0, 1);
	default:
	{
		uint d = buf[w + 1];
		return float4(float(short(c & 0xffff)), float(short(c >> 16)), float(short(d & 0xffff)), float(short(d >> 16)));
	}
	}
}

static float4 material_color(uint src, float4 mat, float4 c1, float4 c2, bool has1, bool has2, bool colorvertex)
{
	if (colorvertex)
	{
		if (src == 1 && has1) return c1;
		if (src == 2 && has2) return c2;
	}
	return mat;
}

vertex VOut vs_main(uint vid [[vertex_id]], device const uint* buf [[buffer(0)]], constant VSU& u [[buffer(1)]])
{
	VOut o;
	float4 p = fetch(buf, u, VS_POSITION, vid, float4(0, 0, 0, 1));
	bool hasDiffuse = u.attr[VS_DIFFUSE].y != VT_NONE, hasSpecular = u.attr[VS_SPECULAR].y != VT_NONE;
	float4 vdiff = fetch(buf, u, VS_DIFFUSE, vid, float4(1));
	float4 vspec = fetch(buf, u, VS_SPECULAR, vid, float4(0));
	float3 P = float3(0), N = float3(0, 0, 1);
	float viewz;
	if (u.flags & F_PRE)
	{
		/* Screen-space XYZRHW (D3D pixel centres are at integer coordinates). */
		float w = p.w != 0 ? 1.0 / p.w : 1.0;
		float x = (p.x + 0.5 - u.vp.x) / u.vp.z * 2 - 1;
		float y = 1 - (p.y + 0.5 - u.vp.y) / u.vp.w * 2;
		float z = (p.z - u.vpz.x) / max(u.vpz.y - u.vpz.x, 1e-6);
		o.pos = float4(x * w, y * w, z * w, w);
		viewz = w;
		o.diffuse = vdiff;
		o.specular = vspec;
	}
	else
	{
		float4 vpos = u.wv * float4(p.xyz, 1);
		P = vpos.xyz;
		o.pos = u.proj * vpos;
		o.pos.x += o.pos.w / u.vp.z;   /* D3D half-pixel convention */
		o.pos.y -= o.pos.w / u.vp.w;
		viewz = P.z;
		if (u.attr[VS_NORMAL].y != VT_NONE)
		{
			N = (u.wv * float4(fetch(buf, u, VS_NORMAL, vid, float4(0, 0, 1, 0)).xyz, 0)).xyz;
			if (u.flags & F_NORMALIZE)
				N = normalize(N);
		}
		if (u.flags & F_LIGHTING)
		{
			bool cv = (u.flags & F_COLORVERTEX) != 0;
			float4 Md = material_color(u.matsrc & 3, u.mat_diffuse, vdiff, vspec, hasDiffuse, hasSpecular, cv);
			float4 Ma = material_color((u.matsrc >> 4) & 3, u.mat_ambient, vdiff, vspec, hasDiffuse, hasSpecular, cv);
			float4 Ms = material_color((u.matsrc >> 2) & 3, u.mat_specular, vdiff, vspec, hasDiffuse, hasSpecular, cv);
			float4 Me = material_color((u.matsrc >> 6) & 3, u.mat_emissive, vdiff, vspec, hasDiffuse, hasSpecular, cv);
			float3 amb = u.global_ambient.rgb, dif = float3(0), spe = float3(0);
			float3 V = (u.flags & F_LOCALVIEWER) ? normalize(-P) : float3(0, 0, -1);
			for (uint i = 0; i < u.nlights; i++)
			{
				Light L = u.lights[i];
				uint type = uint(L.atten.w);
				float3 Ld;
				float att = 1, spot = 1;
				if (type == 3)
					Ld = normalize(-L.direction.xyz);
				else
				{
					float3 d = L.position.xyz - P;
					float dist = length(d);
					if (dist > L.position.w)
						continue;
					Ld = d / max(dist, 1e-6);
					att = 1.0 / max(L.atten.x + L.atten.y * dist + L.atten.z * dist * dist, 1e-6);
					if (type == 2)
					{
						float rho = dot(-Ld, normalize(L.direction.xyz));
						if (rho <= L.spot.y)
							spot = 0;
						else if (rho < L.spot.x)
							spot = pow(saturate((rho - L.spot.y) / max(L.spot.x - L.spot.y, 1e-6)), L.direction.w);
					}
				}
				float ndl = max(dot(N, Ld), 0.0);
				amb += L.ambient.rgb * att * spot;
				dif += L.diffuse.rgb * ndl * att * spot;
				if ((u.flags & F_SPECULAR) && ndl > 0)
				{
					float3 H = normalize(V + Ld);
					spe += L.specular.rgb * pow(max(dot(N, H), 0.0), u.mat_power) * att * spot;
				}
			}
			o.diffuse = float4(saturate(Ma.rgb * amb + Md.rgb * dif + Me.rgb), Md.a);
			o.specular = float4(saturate(Ms.rgb * spe), vspec.a);
		}
		else
		{
			o.diffuse = vdiff;
			o.specular = vspec;
		}
	}

	/* Fog factor (1 = no fog). */
	o.fog = 1;
	uint fogmode = uint(u.fog.w);
	if (u.flags & F_FOGSPECALPHA)
		o.fog = vspec.a;
	else if (fogmode)
	{
		float d = (u.flags & F_RANGEFOG) && !(u.flags & F_PRE) ? length(P) : abs(viewz);
		if (fogmode == 3)
			o.fog = saturate((u.fog.y - d) / max(u.fog.y - u.fog.x, 1e-6));
		else if (fogmode == 1)
			o.fog = saturate(exp(-d * u.fog.z));
		else
			o.fog = saturate(exp(-(d * u.fog.z) * (d * u.fog.z)));
	}

	float4 tc[8];
	for (uint t = 0; t < 8; t++)
	{
		uint idx = u.tci[t].x, gen = idx >> 16, flags = u.tci[t].y;
		float4 v;
		if (gen == 1)
			v = float4(N, 1);
		else if (gen == 2)
			v = float4(P, 1);
		else if (gen == 3)
			v = float4(reflect(normalize(P), N), 1);
		else
		{
			uint set = idx & 7;
			v = fetch(buf, u, VS_TEXCOORD0 + set, vid, float4(0, 0, 0, 1));
			if (flags & 255)
			{
				uint type = u.attr[VS_TEXCOORD0 + set].y;
				/* D3D expands to 4D with 1 after the last component. */
				if (type == VT_FLOAT1) v = float4(v.x, 1, 0, 0);
				else if (type == VT_FLOAT2) v = float4(v.xy, 1, 0);
				else if (type == VT_FLOAT3) v = float4(v.xyz, 1);
			}
		}
		if (flags & 255)
			v = u.texmat[t] * v;
		tc[t] = v;
	}
	o.t0 = tc[0]; o.t1 = tc[1]; o.t2 = tc[2]; o.t3 = tc[3];
	o.t4 = tc[4]; o.t5 = tc[5]; o.t6 = tc[6]; o.t7 = tc[7];
	o.psize = u.point_size;
	return o;
}

struct FSU
{
	uint4 stage[8];    /* colorop|arg1<<8|arg2<<16|arg0<<24, alpha same, resultarg|textype<<8, projdiv component (0 = none) */
	float4 bumpmat[8];
	float4 tfactor;
	float4 fogcolor;
	uint flags, alphafunc;
	float alpharef;
	uint pad;
};

enum { FF_ALPHATEST = 1, FF_FOG = 2, FF_SPECULAR = 4 };

static float4 arg(uint a, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 specular, float4 temp)
{
	float4 r;
	switch (a & 15)
	{
	case 0: r = diffuse; break;
	case 1: r = current; break;
	case 2: r = tex; break;
	case 3: r = tfactor; break;
	case 4: r = specular; break;
	default: r = temp; break;
	}
	if (a & 0x10) r = 1 - r;
	if (a & 0x20) r = r.aaaa;
	return r;
}

static float4 combine(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 tex, float4 tfactor, float4 current)
{
	switch (op)
	{
	case 2: return a1;
	case 3: return a2;
	case 4: return a1 * a2;
	case 5: return a1 * a2 * 2;
	case 6: return a1 * a2 * 4;
	case 7: return a1 + a2;
	case 8: return a1 + a2 - 0.5;
	case 9: return (a1 + a2 - 0.5) * 2;
	case 10: return a1 - a2;
	case 11: return a1 + a2 - a1 * a2;
	case 12: return mix(a2, a1, diffuse.a);
	case 13: return mix(a2, a1, tex.a);
	case 14: return mix(a2, a1, tfactor.a);
	case 15: return a1 + a2 * (1 - tex.a);
	case 16: return mix(a2, a1, current.a);
	case 18: return float4(a1.rgb + a1.a * a2.rgb, a1.a);
	case 19: return float4(a1.rgb * a2.rgb + a1.a, a1.a);
	case 20: return float4(a1.rgb + (1 - a1.a) * a2.rgb, a1.a);
	case 21: return float4((1 - a1.rgb) * a2.rgb + a1.a, a1.a);
	case 24: return float4(saturate(dot((a1.rgb - 0.5) * 2, (a2.rgb - 0.5) * 2)));
	case 25: return a0 + a1 * a2;
	case 26: return mix(a2, a1, a0);
	default: return current;
	}
}

static bool alpha_test(uint func, float a, float ref)
{
	float v = round(saturate(a) * 255);
	switch (func)
	{
	case 1: return false;
	case 2: return v < ref;
	case 3: return v == ref;
	case 4: return v <= ref;
	case 5: return v > ref;
	case 6: return v != ref;
	case 7: return v >= ref;
	default: return true;
	}
}

fragment float4 fs_main(VOut in [[stage_in]], constant FSU& u [[buffer(0)]],
	array<texture2d<float>, 8> t2 [[texture(0)]], array<texturecube<float>, 8> tcube [[texture(8)]],
	array<sampler, 8> smp [[sampler(0)]])
{
	float4 tc[8] = { in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7 };
	float4 diffuse = in.diffuse, specular = in.specular;
	float4 current = diffuse, temp = float4(0);
	float2 bump = float2(0);
	bool bumped = false;
	for (uint s = 0; s < 8; s++)
	{
		uint4 st = u.stage[s];
		uint colorop = st.x & 255;
		if (colorop == 1)
			break;
		uint textype = (st.z >> 8) & 255;
		float4 coord = tc[s];
		if (st.w)
			coord /= coord[st.w - 1];
		if (bumped)
		{
			coord.xy += bump;
			bumped = false;
		}
		float4 tex = float4(1);
		if (textype == 1)
			tex = t2[s].sample(smp[s], coord.xy);
		else if (textype == 2)
			tex = tcube[s].sample(smp[s], coord.xyz);
		if (colorop == 22 || colorop == 23)
		{
			/* Bump env map: perturb the next stage's coordinates; V8U8 is stored biased. */
			float2 duv = tex.rg * 2 - 1;
			float4 m = u.bumpmat[s];
			bump = float2(m.x * duv.x + m.z * duv.y, m.y * duv.x + m.w * duv.y);
			bumped = true;
			continue;
		}
		float4 c1 = arg((st.x >> 8) & 255, diffuse, current, tex, u.tfactor, specular, temp);
		float4 c2 = arg((st.x >> 16) & 255, diffuse, current, tex, u.tfactor, specular, temp);
		float4 c0 = arg(st.x >> 24, diffuse, current, tex, u.tfactor, specular, temp);
		float4 col = combine(colorop, c1, c2, c0, diffuse, tex, u.tfactor, current);
		uint alphaop = st.y & 255;
		float alpha;
		if (colorop == 24)
			alpha = col.a;
		else if (alphaop == 1)
			alpha = current.a;
		else
		{
			float4 a1 = arg((st.y >> 8) & 255, diffuse, current, tex, u.tfactor, specular, temp);
			float4 a2 = arg((st.y >> 16) & 255, diffuse, current, tex, u.tfactor, specular, temp);
			float4 a0 = arg(st.y >> 24, diffuse, current, tex, u.tfactor, specular, temp);
			alpha = combine(alphaop, a1, a2, a0, diffuse, tex, u.tfactor, current).a;
		}
		float4 r = saturate(float4(col.rgb, alpha));
		if ((st.z & 255) == 5)
			temp = r;
		else
			current = r;
	}
	if (u.flags & FF_SPECULAR)
		current.rgb = saturate(current.rgb + specular.rgb);
	if ((u.flags & FF_ALPHATEST) && !alpha_test(u.alphafunc, current.a, u.alpharef))
		discard_fragment();
	if (u.flags & FF_FOG)
		current.rgb = mix(u.fogcolor.rgb, current.rgb, saturate(in.fog));
	return current;
}

/* Present: scale the offscreen back buffer into the drawable. */
struct BlitOut { float4 pos [[position]]; float2 uv; };
vertex BlitOut blit_vs(uint vid [[vertex_id]])
{
	float2 p = float2((vid << 1) & 2, vid & 2);
	BlitOut o;
	o.pos = float4(p * 2 - 1, 0, 1);
	o.uv = float2(p.x, 1 - p.y);
	return o;
}
fragment float4 blit_fs(BlitOut in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	return float4(t.sample(s, in.uv).rgb, 1);
}
