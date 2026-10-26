/*=============================================================================
	hle_other.c: Small DLLs: ADVAPI32 (registry), WINMM (timers), SHELL32,
	OLE32, COMCTL32, COMDLG32, IMM32, WSOCK32 (networking off), D3D8/DDRAW/
	DINPUT8 creation (stubs until the Metal/input layers are attached),
	and IFC23 (force feedback: none).
=============================================================================*/
#include <stdlib.h>
#include <unistd.h>

#include "hle.h"

/* ADVAPI32: no registry; the game falls back to its .ini defaults. */
HLE(RegOpenKeyA) { HLE_RET(c, 2, 12); }
HLE(RegOpenKeyExA) { HLE_RET(c, 2, 20); }
HLE(RegCreateKeyExA) { if (ARG(7)) ST4(ARG(7), 0x8000); HLE_RET(c, 0, 36); }
HLE(RegQueryValueExA) { HLE_RET(c, 2, 24); }
HLE(RegSetValueExA) { HLE_RET(c, 0, 24); }
HLE(RegCloseKey) { HLE_RET(c, 0, 4); }
HLE(GetUserNameW) { g_put_wstr(ARG(0), "Player", LD4(ARG(1))); ST4(ARG(1), 7); HLE_RET(c, 1, 8); }
HLE(GetUserNameA) { snprintf((char*)GP(ARG(0)), LD4(ARG(1)), "Player"); ST4(ARG(1), 7); HLE_RET(c, 1, 8); }
const HleEntry hle_advapi32[] = {
	ENTRY(RegOpenKeyA), ENTRY(RegOpenKeyExA), ENTRY(RegCreateKeyExA), ENTRY(RegQueryValueExA),
	ENTRY(RegSetValueExA), ENTRY(RegCloseKey), ENTRY(GetUserNameW), ENTRY(GetUserNameA), { NULL, NULL } };

/* WINMM. */
extern u32 hle_tick_count(void);
HLE(timeGetTime) { HLE_RET(c, hle_tick_count(), 0); }
HLE(timeBeginPeriod) { HLE_RET(c, 0, 4); }
HLE(timeEndPeriod) { HLE_RET(c, 0, 4); }
const HleEntry hle_winmm[] = { ENTRY(timeGetTime), ENTRY(timeBeginPeriod), ENTRY(timeEndPeriod), { NULL, NULL } };

/* SHELL32 / OLE32 / COMCTL32 / COMDLG32 / IMM32 / VERSION. */
HLE(ShellExecuteA) { HLE_RET(c, 2, 24); }
HLE(ShellExecuteW) { HLE_RET(c, 2, 24); }
HLE(Shell_NotifyIconA) { HLE_RET(c, 1, 8); }
HLE(CoCreateGuid)
{
	for (int i = 0; i < 4; i++)
		ST4(ARG(0) + 4 * i, (u32)arc4random());
	HLE_RET(c, 0, 4);
}
HLE(InitCommonControls) { HLE_RET(c, 0, 0); }
HLE(ChooseColorA) { HLE_RET(c, 0, 4); }
HLE(GetSaveFileNameA) { HLE_RET(c, 0, 4); }
HLE(ImmAssociateContext) { HLE_RET(c, 0, 8); }
HLE(ImmCreateContext) { HLE_RET(c, 1, 0); }
HLE(ImmDestroyContext) { HLE_RET(c, 1, 4); }
HLE(ImmDisableIME) { HLE_RET(c, 1, 4); }
HLE(ImmGetCompositionStringW) { HLE_RET(c, 0, 16); }
HLE(ImmGetContext) { HLE_RET(c, 1, 4); }
HLE(ImmReleaseContext) { HLE_RET(c, 1, 8); }
const HleEntry hle_misc[] = {
	ENTRY(ShellExecuteA), ENTRY(ShellExecuteW), ENTRY(Shell_NotifyIconA), ENTRY(CoCreateGuid),
	ENTRY_AS("#17", InitCommonControls), ENTRY(ChooseColorA), ENTRY(GetSaveFileNameA), ENTRY(ImmAssociateContext),
	ENTRY(ImmCreateContext), ENTRY(ImmDestroyContext), ENTRY(ImmDisableIME), ENTRY(ImmGetCompositionStringW),
	ENTRY(ImmGetContext), ENTRY(ImmReleaseContext),
	{ NULL, NULL } };

/* WSOCK32 (by ordinal). Networking is off: WSAStartup fails, so IpDrv
   reports no network and the single-player game proceeds. */
