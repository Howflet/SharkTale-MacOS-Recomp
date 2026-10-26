/*=============================================================================
	hle_openal.c: OpenAL for ALAudio, bridged to macOS OpenAL.framework.

	ALAudio loads DefOpenAL32/OpenAL32 and fetches every entry point with
	GetProcAddress; they resolve here. OpenAL's Windows calling convention is
	cdecl. Object names (sources, buffers) are 32-bit and pass through;
	devices and contexts are host pointers, so the guest sees small handles.
	EAX is reported as absent.
=============================================================================*/
#define OPENAL_DEPRECATED  /* OpenAL.framework is deprecated but still ships */
#include <OpenAL/al.h>
#include <OpenAL/alc.h>

#include <stdlib.h>

#include "hle.h"

#define CRET(c, v) HLE_RET(c, v, 0)
#define CRETF(c, v) HLE_RETF(c, v, 0)

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/*-----------------------------------------------------------------------------
	Handles for devices and contexts.
-----------------------------------------------------------------------------*/

#define AL_HANDLE_BASE 0x0a100000u
static void* Handles[64];

static u32 to_handle(void* p)
{
	if (!p)
		return 0;
	for (int i = 0; i < 64; i++)
		if (Handles[i] == p)
			return AL_HANDLE_BASE + 16 * (u32)i;
	for (int i = 0; i < 64; i++)
		if (!Handles[i])
		{
			Handles[i] = p;
			return AL_HANDLE_BASE + 16 * (u32)i;
		}
	return 0;
}
static void* from_handle(u32 h)
{
	u32 i = (h - AL_HANDLE_BASE) / 16;
	return h >= AL_HANDLE_BASE && i < 64 ? Handles[i] : NULL;
}
static void drop_handle(u32 h)
{
	u32 i = (h - AL_HANDLE_BASE) / 16;
	if (h >= AL_HANDLE_BASE && i < 64)
		Handles[i] = NULL;
}

/* Strings returned to the guest are copied once into HLE static memory. */
static u32 guest_string(const char* s)
{
	static struct { const char* host; u32 guest; } cache[64];
	static int n;
	if (!s)
		return 0;
	for (int i = 0; i < n; i++)
		if (cache[i].host == s)
			return cache[i].guest;
	u32 len = (u32)strlen(s) + 1;
	u32 g = hle_static_alloc(len);
	memcpy(GP(g), s, len);
	if (n < 64)
	{
		cache[n].host = s;
		cache[n++].guest = g;
	}
	return g;
}

static int is_eax(const char* name) { return !strncasecmp(name, "EAX", 3); }

#define P(i) ((void*)GP(ARG(i)))

/*-----------------------------------------------------------------------------
	al*
-----------------------------------------------------------------------------*/

