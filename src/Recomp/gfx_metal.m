/*=============================================================================
	gfx_metal.m: Metal back end and macOS window/input for the D3D8 front end.

	Rendering goes to an offscreen back buffer at the game's resolution,
	which Present scales into the window's CAMetalLayer. Everything runs on
	the main thread (the guest's main thread is the process main thread).

	Environment:
	  SHARK_HEADLESS=1          no window (offscreen rendering only)
	  SHARK_DUMP=<dir>          write presented frames as PNGs to <dir>
	  SHARK_DUMP_EVERY=<n>      ... every n-th frame (default 60)
	  SHARK_MAXFPS=<n>          frame rate cap (default 60, 0 = uncapped)
=============================================================================*/
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "input.h"
#include "gfx_shaders.h"  /* generated: gfx_shaders_metal[], gfx_shaders_metal_len */

/* Must match VSU/FSU in gfx_shaders.metal. */
typedef struct { float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4]; } ULight;
typedef struct
{
	float wv[16], proj[16];
	float texmat[8][16];
	float vp[4], vpz[4];
	float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
	float global_ambient[4];
	float fog[4];
	ULight lights[8];
	uint32_t attr[16][4];
	uint32_t streams[8][4];
	uint32_t tci[8][4];
	uint32_t flags, matsrc, nlights;
	float point_size;
	float mat_power, pad[3];
} VSU;
typedef struct
{
	uint32_t stage[8][4];
	float bumpmat[8][4];
	float tfactor[4];
	float fogcolor[4];
	uint32_t flags, alphafunc;
	float alpharef;
	uint32_t pad;
} FSU;

#define RING_SIZE (48u << 20)
#define RING_COUNT 3

static struct
{
	int ok, headless, width, height;
	id<MTLDevice> dev;
	id<MTLCommandQueue> queue;
	id<MTLLibrary> lib;
	id<MTLFunction> vs, fs;
	id<MTLRenderPipelineState> blit;
	NSMutableDictionary* pipelines;
	NSMutableDictionary* depth_states;
	NSMutableDictionary* samplers;
	id<MTLTexture> color, depth, white2d, whitecube;
	NSWindow* window;
	NSView* view;
	CAMetalLayer* layer;
	id<MTLBuffer> ring[RING_COUNT];
	int ring_index;
	uint32_t ring_pos;
	dispatch_semaphore_t inflight;
	id<MTLCommandBuffer> cmd;
	id<MTLRenderCommandEncoder> enc;
	uint32_t clear_flags, clear_color, clear_stencil;
	float clear_z;
	char dump_path[1024];
	const char* dump_dir;
	int dump_every;
	unsigned frame;
	unsigned draws;
} G;

/*-----------------------------------------------------------------------------
	Window and input.
-----------------------------------------------------------------------------*/

@interface SharkView : NSView
@end
@implementation SharkView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
/* Over the game the system cursor is invisible (the game draws its own in
   menus); outside it, e.g. on the window's resize edges, it is normal. */
- (void)resetCursorRects
{
	static NSCursor* blank;
	if (!blank)
	{
		NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
		blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
	}
	[self addCursorRect:self.bounds cursor:blank];
}
@end

/* The game captures the mouse (SetCapture/ClipCursor) and reads relative
   motion through DirectInput, menus included. Capturing freezes and hides the
   system cursor.
     Full screen: captured whenever the window is focused.
     Windowed:    free until you click in the game (so the window can be moved
                  and resized); pressing Command releases it again.
   Focus loss (Cmd-Tab, minimise) always releases; a click captures again. */
static int Captured, IsFullscreen;
static double VirtualX = -1, VirtualY = -1;
static void set_capture(int on)
{
	if (on == Captured)
		return;
	Captured = on;
	VirtualX = VirtualY = -1;  /* the virtual cursor restarts from the current position */
	CGAssociateMouseAndMouseCursorPosition(on ? false : true);
	if (on)
		[NSCursor hide];
	else
		[NSCursor unhide];
}

@interface SharkWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation SharkWindowDelegate
- (void)windowDidBecomeKey:(NSNotification*)n { (void)n; set_capture(IsFullscreen); }
- (void)windowDidEnterFullScreen:(NSNotification*)n { (void)n; IsFullscreen = 1; set_capture([G.window isKeyWindow]); }
- (void)windowDidExitFullScreen:(NSNotification*)n { (void)n; IsFullscreen = 0; set_capture(0); }
- (void)windowDidResignKey:(NSNotification*)n { (void)n; set_capture(0); }
- (void)windowWillMiniaturize:(NSNotification*)n { (void)n; set_capture(0); }
- (BOOL)windowShouldClose:(NSWindow*)sender
{
	extern void hle_post_message(u32 hwnd, u32 msg, u32 wp, u32 lp);
	hle_post_message(0, 0x0010 /* WM_CLOSE */, 0, 0);
	return NO;
}
@end

/* macOS virtual key code -> DirectInput scan code. */
static const uint8_t MacToDik[128] = {
	[0] = 30, [1] = 31, [2] = 32, [3] = 33, [4] = 35, [5] = 34, [6] = 44, [7] = 45, [8] = 46, [9] = 47,
	[11] = 48, [12] = 16, [13] = 17, [14] = 18, [15] = 19, [16] = 21, [17] = 20, [18] = 2, [19] = 3,
	[20] = 4, [21] = 5, [22] = 7, [23] = 6, [24] = 13, [25] = 10, [26] = 8, [27] = 12, [28] = 9, [29] = 11,
	[30] = 27, [31] = 24, [32] = 22, [33] = 26, [34] = 23, [35] = 25, [36] = 28, [37] = 38, [38] = 36,
	[39] = 40, [40] = 37, [41] = 39, [42] = 43, [43] = 51, [44] = 53, [45] = 49, [46] = 50, [47] = 52,
	[48] = 15, [49] = 57, [50] = 41, [51] = 14, [53] = 1, [55] = 0xDB, [56] = 42, [57] = 58, [58] = 56,
	[59] = 29, [60] = 54, [61] = 0xB8, [62] = 0x9D, [65] = 83, [67] = 55, [69] = 78, [71] = 69, [75] = 0xB5,
	[76] = 0x9C, [78] = 74, [82] = 82, [83] = 79, [84] = 80, [85] = 81, [86] = 75, [87] = 76, [88] = 77,
	[89] = 71, [91] = 72, [92] = 73, [96] = 63, [97] = 64, [98] = 65, [99] = 61, [100] = 66, [101] = 67,
	[103] = 87, [109] = 68, [111] = 88, [114] = 0xD2, [115] = 0xC7, [116] = 0xC9, [117] = 0xD3, [118] = 62,
	[119] = 0xCF, [120] = 60, [121] = 0xD1, [122] = 59, [123] = 0xCB, [124] = 0xCD, [125] = 0xD0, [126] = 0xC8,
};

