/*=============================================================================
	main.c: Native entry point for the recompiled Shark Tale.

	  sharktale [--game-dir <host path to the game's System dir>] [game args...]

	Loads Game.exe and its imports into guest memory, runs DLL entry points,
	then runs Game.exe's CRT entry (WinMainCRTStartup), all as native code.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#include "recomp.h"
#include "runtime.h"

const char* g_command_line;

/* Without --game-dir: ./game/System, else next to the executable
   (<app>/Contents/Resources/game/System in the app bundle, or ../game/System). */
static const char* default_game_dir(void)
{
	struct stat st;
	if (!stat("game/System/Game.ini", &st))
		return "game/System";
	static char path[PATH_MAX];
	char exe[PATH_MAX], real[PATH_MAX];
	uint32_t n = sizeof exe;
	if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real))
		return "game/System";
	char* slash = strrchr(real, '/');
	if (slash)
		*slash = 0;
	const char* candidates[] = { "../Resources/game/System", "../game/System", "game/System" };
	for (int i = 0; i < 3; i++)
	{
		snprintf(path, sizeof path, "%s/%s", real, candidates[i]);
		char probe[PATH_MAX + 16];
		snprintf(probe, sizeof probe, "%s/Game.ini", path);
		if (!stat(probe, &st))
			return path;
	}
	return "game/System";
}

int main(int argc, char** argv)
{
	/* Launched from Finder (no terminal): keep a log in ~/Library/Logs. */
	if (getppid() == 1 && !isatty(STDERR_FILENO) && getenv("HOME"))
	{
		char log[PATH_MAX];
		snprintf(log, sizeof log, "%s/Library/Logs/SharkTaleNative.log", getenv("HOME"));
		freopen(log, "w", stderr);
	}
	setvbuf(stderr, NULL, _IONBF, 0);
	g_game_dir = default_game_dir();
	static char cmd[4096] = "\"C:\\Shark Tale\\System\\Game.exe\"";
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--game-dir") && i + 1 < argc)
		{
			g_game_dir = argv[++i];
			continue;
		}
		strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
		strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
	}
	g_command_line = cmd;

	rt_mem_init();
	Ctx* c = rt_new_thread_ctx();
	GuestModule* exe = rt_load_module("Game.exe");
	if (!exe)
	{
		fprintf(stderr, "Game.exe not found in %s\n", g_game_dir);
		return 1;
	}
	fprintf(stderr, "[main] %u guest functions registered\n", rt_dispatch_count());
	extern void rt_install_hooks(void);
	rt_install_hooks();
	extern void rt_start_sampler(Ctx*);
	rt_start_sampler(c);
	rt_init_modules(c);
	extern void rt_selftest(Ctx*);
	rt_selftest(c);
	fprintf(stderr, "[main] entering Game.exe at %#010x (cmdline: %s)\n", exe->entry, g_command_line);
	u32 r = rt_call_guest(c, exe->entry, 0, NULL, 0);
	fprintf(stderr, "[main] Game.exe returned %u\n", r);
	return (int)r;
}
