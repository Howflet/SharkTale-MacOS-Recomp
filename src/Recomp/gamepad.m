/*=============================================================================
	gamepad.m: Game controllers (GameController.framework) as keyboard and mouse.

	Shark Tale's menus are driven by the mouse and its PC controls by keys and
	mouse, so a controller drives those (the game's own key bindings, and the
	virtual cursor in menus) rather than a DirectInput joystick, whose default
	bindings expect an old button-to-swim gamepad. Any controller macOS
	supports works: Xbox, DualSense/DualShock, Switch Pro, MFi.

	Two layouts, switched by whether a menu wants the cursor (rt_menu_active):

	  Gameplay
	    Left stick       W A S D       swim, strafe
	    Right stick      mouse         camera
	    A / Cross        left button   swim forward
	    B / Circle       right button  Use | Fire (attack)
	    X / Square       Space         launch urchin; skip movies
	    Y / Triangle     F             use
	    RT / LT          Shift / Ctrl  swim forward / back
	    D-pad            arrow keys
	    Menu (Start)     Escape        pause menu
	    Options (Back)   Backspace     skip cutscene

	  Menus
	    Left stick       mouse         cursor
	    A / Cross        Enter         select the highlighted item, or
	                     left button   click at the cursor, if the stick moved it last
	    B / Circle       Escape        back
	    Menu (Start)     Escape        back
	    D-pad            arrow keys

	Environment:
	  SHARK_PAD_SPEED=<n>   stick speed at full tilt, points/s (default 1200)
	  SHARK_PAD_TRACE=1     log every control change and the stick's motion
=============================================================================*/
#import <GameController/GameController.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "input.h"

enum { KEY, MOUSE };
typedef struct { int kind, code; } Binding;

/* What each control sends: a DIK scan code or a mouse button. */
enum
{
	G_UP, G_DOWN, G_LEFT, G_RIGHT,           /* gameplay: left stick */
	G_A, G_B, G_X, G_Y, G_RT, G_LT,
	G_DUP, G_DDOWN, G_DLEFT, G_DRIGHT,
	G_MENU, G_OPTIONS,
	M_ENTER, M_CLICK, M_BACK,                /* menus */
	M_DUP, M_DDOWN, M_DLEFT, M_DRIGHT,
	C_COUNT
};
static const Binding Bindings[C_COUNT] = {
	[G_UP] = { KEY, 17 }, [G_DOWN] = { KEY, 31 }, [G_LEFT] = { KEY, 30 }, [G_RIGHT] = { KEY, 32 },
	[G_A] = { MOUSE, 0 }, [G_B] = { MOUSE, 1 }, [G_X] = { KEY, 57 }, [G_Y] = { KEY, 33 },
	[G_RT] = { KEY, 42 }, [G_LT] = { KEY, 29 },
	[G_DUP] = { KEY, 0xC8 }, [G_DDOWN] = { KEY, 0xD0 }, [G_DLEFT] = { KEY, 0xCB }, [G_DRIGHT] = { KEY, 0xCD },
	[G_MENU] = { KEY, 1 }, [G_OPTIONS] = { KEY, 14 },
	[M_ENTER] = { KEY, 28 }, [M_CLICK] = { MOUSE, 0 }, [M_BACK] = { KEY, 1 },
	[M_DUP] = { KEY, 0xC8 }, [M_DDOWN] = { KEY, 0xD0 }, [M_DLEFT] = { KEY, 0xCB }, [M_DRIGHT] = { KEY, 0xCD },
};
static const char* const Names[C_COUNT] = {
	"stick up", "stick down", "stick left", "stick right", "A", "B", "X", "Y", "RT", "LT",
	"dpad up", "dpad down", "dpad left", "dpad right", "menu", "options",
	"menu enter", "menu click", "menu back", "menu dpad up", "menu dpad down", "menu dpad left", "menu dpad right",
};
static int Held[C_COUNT], Trace;

static void set_control(int i, int down)
{
	if (Held[i] == down)
		return;
	Held[i] = down;
	if (Trace)
		fprintf(stderr, "[pad] %s %s\n", Names[i], down ? "down" : "up");
	if (Bindings[i].kind == KEY)
		host_key((u8)Bindings[i].code, down);
	else
		host_mouse_button(Bindings[i].code, down);
}