HLE(alEnable) { alEnable((ALenum)ARG(0)); CRET(c, 0); }
HLE(alDisable) { alDisable((ALenum)ARG(0)); CRET(c, 0); }
HLE(alIsEnabled) { CRET(c, alIsEnabled((ALenum)ARG(0))); }
HLE(alGetBoolean) { CRET(c, alGetBoolean((ALenum)ARG(0))); }
HLE(alGetInteger) { CRET(c, (u32)alGetInteger((ALenum)ARG(0))); }
HLE(alGetFloat) { CRETF(c, alGetFloat((ALenum)ARG(0))); }
HLE(alGetDouble) { CRETF(c, alGetDouble((ALenum)ARG(0))); }
HLE(alGetBooleanv) { alGetBooleanv((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetIntegerv) { alGetIntegerv((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetFloatv) { alGetFloatv((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetDoublev) { alGetDoublev((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetString) { CRET(c, guest_string((const char*)alGetString((ALenum)ARG(0)))); }
HLE(alGetError) { CRET(c, (u32)alGetError()); }
HLE(alIsExtensionPresent)
{
	const char* n = g_str(ARG(0));
	CRET(c, is_eax(n) ? AL_FALSE : alIsExtensionPresent(n));
}
HLE(alGetProcAddress)
{
	const char* n = g_str(ARG(0));
	CRET(c, is_eax(n) ? 0 : rt_hle_address("OpenAL32.dll", n));
}
HLE(alGetEnumValue) { CRET(c, (u32)alGetEnumValue(g_str(ARG(0)))); }

/* SHARK_MUTE=1 (automated runs): keep the listener silent. */
static int muted(void)
{
	static int m = -1;
	if (m < 0)
		m = getenv("SHARK_MUTE") != NULL;
	return m;
}
static unsigned Plays;
HLE(alListenerf)
{
	alListenerf((ALenum)ARG(0), muted() && ARG(0) == AL_GAIN ? 0.0f : ARGF(1));
	CRET(c, 0);
}
HLE(alListener3f) { alListener3f((ALenum)ARG(0), ARGF(1), ARGF(2), ARGF(3)); CRET(c, 0); }
HLE(alListenerfv) { alListenerfv((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alListeneri) { alListeneri((ALenum)ARG(0), (ALint)ARG(1)); CRET(c, 0); }
HLE(alGetListenerf) { alGetListenerf((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetListener3f) { alGetListener3f((ALenum)ARG(0), P(1), P(2), P(3)); CRET(c, 0); }
HLE(alGetListenerfv) { alGetListenerfv((ALenum)ARG(0), P(1)); CRET(c, 0); }
HLE(alGetListeneri) { alGetListeneri((ALenum)ARG(0), P(1)); CRET(c, 0); }

HLE(alGenSources) { alGenSources((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alDeleteSources) { alDeleteSources((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alIsSource) { CRET(c, alIsSource(ARG(0))); }
HLE(alSourcef) { alSourcef(ARG(0), (ALenum)ARG(1), ARGF(2)); CRET(c, 0); }
HLE(alSource3f) { alSource3f(ARG(0), (ALenum)ARG(1), ARGF(2), ARGF(3), ARGF(4)); CRET(c, 0); }
HLE(alSourcefv) { alSourcefv(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }
HLE(alSourcei) { alSourcei(ARG(0), (ALenum)ARG(1), (ALint)ARG(2)); CRET(c, 0); }
HLE(alGetSourcef) { alGetSourcef(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }
HLE(alGetSource3f) { alGetSource3f(ARG(0), (ALenum)ARG(1), P(2), P(3), P(4)); CRET(c, 0); }
HLE(alGetSourcefv) { alGetSourcefv(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }
HLE(alGetSourcei) { alGetSourcei(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }
HLE(alSourcePlay)
{
	alSourcePlay(ARG(0));
	unsigned n = ++Plays;
	if ((n & (n - 1)) == 0)
		fprintf(stderr, "[openal] %u sounds started\n", n);
	CRET(c, 0);
}
HLE(alSourceStop) { alSourceStop(ARG(0)); CRET(c, 0); }
HLE(alSourceRewind) { alSourceRewind(ARG(0)); CRET(c, 0); }
HLE(alSourcePause) { alSourcePause(ARG(0)); CRET(c, 0); }
HLE(alSourcePlayv) { alSourcePlayv((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alSourceStopv) { alSourceStopv((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alSourceRewindv) { alSourceRewindv((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alSourcePausev) { alSourcePausev((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alSourceQueueBuffers) { alSourceQueueBuffers(ARG(0), (ALsizei)ARG(1), P(2)); CRET(c, 0); }
HLE(alSourceUnqueueBuffers) { alSourceUnqueueBuffers(ARG(0), (ALsizei)ARG(1), P(2)); CRET(c, 0); }

HLE(alGenBuffers) { alGenBuffers((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alDeleteBuffers) { alDeleteBuffers((ALsizei)ARG(0), P(1)); CRET(c, 0); }
HLE(alIsBuffer) { CRET(c, alIsBuffer(ARG(0))); }
/* SHARK_DUMP_AUDIO=<dir>: write the first 32 buffers as WAV files (to check decoding while muted). */
static void dump_wav(u32 fmt, const void* data, u32 size, u32 freq)
{
	static int count;
	const char* dir = getenv("SHARK_DUMP_AUDIO");
	if (!dir || count >= 32)
		return;
	int ch = fmt == AL_FORMAT_MONO8 || fmt == AL_FORMAT_MONO16 ? 1 : 2;
	int bits = fmt == AL_FORMAT_MONO8 || fmt == AL_FORMAT_STEREO8 ? 8 : 16;
	char path[1024];
	snprintf(path, sizeof path, "%s/buf_%02d.wav", dir, count++);
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	u32 hdr[11] = { 0x46464952u, 36 + size, 0x45564157u, 0x20746d66u, 16,
		1u | (u32)ch << 16, freq, freq * (u32)ch * (u32)bits / 8, (u32)(ch * bits / 8) | (u32)bits << 16, 0x61746164u, size };
	fwrite(hdr, 4, 11, f);
	fwrite(data, 1, size, f);
	fclose(f);
}
HLE(alBufferData)
{
	dump_wav(ARG(1), P(2), ARG(3), ARG(4));
	alBufferData(ARG(0), (ALenum)ARG(1), P(2), (ALsizei)ARG(3), (ALsizei)ARG(4));
	CRET(c, 0);
}
HLE(alGetBufferf) { alGetBufferf(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }
HLE(alGetBufferi) { alGetBufferi(ARG(0), (ALenum)ARG(1), P(2)); CRET(c, 0); }

HLE(alDopplerFactor) { alDopplerFactor(ARGF(0)); CRET(c, 0); }
HLE(alDopplerVelocity) { alDopplerVelocity(ARGF(0)); CRET(c, 0); }
HLE(alDistanceModel) { alDistanceModel((ALenum)ARG(0)); CRET(c, 0); }

/* EAX (if fetched by name anyway): not supported. */
HLE(EAXSet) { CRET(c, 0x80004005u); }
HLE(EAXGet) { CRET(c, 0x80004005u); }

/*-----------------------------------------------------------------------------
	alc*
-----------------------------------------------------------------------------*/

HLE(alcOpenDevice)
{
	/* Windows device names ("Generic Hardware", "DirectSound3D") mean nothing here: use the default. */
	ALCdevice* d = alcOpenDevice(NULL);
	fprintf(stderr, "[openal] alcOpenDevice(%s) -> %s\n", ARG(0) ? g_str(ARG(0)) : "default", d ? "ok" : "failed");
	CRET(c, to_handle(d));
}
HLE(alcCloseDevice)
{
	ALCdevice* d = from_handle(ARG(0));
	if (d)
		alcCloseDevice(d);
	drop_handle(ARG(0));
	CRET(c, 1);
}
HLE(alcCreateContext)
{
	ALCdevice* d = from_handle(ARG(0));
	/* Attribute list: pairs of 32-bit ints, zero-terminated. */
	ALCint attrs[64];
	int n = 0;
	for (u32 p = ARG(1); p && n < 62; p += 8)
	{
		ALCint k = (ALCint)LD4(p);
		if (!k)
			break;
		attrs[n++] = k;
		attrs[n++] = (ALCint)LD4(p + 4);
	}
	attrs[n] = 0;
	CRET(c, d ? to_handle(alcCreateContext(d, ARG(1) ? attrs : NULL)) : 0);
}
HLE(alcMakeContextCurrent)
{
	ALCboolean ok = alcMakeContextCurrent(from_handle(ARG(0)));
	if (ok && muted())
		alListenerf(AL_GAIN, 0.0f);
	CRET(c, ok);
}
HLE(alcProcessContext) { if (from_handle(ARG(0))) alcProcessContext(from_handle(ARG(0))); CRET(c, 0); }
HLE(alcSuspendContext) { if (from_handle(ARG(0))) alcSuspendContext(from_handle(ARG(0))); CRET(c, 0); }
HLE(alcDestroyContext)
{
	ALCcontext* x = from_handle(ARG(0));
	if (x)
		alcDestroyContext(x);
	drop_handle(ARG(0));
	CRET(c, 0);
}
HLE(alcGetError) { CRET(c, (u32)alcGetError(from_handle(ARG(0)))); }
HLE(alcGetCurrentContext) { CRET(c, to_handle(alcGetCurrentContext())); }
HLE(alcGetContextsDevice) { CRET(c, to_handle(alcGetContextsDevice(from_handle(ARG(0))))); }
HLE(alcGetString) { CRET(c, guest_string(alcGetString(from_handle(ARG(0)), (ALCenum)ARG(1)))); }
HLE(alcGetIntegerv)
{
	if (ARG(3))
		alcGetIntegerv(from_handle(ARG(0)), (ALCenum)ARG(1), (ALCsizei)ARG(2), P(3));
	CRET(c, 0);
}
HLE(alcIsExtensionPresent)
{
	const char* n = g_str(ARG(1));
	CRET(c, is_eax(n) ? 0 : alcIsExtensionPresent(from_handle(ARG(0)), n));
}
HLE(alcGetProcAddress) { CRET(c, rt_hle_address("OpenAL32.dll", g_str(ARG(1)))); }
HLE(alcGetEnumValue) { CRET(c, (u32)alcGetEnumValue(from_handle(ARG(0)), g_str(ARG(1)))); }

const HleEntry hle_openal[] = {
	ENTRY(alEnable), ENTRY(alDisable), ENTRY(alIsEnabled), ENTRY(alGetBoolean), ENTRY(alGetInteger),
	ENTRY(alGetFloat), ENTRY(alGetDouble), ENTRY(alGetBooleanv), ENTRY(alGetIntegerv), ENTRY(alGetFloatv),
	ENTRY(alGetDoublev), ENTRY(alGetString), ENTRY(alGetError), ENTRY(alIsExtensionPresent),
	ENTRY(alGetProcAddress), ENTRY(alGetEnumValue), ENTRY(alListenerf), ENTRY(alListener3f), ENTRY(alListenerfv),
	ENTRY(alListeneri), ENTRY(alGetListenerf), ENTRY(alGetListener3f), ENTRY(alGetListenerfv),
	ENTRY(alGetListeneri), ENTRY(alGenSources), ENTRY(alDeleteSources), ENTRY(alIsSource), ENTRY(alSourcef),
	ENTRY(alSource3f), ENTRY(alSourcefv), ENTRY(alSourcei), ENTRY(alGetSourcef), ENTRY(alGetSource3f),
	ENTRY(alGetSourcefv), ENTRY(alGetSourcei), ENTRY(alSourcePlay), ENTRY(alSourceStop), ENTRY(alSourceRewind),
	ENTRY(alSourcePause), ENTRY(alSourcePlayv), ENTRY(alSourceStopv), ENTRY(alSourceRewindv),
	ENTRY(alSourcePausev), ENTRY(alSourceQueueBuffers), ENTRY(alSourceUnqueueBuffers), ENTRY(alGenBuffers),
	ENTRY(alDeleteBuffers), ENTRY(alIsBuffer), ENTRY(alBufferData), ENTRY(alGetBufferf), ENTRY(alGetBufferi),
	ENTRY(alDopplerFactor), ENTRY(alDopplerVelocity), ENTRY(alDistanceModel), ENTRY(EAXSet), ENTRY(EAXGet),
	ENTRY(alcOpenDevice), ENTRY(alcCloseDevice), ENTRY(alcCreateContext), ENTRY(alcMakeContextCurrent),
	ENTRY(alcProcessContext), ENTRY(alcSuspendContext), ENTRY(alcDestroyContext), ENTRY(alcGetError),
	ENTRY(alcGetCurrentContext), ENTRY(alcGetContextsDevice), ENTRY(alcGetString), ENTRY(alcGetIntegerv),
	ENTRY(alcIsExtensionPresent), ENTRY(alcGetProcAddress), ENTRY(alcGetEnumValue),
	{ NULL, NULL } };
