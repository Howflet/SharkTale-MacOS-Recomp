/*=============================================================================
	gamepad.m: Game controllers (GameController.framework) as keyboard and mouse.

	Shark Tale's menus are driven by the mouse and its PC controls by keys and
	mouse, so a controller drives those (the game's own key bindings, and the
	virtual cursor in menus) rather than a DirectInput joystick, whose default
	bindings expect an old button-to-swim gamepad. Any controller macOS
	supports works: Xbox, DualSense/DualShock, Switch Pro, MFi.

	  Left stick       W A S D       swim, strafe
	  Right stick      mouse         camera; the cursor in menus
	  A / Cross        left button   swim forward; click in menus
	  B / Circle       right button  Use | Fire (attack)
	  X / Square       Space         launch urchin; skip movies
	  Y / Triangle     F             use
	  RT / LT          Shift / Ctrl  swim forward / back
	  D-pad            arrow keys
	  Menu (Start)     Escape        main menu
	  Options (Back)   Backspace     skip cutscene

	Environment:
	  SHARK_PAD_SPEED=<n>   right stick speed at full tilt, points/s (default 1200)
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
	C_UP, C_DOWN, C_LEFT, C_RIGHT,           /* left stick */
	C_A, C_B, C_X, C_Y, C_RT, C_LT,
	C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT,
	C_MENU, C_OPTIONS,
	C_COUNT
};
static const Binding Bindings[C_COUNT] = {
	[C_UP] = { KEY, 17 }, [C_DOWN] = { KEY, 31 }, [C_LEFT] = { KEY, 30 }, [C_RIGHT] = { KEY, 32 },
	[C_A] = { MOUSE, 0 }, [C_B] = { MOUSE, 1 }, [C_X] = { KEY, 57 }, [C_Y] = { KEY, 33 },
	[C_RT] = { KEY, 42 }, [C_LT] = { KEY, 29 },
	[C_DUP] = { KEY, 0xC8 }, [C_DDOWN] = { KEY, 0xD0 }, [C_DLEFT] = { KEY, 0xCB }, [C_DRIGHT] = { KEY, 0xCD },
	[C_MENU] = { KEY, 1 }, [C_OPTIONS] = { KEY, 14 },
};
static int Held[C_COUNT];

static void set_control(int i, int down)
{
	if (Held[i] == down)
		return;
	Held[i] = down;
	if (Bindings[i].kind == KEY)
		host_key((u8)Bindings[i].code, down);
	else
		host_mouse_button(Bindings[i].code, down);
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
		watch_connections();
	}
	uint64_t now = mach_absolute_time();
	double dt = last ? (double)(now - last) * tb.numer / tb.denom / 1e9 : 0;
	last = now;
	if (dt > 0.1)
		dt = 0.1;  /* after a stall, don't fling the camera */

	GCExtendedGamepad* gp = active ? current_pad() : nil;
	if (!gp)
	{
		for (int i = 0; i < C_COUNT; i++)
			set_control(i, 0);
		return;
	}

	float lx = gp.leftThumbstick.xAxis.value, ly = gp.leftThumbstick.yAxis.value;
	set_control(C_UP, stick_key(Held[C_UP], ly));
	set_control(C_DOWN, stick_key(Held[C_DOWN], -ly));
	set_control(C_LEFT, stick_key(Held[C_LEFT], -lx));
	set_control(C_RIGHT, stick_key(Held[C_RIGHT], lx));
	set_control(C_A, gp.buttonA.pressed);
	set_control(C_B, gp.buttonB.pressed);
	set_control(C_X, gp.buttonX.pressed);
	set_control(C_Y, gp.buttonY.pressed);
	set_control(C_RT, gp.rightTrigger.pressed);
	set_control(C_LT, gp.leftTrigger.pressed);
	set_control(C_DUP, gp.dpad.up.pressed);
	set_control(C_DDOWN, gp.dpad.down.pressed);
	set_control(C_DLEFT, gp.dpad.left.pressed);
	set_control(C_DRIGHT, gp.dpad.right.pressed);
	set_control(C_MENU, gp.buttonMenu.pressed);
	set_control(C_OPTIONS, gp.buttonOptions.pressed);

	/* Right stick: dead zone, then a squared response for fine aim. Up on the
	   stick is up on screen (negative mouse y). */
	double rx = gp.rightThumbstick.xAxis.value, ry = -gp.rightThumbstick.yAxis.value;
	double mag = sqrt(rx * rx + ry * ry);
	const double dead = 0.15;
	if (mag > dead && dt > 0)
	{
		double m = (fmin(mag, 1.0) - dead) / (1 - dead);
		double scale = m * m * speed * dt / mag;
		host_mouse_motion(rx * scale, ry * scale);
	}
}