/* Apply a whole frame's state, releases first, so a control shared by both
   layouts (Cross is the left button in each) is let go before it is pressed. */
static void set_controls(const int* want)
{
	for (int i = 0; i < C_COUNT; i++)
		if (!want[i])
			set_control(i, 0);
	for (int i = 0; i < C_COUNT; i++)
		if (want[i])
			set_control(i, 1);
}

/* Buttons held when the layout switches stay inert until released, so the
   press that opened a menu (Menu, Escape) does not also close it, and the
   Cross that started the game does not also swim. */
enum { B_A, B_B, B_X, B_Y, B_RT, B_LT, B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT, B_MENU, B_OPTIONS, B_COUNT };
static int Stale[B_COUNT];

static int fresh(int b, BOOL pressed)
{
	if (!pressed)
		Stale[b] = 0;
	return pressed && !Stale[b];
}

/* A stick as relative mouse motion: dead zone, then a squared response for
   fine control. Up on the stick is up on screen (negative mouse y). */
static int stick_mouse(GCControllerDirectionPad* stick, double speed, double dt)
{
	double x = stick.xAxis.value, y = -stick.yAxis.value;
	double mag = sqrt(x * x + y * y);
	const double dead = 0.25;  /* worn sticks rest at up to about 0.2 */
	if (mag <= dead || dt <= 0)
		return 0;
	double m = (fmin(mag, 1.0) - dead) / (1 - dead);
	double scale = m * m * speed * dt / mag;
	host_mouse_motion(x * scale, y * scale);
	if (Trace)
	{
		static double sum_x, sum_y, since;
		sum_x += x * scale;
		sum_y += y * scale;
		if ((since += dt) >= 0.5)
		{
			extern s32 g_cursor_x, g_cursor_y;
			fprintf(stderr, "[pad] stick %.2f %.2f: moved %.0f %.0f points, cursor %d %d\n", x, y, sum_x, sum_y,
				g_cursor_x, g_cursor_y);
			sum_x = sum_y = since = 0;
		}
	}
	return 1;
}

/* A stick direction as a key: pressed past 0.5, released below 0.35. */
static int stick_key(int held, float v) { return held ? v > 0.35f : v > 0.5f; }

static void watch_connections(void)
{
	NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
	[nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
		GCController* c = n.object;
		fprintf(stderr, "[pad] connected: %s%s\n", [c.vendorName ?: @"controller" UTF8String],
			c.extendedGamepad ? "" : " (not a full gamepad; ignored)");
	}];
	[nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
		fprintf(stderr, "[pad] disconnected: %s\n", [((GCController*)n.object).vendorName ?: @"controller" UTF8String]);
	}];
}

static GCExtendedGamepad* current_pad(void)
{
	GCController* c = GCController.current;
	if (c.extendedGamepad)
		return c.extendedGamepad;
	for (GCController* k in GCController.controllers)
		if (k.extendedGamepad)
			return k.extendedGamepad;
	return nil;
}

