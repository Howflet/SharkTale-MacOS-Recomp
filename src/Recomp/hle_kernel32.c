/*=============================================================================
	hle_kernel32.c: KERNEL32 for the recompiled game (files, modules, memory,
	threads, synchronization, time, system info, string conversion).
=============================================================================*/
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"

enum
{
	ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
	ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18, ERROR_ALREADY_EXISTS = 183,
};
#define INVALID_HANDLE_VALUE 0xffffffffu

/*-----------------------------------------------------------------------------
	Last error, handles.
-----------------------------------------------------------------------------*/

static __thread u32 LastError;
void hle_set_last_error(u32 e) { LastError = e; }

typedef struct Handle { int kind; void* host; } Handle;
static Handle Handles[4096];
static pthread_mutex_t HandleLock = PTHREAD_MUTEX_INITIALIZER;

u32 hle_handle_new(int kind, void* host)
{
	pthread_mutex_lock(&HandleLock);
	for (u32 i = 1; i < 4096; i++)
		if (!Handles[i].kind)
		{
			Handles[i].kind = kind;
			Handles[i].host = host;
			pthread_mutex_unlock(&HandleLock);
			return 0x100 + 4 * i;
		}
	pthread_mutex_unlock(&HandleLock);
	return 0;
}

void* hle_handle_get(u32 h, int kind)
{
	u32 i = (h - 0x100) / 4;
	if (h < 0x100 || i >= 4096 || Handles[i].kind != kind)
		return NULL;
	return Handles[i].host;
}

static int handle_kind(u32 h)
{
	u32 i = (h - 0x100) / 4;
	return (h >= 0x100 && i < 4096) ? Handles[i].kind : 0;
}

void hle_handle_close(u32 h)
{
	u32 i = (h - 0x100) / 4;
	if (h >= 0x100 && i < 4096)
	{
		Handles[i].kind = 0;
		Handles[i].host = NULL;
	}
}

/*-----------------------------------------------------------------------------
	Paths. The game sees itself installed at C:\Shark Tale\System.
-----------------------------------------------------------------------------*/

#define GUEST_ROOT "C:\\Shark Tale"
char g_guest_cwd[1024] = GUEST_ROOT "\\System";

static void host_root(char* out, size_t cap)
{
	/* g_game_dir is <root>/System */
	snprintf(out, cap, "%s", g_game_dir);
	char* slash = strrchr(out, '/');
	if (slash)
		*slash = 0;
}

/* Writable overlay root (host path, no trailing slash); empty = write in place. */
char g_user_root[1024];

void hle_host_path(const char* guest, char* out, size_t cap)
{
	char full[2048];
	if (guest[0] && guest[1] == ':')
		snprintf(full, sizeof full, "%s", guest);
	else if (guest[0] == '\\' || guest[0] == '/')
		snprintf(full, sizeof full, "C:%s", guest);
	else
		snprintf(full, sizeof full, "%s\\%s", g_guest_cwd, guest);
	for (char* p = full; *p; p++)
		if (*p == '/')
			*p = '\\';
	char root[1024];
	host_root(root, sizeof root);
	size_t rl = strlen(GUEST_ROOT);
	const char* rest;
	if (!strncasecmp(full, GUEST_ROOT, rl) && (full[rl] == '\\' || !full[rl]))
	{
		rest = full + rl;
		snprintf(out, cap, "%s%s", root, rest);
	}
	else
	{
		/* Other drive paths live in a sandbox next to the game. */
		snprintf(out, cap, "%s/_drive_%c%s", root, tolower((unsigned char)full[0]), full + 2);
	}
	for (char* p = out; *p; p++)
		if (*p == '\\')
			*p = '/';
	/* Writable overlay (app bundle): the player's copy wins; saves always live there. */
	if (g_user_root[0] && !strncmp(out, root, strlen(root)))
	{
		const char* rel = out + strlen(root);
		char user[2048];
		snprintf(user, sizeof user, "%s%s", g_user_root, rel);
		if (!strncasecmp(rel, "/System/Save", 12) || !access(user, F_OK))
			snprintf(out, cap, "%s", user);
	}
}

/* For writes: a path inside the (read-only) game files moves to the overlay,
   copying the original first unless it is about to be replaced. */
static void mkdir_p(const char* path);
static int copy_file(const char* a, const char* b, int fail_if_exists);
static void writable(char* host, size_t cap, int keep_contents)
{
	if (!g_user_root[0])
		return;
	char root[1024];
	host_root(root, sizeof root);
	size_t rl = strlen(root);
	if (strncmp(host, root, rl))
		return;
	char user[2048];
	snprintf(user, sizeof user, "%s%s", g_user_root, host + rl);
	char dir[2048];
	snprintf(dir, sizeof dir, "%s", user);
	char* slash = strrchr(dir, '/');
	if (slash)
	{
		*slash = 0;
		mkdir_p(dir);
	}
	if (keep_contents && access(user, F_OK) && !access(host, F_OK))
		copy_file(host, user, 0);
	snprintf(host, cap, "%s", user);
}

void hle_host_wpath(u32 gw, char* out, size_t cap)
{
	char tmp[2048];
	g_wstr_utf8(gw, tmp, sizeof tmp);
	hle_host_path(tmp, out, cap);
}

static void guest_path_a(Ctx* c, u32 p, char* out, size_t cap) { hle_host_path(g_str(p), out, cap); }

/*-----------------------------------------------------------------------------
	Files.
-----------------------------------------------------------------------------*/