#define WSAEFAULT 10014
static u32 WsaError = 10091;  /* WSASYSNOTREADY */
HLE(WSAStartup) { HLE_RET(c, 10091, 8); }
HLE(WSACleanup) { HLE_RET(c, 0, 0); }
HLE(WSAGetLastError) { HLE_RET(c, WsaError, 0); }
HLE(socket_fail) { HLE_RET(c, 0xffffffffu, 12); }
HLE(sock3_fail) { HLE_RET(c, 0xffffffffu, 12); }
HLE(sock1_fail) { HLE_RET(c, 0xffffffffu, 4); }
HLE(sock2_fail) { HLE_RET(c, 0xffffffffu, 8); }
HLE(sock4_fail) { HLE_RET(c, 0xffffffffu, 16); }
HLE(sock5_fail) { HLE_RET(c, 0xffffffffu, 20); }
HLE(sock6_fail) { HLE_RET(c, 0xffffffffu, 24); }
HLE(htonl) { HLE_RET(c, __builtin_bswap32(ARG(0)), 4); }
HLE(htons) { HLE_RET(c, __builtin_bswap16((u16)ARG(0)), 4); }
HLE(inet_addr) { HLE_RET(c, 0xffffffffu, 4); }
HLE(gethostbyname) { HLE_RET(c, 0, 4); }
HLE(gethostname) { snprintf((char*)GP(ARG(0)), ARG(1), "sharktale"); HLE_RET(c, 0, 8); }
HLE(WSAFDIsSet) { HLE_RET(c, 0, 8); }
const HleEntry hle_wsock32[] = {
	ENTRY_AS("#1", sock3_fail), ENTRY_AS("#2", sock3_fail), ENTRY_AS("#3", sock1_fail), ENTRY_AS("#4", sock3_fail),
	ENTRY_AS("#6", sock3_fail), ENTRY_AS("#7", sock5_fail), ENTRY_AS("#8", htonl), ENTRY_AS("#9", htons),
	ENTRY_AS("#10", inet_addr), ENTRY_AS("#12", sock3_fail), ENTRY_AS("#13", sock2_fail), ENTRY_AS("#14", htonl),
	ENTRY_AS("#15", htons), ENTRY_AS("#16", sock4_fail), ENTRY_AS("#17", sock6_fail), ENTRY_AS("#18", sock5_fail),
	ENTRY_AS("#19", sock4_fail), ENTRY_AS("#20", sock6_fail), ENTRY_AS("#21", sock5_fail), ENTRY_AS("#22", sock2_fail),
	ENTRY_AS("#23", socket_fail), ENTRY_AS("#52", gethostbyname), ENTRY_AS("#57", gethostname),
	ENTRY_AS("#111", WSAGetLastError), ENTRY_AS("#115", WSAStartup), ENTRY_AS("#116", WSACleanup),
	ENTRY_AS("#151", WSAFDIsSet),
	{ NULL, NULL } };

/* BINKW32 lives in hle_bink.c. */

/* IFC23 (Immersion force feedback): thiscall members (callee pops) and one
   cdecl static. No device: everything reports failure. */
HLE(ImmProjectCtor) { u32 t = R4(REG_ECX); memset(GP(t), 0, 16); HLE_RET(c, t, 0); }
HLE(ImmProjectDtor) { HLE_RET(c, 0, 0); }
HLE(ImmCreateDevice) { HLE_RET(c, 0, 0); }       /* static cdecl: caller pops */
HLE(ImmThis0) { HLE_RET(c, 0, 0); }
HLE(ImmThis4) { HLE_RET(c, 0, 4); }
HLE(ImmThis8) { HLE_RET(c, 0, 8); }
HLE(ImmThis12) { HLE_RET(c, 0, 12); }
HLE(ImmThis16) { HLE_RET(c, 0, 16); }
const HleEntry hle_ifc23[] = {
	ENTRY_AS("??0CImmProject@@QAE@XZ", ImmProjectCtor),
	ENTRY_AS("??1CImmProject@@QAE@XZ", ImmProjectDtor),
	ENTRY_AS("?CreateDevice@CImmDevice@@SAPAV1@PAUHINSTANCE__@@PAUHWND__@@@Z", ImmCreateDevice),
	ENTRY_AS("?CreateEffectByIndex@CImmProject@@QAEPAVCImmCompoundEffect@@HPAVCImmDevice@@K@Z", ImmThis12),
	ENTRY_AS("?GetEffectName@CImmProject@@QAEPBDH@Z", ImmThis4),
	ENTRY_AS("?GetEffectType@CImmProject@@QAEKPBD@Z", ImmThis4),
	ENTRY_AS("?GetNumEffects@CImmProject@@QAEHXZ", ImmThis0),
	ENTRY_AS("?OpenFile@CImmProject@@QAEHPBDPAVCImmDevice@@@Z", ImmThis8),
	ENTRY_AS("?Start@CImmProject@@QAEHPBDKKPAVCImmDevice@@@Z", ImmThis16),
	ENTRY_AS("?Stop@CImmProject@@QAEHPBD@Z", ImmThis4),
	{ NULL, NULL } };