void gamepad_poll(int active)
{
	static int init;
	static double speed = 1200;
	static uint64_t last;
	static mach_timebase_info_data_t tb;
	if (!init)
	{
		init = 1;
		const char* s = getenv("SHARK_PAD_SPEED");
		if (s && atof(s) > 0)
			speed = atof(s);
		mach_timebase_info(&tb);
		Trace = getenv("SHARK_PAD_TRACE") != NULL;
		watch_connections();
	}
	uint64_t now = mach_absolute_time();
	double dt = last ? (double)(now - last) * tb.numer / tb.denom / 1e9 : 0;
	last = now;
	if (dt > 0.1)
		dt = 0.1;  /* after a stall, don't fling the camera */

	int want[C_COUNT] = { 0 };
	GCExtendedGamepad* gp = active ? current_pad() : nil;
	if (!gp)
	{
		set_controls(want);
		return;
	}

	if (Trace)
	{
		/* Every change of a raw element (stick directions in 0.25 steps). */
		static int described;
		static NSMutableDictionary<NSString*, NSNumber*>* seen;
		if (!described)
		{
			described = 1;
			seen = [NSMutableDictionary dictionary];
			fprintf(stderr, "[pad] profile %s, category %s\n", [NSStringFromClass([gp class]) UTF8String],
				[gp.controller.productCategory UTF8String]);
		}
		GCPhysicalInputProfile* prof = gp.controller.physicalInputProfile;
		for (NSString* name in prof.buttons)
		{
			float v = prof.buttons[name].value;
			float q = roundf(v * 4) / 4;
			if (q != seen[name].floatValue)
			{
				seen[name] = @(q);
				fprintf(stderr, "[pad] raw %s = %.2f\n", [name UTF8String], v);
			}
		}
	}

	extern int rt_menu_active(void);
	int menu = rt_menu_active();
	static int was_menu = -1;
	if (Trace && menu != was_menu)
		fprintf(stderr, "[pad] %s layout\n", menu ? "menu" : "gameplay");
	if (menu != was_menu)
	{
		GCControllerButtonInput* const buttons[B_COUNT] = { gp.buttonA, gp.buttonB, gp.buttonX, gp.buttonY,
			gp.rightTrigger, gp.leftTrigger, gp.dpad.up, gp.dpad.down, gp.dpad.left, gp.dpad.right, gp.buttonMenu,
			gp.buttonOptions };
		for (int b = 0; b < B_COUNT; b++)
			Stale[b] = buttons[b].pressed;
	}
	was_menu = menu;

	if (menu)
	{
		/* Cross acts on what was navigated to last: the highlighted item after
		   the d-pad (Enter), the item under the cursor after the stick (click).
		   The choice is kept while Cross is held, so its release matches. */
		static int pointing;
		want[M_DUP] = fresh(B_DUP, gp.dpad.up.pressed);
		want[M_DDOWN] = fresh(B_DDOWN, gp.dpad.down.pressed);
		want[M_DLEFT] = fresh(B_DLEFT, gp.dpad.left.pressed);
		want[M_DRIGHT] = fresh(B_DRIGHT, gp.dpad.right.pressed);
		if (want[M_DUP] || want[M_DDOWN] || want[M_DLEFT] || want[M_DRIGHT])
			pointing = 0;
		int cross = fresh(B_A, gp.buttonA.pressed);
		if (!Held[M_ENTER] && !Held[M_CLICK])
		{
			want[M_ENTER] = cross && !pointing;
			want[M_CLICK] = cross && pointing;
		}
		else
		{
			want[M_ENTER] = cross && Held[M_ENTER];
			want[M_CLICK] = cross && Held[M_CLICK];
		}
		want[M_BACK] = fresh(B_B, gp.buttonB.pressed) | fresh(B_MENU, gp.buttonMenu.pressed);
		set_controls(want);
		if (stick_mouse(gp.leftThumbstick, speed, dt))
			pointing = 1;
		return;
	}

	float lx = gp.leftThumbstick.xAxis.value, ly = gp.leftThumbstick.yAxis.value;
	want[G_UP] = stick_key(Held[G_UP], ly);
	want[G_DOWN] = stick_key(Held[G_DOWN], -ly);
	want[G_LEFT] = stick_key(Held[G_LEFT], -lx);
	want[G_RIGHT] = stick_key(Held[G_RIGHT], lx);
	want[G_A] = fresh(B_A, gp.buttonA.pressed);
	want[G_B] = fresh(B_B, gp.buttonB.pressed);
	want[G_X] = fresh(B_X, gp.buttonX.pressed);
	want[G_Y] = fresh(B_Y, gp.buttonY.pressed);
	want[G_RT] = fresh(B_RT, gp.rightTrigger.pressed);
	want[G_LT] = fresh(B_LT, gp.leftTrigger.pressed);
	want[G_DUP] = fresh(B_DUP, gp.dpad.up.pressed);
	want[G_DDOWN] = fresh(B_DDOWN, gp.dpad.down.pressed);
	want[G_DLEFT] = fresh(B_DLEFT, gp.dpad.left.pressed);
	want[G_DRIGHT] = fresh(B_DRIGHT, gp.dpad.right.pressed);
	want[G_MENU] = fresh(B_MENU, gp.buttonMenu.pressed);
	want[G_OPTIONS] = fresh(B_OPTIONS, gp.buttonOptions.pressed);
	set_controls(want);
	stick_mouse(gp.rightThumbstick, speed, dt);
}