/* DirectInput scan code -> Windows virtual key. */
static uint8_t dik_to_vk(uint8_t k)
{
	static const char* letters = "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
	if (k >= 16 && k <= 50 && letters[k - 16])
		return (uint8_t)letters[k - 16];
	if (k >= 2 && k <= 10) return (uint8_t)('1' + k - 2);
	if (k == 11) return '0';
	if (k >= 59 && k <= 68) return (uint8_t)(0x70 + k - 59);
	switch (k)
	{
	case 1: return 0x1B; case 12: return 0xBD; case 13: return 0xBB; case 14: return 0x08; case 15: return 0x09;
	case 26: return 0xDB; case 27: return 0xDD; case 28: case 0x9C: return 0x0D; case 29: case 0x9D: return 0x11;
	case 39: return 0xBA; case 40: return 0xDE; case 41: return 0xC0; case 42: case 54: return 0x10; case 43: return 0xDC;
	case 51: return 0xBC; case 52: return 0xBE; case 53: return 0xBF; case 55: return 0x6A; case 56: case 0xB8: return 0x12;
	case 57: return 0x20; case 58: return 0x14; case 69: return 0x90; case 71: return 0x67; case 72: return 0x68;
	case 73: return 0x69; case 74: return 0x6D; case 75: return 0x64; case 76: return 0x65; case 77: return 0x66;
	case 78: return 0x6B; case 79: return 0x61; case 80: return 0x62; case 81: return 0x63; case 82: return 0x60;
	case 83: return 0x6E; case 87: return 0x7A; case 88: return 0x7B; case 0xB5: return 0x6F; case 0xC7: return 0x24;
	case 0xC8: return 0x26; case 0xC9: return 0x21; case 0xCB: return 0x25; case 0xCD: return 0x27; case 0xCF: return 0x23;
	case 0xD0: return 0x28; case 0xD1: return 0x22; case 0xD2: return 0x2D; case 0xD3: return 0x2E; case 0xDB: return 0x5B;
	default: return 0;
	}
}

extern void hle_post_message(u32 hwnd, u32 msg, u32 wp, u32 lp);
extern s32 g_cursor_x, g_cursor_y;
extern u8 g_key_state[256];

static void key_event(uint8_t dik, int down, int repeat)
{
	if (!dik)
		return;
	uint8_t vk = dik_to_vk(dik);
	if (!repeat)
		input_key(dik, down);
	if (vk)
	{
		g_key_state[vk] = down ? 0x80 : 0;
		if (vk == 0x10) g_key_state[dik == 54 ? 0xA1 : 0xA0] = down ? 0x80 : 0;
		if (vk == 0x11) g_key_state[dik == 0x9D ? 0xA3 : 0xA2] = down ? 0x80 : 0;
		if (vk == 0x12) g_key_state[dik == 0xB8 ? 0xA5 : 0xA4] = down ? 0x80 : 0;
		u32 lp = 1 | ((u32)(dik & 0x7f) << 16) | (dik & 0x80 ? 1u << 24 : 0) | (down ? (repeat ? 1u << 30 : 0) : 3u << 30);
		int sys = vk == 0x12;
		hle_post_message(0, down ? (sys ? 0x104u : 0x100u) : (sys ? 0x105u : 0x101u), vk, lp);
	}
}

/* While captured the macOS cursor is frozen, so the absolute position the game's
   menus follow (WM_MOUSEMOVE) comes from a virtual cursor moved by the relative
   motion, in game pixels. Uncaptured, it is the real position in the view. */

/* Relative motion (dx, dy in view points); `at` is the cursor's position in
   the view, needed only while uncaptured. */
static void mouse_motion(double dx, double dy, NSPoint at)
{
	NSSize sz = G.view.bounds.size;
	double sx = G.width / (sz.width > 0 ? sz.width : 1), sy = G.height / (sz.height > 0 ? sz.height : 1);
	double x, y;
	if (Captured)
	{
		if (VirtualX < 0)
		{
			VirtualX = g_cursor_x;
			VirtualY = g_cursor_y;
		}
		VirtualX += dx * sx;
		VirtualY += dy * sy;
		x = VirtualX;
		y = VirtualY;
	}
	else
	{
		x = at.x * sx;
		y = (sz.height - at.y) * sy;
	}
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > G.width - 1) x = G.width - 1;
	if (y > G.height - 1) y = G.height - 1;
	if (Captured)
	{
		VirtualX = x;
		VirtualY = y;
	}
	g_cursor_x = (int)x;
	g_cursor_y = (int)y;
	/* DirectInput gets the raw motion; keep the fractional part between events. */
	static double fx, fy;
	fx += dx;
	fy += dy;
	s32 idx = (s32)fx, idy = (s32)fy;
	fx -= idx;
	fy -= idy;
	if (idx || idy)
		input_mouse_move(idx, idy);
	hle_post_message(0, 0x0200 /* WM_MOUSEMOVE */, 0, (u32)(g_cursor_x & 0xffff) | ((u32)g_cursor_y << 16));
}

static void mouse_position(NSEvent* e)
{
	mouse_motion([e deltaX], [e deltaY], [G.view convertPoint:[e locationInWindow] fromView:nil]);
}

static void mouse_button(NSEvent* e, int button, int down)
{
	(void)e;
	static const u32 msgs[3][2] = { { 0x202, 0x201 }, { 0x205, 0x204 }, { 0x208, 0x207 } };
	static const u8 vks[3] = { 0x01, 0x02, 0x04 };
	input_mouse_button(button, down);
	g_key_state[vks[button]] = down ? 0x80 : 0;
	hle_post_message(0, msgs[button][down], 0, (u32)(g_cursor_x & 0xffff) | ((u32)g_cursor_y << 16));
}

/* Synthetic input for the game controller (gamepad.m). Using the pad
   captures the mouse, as a click in the game does. */
void host_key(u8 dik, int down) { key_event(dik, down, 0); }
void host_mouse_button(int button, int down)
{
	if (down && !Captured)
		set_capture(1);
	mouse_button(nil, button, down);
}
void host_mouse_motion(double dx, double dy)
{
	if (!Captured)
		set_capture(1);
	mouse_motion(dx, dy, NSZeroPoint);
}

