/*=============================================================================
	hle_user32.c: USER32/GDI32 for the recompiled game.

	A small window manager: classes (with guest WndProcs), windows (rect,
	styles, window longs, props), a posted-message queue, and synchronous
	SendMessage into guest window procedures. Painting/GDI is inert: the game
	renders through its render device, not GDI. Platform presentation
	(an AppKit window) attaches through the hooks at the bottom.
=============================================================================*/
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "hle.h"

enum { WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
	WM_SHOWWINDOW = 0x18, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_QUIT = 0x12 };

/*-----------------------------------------------------------------------------
	Classes and windows.
-----------------------------------------------------------------------------*/

typedef struct WndClass { char name[128]; u32 wndproc; u32 atom; u32 extra; } WndClass;
static WndClass Classes[256];
static int NumClasses;
static u32 NextAtom = 0xc100;

typedef struct Window
{
	int used;
	u32 hwnd, parent, wndproc, style, exstyle, id, userdata, menu;
	int destroying;
	s32 x, y, w, h;
	int visible;
	u32 longs[64];  /* GWL_* negative indices and extra bytes */
	char cls[128];
	struct { char name[64]; u32 value; } props[16];
} Window;

#define MAX_WINDOWS 512
static Window Windows[MAX_WINDOWS];
#define HWND_BASE 0x00010000u
static u32 MainWindow;

static Window* win(u32 h)
{
	u32 i = (h - HWND_BASE) / 4;
	return (h >= HWND_BASE && i < MAX_WINDOWS && Windows[i].used) ? &Windows[i] : NULL;
}

static WndClass* find_class(const char* name, u32 atom_or_ptr)
{
	for (int i = 0; i < NumClasses; i++)
		if ((name && !strcasecmp(Classes[i].name, name)) || (!name && Classes[i].atom == atom_or_ptr))
			return &Classes[i];
	return NULL;
}

static u32 register_class(Ctx* c, u32 wc, int wide)
{
	/* WNDCLASSEX: cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon,
	   hCursor, hbrBackground, lpszMenuName, lpszClassName, hIconSm */
	WndClass* k = &Classes[NumClasses++];
	k->wndproc = LD4(wc + 8);
	k->extra = LD4(wc + 16);
	u32 name = LD4(wc + 40);
	if (wide)
		g_wstr_utf8(name, k->name, sizeof k->name);
	else
		snprintf(k->name, sizeof k->name, "%s", g_str(name));
	k->atom = NextAtom++;
	return k->atom;
}

HLE(RegisterClassExW) { HLE_RET(c, register_class(c, ARG(0), 1), 4); }
HLE(RegisterClassExA) { HLE_RET(c, register_class(c, ARG(0), 0), 4); }

static int class_info(u32 name, int wide, u32 out)
{
	char n[128];
	if (name < 0x10000)
		return 0;
	if (wide)
		g_wstr_utf8(name, n, sizeof n);
	else
		snprintf(n, sizeof n, "%s", g_str(name));
	WndClass* k = find_class(n, 0);
	memset(GP(out + 4), 0, 44);
	if (!k)
	{
		/* System control classes (EDIT, BUTTON, RICHEDIT...): a default window procedure. */
		ST4(out + 8, rt_hle_address("USER32.dll", "DefWindowProcW"));
		return 0xc001;
	}
	ST4(out + 8, k->wndproc);
	return (int)k->atom;
}
HLE(GetClassInfoExW) { HLE_RET(c, class_info(ARG(1), 1, ARG(2)), 12); }
HLE(GetClassInfoExA) { HLE_RET(c, class_info(ARG(1), 0, ARG(2)), 12); }

/* Call a guest window procedure (stdcall LRESULT (HWND, UINT, WPARAM, LPARAM)). */
static u32 call_wndproc(Ctx* c, u32 proc, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
	if (!proc)
		return 0;
	u32 args[4] = { hwnd, msg, wp, lp };
	return rt_call_guest(c, proc, 4, args, 1);
}

static u32 send(Ctx* c, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
	Window* w = win(hwnd);
	return w ? call_wndproc(c, w->wndproc, hwnd, msg, wp, lp) : 0;
}