typedef struct HFile { int fd; } HFile;

static u32 create_file(Ctx* c, const char* host_in, u32 access, u32 disposition)
{
	char host[2048];
	snprintf(host, sizeof host, "%s", host_in);
	if ((access & 0x40000000u) || disposition == 1 || disposition == 2)  /* GENERIC_WRITE or creating */
		writable(host, sizeof host, disposition != 2 && disposition != 5);
	int flags = 0;
	int write = (access & 0x40000000u) != 0, read = (access & 0x80000000u) != 0 || !write;
	flags = (read && write) ? O_RDWR : write ? O_WRONLY : O_RDONLY;
	switch (disposition)
	{
	case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
	case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
	case 3: break;                              /* OPEN_EXISTING */
	case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
	case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
	}
	struct stat st;
	if (!stat(host, &st) && S_ISDIR(st.st_mode))
	{
		hle_set_last_error(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	int fd = open(host, flags, 0644);
	static int trace = -1;
	if (trace < 0)
		trace = getenv("SHARK_TRACE_FILES") != NULL;
	if (trace)
		fprintf(stderr, "[file] %s %s -> %s\n", write ? "write" : "read", host, fd < 0 ? strerror(errno) : "ok");
	if (fd < 0)
	{
		hle_set_last_error(errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	HFile* f = malloc(sizeof *f);
	f->fd = fd;
	hle_set_last_error(ERROR_SUCCESS);
	return hle_handle_new(H_FILE, f);
}

HLE(CreateFileW)
{
	char host[2048];
	hle_host_wpath(ARG(0), host, sizeof host);
	HLE_RET(c, create_file(c, host, ARG(1), ARG(4)), 28);
}

HLE(CreateFileA)
{
	char host[2048];
	guest_path_a(c, ARG(0), host, sizeof host);
	HLE_RET(c, create_file(c, host, ARG(1), ARG(4)), 28);
}

HLE(ReadFile)
{
	HFile* f = hle_handle_get(ARG(0), H_FILE);
	u32 buf = ARG(1), n = ARG(2), out = ARG(3);
	if (!f)
		HLE_RET(c, 0, 20);
	ssize_t r = read(f->fd, GP(buf), n);
	if (out)
		ST4(out, r > 0 ? (u32)r : 0);
	HLE_RET(c, r >= 0, 20);
}

HLE(WriteFile)
{
	u32 h = ARG(0), buf = ARG(1), n = ARG(2), out = ARG(3);
	HFile* f = hle_handle_get(h, H_FILE);
	ssize_t r = f ? write(f->fd, GP(buf), n) : (ssize_t)n;  /* unknown handles (console): swallow */
	if (out)
		ST4(out, r > 0 ? (u32)r : 0);
	HLE_RET(c, r >= 0, 20);
}

HLE(SetFilePointer)
{
	HFile* f = hle_handle_get(ARG(0), H_FILE);
	s64 dist = (s32)ARG(1);
	if (ARG(2))
		dist = (s64)(((u64)LD4(ARG(2)) << 32) | ARG(1));
	if (!f)
		HLE_RET(c, 0xffffffffu, 16);
	off_t r = lseek(f->fd, dist, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
	if (ARG(2))
		ST4(ARG(2), (u32)((u64)r >> 32));
	HLE_RET(c, (u32)r, 16);
}

HLE(GetFileSize)
{
	HFile* f = hle_handle_get(ARG(0), H_FILE);
	struct stat st;
	if (!f || fstat(f->fd, &st))
		HLE_RET(c, 0xffffffffu, 8);
	if (ARG(1))
		ST4(ARG(1), (u32)((u64)st.st_size >> 32));
	HLE_RET(c, (u32)st.st_size, 8);
}

typedef struct HMapping { int fd; u32 view; } HMapping;

HLE(CloseHandle)
{
	u32 h = ARG(0);
	switch (handle_kind(h))
	{
	case H_FILE: { HFile* f = hle_handle_get(h, H_FILE); close(f->fd); free(f); break; }
	case H_MAPPING: { HMapping* m = hle_handle_get(h, H_MAPPING); free(m); break; }
	default: break;
	}
	hle_handle_close(h);
	HLE_RET(c, 1, 4);
}

static void file_attr_data(const struct stat* st, u32 out)
{
	/* attrs, creation, access, write FILETIME, size high, size low */
	ST4(out, S_ISDIR(st->st_mode) ? 0x10 : 0x80);
	u64 ft = ((u64)st->st_mtime + 11644473600ull) * 10000000ull;
	for (int i = 0; i < 3; i++)
		ST8(out + 4 + 8 * i, ft);
	ST4(out + 28, (u32)((u64)st->st_size >> 32));
	ST4(out + 32, (u32)st->st_size);
}

HLE(GetFileAttributesExW)
{
	char host[2048];
	hle_host_wpath(ARG(0), host, sizeof host);
	struct stat st;
	if (stat(host, &st))
	{
		hle_set_last_error(ERROR_FILE_NOT_FOUND);
		HLE_RET(c, 0, 12);
	}
	file_attr_data(&st, ARG(2));
	HLE_RET(c, 1, 12);
}

HLE(SetFileAttributesA) { HLE_RET(c, 1, 8); }
HLE(SetFileAttributesW) { HLE_RET(c, 1, 8); }

static void mkdir_p(const char* path)
{
	char tmp[2048];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char* p = tmp + 1; *p; p++)
		if (*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

HLE(CreateDirectoryW) { char h[2048]; hle_host_wpath(ARG(0), h, sizeof h); writable(h, sizeof h, 0); mkdir_p(h); HLE_RET(c, 1, 8); }
HLE(CreateDirectoryA) { char h[2048]; guest_path_a(c, ARG(0), h, sizeof h); writable(h, sizeof h, 0); mkdir_p(h); HLE_RET(c, 1, 8); }
HLE(RemoveDirectoryW) { char h[2048]; hle_host_wpath(ARG(0), h, sizeof h); HLE_RET(c, rmdir(h) == 0, 4); }
HLE(RemoveDirectoryA) { char h[2048]; guest_path_a(c, ARG(0), h, sizeof h); HLE_RET(c, rmdir(h) == 0, 4); }
/* With the overlay, game files themselves are never deleted (only the player's copies). */
HLE(DeleteFileW) { char h[2048]; hle_host_wpath(ARG(0), h, sizeof h); writable(h, sizeof h, 0); HLE_RET(c, unlink(h) == 0, 4); }
HLE(DeleteFileA) { char h[2048]; guest_path_a(c, ARG(0), h, sizeof h); writable(h, sizeof h, 0); HLE_RET(c, unlink(h) == 0, 4); }
HLE(MoveFileW)
{
	char a[2048], b[2048];
	hle_host_wpath(ARG(0), a, sizeof a);
	hle_host_wpath(ARG(1), b, sizeof b);
	writable(a, sizeof a, 1);
	writable(b, sizeof b, 0);
	HLE_RET(c, rename(a, b) == 0, 8);
}
HLE(MoveFileA)
{
	char a[2048], b[2048];
	guest_path_a(c, ARG(0), a, sizeof a);
	guest_path_a(c, ARG(1), b, sizeof b);
	writable(a, sizeof a, 1);
	writable(b, sizeof b, 0);
	HLE_RET(c, rename(a, b) == 0, 8);
}

static int copy_file(const char* a, const char* b, int fail_if_exists)
{
	if (fail_if_exists && !access(b, F_OK))
		return 0;
	FILE* in = fopen(a, "rb");
	if (!in)
		return 0;
	FILE* out = fopen(b, "wb");
	if (!out)
	{
		fclose(in);
		return 0;
	}
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return 1;
}
HLE(CopyFileW)
{
	char a[2048], b[2048];
	hle_host_wpath(ARG(0), a, sizeof a);
	hle_host_wpath(ARG(1), b, sizeof b);
	writable(b, sizeof b, 0);
	HLE_RET(c, copy_file(a, b, ARG(2)), 12);
}
HLE(CopyFileA)
{
	char a[2048], b[2048];
	guest_path_a(c, ARG(0), a, sizeof a);
	guest_path_a(c, ARG(1), b, sizeof b);
	writable(b, sizeof b, 0);
	HLE_RET(c, copy_file(a, b, ARG(2)), 12);
}

/* FindFirstFile / FindNextFile: directory scan with a case-insensitive wildcard. */
typedef struct HFind { DIR* dir; char dirpath[2048]; char pattern[256]; int wide; } HFind;

static int wild_match(const char* p, const char* s)
{
	if (!*p)
		return !*s;
	if (*p == '*')
		return wild_match(p + 1, s) || (*s && wild_match(p, s + 1));
	if (!*s)
		return 0;
	if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))
		return wild_match(p + 1, s + 1);
	return 0;
}

static int find_next(HFind* f, u32 out)
{
	struct dirent* e;
	while ((e = readdir(f->dir)))
	{
		if (!wild_match(f->pattern, e->d_name))
			continue;
		char full[4096];
		snprintf(full, sizeof full, "%s/%s", f->dirpath, e->d_name);
		struct stat st;
		if (stat(full, &st))
			continue;
		/* WIN32_FIND_DATA: attrs, 3 FILETIME, size hi, size lo, 2 reserved, cFileName[260]... */
		u32 size = f->wide ? 592 : 320;
		memset(GP(out), 0, size);
		file_attr_data(&st, out);  /* attrs + times; then size at 28/32 like WIN32_FIND_DATA */
		if (f->wide)
			g_put_wstr(out + 44, e->d_name, 260);
		else
			snprintf((char*)GP(out + 44), 260, "%s", e->d_name);
		return 1;
	}
	return 0;
}

static u32 find_first(Ctx* c, const char* hostpattern, u32 out, int wide)
{
	HFind* f = calloc(1, sizeof *f);
	snprintf(f->dirpath, sizeof f->dirpath, "%s", hostpattern);
	char* slash = strrchr(f->dirpath, '/');
	if (slash)
	{
		snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
		*slash = 0;
	}
	if (!strcmp(f->pattern, "*.*"))
		strcpy(f->pattern, "*");
	f->wide = wide;
	f->dir = opendir(f->dirpath);
	if (!f->dir || !find_next(f, out))
	{
		if (f->dir)
			closedir(f->dir);
		free(f);
		hle_set_last_error(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return hle_handle_new(H_FIND, f);
}

HLE(FindFirstFileW) { char h[2048]; hle_host_wpath(ARG(0), h, sizeof h); HLE_RET(c, find_first(c, h, ARG(1), 1), 8); }
HLE(FindFirstFileA) { char h[2048]; guest_path_a(c, ARG(0), h, sizeof h); HLE_RET(c, find_first(c, h, ARG(1), 0), 8); }
HLE(FindNextFileW)
{
	HFind* f = hle_handle_get(ARG(0), H_FIND);
	int ok = f && find_next(f, ARG(1));
	if (!ok)
		hle_set_last_error(ERROR_NO_MORE_FILES);
	HLE_RET(c, ok, 8);
}
HLE(FindClose)
{
	HFind* f = hle_handle_get(ARG(0), H_FIND);
	if (f)
	{
		closedir(f->dir);
		free(f);
		hle_handle_close(ARG(0));
	}
	HLE_RET(c, 1, 4);
}

/* File mappings: whole-file read into guest memory (read-only use). */
HLE(CreateFileMappingA)
{
	HFile* f = hle_handle_get(ARG(0), H_FILE);
	if (!f)
		HLE_RET(c, 0, 24);
	HMapping* m = calloc(1, sizeof *m);
	m->fd = f->fd;
	HLE_RET(c, hle_handle_new(H_MAPPING, m), 24);
}
HLE(MapViewOfFile)
{
	HMapping* m = hle_handle_get(ARG(0), H_MAPPING);
	struct stat st;
	if (!m || fstat(m->fd, &st))
		HLE_RET(c, 0, 20);
	u32 view = rt_heap_alloc((u32)st.st_size + 1);
	pread(m->fd, GP(view), (size_t)st.st_size, 0);
	m->view = view;
	HLE_RET(c, view, 20);
}
HLE(UnmapViewOfFile) { rt_heap_free(ARG(0)); HLE_RET(c, 1, 4); }

HLE(GetCurrentDirectoryW) { HLE_RET(c, g_put_wstr(ARG(1), g_guest_cwd, ARG(0)), 8); }
HLE(GetCurrentDirectoryA) { snprintf((char*)GP(ARG(1)), ARG(0), "%s", g_guest_cwd); HLE_RET(c, strlen(g_guest_cwd), 8); }
static void set_cwd(const char* p)
{
	char full[1024];
	if (p[0] && p[1] == ':')
		snprintf(full, sizeof full, "%s", p);
	else
		snprintf(full, sizeof full, "%s\\%s", g_guest_cwd, p);
	/* Collapse "dir\.." segments. */
	char out[1024] = "";
	char* save = NULL;
	for (char* tok = strtok_r(full, "\\/", &save); tok; tok = strtok_r(NULL, "\\/", &save))
	{
		if (!strcmp(tok, "."))
			continue;
		if (!strcmp(tok, ".."))
		{
			char* s = strrchr(out, '\\');
			if (s)
				*s = 0;
			continue;
		}
		if (out[0])
			strcat(out, "\\");
		strcat(out, tok);
	}
	snprintf(g_guest_cwd, sizeof g_guest_cwd, "%s", out);
}
HLE(SetCurrentDirectoryW) { char tmp[1024]; g_wstr_utf8(ARG(0), tmp, sizeof tmp); set_cwd(tmp); HLE_RET(c, 1, 4); }
HLE(SetCurrentDirectoryA) { set_cwd(g_str(ARG(0))); HLE_RET(c, 1, 4); }

HLE(GetDiskFreeSpaceExW)
{
	for (int i = 1; i <= 3; i++)
		if (ARG(i))
			ST8(ARG(i), 20ull << 30);
	HLE_RET(c, 1, 16);
}
HLE(GetDiskFreeSpaceExA)
{
	for (int i = 1; i <= 3; i++)
		if (ARG(i))
			ST8(ARG(i), 20ull << 30);
	HLE_RET(c, 1, 16);
}

/*-----------------------------------------------------------------------------
	Modules.
-----------------------------------------------------------------------------*/

/* DLLs we provide natively get fake module handles so GetProcAddress works. */
static const char* FakeModules[] = { "OpenAL32.dll", "DefOpenAL32.dll", "kernel32.dll", "user32.dll",
	"d3d8.dll", "dinput8.dll", "ddraw.dll", "binkw32.dll", "winmm.dll", "msvcr70.dll", "RICHED32.dll",
	"RICHED20.dll", "comctl32.dll", NULL };
#define FAKE_MODULE_BASE 0x7e900000u

static u32 load_library(Ctx* c, const char* name)
{
	const char* base = name;
	for (const char* p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	GuestModule* m = rt_load_module(base);
	if (m)
	{
		rt_init_modules(c);
		return m->base;
	}
	for (int i = 0; FakeModules[i]; i++)
	{
		char with_ext[128];
		snprintf(with_ext, sizeof with_ext, "%s%s", base, strchr(base, '.') ? "" : ".dll");
		if (!strcasecmp(with_ext, FakeModules[i]))
			return FAKE_MODULE_BASE + 0x10000 * i;
	}
	fprintf(stderr, "[kernel32] LoadLibrary(%s): not available\n", name);
	hle_set_last_error(126);
	return 0;
}

HLE(LoadLibraryA) { HLE_RET(c, load_library(c, g_str(ARG(0))), 4); }
HLE(LoadLibraryW) { char n[512]; g_wstr_utf8(ARG(0), n, sizeof n); HLE_RET(c, load_library(c, n), 4); }
HLE(LoadLibraryExW) { char n[512]; g_wstr_utf8(ARG(0), n, sizeof n); HLE_RET(c, load_library(c, n), 12); }
HLE(FreeLibrary) { HLE_RET(c, 1, 4); }
HLE(DisableThreadLibraryCalls) { HLE_RET(c, 1, 4); }

static u32 module_handle(const char* name)
{
	if (!name)
	{
		GuestModule* exe = rt_find_module("Game.exe");
		return exe ? exe->base : 0;
	}
	char file[256];
	snprintf(file, sizeof file, "%s%s", name, strchr(name, '.') ? "" : ".dll");
	GuestModule* m = rt_find_module(file);
	if (m)
		return m->base;
	for (int i = 0; FakeModules[i]; i++)
		if (!strcasecmp(file, FakeModules[i]))
			return FAKE_MODULE_BASE + 0x10000 * i;
	return 0;
}

HLE(GetModuleHandleA) { HLE_RET(c, module_handle(ARG(0) ? g_str(ARG(0)) : NULL), 4); }
HLE(GetModuleHandleW)
{
	char n[512];
	HLE_RET(c, module_handle(ARG(0) ? g_wstr_utf8(ARG(0), n, sizeof n) : NULL), 4);
}

static const char* module_name_of(u32 h)
{
	if (!h)
		return "Game.exe";
	GuestModule* m = rt_module_at(h);
	if (m)
		return m->name;
	if (h >= FAKE_MODULE_BASE && h < FAKE_MODULE_BASE + 0x10000 * 16)
		return FakeModules[(h - FAKE_MODULE_BASE) / 0x10000];
	return "Game.exe";
}

HLE(GetModuleFileNameW)
{
	char path[512];
	snprintf(path, sizeof path, GUEST_ROOT "\\System\\%s", module_name_of(ARG(0)));
	HLE_RET(c, g_put_wstr(ARG(1), path, ARG(2)), 12);
}
HLE(GetModuleFileNameA)
{
	char path[512];
	snprintf(path, sizeof path, GUEST_ROOT "\\System\\%s", module_name_of(ARG(0)));
	snprintf((char*)GP(ARG(1)), ARG(2), "%s", path);
	HLE_RET(c, strlen(path), 12);
}

HLE(GetProcAddress)
{
	u32 h = ARG(0), name = ARG(1);
	char nbuf[16];
	const char* n = name < 0x10000 ? (snprintf(nbuf, sizeof nbuf, "#%u", name), nbuf) : g_str(name);
	GuestModule* m = rt_module_at(h);
	if (m)
		HLE_RET(c, rt_module_export(m, name < 0x10000 ? (const char*)(uintptr_t)name : n), 8);
	if (h >= FAKE_MODULE_BASE && h < FAKE_MODULE_BASE + 0x10000 * 16)
		HLE_RET(c, rt_hle_address(FakeModules[(h - FAKE_MODULE_BASE) / 0x10000], n), 8);
	HLE_RET(c, 0, 8);
}

/* Resources: the game's icons/dialogs are not needed. */
HLE_CONST(FindResourceA, 0, 12)
HLE_CONST(FindResourceW, 0, 12)
HLE_CONST(LoadResource, 0, 8)
HLE_CONST(LockResource, 0, 4)
HLE_CONST(SizeofResource, 0, 8)

/*-----------------------------------------------------------------------------
	Memory.
-----------------------------------------------------------------------------*/

HLE(VirtualAlloc)
{
	u32 addr = ARG(0), size = ARG(1);
	if (addr)
		HLE_RET(c, addr, 16);  /* commit of a previous reservation */
	HLE_RET(c, rt_pages_alloc(size), 16);
}
HLE(VirtualFree)
{
	if (ARG(2) & 0x8000)  /* MEM_RELEASE; decommits keep the pages */
		rt_pages_free(ARG(0));
	HLE_RET(c, 1, 12);
}
HLE(GlobalAlloc)
{
	u32 p = rt_heap_alloc(ARG(1));
	if (ARG(0) & 0x40)
		memset(GP(p), 0, ARG(1));
	HLE_RET(c, p, 8);
}
HLE(GlobalLock) { HLE_RET(c, ARG(0), 4); }
HLE_CONST(GlobalUnlock, 1, 4)
HLE_CONST(GetProcessHeap, 0x00500000, 0)
HLE(HeapFree) { rt_heap_free(ARG(2)); HLE_RET(c, 1, 12); }

HLE(GlobalMemoryStatus)
{
	u32 m = ARG(0);
	ST4(m + 0, 32);
	ST4(m + 4, 20);                 /* load % */
	ST4(m + 8, 1024u << 20);        /* total phys */
	ST4(m + 12, 768u << 20);        /* avail phys */
	ST4(m + 16, 2047u << 20);       /* total page file */
	ST4(m + 20, 1536u << 20);
	ST4(m + 24, 2047u << 20);       /* total virtual */
	ST4(m + 28, 1536u << 20);
	HLE_RET(c, 0, 4);
}

HLE(GetProcessWorkingSetSize)
{
	ST4(ARG(1), 64u << 20);
	ST4(ARG(2), 512u << 20);
	HLE_RET(c, 1, 12);
}

/*-----------------------------------------------------------------------------
	Synchronization and threads.
-----------------------------------------------------------------------------*/

/* CRITICAL_SECTION: we store a host mutex pointer in its first field slot. */
static pthread_mutex_t* cs_mutex(u32 cs)
{
	u32 key = LD4(cs + 20);  /* SpinCount slot, unused by the game */
	return (pthread_mutex_t*)(uintptr_t)(((u64)LD4(cs + 16) << 32) | key);
}
HLE(InitializeCriticalSection)
{
	u32 cs = ARG(0);
	memset(GP(cs), 0, 24);
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_t* m = malloc(sizeof *m);
	pthread_mutex_init(m, &a);
	u64 p = (u64)(uintptr_t)m;
	ST4(cs + 16, (u32)(p >> 32));
	ST4(cs + 20, (u32)p);
	HLE_RET(c, 0, 4);
}
HLE(DeleteCriticalSection) { HLE_RET(c, 0, 4); }
HLE(EnterCriticalSection)
{
	pthread_mutex_t* m = cs_mutex(ARG(0));
	if (m)
		pthread_mutex_lock(m);
	HLE_RET(c, 0, 4);
}
HLE(LeaveCriticalSection)
{
	pthread_mutex_t* m = cs_mutex(ARG(0));
	if (m)
		pthread_mutex_unlock(m);
	HLE_RET(c, 0, 4);
}

HLE(CreateMutexA) { hle_set_last_error(0); HLE_RET(c, hle_handle_new(H_MUTEX, NULL), 12); }
HLE(CreateMutexW) { hle_set_last_error(0); HLE_RET(c, hle_handle_new(H_MUTEX, NULL), 12); }

typedef struct ThreadStart { u32 fn, arg; } ThreadStart;
static void* thread_main(void* p)
{
	ThreadStart ts = *(ThreadStart*)p;
	free(p);
	Ctx* c = rt_new_thread_ctx();
	u32 args[1] = { ts.arg };
	rt_call_guest(c, ts.fn, 1, args, 1);
	return NULL;
}
HLE(CreateThread)
{
	ThreadStart* ts = malloc(sizeof *ts);
	ts->fn = ARG(2);
	ts->arg = ARG(3);
	pthread_t t;
	pthread_create(&t, NULL, thread_main, ts);
	pthread_detach(t);
	u32 h = hle_handle_new(H_THREAD, NULL);
	if (ARG(5))
		ST4(ARG(5), h);
	HLE_RET(c, h, 24);
}
HLE(ExitThread) { pthread_exit(NULL); }
HLE_CONST(SetThreadPriority, 1, 8)
HLE_CONST(GetCurrentThread, 0xfffffffeu, 0)
HLE(GetCurrentThreadId) { HLE_RET(c, 0x100 + c->thread_id, 0); }
HLE_CONST(GetCurrentProcess, 0xffffffffu, 0)
HLE_CONST(GetCurrentProcessId, 0x1234, 0)
HLE(Sleep) { usleep(ARG(0) * 1000); HLE_RET(c, 0, 4); }
HLE(ExitProcess) { fprintf(stderr, "[kernel32] ExitProcess(%u)\n", ARG(0)); exit((int)ARG(0)); }
HLE_CONST(CreateProcessA, 0, 40)
HLE_CONST(CreateProcessW, 0, 40)
HLE_CONST(GetExitCodeProcess, 0, 8)
HLE_CONST(SetConsoleCtrlHandler, 1, 8)
HLE(DebugBreak) { recomp_fail(c, 0, "DebugBreak"); }

/*-----------------------------------------------------------------------------
	Time.
-----------------------------------------------------------------------------*/

static u64 now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}
static u64 StartMs;
u32 hle_tick_count(void)
{
	if (!StartMs)
		StartMs = now_ms() - 1000;
	return (u32)(now_ms() - StartMs);
}
HLE(GetTickCount) { HLE_RET(c, hle_tick_count(), 0); }

static void put_systemtime(u32 st, const struct tm* t, int ms)
{
	ST2(st + 0, (u16)(t->tm_year + 1900));
	ST2(st + 2, (u16)(t->tm_mon + 1));
	ST2(st + 4, (u16)t->tm_wday);
	ST2(st + 6, (u16)t->tm_mday);
	ST2(st + 8, (u16)t->tm_hour);
	ST2(st + 10, (u16)t->tm_min);
	ST2(st + 12, (u16)t->tm_sec);
	ST2(st + 14, (u16)ms);
}
HLE(GetLocalTime)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm t;
	localtime_r(&tv.tv_sec, &t);
	put_systemtime(ARG(0), &t, (int)(tv.tv_usec / 1000));
	HLE_RET(c, 0, 4);
}
HLE(SystemTimeToFileTime)
{
	u32 st = ARG(0);
	struct tm t = { 0 };
	t.tm_year = LD2(st) - 1900;
	t.tm_mon = LD2(st + 2) - 1;
	t.tm_mday = LD2(st + 6);
	t.tm_hour = LD2(st + 8);
	t.tm_min = LD2(st + 10);
	t.tm_sec = LD2(st + 12);
	time_t secs = timegm(&t);
	ST8(ARG(1), ((u64)secs + 11644473600ull) * 10000000ull + (u64)LD2(st + 14) * 10000);
	HLE_RET(c, 1, 8);
}

/*-----------------------------------------------------------------------------
	System information.
-----------------------------------------------------------------------------*/

static void fill_osversion(u32 v, int wide)
{
	/* OSVERSIONINFO: size, major, minor, build, platform, szCSDVersion[128] */
	ST4(v + 4, 5);
	ST4(v + 8, 1);
	ST4(v + 12, 2600);
	ST4(v + 16, 2);
	if (wide)
		g_put_wstr(v + 20, "Service Pack 2", 128);
	else
		snprintf((char*)GP(v + 20), 128, "Service Pack 2");
}
HLE(GetVersionExW) { fill_osversion(ARG(0), 1); HLE_RET(c, 1, 4); }
HLE(GetVersionExA) { fill_osversion(ARG(0), 0); HLE_RET(c, 1, 4); }

HLE(GetSystemInfo)
{
	u32 s = ARG(0);
	memset(GP(s), 0, 36);
	ST4(s + 4, 4096);         /* page size */
	ST4(s + 8, 0x10000);      /* min app address */
	ST4(s + 12, 0x7ffeffff);  /* max app address */
	ST4(s + 16, 1);           /* active processor mask */
	ST4(s + 20, 1);           /* number of processors */
	ST4(s + 24, 586);         /* processor type */
	ST4(s + 28, 0x10000);     /* allocation granularity */
	ST2(s + 32, 6);           /* level */
	ST2(s + 34, 0x0806);      /* revision */
	HLE_RET(c, 0, 4);
}

HLE(IsProcessorFeaturePresent)
{
	u32 f = ARG(0);  /* 6 = XMMI (SSE), 10 = XMMI64 (SSE2), 7 = 3DNow!, 3 = MMX */
	HLE_RET(c, f == 3 || f == 6 || f == 10, 4);
}

HLE(GetComputerNameW) { g_put_wstr(ARG(0), "SHARKTALE", 16); if (ARG(1)) ST4(ARG(1), 9); HLE_RET(c, 1, 8); }
HLE(GetComputerNameA) { snprintf((char*)GP(ARG(0)), 16, "SHARKTALE"); if (ARG(1)) ST4(ARG(1), 9); HLE_RET(c, 1, 8); }
HLE(GetWindowsDirectoryW) { HLE_RET(c, g_put_wstr(ARG(0), "C:\\WINDOWS", ARG(1)), 8); }
HLE(GetWindowsDirectoryA) { snprintf((char*)GP(ARG(0)), ARG(1), "C:\\WINDOWS"); HLE_RET(c, 10, 8); }
HLE(GetSystemDirectoryW) { HLE_RET(c, g_put_wstr(ARG(0), "C:\\WINDOWS\\system32", ARG(1)), 8); }
HLE(GetSystemDirectoryA) { snprintf((char*)GP(ARG(0)), ARG(1), "C:\\WINDOWS\\system32"); HLE_RET(c, 19, 8); }
HLE_CONST(GetSystemDefaultLangID, 0x0409, 0)
HLE(GetLocaleInfoW) { u32 n = ARG(3) ? g_put_wstr(ARG(2), "1", ARG(3)) + 1 : 2; HLE_RET(c, n, 16); }
HLE(GetLocaleInfoA) { if (ARG(3)) snprintf((char*)GP(ARG(2)), ARG(3), "1"); HLE_RET(c, 2, 16); }

static u32 CommandLineW, CommandLineA;
extern const char* g_command_line;  /* set by main */
HLE(GetCommandLineW)
{
	if (!CommandLineW)
	{
		u32 n = (u32)strlen(g_command_line) + 1;
		CommandLineW = hle_static_alloc(2 * n);
		g_put_wstr(CommandLineW, g_command_line, n);
	}
	HLE_RET(c, CommandLineW, 0);
}
u32 hle_command_line_a(void)
{
	if (!CommandLineA)
	{
		u32 n = (u32)strlen(g_command_line) + 1;
		CommandLineA = hle_static_alloc(n);
		memcpy(GP(CommandLineA), g_command_line, n);
	}
	return CommandLineA;
}
HLE(GetStartupInfoA)
{
	u32 s = ARG(0);
	memset(GP(s), 0, 68);
	ST4(s, 68);
	HLE_RET(c, 0, 4);
}

HLE(GetLastError) { HLE_RET(c, LastError, 0); }

HLE(FormatMessageW)
{
	/* (flags, source, msgid, lang, buffer, size, args) */
	char msg[64];
	snprintf(msg, sizeof msg, "Error %u", ARG(2));
	if (ARG(0) & 0x100)
	{
		u32 buf = rt_heap_alloc(2 * 64);
		g_put_wstr(buf, msg, 64);
		ST4(ARG(4), buf);
	}
	else
		g_put_wstr(ARG(4), msg, ARG(5));
	HLE_RET(c, strlen(msg), 28);
}
HLE(FormatMessageA)
{
	char msg[64];
	snprintf(msg, sizeof msg, "Error %u", ARG(2));
	if (ARG(0) & 0x100)
	{
		u32 buf = rt_heap_alloc(64);
		snprintf((char*)GP(buf), 64, "%s", msg);
		ST4(ARG(4), buf);
	}
	else
		snprintf((char*)GP(ARG(4)), ARG(5), "%s", msg);
	HLE_RET(c, strlen(msg), 28);
}

HLE(OutputDebugStringA) { fprintf(stderr, "[debug] %s", g_str(ARG(0))); HLE_RET(c, 0, 4); }
HLE(OutputDebugStringW) { char t[4096]; fprintf(stderr, "[debug] %s", g_wstr_utf8(ARG(0), t, sizeof t)); HLE_RET(c, 0, 4); }

static u32 NextAtom = 0xc000;
HLE(GlobalAddAtomW) { HLE_RET(c, NextAtom++, 4); }
HLE_CONST(GlobalDeleteAtom, 0, 4)

/* Code pages: the game only needs ASCII/Latin-1. */
HLE(MultiByteToWideChar)
{
	/* (codepage, flags, src, srclen, dst, dstlen) */
	u32 src = ARG(2), dst = ARG(4), cap = ARG(5);
	s32 len = (s32)ARG(3);
	if (len < 0)
		len = (s32)strlen(g_str(src)) + 1;
	if (!cap)
		HLE_RET(c, (u32)len, 24);
	u32 n = (u32)len < cap ? (u32)len : cap;
	for (u32 i = 0; i < n; i++)
		ST2(dst + 2 * i, LD1(src + i));
	HLE_RET(c, n, 24);
}
HLE(WideCharToMultiByte)
{
	/* (codepage, flags, src, srclen, dst, dstlen, default, useddefault) */
	u32 src = ARG(2), dst = ARG(4), cap = ARG(5);
	s32 len = (s32)ARG(3);
	if (len < 0)
		len = (s32)g_wcslen(src) + 1;
	if (!cap)
		HLE_RET(c, (u32)len, 32);
	u32 n = (u32)len < cap ? (u32)len : cap;
	for (u32 i = 0; i < n; i++)
	{
		u16 ch = LD2(src + 2 * i);
		ST1(dst + i, ch < 256 ? (u8)ch : '?');
	}
	HLE_RET(c, n, 32);
}

const HleEntry hle_kernel32[] = {
	ENTRY(CreateFileW), ENTRY(CreateFileA), ENTRY(ReadFile), ENTRY(WriteFile), ENTRY(SetFilePointer),
	ENTRY(GetFileSize), ENTRY(CloseHandle), ENTRY(GetFileAttributesExW), ENTRY(SetFileAttributesA),
	ENTRY(SetFileAttributesW), ENTRY(CreateDirectoryW), ENTRY(CreateDirectoryA), ENTRY(RemoveDirectoryW),
	ENTRY(RemoveDirectoryA), ENTRY(DeleteFileW), ENTRY(DeleteFileA), ENTRY(MoveFileW), ENTRY(MoveFileA),
	ENTRY(CopyFileW), ENTRY(CopyFileA), ENTRY(FindFirstFileW), ENTRY(FindFirstFileA), ENTRY(FindNextFileW),
	ENTRY_AS("FindNextFileA", FindNextFileW), ENTRY(FindClose), ENTRY(CreateFileMappingA), ENTRY(MapViewOfFile),
	ENTRY(UnmapViewOfFile), ENTRY(GetCurrentDirectoryW), ENTRY(GetCurrentDirectoryA), ENTRY(SetCurrentDirectoryW),
	ENTRY(SetCurrentDirectoryA), ENTRY(GetDiskFreeSpaceExW), ENTRY(GetDiskFreeSpaceExA), ENTRY(LoadLibraryA),
	ENTRY(LoadLibraryW), ENTRY(LoadLibraryExW), ENTRY(FreeLibrary), ENTRY(DisableThreadLibraryCalls),
	ENTRY(GetModuleHandleA), ENTRY(GetModuleHandleW), ENTRY(GetModuleFileNameW), ENTRY(GetModuleFileNameA),
	ENTRY(GetProcAddress), ENTRY(FindResourceA), ENTRY(FindResourceW), ENTRY(LoadResource), ENTRY(LockResource),
	ENTRY(SizeofResource), ENTRY(VirtualAlloc), ENTRY(VirtualFree), ENTRY(GlobalAlloc), ENTRY(GlobalLock),
	ENTRY(GlobalUnlock), ENTRY(GetProcessHeap), ENTRY(HeapFree), ENTRY(GlobalMemoryStatus),
	ENTRY(GetProcessWorkingSetSize), ENTRY(InitializeCriticalSection), ENTRY(DeleteCriticalSection),
	ENTRY(EnterCriticalSection), ENTRY(LeaveCriticalSection), ENTRY(CreateMutexA), ENTRY(CreateMutexW),
	ENTRY(CreateThread), ENTRY(ExitThread), ENTRY(SetThreadPriority), ENTRY(GetCurrentThread),
	ENTRY(GetCurrentThreadId), ENTRY(GetCurrentProcess), ENTRY(GetCurrentProcessId), ENTRY(Sleep),
	ENTRY(ExitProcess), ENTRY(CreateProcessA), ENTRY(CreateProcessW), ENTRY(GetExitCodeProcess),
	ENTRY(SetConsoleCtrlHandler), ENTRY(DebugBreak), ENTRY(GetTickCount), ENTRY(GetLocalTime),
	ENTRY(SystemTimeToFileTime), ENTRY(GetVersionExW), ENTRY(GetVersionExA), ENTRY(GetSystemInfo),
	ENTRY(IsProcessorFeaturePresent), ENTRY(GetComputerNameW), ENTRY(GetComputerNameA),
	ENTRY(GetWindowsDirectoryW), ENTRY(GetWindowsDirectoryA), ENTRY(GetSystemDirectoryW),
	ENTRY(GetSystemDirectoryA), ENTRY(GetSystemDefaultLangID), ENTRY(GetLocaleInfoW), ENTRY(GetLocaleInfoA),
	ENTRY(GetCommandLineW), ENTRY(GetStartupInfoA), ENTRY(GetLastError), ENTRY(FormatMessageW),
	ENTRY(FormatMessageA), ENTRY(OutputDebugStringA), ENTRY(OutputDebugStringW), ENTRY(GlobalAddAtomW),
	ENTRY(GlobalDeleteAtom), ENTRY(MultiByteToWideChar), ENTRY(WideCharToMultiByte),
	{ NULL, NULL }
};