void gfx_pump_events(void)
{
	if (!G.ok || G.headless)
		return;
	@autoreleasepool
	{
		NSEvent* e;
		while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
										  inMode:NSDefaultRunLoopMode dequeue:YES]))
		{
			int forward = 1;
			switch ([e type])
			{
			case NSEventTypeKeyDown:
			case NSEventTypeKeyUp:
			{
				unsigned short code = [e keyCode];
				int down = [e type] == NSEventTypeKeyDown;
				if (down && ([e modifierFlags] & NSEventModifierFlagCommand) && code == 12 /* Cmd-Q */)
				{
					hle_post_message(0, 0x0010, 0, 0);
					forward = 0;
					break;
				}
				key_event(code < 128 ? MacToDik[code] : 0, down, down && [e isARepeat]);
				if (down)
				{
					NSString* s = [e characters];
					for (NSUInteger i = 0; i < [s length]; i++)
					{
						unichar ch = [s characterAtIndex:i];
						if (ch == 0x7f) ch = 8;           /* backspace */
						if (ch == 3) ch = 13;             /* keypad enter */
						if (ch < 0xF700)                  /* skip function-key private-use codes */
							hle_post_message(0, 0x0102 /* WM_CHAR */, ch, 1);
					}
				}
				forward = 0;  /* don't beep */
				break;
			}
			case NSEventTypeFlagsChanged:
			{
				unsigned short code = [e keyCode];
				NSEventModifierFlags f = [e modifierFlags];
				int down = 0;
				switch (code)
				{
				case 56: case 60: down = (f & NSEventModifierFlagShift) != 0; break;
				case 59: case 62: down = (f & NSEventModifierFlagControl) != 0; break;
				case 58: case 61: down = (f & NSEventModifierFlagOption) != 0; break;
				case 55: case 54:
					down = (f & NSEventModifierFlagCommand) != 0;
					if (down && !IsFullscreen)
						set_capture(0);  /* windowed: Command frees the mouse */
					break;
				case 57: down = (f & NSEventModifierFlagCapsLock) != 0; break;
				}
				key_event(code < 128 ? MacToDik[code] : 0, down, 0);
				break;
			}
			case NSEventTypeMouseMoved:
			case NSEventTypeLeftMouseDragged:
			case NSEventTypeRightMouseDragged:
			case NSEventTypeOtherMouseDragged:
				mouse_position(e);
				break;
			case NSEventTypeLeftMouseDown:
				if (!Captured && [G.window isKeyWindow])
				{
					NSPoint p = [G.view convertPoint:[e locationInWindow] fromView:nil];
					if (NSPointInRect(p, G.view.bounds))
					{
						set_capture(1);  /* this click only captures */
						break;
					}
				}
				mouse_button(e, 0, 1);
				break;
			case NSEventTypeLeftMouseUp: mouse_button(e, 0, 0); break;
			case NSEventTypeRightMouseDown: mouse_button(e, 1, 1); break;
			case NSEventTypeRightMouseUp: mouse_button(e, 1, 0); break;
			case NSEventTypeOtherMouseDown: mouse_button(e, 2, 1); break;
			case NSEventTypeOtherMouseUp: mouse_button(e, 2, 0); break;
			case NSEventTypeScrollWheel:
			{
				double dy = [e scrollingDeltaY];
				if ([e hasPreciseScrollingDeltas])
					dy /= 10;
				int steps = (int)(dy > 0 ? dy + 0.5 : dy - 0.5);
				if (steps)
				{
					input_mouse_wheel(steps * 120);
					hle_post_message(0, 0x020A, (u32)(steps * 120) << 16,
						(u32)(g_cursor_x & 0xffff) | ((u32)g_cursor_y << 16));
				}
				break;
			}
			default:
				break;
			}
			if (forward)
				[NSApp sendEvent:e];
		}
	}
}

void hle_platform_pump(void) { gfx_pump_events(); }

/* SHARK_SCRIPT="<frame>:<action>,..." injects input for automated runs.
   Actions: k<dik> (press, released 4 frames later), c<x>/<y> (left click),
   m<x>/<y> (move the cursor), t (trace the next frame's draws), p (dump the next frame),
   x<console command> (run through UGameEngine::Exec), q (close the window). */
static void run_script(unsigned frame)
{
	static const char* script;
	static int init;
	static struct { unsigned frame; uint8_t dik; } releases[64];
	static int nrel;
	if (!init)
	{
		init = 1;
		script = getenv("SHARK_SCRIPT");
	}
	for (int i = 0; i < nrel; i++)
		if (releases[i].frame == frame)
		{
			if (releases[i].dik == 0xff)
				mouse_button(nil, 0, 0);
			else
				key_event(releases[i].dik, 0, 0);
			releases[i] = releases[--nrel];
			i--;
		}
	if (!script)
		return;
	for (const char* p = script; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
	{
		char* end;
		unsigned f = (unsigned)strtoul(p, &end, 10);
		if (f != frame || *end != ':')
			continue;
		char kind = end[1];
		if (kind == 'x')
		{
			/* x<console command> up to the next comma */
			char cmd[256];
			const char* e = strchr(end + 2, ',');
			size_t n = e ? (size_t)(e - (end + 2)) : strlen(end + 2);
			if (n >= sizeof cmd)
				n = sizeof cmd - 1;
			memcpy(cmd, end + 2, n);
			cmd[n] = 0;
			extern void rt_console_queue(const char*);
			rt_console_queue(cmd);
			fprintf(stderr, "[script] frame %u: console %s\n", frame, cmd);
			continue;
		}
		int a = (int)strtol(end + 2, &end, 10), b = *end == '/' ? (int)strtol(end + 1, NULL, 10) : 0;
		fprintf(stderr, "[script] frame %u: %c %d %d\n", frame, kind, a, b);
		if (kind == 'q')
		{
			hle_post_message(0, 0x0010 /* WM_CLOSE */, 0, 0);
		}
		else if (kind == 't')
		{
			extern void d3d8_trace_next_frame(void);
			d3d8_trace_next_frame();
		}
		else if (kind == 'p')
		{
			char path[1100];
			snprintf(path, sizeof path, "%s/shot_%05u.png", G.dump_dir ? G.dump_dir : ".", frame);
			gfx_dump_next_frame(path);
		}
		else if (kind == 'k')
		{
			key_event((uint8_t)a, 1, 0);
			if (nrel < 64) releases[nrel++] = (typeof(releases[0])){ frame + 4, (uint8_t)a };
		}
		else if (kind == 'c' || kind == 'm')
		{
			g_cursor_x = a;
			g_cursor_y = b;
			hle_post_message(0, 0x0200, 0, (u32)(a & 0xffff) | ((u32)b << 16));
			if (kind == 'c')
			{
				mouse_button(nil, 0, 1);
				if (nrel < 64) releases[nrel++] = (typeof(releases[0])){ frame + 4, 0xff };
			}
		}
	}
}

/*-----------------------------------------------------------------------------
	Init.
-----------------------------------------------------------------------------*/

static id<MTLTexture> make_target(MTLPixelFormat fmt, int w, int h, MTLStorageMode storage)
{
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)w
																			   height:(NSUInteger)h mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = storage;
	return [G.dev newTextureWithDescriptor:d];
}