static u32 create_window(Ctx* c, int wide)
{
	/* (exstyle, class, title, style, x, y, w, h, parent, menu, instance, param) */
	u32 cls = ARG(1);
	char cname[128];
	WndClass* k;
	if (cls < 0x10000)
		k = find_class(NULL, cls);
	else
	{
		if (wide)
			g_wstr_utf8(cls, cname, sizeof cname);
		else
			snprintf(cname, sizeof cname, "%s", g_str(cls));
		k = find_class(cname, 0);
	}
	int slot = -1;
	for (int i = 0; i < MAX_WINDOWS; i++)
		if (!Windows[i].used)
		{
			slot = i;
			break;
		}
	if (slot < 0)
		return 0;
	Window* w = &Windows[slot];
	memset(w, 0, sizeof *w);
	w->used = 1;
	w->hwnd = HWND_BASE + 4 * (u32)slot;
	w->exstyle = ARG(0);
	w->style = ARG(3);
	w->x = (s32)ARG(4) == (s32)0x80000000 ? 0 : (s32)ARG(4);
	w->y = (s32)ARG(5) == (s32)0x80000000 ? 0 : (s32)ARG(5);
	w->w = (s32)ARG(6) == (s32)0x80000000 ? 1024 : (s32)ARG(6);
	w->h = (s32)ARG(7) == (s32)0x80000000 ? 768 : (s32)ARG(7);
	w->parent = ARG(8);
	w->id = ARG(9);
	w->wndproc = k ? k->wndproc : 0;
	snprintf(w->cls, sizeof w->cls, "%s", k ? k->name : "?");
	if (!w->parent && !MainWindow)
		MainWindow = w->hwnd;

	/* CREATESTRUCT on the guest stack area: param, inst, menu, parent, cy, cx, y, x, style, name, class, exstyle */
	u32 cs = rt_heap_alloc(48);
	u32 vals[12] = { ARG(11), ARG(10), ARG(9), ARG(8), (u32)w->h, (u32)w->w, (u32)w->y, (u32)w->x,
		ARG(3), ARG(2), ARG(1), ARG(0) };
	for (int i = 0; i < 12; i++)
		ST4(cs + 4 * i, vals[i]);
	u32 hwnd = w->hwnd;
	if (!call_wndproc(c, w->wndproc, hwnd, WM_NCCREATE, 0, cs) && w->wndproc)
	{
		/* NCCREATE refused: destroy. */
	}
	call_wndproc(c, w->wndproc, hwnd, WM_CREATE, 0, cs);
	rt_heap_free(cs);
	if (ARG(3) & 0x10000000u)  /* WS_VISIBLE */
		win(hwnd)->visible = 1;
	return hwnd;
}

/* CreateWindowEx re-reads ARG() after guest calls, so preserve ESP first. */
HLE(CreateWindowExW)
{
	u32 esp = R4(REG_ESP);
	u32 h = create_window(c, 1);
	R4(REG_ESP) = esp;
	HLE_RET(c, h, 48);
}
HLE(CreateWindowExA)
{
	u32 esp = R4(REG_ESP);
	u32 h = create_window(c, 0);
	R4(REG_ESP) = esp;
	HLE_RET(c, h, 48);
}

static void destroy_window(Ctx* c, u32 h)
{
	Window* w = win(h);
	if (!w || w->destroying)
		return;
	w->destroying = 1;
	send(c, h, WM_DESTROY, 0, 0);
	send(c, h, WM_NCDESTROY, 0, 0);
	w->used = 0;
	if (h == MainWindow)
		MainWindow = 0;
}

HLE(DestroyWindow)
{
	u32 esp = R4(REG_ESP), h = ARG(0);
	destroy_window(c, h);
	R4(REG_ESP) = esp;
	HLE_RET(c, 1, 4);
}

/* Default handling: WM_CLOSE destroys the window, as on Windows (that is how
   the engine learns the player closed the game window). */
