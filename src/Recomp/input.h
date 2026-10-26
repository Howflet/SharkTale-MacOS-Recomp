/*=============================================================================
	input.h: Host input state shared by the platform layer and DirectInput.
	Keys use DirectInput scan codes (DIK_*).
=============================================================================*/
#pragma once

#include "recomp.h"

#define INPUT_QUEUE 256

typedef struct InputEvent { u32 ofs, data, time, seq; } InputEvent;
typedef struct InputQueue { InputEvent ev[INPUT_QUEUE]; int head, tail; } InputQueue;

typedef struct InputState
{
	u8 keys[256];
	u8 buttons[8];
	s32 mouse_dx, mouse_dy, mouse_dz;
	u32 sequence;
	InputQueue kbd_queue, mouse_queue;
} InputState;

extern InputState g_input;

void input_key(u8 dik, int down);
void input_mouse_move(s32 dx, s32 dy);
void input_mouse_wheel(s32 dz);
void input_mouse_button(int button, int down);

/* Platform layer entry points for synthetic input (gfx_metal.m): a key as if
   typed, a mouse button, and relative mouse motion in view points. */
void host_key(u8 dik, int down);
void host_mouse_button(int button, int down);
void host_mouse_motion(double dx, double dy);

/* Game controller (gamepad.m): poll once per event pump; inactive releases all. */
void gamepad_poll(int active);