static void make_targets(void)
{
	G.color = make_target(MTLPixelFormatBGRA8Unorm, G.width, G.height, MTLStorageModeShared);
	G.depth = make_target(MTLPixelFormatDepth32Float_Stencil8, G.width, G.height, MTLStorageModePrivate);
	G.clear_flags = 7;
	G.clear_color = 0xff000000u;
	G.clear_z = 1;
	G.clear_stencil = 0;
}

int gfx_init(int width, int height, int fullscreen)
{
	/* fullscreen: the game's StartupFullscreen setting; the window enters macOS full screen. */
	if (G.ok)
	{
		gfx_resize(width, height);
		return 1;
	}
	@autoreleasepool
	{
		G.dev = MTLCreateSystemDefaultDevice();
		if (!G.dev)
		{
			fprintf(stderr, "[gfx] no Metal device\n");
			return 0;
		}
		if (![G.dev supportsBCTextureCompression])
			fprintf(stderr, "[gfx] warning: GPU lacks BC texture compression\n");
		G.queue = [G.dev newCommandQueue];
		NSError* err = nil;
		NSString* src = [[NSString alloc] initWithBytes:gfx_shaders_metal length:gfx_shaders_metal_len
											   encoding:NSUTF8StringEncoding];
		MTLCompileOptions* opts = [MTLCompileOptions new];
		opts.mathMode = MTLMathModeFast;
		G.lib = [G.dev newLibraryWithSource:src options:opts error:&err];
		if (!G.lib)
		{
			fprintf(stderr, "[gfx] shader compile failed: %s\n", [[err description] UTF8String]);
			return 0;
		}
		G.vs = [G.lib newFunctionWithName:@"vs_main"];
		G.fs = [G.lib newFunctionWithName:@"fs_main"];
		MTLRenderPipelineDescriptor* bd = [MTLRenderPipelineDescriptor new];
		bd.vertexFunction = [G.lib newFunctionWithName:@"blit_vs"];
		bd.fragmentFunction = [G.lib newFunctionWithName:@"blit_fs"];
		bd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		G.blit = [G.dev newRenderPipelineStateWithDescriptor:bd error:&err];
		G.pipelines = [NSMutableDictionary new];
		G.depth_states = [NSMutableDictionary new];
		G.samplers = [NSMutableDictionary new];
		for (int i = 0; i < RING_COUNT; i++)
			G.ring[i] = [G.dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
		G.inflight = dispatch_semaphore_create(RING_COUNT);

		uint32_t white[6] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
		MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																					   width:1 height:1 mipmapped:NO];
		G.white2d = [G.dev newTextureWithDescriptor:wd];
		[G.white2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
		MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																						 size:1 mipmapped:NO];
		G.whitecube = [G.dev newTextureWithDescriptor:cd];
		for (int f = 0; f < 6; f++)
			[G.whitecube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:(NSUInteger)f withBytes:white
						   bytesPerRow:4 bytesPerImage:4];

		G.width = width;
		G.height = height;
		make_targets();

		const char* dump = getenv("SHARK_DUMP");
		G.dump_dir = dump && *dump ? dump : NULL;
		G.dump_every = getenv("SHARK_DUMP_EVERY") ? atoi(getenv("SHARK_DUMP_EVERY")) : 60;
		if (G.dump_every < 1)
			G.dump_every = 1;
		G.headless = getenv("SHARK_HEADLESS") && atoi(getenv("SHARK_HEADLESS"));

		if (!G.headless)
		{
			[NSApplication sharedApplication];
			[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
			[NSApp finishLaunching];
			NSRect screen = [[NSScreen mainScreen] visibleFrame];
			CGFloat w = width, h = height;
			CGFloat fit = fmin(1.0, fmin(screen.size.width * 0.9 / w, screen.size.height * 0.9 / h));
			NSRect r = NSMakeRect(0, 0, w * fit, h * fit);
			G.window = [[NSWindow alloc] initWithContentRect:r
												   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
															 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
													 backing:NSBackingStoreBuffered defer:NO];
			[G.window setTitle:@"Shark Tale"];
			[G.window setContentAspectRatio:NSMakeSize(w, h)];
			[G.window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
			static SharkWindowDelegate* delegate;
			delegate = [SharkWindowDelegate new];
			[G.window setDelegate:delegate];
			[G.window setReleasedWhenClosed:NO];
			G.view = [[SharkView alloc] initWithFrame:r];
			[G.view setWantsLayer:YES];
			G.layer = (CAMetalLayer*)[G.view layer];
			G.layer.device = G.dev;
			G.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			G.layer.framebufferOnly = YES;
			G.layer.displaySyncEnabled = getenv("SHARK_NOVSYNC") ? NO : YES;
			G.layer.maximumDrawableCount = 3;
			[G.window setContentView:G.view];
			[G.window center];
			[G.window makeKeyAndOrderFront:nil];
			[G.window makeFirstResponder:G.view];
			[G.window setAcceptsMouseMovedEvents:YES];
			[NSApp activateIgnoringOtherApps:YES];
			if (fullscreen && !getenv("SHARK_WINDOWED"))
				[G.window toggleFullScreen:nil];
		}
		G.ok = 1;
		fprintf(stderr, "[gfx] Metal on %s, %dx%d%s\n", [[G.dev name] UTF8String], width, height,
			G.headless ? " (headless)" : "");
	}
	return 1;
}

void gfx_resize(int width, int height)
{
	if (width == G.width && height == G.height)
		return;
	G.width = width;
	G.height = height;
	make_targets();
	if (G.window)
		[G.window setContentAspectRatio:NSMakeSize(width, height)];
}

/*-----------------------------------------------------------------------------
	Textures.
-----------------------------------------------------------------------------*/

void* gfx_texture_create(int format, int width, int height, int levels, int is_cube, int render_target)
{
	MTLPixelFormat f = format == GFX_BC1 ? MTLPixelFormatBC1_RGBA : format == GFX_BC2 ? MTLPixelFormatBC2_RGBA
		: format == GFX_BC3 ? MTLPixelFormatBC3_RGBA : MTLPixelFormatBGRA8Unorm;
	MTLTextureDescriptor* d = is_cube
		? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:f size:(NSUInteger)width mipmapped:levels > 1]
		: [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:(NSUInteger)width height:(NSUInteger)height
														 mipmapped:levels > 1];
	d.mipmapLevelCount = (NSUInteger)(levels < 1 ? 1 : levels);
	d.usage = MTLTextureUsageShaderRead | (render_target && f == MTLPixelFormatBGRA8Unorm ? MTLTextureUsageRenderTarget : 0);
	id<MTLTexture> t = [G.dev newTextureWithDescriptor:d];
	return t ? (__bridge_retained void*)t : NULL;
}

void gfx_texture_upload(void* tex, int face, int level, int width, int height, const void* data, int pitch)
{
	id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
	if (!t || level >= (int)t.mipmapLevelCount)
		return;
	MTLPixelFormat f = t.pixelFormat;
	NSUInteger rows = f == MTLPixelFormatBGRA8Unorm ? (NSUInteger)height : (NSUInteger)(height + 3) / 4;
	[t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height) mipmapLevel:(NSUInteger)level
			   slice:(NSUInteger)face withBytes:data bytesPerRow:(NSUInteger)pitch bytesPerImage:(NSUInteger)pitch * rows];
}