static void def_window_proc(Ctx* c)
{
	u32 esp = R4(REG_ESP), h = ARG(0), msg = ARG(1);
	u32 r = msg == WM_NCCREATE ? 1 : 0;
	if (msg == 0x0010)  /* WM_CLOSE */
		destroy_window(c, h);
	R4(REG_ESP) = esp;
	HLE_RET(c, r, 16);
}
HLE(DefWindowProcW) { def_window_proc(c); }
HLE(DefWindowProcA) { def_window_proc(c); }
HLE(DefMDIChildProcW) { HLE_RET(c, 0, 16); }
HLE(DefMDIChildProcA) { HLE_RET(c, 0, 16); }
HLE(CallWindowProcW)
{
	u32 esp = R4(REG_ESP);
	u32 r = call_wndproc(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
	R4(REG_ESP) = esp;
	HLE_RET(c, r, 20);
}
HLE(CallWindowProcA)
{
	u32 esp = R4(REG_ESP);
	u32 r = call_wndproc(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
	R4(REG_ESP) = esp;
	HLE_RET(c, r, 20);
}
HLE(SendMessageW)
{
	u32 esp = R4(REG_ESP);
	u32 r = send(c, ARG(0), ARG(1), ARG(2), ARG(3));
	R4(REG_ESP) = esp;
	HLE_RET(c, r, 16);
}
HLE(SendMessageA)
{
	u32 esp = R4(REG_ESP);
	u32 r = send(c, ARG(0), ARG(1), ARG(2), ARG(3));
	R4(REG_ESP) = esp;
	HLE_RET(c, r, 16);
}

/*-----------------------------------------------------------------------------
	Message queue (posted messages and platform input).
-----------------------------------------------------------------------------*/

static u32 Foreground, Active, Focus;

typedef struct Msg { u32 hwnd, msg, wp, lp, time, x, y; } Msg;
static Msg Queue[1024];
static int QHead, QTail;
static int QuitPosted;

void hle_post_message(u32 hwnd, u32 msg, u32 wp, u32 lp)
{
	int next = (QTail + 1) % 1024;
	if (next == QHead)
		return;
	extern u32 hle_tick_count(void);
	Queue[QTail] = (Msg){ hwnd ? hwnd : Focus && win(Focus) ? Focus : MainWindow, msg, wp, lp, hle_tick_count(), 0, 0 };
	QTail = next;
}

HLE(PostMessageW) { hle_post_message(ARG(0), ARG(1), ARG(2), ARG(3)); HLE_RET(c, 1, 16); }
HLE(PostMessageA) { hle_post_message(ARG(0), ARG(1), ARG(2), ARG(3)); HLE_RET(c, 1, 16); }
HLE(PostQuitMessage) { QuitPosted = 1; hle_post_message(0, WM_QUIT, ARG(0), 0); HLE_RET(c, 0, 4); }

void hle_platform_pump(void);  /* platform layer: turns host events into posted messages */

static int peek(Ctx* c, u32 out, int remove)
{
	hle_platform_pump();
	if (QHead == QTail)
		return 0;
	Msg* m = &Queue[QHead];
	ST4(out + 0, m->hwnd);
	ST4(out + 4, m->msg);
	ST4(out + 8, m->wp);
	ST4(out + 12, m->lp);
	ST4(out + 16, m->time);
	ST4(out + 20, m->x);
	ST4(out + 24, m->y);
	if (remove)
		QHead = (QHead + 1) % 1024;
	return 1;
}
HLE(PeekMessageW) { HLE_RET(c, peek(c, ARG(0), ARG(4) & 1), 20); }
HLE(PeekMessageA) { HLE_RET(c, peek(c, ARG(0), ARG(4) & 1), 20); }
HLE(GetMessageW)
{
	/* Wait briefly for input; then hand back WM_NULL rather than blocking forever. */
	for (int i = 0; i < 16 && !peek(c, ARG(0), 1); i++)
		usleep(1000);
	if (QHead == QTail && LD4(ARG(0) + 4) != WM_QUIT)
	{
		u32 m = ARG(0);
		memset(GP(m), 0, 28);
		ST4(m, ARG(1) ? ARG(1) : MainWindow);
	}
	HLE_RET(c, LD4(ARG(0) + 4) != WM_QUIT, 16);
}
HLE(TranslateMessage) { HLE_RET(c, 0, 4); }
static u32 dispatch(Ctx* c, u32 m)
{
	return send(c, LD4(m), LD4(m + 4), LD4(m + 8), LD4(m + 12));
}
HLE(DispatchMessageW) { u32 esp = R4(REG_ESP); u32 r = dispatch(c, ARG(0)); R4(REG_ESP) = esp; HLE_RET(c, r, 4); }
HLE(DispatchMessageA) { u32 esp = R4(REG_ESP); u32 r = dispatch(c, ARG(0)); R4(REG_ESP) = esp; HLE_RET(c, r, 4); }
HLE(GetMessageTime) { extern u32 hle_tick_count(void); HLE_RET(c, hle_tick_count(), 0); }

/*-----------------------------------------------------------------------------
	Window state.
-----------------------------------------------------------------------------*/

static void put_rect(u32 r, s32 l, s32 t, s32 rr, s32 b)
{
	ST4(r, (u32)l);
	ST4(r + 4, (u32)t);
	ST4(r + 8, (u32)rr);
	ST4(r + 12, (u32)b);
}

HLE(GetClientRect)
{
	Window* w = win(ARG(0));
	put_rect(ARG(1), 0, 0, w ? w->w : 1024, w ? w->h : 768);
	HLE_RET(c, w != NULL, 8);
}
HLE(GetWindowRect)
{
	Window* w = win(ARG(0));
	if (w)
		put_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h);
	else
		put_rect(ARG(1), 0, 0, 1024, 768);
	HLE_RET(c, 1, 8);
}
HLE(AdjustWindowRect) { HLE_RET(c, 1, 12); }
HLE(MoveWindow)
{
	Window* w = win(ARG(0));
	if (w)
	{
		w->x = (s32)ARG(1);
		w->y = (s32)ARG(2);
		w->w = (s32)ARG(3);
		w->h = (s32)ARG(4);
	}
	HLE_RET(c, 1, 24);
}
HLE(SetWindowPos)
{
	/* (hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE 1, SWP_NOMOVE 2 */
	Window* w = win(ARG(0));
	if (w)
	{
		if (!(ARG(6) & 2)) { w->x = (s32)ARG(2); w->y = (s32)ARG(3); }
		if (!(ARG(6) & 1)) { w->w = (s32)ARG(4); w->h = (s32)ARG(5); }
	}
	HLE_RET(c, 1, 28);
}
HLE(ShowWindow)
{
	Window* w = win(ARG(0));
	int was = w ? w->visible : 0;
	if (w)
		w->visible = ARG(1) != 0;
	HLE_RET(c, was, 8);
}
HLE(UpdateWindow) { HLE_RET(c, 1, 4); }
HLE(IsWindow) { HLE_RET(c, win(ARG(0)) != NULL, 4); }
HLE(IsWindowEnabled) { HLE_RET(c, 1, 4); }
HLE(IsIconic) { HLE_RET(c, 0, 4); }
HLE(IsZoomed) { HLE_RET(c, 0, 4); }
HLE(EnableWindow) { HLE_RET(c, 0, 8); }
HLE(GetParent) { Window* w = win(ARG(0)); HLE_RET(c, w ? w->parent : 0, 4); }
HLE(SetParent) { Window* w = win(ARG(0)); u32 old = w ? w->parent : 0; if (w) w->parent = ARG(1); HLE_RET(c, old, 8); }
HLE(GetWindow) { HLE_RET(c, 0, 8); }
HLE(GetDlgItem) { HLE_RET(c, 0, 8); }
HLE(GetDlgCtrlID) { Window* w = win(ARG(0)); HLE_RET(c, w ? w->id : 0, 4); }
HLE(GetDesktopWindow) { HLE_RET(c, 0x0000fff0u, 0); }
static u32 or_main(u32 h) { return h ? h : MainWindow; }
HLE(GetActiveWindow) { HLE_RET(c, or_main(Active), 0); }
HLE(GetForegroundWindow) { HLE_RET(c, or_main(Foreground), 0); }
HLE(GetFocus) { HLE_RET(c, or_main(Focus), 0); }
HLE(SetFocus) { u32 o = or_main(Focus); Focus = ARG(0); HLE_RET(c, o, 4); }
HLE(SetActiveWindow) { u32 o = or_main(Active); Active = ARG(0); HLE_RET(c, o, 4); }
HLE(SetForegroundWindow) { Foreground = Active = Focus = ARG(0); HLE_RET(c, 1, 4); }
HLE(BringWindowToTop) { HLE_RET(c, 1, 4); }
static u32 Capture;
HLE(GetCapture) { HLE_RET(c, Capture, 0); }
HLE(SetCapture) { u32 o = Capture; Capture = ARG(0); HLE_RET(c, o, 4); }
HLE(ReleaseCapture) { Capture = 0; HLE_RET(c, 1, 0); }
HLE(WindowFromPoint) { HLE_RET(c, MainWindow, 8); }
HLE(GetWindowThreadProcessId) { if (ARG(1)) ST4(ARG(1), 0x1234); HLE_RET(c, 0x100, 8); }
HLE(GetWindowTextA) { if (ARG(2)) ST1(ARG(1), 0); HLE_RET(c, 0, 12); }

static u32* window_long(Window* w, s32 index)
{
	switch (index)
	{
	case -4: return &w->wndproc;    /* GWL_WNDPROC */
	case -16: return &w->style;     /* GWL_STYLE */
	case -20: return &w->exstyle;   /* GWL_EXSTYLE */
	case -12: return &w->id;        /* GWL_ID */
	case -21: return &w->userdata;  /* GWL_USERDATA */
	case -8: return &w->parent;     /* GWL_HWNDPARENT */
	default:
		if (index >= 0 && index / 4 < 64)
			return &w->longs[index / 4];
		return NULL;
	}
}
HLE(GetWindowLongW) { Window* w = win(ARG(0)); u32* p = w ? window_long(w, (s32)ARG(1)) : NULL; HLE_RET(c, p ? *p : 0, 8); }
HLE(GetWindowLongA) { Window* w = win(ARG(0)); u32* p = w ? window_long(w, (s32)ARG(1)) : NULL; HLE_RET(c, p ? *p : 0, 8); }
HLE(SetWindowLongW)
{
	Window* w = win(ARG(0));
	u32* p = w ? window_long(w, (s32)ARG(1)) : NULL;
	u32 old = p ? *p : 0;
	if (p)
		*p = ARG(2);
	HLE_RET(c, old, 12);
}
HLE(SetWindowLongA)
{
	Window* w = win(ARG(0));
	u32* p = w ? window_long(w, (s32)ARG(1)) : NULL;
	u32 old = p ? *p : 0;
	if (p)
		*p = ARG(2);
	HLE_RET(c, old, 12);
}

static u32* prop(Window* w, const char* name, int create)
{
	for (int i = 0; i < 16; i++)
		if (w->props[i].name[0] && !strcmp(w->props[i].name, name))
			return &w->props[i].value;
	if (create)
		for (int i = 0; i < 16; i++)
			if (!w->props[i].name[0])
			{
				snprintf(w->props[i].name, sizeof w->props[i].name, "%s", name);
				return &w->props[i].value;
			}
	return NULL;
}
static void prop_name(u32 p, int wide, char* out)
{
	if (p < 0x10000)
		snprintf(out, 64, "#%u", p);
	else if (wide)
		g_wstr_utf8(p, out, 64);
	else
		snprintf(out, 64, "%s", g_str(p));
}
HLE(SetPropW) { Window* w = win(ARG(0)); char n[64]; prop_name(ARG(1), 1, n); u32* v = w ? prop(w, n, 1) : NULL; if (v) *v = ARG(2); HLE_RET(c, v != NULL, 12); }
HLE(SetPropA) { Window* w = win(ARG(0)); char n[64]; prop_name(ARG(1), 0, n); u32* v = w ? prop(w, n, 1) : NULL; if (v) *v = ARG(2); HLE_RET(c, v != NULL, 12); }
HLE(RemovePropW) { Window* w = win(ARG(0)); char n[64]; prop_name(ARG(1), 1, n); u32* v = w ? prop(w, n, 0) : NULL; u32 o = v ? *v : 0; if (v) *v = 0; HLE_RET(c, o, 8); }
HLE(RemovePropA) { Window* w = win(ARG(0)); char n[64]; prop_name(ARG(1), 0, n); u32* v = w ? prop(w, n, 0) : NULL; u32 o = v ? *v : 0; if (v) *v = 0; HLE_RET(c, o, 8); }

/* Coordinates (no window offsets in the single-window model). */
HLE(ClientToScreen) { HLE_RET(c, 1, 8); }
HLE(ScreenToClient) { HLE_RET(c, 1, 8); }
HLE(MapWindowPoints) { HLE_RET(c, 0, 16); }

/*-----------------------------------------------------------------------------
	Input state, cursor, metrics.
-----------------------------------------------------------------------------*/

s32 g_cursor_x = 400, g_cursor_y = 300;
u8 g_key_state[256];
HLE(GetCursorPos) { ST4(ARG(0), (u32)g_cursor_x); ST4(ARG(0) + 4, (u32)g_cursor_y); HLE_RET(c, 1, 4); }
HLE(SetCursorPos) { g_cursor_x = (s32)ARG(0); g_cursor_y = (s32)ARG(1); HLE_RET(c, 1, 8); }
HLE(GetKeyState) { HLE_RET(c, g_key_state[ARG(0) & 0xff] ? 0x8000 : 0, 4); }
HLE(SetKeyboardState) { HLE_RET(c, 1, 4); }
HLE(MapVirtualKeyW) { HLE_RET(c, ARG(0), 8); }
HLE(GetKeyNameTextW) { if (ARG(2)) ST2(ARG(1), 0); HLE_RET(c, 0, 12); }
static s32 CursorCount;
HLE(ShowCursor) { CursorCount += ARG(0) ? 1 : -1; HLE_RET(c, (u32)CursorCount, 4); }
HLE(SetCursor) { HLE_RET(c, 1, 4); }
HLE(GetCursor) { HLE_RET(c, 1, 0); }
HLE(ClipCursor) { HLE_RET(c, 1, 4); }
HLE(LoadCursorW) { HLE_RET(c, 1, 8); }
HLE(LoadCursorA) { HLE_RET(c, 1, 8); }
HLE(LoadIconW) { HLE_RET(c, 1, 8); }
HLE(LoadIconA) { HLE_RET(c, 1, 8); }
HLE(LoadImageA) { HLE_RET(c, 1, 24); }

s32 g_screen_w = 1280, g_screen_h = 800;
HLE(GetSystemMetrics)
{
	switch (ARG(0))
	{
	case 0: HLE_RET(c, (u32)g_screen_w, 4);  /* SM_CXSCREEN */
	case 1: HLE_RET(c, (u32)g_screen_h, 4);  /* SM_CYSCREEN */
	case 5: case 6: HLE_RET(c, 1, 4);         /* borders */
	case 4: HLE_RET(c, 20, 4);                /* caption */
	case 32: case 33: HLE_RET(c, 4, 4);       /* frame */
	default: HLE_RET(c, 0, 4);
	}
}
HLE(SystemParametersInfoW) { HLE_RET(c, 1, 16); }
HLE(SystemParametersInfoA) { HLE_RET(c, 1, 16); }
HLE(GetSysColor) { HLE_RET(c, 0xc0c0c0, 4); }
HLE(GetSysColorBrush) { HLE_RET(c, 1, 4); }
HLE(MessageBeep) { HLE_RET(c, 1, 4); }
HLE(MessageBoxW)
{
	char t[2048], cap[256];
	fprintf(stderr, "[MessageBox] %s: %s\n", g_wstr_utf8(ARG(2), cap, sizeof cap), g_wstr_utf8(ARG(1), t, sizeof t));
	HLE_RET(c, 1, 16);
}
HLE(MessageBoxA)
{
	fprintf(stderr, "[MessageBox] %s: %s\n", g_str(ARG(2)), g_str(ARG(1)));
	HLE_RET(c, 1, 16);
}

static u32 NextMsgId = 0xc000;
HLE(RegisterWindowMessageW) { HLE_RET(c, NextMsgId++, 4); }
HLE(RegisterWindowMessageA) { HLE_RET(c, NextMsgId++, 4); }
HLE(RegisterHotKey) { HLE_RET(c, 1, 16); }
HLE(UnregisterHotKey) { HLE_RET(c, 1, 8); }
HLE(AttachThreadInput) { HLE_RET(c, 1, 12); }

/* Painting, menus, dialogs, clipboard: inert. */
HLE(BeginPaint) { memset(GP(ARG(1)), 0, 64); HLE_RET(c, 0x5000, 8); }
HLE(EndPaint) { HLE_RET(c, 1, 8); }
HLE(InvalidateRect) { HLE_RET(c, 1, 12); }
HLE(ValidateRect) { HLE_RET(c, 1, 8); }
HLE(GetUpdateRect) { HLE_RET(c, 0, 12); }
HLE(GetDC) { HLE_RET(c, 0x5000, 4); }
HLE(ReleaseDC) { HLE_RET(c, 1, 8); }
HLE(FillRect) { HLE_RET(c, 1, 12); }
HLE(DrawEdge) { HLE_RET(c, 1, 16); }
HLE(DrawFocusRect) { HLE_RET(c, 1, 8); }
HLE(DrawTextW) { HLE_RET(c, 0, 20); }
HLE(DrawTextA) { HLE_RET(c, 0, 20); }
HLE(DrawTextExW) { HLE_RET(c, 0, 24); }
HLE(DrawTextExA) { HLE_RET(c, 0, 24); }
HLE(BeginDeferWindowPos) { HLE_RET(c, 1, 4); }
HLE(EndDeferWindowPos) { HLE_RET(c, 1, 4); }
HLE(EnumChildWindows) { HLE_RET(c, 1, 12); }
static u32 NextMenu = 0x7000;
HLE(CreatePopupMenu) { HLE_RET(c, NextMenu++, 0); }
HLE(DestroyMenu) { HLE_RET(c, 1, 4); }
HLE(GetMenu) { HLE_RET(c, 0, 4); }
HLE(SetMenu) { HLE_RET(c, 1, 8); }
HLE(LoadMenuW) { HLE_RET(c, NextMenu++, 8); }
HLE(LoadMenuA) { HLE_RET(c, NextMenu++, 8); }
HLE(GetSubMenu) { HLE_RET(c, 0, 8); }
HLE(GetMenuItemCount) { HLE_RET(c, 0, 4); }
HLE(GetMenuItemInfoW) { HLE_RET(c, 0, 16); }
HLE(GetMenuItemInfoA) { HLE_RET(c, 0, 16); }
HLE(SetMenuItemInfoW) { HLE_RET(c, 1, 16); }
HLE(SetMenuItemInfoA) { HLE_RET(c, 1, 16); }
HLE(InsertMenuItemA) { HLE_RET(c, 1, 16); }
HLE(GetMenuState) { HLE_RET(c, 0, 12); }
HLE(TrackPopupMenu) { HLE_RET(c, 0, 28); }
HLE(CreateDialogParamW) { HLE_RET(c, 0, 20); }
HLE(CreateDialogParamA) { HLE_RET(c, 0, 20); }
HLE(CreateDialogIndirectParamA) { HLE_RET(c, 0, 20); }
HLE(DialogBoxParamW) { HLE_RET(c, 0xffffffffu, 20); }
HLE(DialogBoxParamA) { HLE_RET(c, 0xffffffffu, 20); }
HLE(EndDialog) { HLE_RET(c, 1, 8); }
HLE(OpenClipboard) { HLE_RET(c, 1, 4); }
HLE(CloseClipboard) { HLE_RET(c, 1, 0); }
HLE(EmptyClipboard) { HLE_RET(c, 1, 0); }
HLE(GetClipboardData) { HLE_RET(c, 0, 4); }
HLE(SetClipboardData) { HLE_RET(c, ARG(1), 8); }

const HleEntry hle_user32[] = {
	ENTRY(RegisterClassExW), ENTRY(RegisterClassExA), ENTRY(GetClassInfoExW), ENTRY(GetClassInfoExA),
	ENTRY(CreateWindowExW), ENTRY(CreateWindowExA), ENTRY(DestroyWindow), ENTRY(DefWindowProcW),
	ENTRY(DefWindowProcA), ENTRY(DefMDIChildProcW), ENTRY(DefMDIChildProcA), ENTRY(CallWindowProcW),
	ENTRY(CallWindowProcA), ENTRY(SendMessageW), ENTRY(SendMessageA), ENTRY(PostMessageW), ENTRY(PostMessageA),
	ENTRY(PostQuitMessage), ENTRY(PeekMessageW), ENTRY(PeekMessageA), ENTRY(GetMessageW), ENTRY(TranslateMessage),
	ENTRY(DispatchMessageW), ENTRY(DispatchMessageA), ENTRY(GetMessageTime), ENTRY(GetClientRect),
	ENTRY(GetWindowRect), ENTRY(AdjustWindowRect), ENTRY(MoveWindow), ENTRY(SetWindowPos), ENTRY(ShowWindow),
	ENTRY(UpdateWindow), ENTRY(IsWindow), ENTRY(IsWindowEnabled), ENTRY(IsIconic), ENTRY(IsZoomed),
	ENTRY(EnableWindow), ENTRY(GetParent), ENTRY(SetParent), ENTRY(GetWindow), ENTRY(GetDlgItem),
	ENTRY(GetDlgCtrlID), ENTRY(GetDesktopWindow), ENTRY(GetActiveWindow), ENTRY(GetForegroundWindow),
	ENTRY(GetFocus), ENTRY(SetFocus), ENTRY(SetActiveWindow), ENTRY(SetForegroundWindow), ENTRY(BringWindowToTop),
	ENTRY(GetCapture), ENTRY(SetCapture), ENTRY(ReleaseCapture), ENTRY(WindowFromPoint),
	ENTRY(GetWindowThreadProcessId), ENTRY(GetWindowTextA), ENTRY(GetWindowLongW), ENTRY(GetWindowLongA),
	ENTRY(SetWindowLongW), ENTRY(SetWindowLongA), ENTRY(SetPropW), ENTRY(SetPropA), ENTRY(RemovePropW),
	ENTRY(RemovePropA), ENTRY(ClientToScreen), ENTRY(ScreenToClient), ENTRY(MapWindowPoints), ENTRY(GetCursorPos),
	ENTRY(SetCursorPos), ENTRY(GetKeyState), ENTRY(SetKeyboardState), ENTRY(MapVirtualKeyW), ENTRY(GetKeyNameTextW),
	ENTRY(ShowCursor), ENTRY(SetCursor), ENTRY(GetCursor), ENTRY(ClipCursor), ENTRY(LoadCursorW), ENTRY(LoadCursorA),
	ENTRY(LoadIconW), ENTRY(LoadIconA), ENTRY(LoadImageA), ENTRY(GetSystemMetrics), ENTRY(SystemParametersInfoW),
	ENTRY(SystemParametersInfoA), ENTRY(GetSysColor), ENTRY(GetSysColorBrush), ENTRY(MessageBeep),
	ENTRY(MessageBoxW), ENTRY(MessageBoxA), ENTRY(RegisterWindowMessageW), ENTRY(RegisterWindowMessageA),
	ENTRY(RegisterHotKey), ENTRY(UnregisterHotKey), ENTRY(AttachThreadInput), ENTRY(BeginPaint), ENTRY(EndPaint),
	ENTRY(InvalidateRect), ENTRY(ValidateRect), ENTRY(GetUpdateRect), ENTRY(GetDC), ENTRY(ReleaseDC),
	ENTRY(FillRect), ENTRY(DrawEdge), ENTRY(DrawFocusRect), ENTRY(DrawTextW), ENTRY(DrawTextA), ENTRY(DrawTextExW),
	ENTRY(DrawTextExA), ENTRY(BeginDeferWindowPos), ENTRY(EndDeferWindowPos), ENTRY(EnumChildWindows),
	ENTRY(CreatePopupMenu), ENTRY(DestroyMenu), ENTRY(GetMenu), ENTRY(SetMenu), ENTRY(LoadMenuW), ENTRY(LoadMenuA),
	ENTRY(GetSubMenu), ENTRY(GetMenuItemCount), ENTRY(GetMenuItemInfoW), ENTRY(GetMenuItemInfoA),
	ENTRY(SetMenuItemInfoW), ENTRY(SetMenuItemInfoA), ENTRY(InsertMenuItemA), ENTRY(GetMenuState),
	ENTRY(TrackPopupMenu), ENTRY(CreateDialogParamW), ENTRY(CreateDialogParamA), ENTRY(CreateDialogIndirectParamA),
	ENTRY(DialogBoxParamW), ENTRY(DialogBoxParamA), ENTRY(EndDialog), ENTRY(OpenClipboard), ENTRY(CloseClipboard),
	ENTRY(EmptyClipboard), ENTRY(GetClipboardData), ENTRY(SetClipboardData),
	{ NULL, NULL }
};

/*-----------------------------------------------------------------------------
	GDI32: inert device contexts and objects.
-----------------------------------------------------------------------------*/

static u32 NextGdi = 0x6000;
HLE(BitBlt) { HLE_RET(c, 1, 36); }
HLE(StretchBlt) { HLE_RET(c, 1, 44); }
HLE(PatBlt) { HLE_RET(c, 1, 24); }
HLE(CreateBitmap) { HLE_RET(c, NextGdi++, 20); }
HLE(CreateCompatibleBitmap) { HLE_RET(c, NextGdi++, 12); }
HLE(CreateCompatibleDC) { HLE_RET(c, NextGdi++, 4); }
HLE(CreateDIBSection)
{
	/* (hdc, BITMAPINFO*, usage, void** bits, section, offset): give real pixel memory. */
	u32 bi = ARG(1);
	s32 w = (s32)LD4(bi + 4), h = (s32)LD4(bi + 8);
	u32 bpp = LD2(bi + 14);
	u32 size = (u32)(abs(w) * abs(h)) * (bpp ? bpp : 32) / 8 + 16;
	if (ARG(3))
		ST4(ARG(3), rt_heap_alloc(size));
	HLE_RET(c, NextGdi++, 24);
}
HLE(CreateDIBitmap) { HLE_RET(c, NextGdi++, 24); }
HLE(CreateFontA) { HLE_RET(c, NextGdi++, 56); }
HLE(CreateFontW) { HLE_RET(c, NextGdi++, 56); }
HLE(CreateFontIndirectA) { HLE_RET(c, NextGdi++, 4); }
HLE(CreatePen) { HLE_RET(c, NextGdi++, 12); }
HLE(CreateSolidBrush) { HLE_RET(c, NextGdi++, 4); }
HLE(DeleteDC) { HLE_RET(c, 1, 4); }
HLE(DeleteObject) { HLE_RET(c, 1, 4); }
HLE(SelectObject) { HLE_RET(c, 1, 8); }
HLE(GetStockObject) { HLE_RET(c, 0x5800 + ARG(0), 4); }
HLE(GetObjectA) { if (ARG(2)) memset(GP(ARG(2)), 0, ARG(1)); HLE_RET(c, ARG(1), 12); }
HLE(GetDeviceCaps)
{
	switch (ARG(1))
	{
	case 8: HLE_RET(c, (u32)g_screen_w, 8);   /* HORZRES */
	case 10: HLE_RET(c, (u32)g_screen_h, 8);  /* VERTRES */
	case 12: HLE_RET(c, 32, 8);               /* BITSPIXEL */
	case 88: case 90: HLE_RET(c, 96, 8);      /* LOGPIXELSX/Y */
	case 116: HLE_RET(c, 60, 8);              /* VREFRESH */
	default: HLE_RET(c, 0, 8);
	}
}
HLE(GetPixel) { HLE_RET(c, 0, 12); }
HLE(GetTextExtentPoint32W) { ST4(ARG(3), 8 * ARG(2)); ST4(ARG(3) + 4, 16); HLE_RET(c, 1, 16); }
HLE(GetTextExtentPoint32A) { ST4(ARG(3), 8 * ARG(2)); ST4(ARG(3) + 4, 16); HLE_RET(c, 1, 16); }
HLE(ExtTextOutA) { HLE_RET(c, 1, 32); }
HLE(TextOutW) { HLE_RET(c, 1, 20); }
HLE(LineTo) { HLE_RET(c, 1, 12); }
HLE(MoveToEx) { HLE_RET(c, 1, 16); }
HLE(Rectangle) { HLE_RET(c, 1, 20); }
HLE(SetBkColor) { HLE_RET(c, 0, 8); }
HLE(SetBkMode) { HLE_RET(c, 1, 8); }
HLE(SetTextColor) { HLE_RET(c, 0, 8); }
HLE(SetDeviceGammaRamp) { HLE_RET(c, 1, 8); }

const HleEntry hle_gdi32[] = {
	ENTRY(BitBlt), ENTRY(StretchBlt), ENTRY(PatBlt), ENTRY(CreateBitmap), ENTRY(CreateCompatibleBitmap),
	ENTRY(CreateCompatibleDC), ENTRY(CreateDIBSection), ENTRY(CreateDIBitmap), ENTRY(CreateFontA), ENTRY(CreateFontW),
	ENTRY(CreateFontIndirectA), ENTRY(CreatePen), ENTRY(CreateSolidBrush), ENTRY(DeleteDC), ENTRY(DeleteObject),
	ENTRY(SelectObject), ENTRY(GetStockObject), ENTRY(GetObjectA), ENTRY(GetDeviceCaps), ENTRY(GetPixel),
	ENTRY(GetTextExtentPoint32W), ENTRY(GetTextExtentPoint32A), ENTRY(ExtTextOutA), ENTRY(TextOutW), ENTRY(LineTo),
	ENTRY(MoveToEx), ENTRY(Rectangle), ENTRY(SetBkColor), ENTRY(SetBkMode), ENTRY(SetTextColor),
	ENTRY(SetDeviceGammaRamp),
	{ NULL, NULL }
};