void gfx_texture_destroy(void* tex)
{
	if (tex)
		CFRelease(tex);
}

/*-----------------------------------------------------------------------------
	Frame and pass management.
-----------------------------------------------------------------------------*/

static void begin_frame(void)
{
	if (G.cmd)
		return;
	dispatch_semaphore_wait(G.inflight, DISPATCH_TIME_FOREVER);
	G.cmd = [G.queue commandBuffer];
	G.ring_index = (G.ring_index + 1) % RING_COUNT;
	G.ring_pos = 0;
	dispatch_semaphore_t sem = G.inflight;
	[G.cmd addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(sem); }];
}

static void end_pass(void)
{
	if (G.enc)
	{
		[G.enc endEncoding];
		G.enc = nil;
	}
}

static void begin_pass(void)
{
	if (G.enc)
		return;
	begin_frame();
	MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
	p.colorAttachments[0].texture = G.color;
	p.colorAttachments[0].storeAction = MTLStoreActionStore;
	if (G.clear_flags & 1)
	{
		uint32_t c = G.clear_color;
		p.colorAttachments[0].loadAction = MTLLoadActionClear;
		p.colorAttachments[0].clearColor = MTLClearColorMake(((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
			(c & 255) / 255.0, (c >> 24) / 255.0);
	}
	else
		p.colorAttachments[0].loadAction = MTLLoadActionLoad;
	p.depthAttachment.texture = G.depth;
	p.depthAttachment.storeAction = MTLStoreActionStore;
	p.depthAttachment.loadAction = (G.clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
	p.depthAttachment.clearDepth = G.clear_z;
	p.stencilAttachment.texture = G.depth;
	p.stencilAttachment.storeAction = MTLStoreActionStore;
	p.stencilAttachment.loadAction = (G.clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
	p.stencilAttachment.clearStencil = G.clear_stencil;
	G.clear_flags = 0;
	G.enc = [G.cmd renderCommandEncoderWithDescriptor:p];
}

void gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const int* rects, int nrects)
{
	(void)rects;
	(void)nrects;
	if (!G.ok)
		return;
	/* D3DCLEAR_TARGET 1, ZBUFFER 2, STENCIL 4: restart the pass with clear load actions. */
	end_pass();
	G.clear_flags |= flags & 7;
	if (flags & 1) G.clear_color = color;
	if (flags & 2) G.clear_z = z;
	if (flags & 4) G.clear_stencil = stencil;
}

static void write_png(id<MTLTexture> t, const char* path)
{
	NSUInteger w = t.width, h = t.height, pitch = w * 4;
	uint8_t* px = malloc(pitch * h);
	[t getBytes:px bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
	for (NSUInteger i = 0; i < w * h; i++)
		px[i * 4 + 3] = 255;
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, pitch, cs,
		kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef img = CGBitmapContextCreateImage(ctx);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
	if (dst)
	{
		CGImageDestinationAddImage(dst, img, NULL);
		CGImageDestinationFinalize(dst);
		CFRelease(dst);
	}
	CFRelease(url);
	CGImageRelease(img);
	CGContextRelease(ctx);
	CGColorSpaceRelease(cs);
	free(px);
}

/* Finish all queued GPU work for this frame so the CPU can see the back buffer. */
static void flush_gpu(void)
{
	if (G.clear_flags)
		begin_pass();
	end_pass();
	if (G.cmd)
	{
		[G.cmd commit];
		[G.cmd waitUntilCompleted];
		G.cmd = nil;
	}
}

void gfx_backbuffer_read(void* dst, int pitch)
{
	if (!G.ok)
		return;
	@autoreleasepool
	{
		flush_gpu();
		[G.color getBytes:dst bytesPerRow:(NSUInteger)pitch fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)G.width, (NSUInteger)G.height)
			  mipmapLevel:0];
	}
}

void gfx_backbuffer_write(const void* src, int pitch)
{
	if (!G.ok)
		return;
	@autoreleasepool
	{
		/* Ordered with the frame's other GPU work: a blit from a staging copy. */
		begin_frame();
		if (G.clear_flags)
			begin_pass();
		end_pass();
		id<MTLBuffer> staging = [G.dev newBufferWithBytes:src length:(NSUInteger)pitch * (NSUInteger)G.height
												  options:MTLResourceStorageModeShared];
		id<MTLBlitCommandEncoder> blit = [G.cmd blitCommandEncoder];
		[blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:(NSUInteger)pitch
			sourceBytesPerImage:(NSUInteger)pitch * (NSUInteger)G.height
					 sourceSize:MTLSizeMake((NSUInteger)G.width, (NSUInteger)G.height, 1)
					  toTexture:G.color destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
	}
}

void gfx_dump_next_frame(const char* path) { snprintf(G.dump_path, sizeof G.dump_path, "%s", path); }

/* Sleeps until the next frame slot so the game runs no faster than
   SHARK_MAXFPS (default 60). Deadlines are absolute, so frames do not drift;
   after a long stall the schedule restarts instead of racing to catch up. */
static void limit_frame_rate(void)
{
	static int init;
	static uint64_t period, next;
	if (!init)
	{
		init = 1;
		const char* env = getenv("SHARK_MAXFPS");
		double fps = env ? atof(env) : 60.0;
		if (fps > 0)
		{
			mach_timebase_info_data_t tb;
			mach_timebase_info(&tb);
			period = (uint64_t)(1e9 / fps * tb.denom / tb.numer);
		}
	}
	if (!period)
		return;
	uint64_t now = mach_absolute_time();
	if (!next || now > next + period)
		next = now;
	else if (now < next)
		mach_wait_until(next);
	next += period;
}

void gfx_present(void)
{
	if (!G.ok)
		return;
	@autoreleasepool
	{
		if (G.clear_flags || !G.cmd)
			begin_pass();
		end_pass();
		id<CAMetalDrawable> drawable = G.layer ? [G.layer nextDrawable] : nil;
		if (drawable)
		{
			MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
			p.colorAttachments[0].texture = drawable.texture;
			p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
			p.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> e = [G.cmd renderCommandEncoderWithDescriptor:p];
			[e setRenderPipelineState:G.blit];
			[e setFragmentTexture:G.color atIndex:0];
			[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[e endEncoding];
			[G.cmd presentDrawable:drawable];
		}
		int dump = G.dump_path[0] || (G.dump_dir && G.frame % (unsigned)G.dump_every == 0);
		[G.cmd commit];
		if (dump)
		{
			[G.cmd waitUntilCompleted];
			char path[1100];
			if (G.dump_path[0])
				snprintf(path, sizeof path, "%s", G.dump_path);
			else
				snprintf(path, sizeof path, "%s/frame_%05u.png", G.dump_dir, G.frame);
			write_png(G.color, path);
			fprintf(stderr, "[gfx] frame %u (%u draws) -> %s\n", G.frame, G.draws, path);
			G.dump_path[0] = 0;
		}
		G.cmd = nil;
		G.frame++;
		G.draws = 0;
		/* Once a frame, not per event pump: the game pumps on every PeekMessage
		   and would never see its queue empty if each pump posted a mouse move. */
		if (!G.headless)
			gamepad_poll([G.window isKeyWindow]);
		limit_frame_rate();
		/* Drawable resizing follows the view (Retina backing). */
		if (G.layer)
		{
			CGSize s = G.view.bounds.size;
			CGFloat scale = G.window.backingScaleFactor;
			CGSize want = CGSizeMake(s.width * scale, s.height * scale);
			if (want.width > 0 && !CGSizeEqualToSize(want, G.layer.drawableSize))
				G.layer.drawableSize = want;
		}
	}
	gfx_pump_events();
	run_script(G.frame);
}

/*-----------------------------------------------------------------------------
	State objects.
-----------------------------------------------------------------------------*/

static MTLBlendFactor blend_factor(uint32_t b, int alpha)
{
	switch (b)
	{
	case 1: return MTLBlendFactorZero;
	case 2: return MTLBlendFactorOne;
	case 3: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
	case 4: return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
	case 5: return MTLBlendFactorSourceAlpha;
	case 6: return MTLBlendFactorOneMinusSourceAlpha;
	case 7: return MTLBlendFactorDestinationAlpha;
	case 8: return MTLBlendFactorOneMinusDestinationAlpha;
	case 9: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
	case 10: return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
	case 11: return MTLBlendFactorSourceAlphaSaturated;
	default: return MTLBlendFactorOne;
	}
}

static MTLBlendOperation blend_op(uint32_t op)
{
	switch (op)
	{
	case 2: return MTLBlendOperationSubtract;
	case 3: return MTLBlendOperationReverseSubtract;
	case 4: return MTLBlendOperationMin;
	case 5: return MTLBlendOperationMax;
	default: return MTLBlendOperationAdd;
	}
}

static id<MTLRenderPipelineState> pipeline_for(const GfxState* s)
{
	uint32_t blend = s->rs[27] ? 1 : 0;
	uint32_t src = s->rs[19], dst = s->rs[20];
	if (src == 12) { src = 5; dst = 6; }       /* BOTHSRCALPHA */
	else if (src == 13) { src = 6; dst = 5; }  /* BOTHINVSRCALPHA */
	uint32_t key = blend | (src & 15) << 1 | (dst & 15) << 5 | (s->rs[171] & 7) << 9 | (s->rs[168] & 15) << 12;
	if (!blend)
		key &= ~0xffeu | (0xfu << 12);
	NSNumber* k = @(key);
	id<MTLRenderPipelineState> p = G.pipelines[k];
	if (p)
		return p;
	MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction = G.vs;
	d.fragmentFunction = G.fs;
	d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	MTLRenderPipelineColorAttachmentDescriptor* ca = d.colorAttachments[0];
	uint32_t wm = s->rs[168] & 15;
	ca.writeMask = (wm & 1 ? MTLColorWriteMaskRed : 0) | (wm & 2 ? MTLColorWriteMaskGreen : 0) |
		(wm & 4 ? MTLColorWriteMaskBlue : 0) | (wm & 8 ? MTLColorWriteMaskAlpha : 0);
	if (blend)
	{
		ca.blendingEnabled = YES;
		ca.sourceRGBBlendFactor = blend_factor(src, 0);
		ca.destinationRGBBlendFactor = blend_factor(dst, 0);
		ca.sourceAlphaBlendFactor = blend_factor(src, 1);
		ca.destinationAlphaBlendFactor = blend_factor(dst, 1);
		ca.rgbBlendOperation = ca.alphaBlendOperation = blend_op(s->rs[171]);
	}
	NSError* err = nil;
	p = [G.dev newRenderPipelineStateWithDescriptor:d error:&err];
	if (!p)
		fprintf(stderr, "[gfx] pipeline failed: %s\n", [[err description] UTF8String]);
	else
		G.pipelines[k] = p;
	return p;
}

static MTLCompareFunction compare_fn(uint32_t f)
{
	static const MTLCompareFunction m[] = { MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess,
		MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
		MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
	return f <= 8 ? m[f] : MTLCompareFunctionAlways;
}

static MTLStencilOperation stencil_op(uint32_t o)
{
	static const MTLStencilOperation m[] = { MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero,
		MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp,
		MTLStencilOperationInvert, MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap };
	return o <= 8 ? m[o] : MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> depth_state_for(const GfxState* s)
{
	uint64_t z = s->rs[7] ? 1 : 0, zw = s->rs[14] ? 1 : 0, st = s->rs[52] ? 1 : 0;
	uint64_t key = z | zw << 1 | (uint64_t)(s->rs[23] & 15) << 2 | st << 6;
	if (st)
		key |= (uint64_t)(s->rs[53] & 15) << 7 | (uint64_t)(s->rs[54] & 15) << 11 | (uint64_t)(s->rs[55] & 15) << 15 |
			(uint64_t)(s->rs[56] & 15) << 19 | (uint64_t)(s->rs[58] & 255) << 23 | (uint64_t)(s->rs[59] & 255) << 31;
	NSNumber* k = @(key);
	id<MTLDepthStencilState> d = G.depth_states[k];
	if (d)
		return d;
	MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = z ? compare_fn(s->rs[23]) : MTLCompareFunctionAlways;
	dd.depthWriteEnabled = z && zw;
	if (st)
	{
		MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
		sd.stencilFailureOperation = stencil_op(s->rs[53]);
		sd.depthFailureOperation = stencil_op(s->rs[54]);
		sd.depthStencilPassOperation = stencil_op(s->rs[55]);
		sd.stencilCompareFunction = compare_fn(s->rs[56]);
		sd.readMask = s->rs[58] & 255;
		sd.writeMask = s->rs[59] & 255;
		dd.frontFaceStencil = sd;
		dd.backFaceStencil = sd;
	}
	d = [G.dev newDepthStencilStateWithDescriptor:dd];
	G.depth_states[k] = d;
	return d;
}

static MTLSamplerAddressMode address_mode(uint32_t a)
{
	switch (a)
	{
	case 2: return MTLSamplerAddressModeMirrorRepeat;
	case 3: return MTLSamplerAddressModeClampToEdge;
	case 4: return MTLSamplerAddressModeClampToBorderColor;
	case 5: return MTLSamplerAddressModeMirrorClampToEdge;
	default: return MTLSamplerAddressModeRepeat;
	}
}

static id<MTLSamplerState> sampler_for(const uint32_t* ts)
{
	uint32_t key = (ts[13] & 7) | (ts[14] & 7) << 3 | (ts[25] & 7) << 6 | (ts[16] & 3) << 9 | (ts[17] & 3) << 11 |
		(ts[18] & 3) << 13 | (ts[21] & 31) << 15 | (ts[20] & 15) << 20;
	NSNumber* k = @(key);
	id<MTLSamplerState> s = G.samplers[k];
	if (s)
		return s;
	MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
	d.sAddressMode = address_mode(ts[13]);
	d.tAddressMode = address_mode(ts[14]);
	d.rAddressMode = address_mode(ts[25]);
	d.magFilter = ts[16] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	d.minFilter = ts[17] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	d.mipFilter = ts[18] == 0 ? MTLSamplerMipFilterNotMipmapped
		: ts[18] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
	if (ts[18] == 0)
		d.lodMaxClamp = 0;
	d.lodMinClamp = (float)(ts[20] & 15);
	uint32_t aniso = ts[17] == 3 || ts[16] == 3 ? (ts[21] ? ts[21] : 1) : 1;
	d.maxAnisotropy = aniso > 16 ? 16 : aniso;
	s = [G.dev newSamplerStateWithDescriptor:d];
	G.samplers[k] = s;
	return s;
}

/*-----------------------------------------------------------------------------
	Drawing.
-----------------------------------------------------------------------------*/

static void unpack_color(uint32_t c, float* out)
{
	out[0] = ((c >> 16) & 255) / 255.0f;
	out[1] = ((c >> 8) & 255) / 255.0f;
	out[2] = (c & 255) / 255.0f;
	out[3] = (c >> 24) / 255.0f;
}

/* Row-vector D3D math: r = a * b. */
static void mat_mul(const float* a, const float* b, float* r)
{
	float t[16];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
	memcpy(r, t, sizeof t);
}
static void xform(const float* v, float w, const float* m, float* out)
{
	for (int j = 0; j < 3; j++)
		out[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + w * m[12 + j];
}

void gfx_draw(const GfxState* s, int prim, int vertex_count, const void* const* streams, const uint32_t* strides,
	const uint32_t* stream_sizes, const void* indices, int index_size, int index_count, int base_vertex)
{
	(void)base_vertex;
	if (!G.ok || vertex_count <= 0)
		return;
	@autoreleasepool
	{
		begin_pass();
		id<MTLRenderPipelineState> pipe = pipeline_for(s);
		if (!pipe)
			return;

		/* Vertex data: all used streams packed into one buffer. */
		uint32_t total = 0;
		for (int i = 0; i < GFX_MAX_STREAMS; i++)
			if (streams[i])
				total += ((stream_sizes[i] + 255) & ~255u) + 256;
		total += (uint32_t)(index_count * 4) + 512;
		id<MTLBuffer> vbuf = nil;
		uint32_t base = 0;
		uint8_t* dst;
		if (((G.ring_pos + 255) & ~255u) + total <= RING_SIZE)
		{
			vbuf = G.ring[G.ring_index];
			base = (G.ring_pos + 255) & ~255u;
			G.ring_pos = base + total;
		}
		else
			vbuf = [G.dev newBufferWithLength:total options:MTLResourceStorageModeShared];
		dst = (uint8_t*)[vbuf contents];

		VSU vu;
		memset(&vu, 0, sizeof vu);
		uint32_t pos = base;
		for (int i = 0; i < GFX_MAX_STREAMS; i++)
		{
			if (!streams[i])
				continue;
			memcpy(dst + pos, streams[i], stream_sizes[i]);
			vu.streams[i][0] = pos;
			vu.streams[i][1] = strides[i];
			pos += (stream_sizes[i] + 255) & ~255u;
		}
		uint32_t index_offset = (pos + 255) & ~255u;
		if (indices)
		{
			if (index_size == 4)
				memcpy(dst + index_offset, indices, (size_t)index_count * 4);
			else
				memcpy(dst + index_offset, indices, (size_t)index_count * 2);
		}

		/* Vertex uniforms. */
		mat_mul(s->world, s->view, vu.wv);
		memcpy(vu.proj, s->proj, 64);
		memcpy(vu.texmat, s->texmat, sizeof vu.texmat);
		vu.vp[0] = s->viewport[0];
		vu.vp[1] = s->viewport[1];
		vu.vp[2] = s->viewport[2] > 0 ? s->viewport[2] : G.width;
		vu.vp[3] = s->viewport[3] > 0 ? s->viewport[3] : G.height;
		vu.vpz[0] = s->viewport[4];
		vu.vpz[1] = s->viewport[5];
		memcpy(vu.mat_diffuse, &s->material[0], 16);
		memcpy(vu.mat_ambient, &s->material[4], 16);
		memcpy(vu.mat_specular, &s->material[8], 16);
		memcpy(vu.mat_emissive, &s->material[12], 16);
		vu.mat_power = s->material[16] > 0 ? s->material[16] : 1;
		unpack_color(s->rs[139], vu.global_ambient);
		float fs, fe, fd;
		memcpy(&fs, &s->rs[36], 4);
		memcpy(&fe, &s->rs[37], 4);
		memcpy(&fd, &s->rs[38], 4);
		vu.fog[0] = fs;
		vu.fog[1] = fe;
		vu.fog[2] = fd;
		uint32_t fogmode = s->rs[35] ? s->rs[35] : s->rs[140];
		vu.fog[3] = s->rs[28] ? (float)fogmode : 0;
		for (int a = 0; a < 16; a++)
		{
			const GfxAttrib* at = a < VS_COUNT ? &s->attrib[a] : NULL;
			vu.attr[a][0] = at ? at->stream : 0;
			vu.attr[a][1] = at ? at->type : VT_NONE;
			vu.attr[a][2] = at ? at->offset : 0;
		}
		uint32_t flags = 0;
		if (s->pretransformed) flags |= 1;
		if (s->rs[137] && !s->pretransformed) flags |= 2;
		if (s->rs[143]) flags |= 4;
		if (s->rs[29]) flags |= 8;
		if (s->rs[142]) flags |= 16;
		if (s->rs[141]) flags |= 32;
		if (s->rs[28] && !s->rs[35] && (s->pretransformed || !s->rs[140])) flags |= 64;
		if (s->rs[48]) flags |= 128;
		vu.flags = flags;
		vu.matsrc = (s->rs[145] & 3) | (s->rs[146] & 3) << 2 | (s->rs[147] & 3) << 4 | (s->rs[148] & 3) << 6;
		float psize;
		memcpy(&psize, &s->rs[154], 4);
		vu.point_size = psize > 0 ? psize : 1;
		int nl = 0;
		if (flags & 2)
			for (int i = 0; i < GFX_MAX_LIGHTS; i++)
			{
				const GfxLight* L = &s->lights[i];
				if (!L->type)
					continue;
				ULight* u = &vu.lights[nl++];
				memcpy(u->diffuse, L->diffuse, 16);
				memcpy(u->specular, L->specular, 16);
				memcpy(u->ambient, L->ambient, 16);
				xform(L->position, 1, s->view, u->position);
				u->position[3] = L->range > 0 ? L->range : 1e30f;
				xform(L->direction, 0, s->view, u->direction);
				u->direction[3] = L->falloff;
				u->atten[0] = L->att0;
				u->atten[1] = L->att1;
				u->atten[2] = L->att2;
				u->atten[3] = (float)L->type;
				u->spot[0] = cosf(L->theta * 0.5f);
				u->spot[1] = cosf(L->phi * 0.5f);
			}
		vu.nlights = (uint32_t)nl;
		for (int t = 0; t < 8; t++)
		{
			vu.tci[t][0] = s->tss[t][11];
			vu.tci[t][1] = s->tss[t][24];
		}

		/* Fragment uniforms. */
		FSU fu;
		memset(&fu, 0, sizeof fu);
		for (int t = 0; t < GFX_MAX_STAGES; t++)
		{
			const uint32_t* ts = s->tss[t];
			id<MTLTexture> tex = (__bridge id<MTLTexture>)s->textures[t];
			uint32_t textype = tex ? (tex.textureType == MTLTextureTypeCube ? 2 : 1) : 0;
			fu.stage[t][0] = (ts[1] & 255) | (ts[2] & 255) << 8 | (ts[3] & 255) << 16 | (ts[26] & 255) << 24;
			fu.stage[t][1] = (ts[4] & 255) | (ts[5] & 255) << 8 | (ts[6] & 255) << 16 | (ts[27] & 255) << 24;
			fu.stage[t][2] = (ts[28] & 255) | textype << 8;
			uint32_t ttf = ts[24];
			fu.stage[t][3] = (ttf & 256) && (ttf & 255) ? (ttf & 255) : 0;
			memcpy(fu.bumpmat[t], &ts[7], 16);
			[G.enc setFragmentTexture:textype == 1 ? tex : G.white2d atIndex:(NSUInteger)t];
			[G.enc setFragmentTexture:textype == 2 ? tex : G.whitecube atIndex:(NSUInteger)(8 + t)];
			[G.enc setFragmentSamplerState:sampler_for(ts) atIndex:(NSUInteger)t];
			if ((ts[1] & 255) == 1)
				break;
		}
		unpack_color(s->rs[60], fu.tfactor);
		unpack_color(s->rs[34], fu.fogcolor);
		fu.flags = (s->rs[15] ? 1 : 0) | (s->rs[28] ? 2 : 0) | (s->rs[29] ? 4 : 0);
		fu.alphafunc = s->rs[25];
		fu.alpharef = (float)(s->rs[24] & 255);

		[G.enc setRenderPipelineState:pipe];
		[G.enc setDepthStencilState:depth_state_for(s)];
		[G.enc setStencilReferenceValue:s->rs[57] & 255];
		MTLViewport vp = { s->viewport[0], s->viewport[1], vu.vp[2], vu.vp[3], s->viewport[4], s->viewport[5] };
		if (vp.originX + vp.width > G.width) vp.width = G.width - vp.originX;
		if (vp.originY + vp.height > G.height) vp.height = G.height - vp.originY;
		[G.enc setViewport:vp];
		[G.enc setFrontFacingWinding:MTLWindingClockwise];
		[G.enc setCullMode:s->rs[22] == 2 ? MTLCullModeFront : s->rs[22] == 3 ? MTLCullModeBack : MTLCullModeNone];
		[G.enc setTriangleFillMode:s->rs[8] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
		float zbias = (float)(s->rs[47] & 31);
		[G.enc setDepthBias:-zbias * 2.0f slopeScale:-zbias * 0.5f clamp:0];
		[G.enc setVertexBuffer:vbuf offset:0 atIndex:0];
		[G.enc setVertexBytes:&vu length:sizeof vu atIndex:1];
		[G.enc setFragmentBytes:&fu length:sizeof fu atIndex:0];

		static const MTLPrimitiveType prims[] = { MTLPrimitiveTypeTriangle, MTLPrimitiveTypePoint, MTLPrimitiveTypeLine,
			MTLPrimitiveTypeLineStrip, MTLPrimitiveTypeTriangle, MTLPrimitiveTypeTriangleStrip };
		MTLPrimitiveType pt = prim >= 1 && prim <= 5 ? prims[prim] : MTLPrimitiveTypeTriangle;
		if (indices)
			[G.enc drawIndexedPrimitives:pt indexCount:(NSUInteger)index_count
							   indexType:index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
							 indexBuffer:vbuf indexBufferOffset:index_offset];
		else
			[G.enc drawPrimitives:pt vertexStart:0 vertexCount:(NSUInteger)vertex_count];
		G.draws++;
	}
}
