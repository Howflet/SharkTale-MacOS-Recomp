# Shark Tale (PC, 2004) — Analysis Notes

Source: `~/Downloads/Shark Tale ISO/Shark Tale.bin` + `.cue` (user-owned copy).
Tags: **[C]** confirmed by direct inspection · **[I]** inferred · **[U]** unknown

## 1. Fingerprint

### Disc image
- [C] Single-track `MODE1/2352` BIN/CUE, 782 MB, ISO9660 filesystem. macOS can't mount it directly. I converted it to a 2048-byte-sector `.iso` (by stripping the sector headers) in the session scratchpad. That copy is temporary.
- [C] Disc root: `setup.exe`, `Shark Tale.msi` (10.5 MB), `MainApp.cab` (612 MB, MS-CAB `MSCF`), `isscript.msi`, `instmsia/instmsiw.exe` (InstallShield 'InstallScript MSI' installer), `Autorun.exe`, `ereg.exe` (Activision web registration), `DirectX/` (DirectX 9.0c redistributable).
- [C] `version.inf`: ExtVersion 1.0, IntVersion 21.0, QA build 3320.
- [C] Binary link timestamps: 2004-08-21 (Game.exe, Engine.dll, Core.dll, D3DDrv.dll).
- [C] The cabinet holds 378 files with flat MSI file keys. Some long names are truncated (e.g. `PH_Toots_TheMaytals_FunkyKin`). → Install through `setup.exe`/MSI so files get their real names and folder layout. Don't run the game from the raw cabinet.

### Executables / DLLs (all PE32 x86)
| File | Role |
|---|---|
| `Game.exe` (244 KB) | Launcher/main exe; imports `d3d8.dll`, `DDRAW.dll` [C] |
| `Core.dll`, `Engine.dll` (4.9 MB), `Window.dll`, `WinDrv.dll` | Unreal Engine 2 core. WinDrv imports `DINPUT8.dll` [C] |
| `D3DDrv.dll` | Renderer, imports `d3d8.dll` [C] |
| `ALAudio.dll` + `DefOpenAL32.dll` | OpenAL audio with EAX 2.0/3.0 (and "EAX3.0Emulated") [C] |
| `StubAudio.dll` | Null audio device (useful fallback) [C] |
| `KWGame.dll`, `GUI.dll`, `Fire.dll`, `IpDrv.dll`, `Editor.dll` | Game/UE2 modules (KW = game codename) [I] |
| `binkw32.dll`, `binkmake.dll`, `radutil.dll`, `bink.exe` | RAD Bink video (`.bik` FMVs, 640 and 800 variants) [C] |
| `ogg.dll`, `vorbis.dll`, `vorbisfile.dll` | Ogg Vorbis music/VO (54 `.ogg`) [C] |
| `IFC23.DLL`, `PrivateProfileString.dll`, `StrUtils.dll`, `msvcr70.dll` | Helpers / MSVC 7.0 runtime [C] |

### Packed archives / data
- [C] Unreal packages: `.unr` maps (19), `.utx` textures (31), `.uax` sounds (17, incl. 146 MB music bank), `.usx` static meshes, `.ukx` animations, `.u` script (12, e.g. `STGame.u`, `KWGame.u`), `.int` localization (167).
- [C] `Packages.md5` is present (UE2 package integrity list).
- [C] Font packages named `UT2003Fonts.utx` / `WarfareFonts.utx` → derived from the UT2003-era UE2 codebase.

### Engine / renderer / audio
- [C] **Engine:** Unreal Engine 2 (UT2003-era branch), developed by Edge of Reality.
- [C] **Renderer:** **Direct3D 8** (`D3DDrv.D3DRenderDevice`, imports `d3d8.dll`). The disc ships the DX9.0c redist only as an installer requirement.
- [C] Default resolution 800×600, `StartupFullscreen=True` (`Default.ini`).
- [C] **Audio:** OpenAL (`ALAudio.ALAudioSubsystem`) + EAX, Ogg Vorbis, Bink.
- [C] **Input:** DirectInput 8.

### Copy protection
- [C] **No SafeDisc, SecuROM, StarForce, LaserLok or TAGES found.** PE section tables are standard (`.text .rdata .data .idata .rsrc/.reloc`, plus `.data1` in D3DDrv). There are no `stxt*`, `.securom` or protection-driver sections, and no known protection strings or driver names in any exe or DLL.
- [C] Game.exe, Engine.dll, Core.dll, WinDrv.dll and KWGame.dll don't import any of the usual disc-check APIs (`GetDriveType`, `GetVolumeInformation`, `DeviceIoControl`).
- [U] A plain disc-presence check elsewhere (e.g. in UnrealScript or through dynamic API lookup) hasn't been ruled out. The Wine test will show it.

## 2. Wine / CrossOver test (nothing installed yet)

- [C] **CrossOver.app is already in /Applications.** No Wine, 7z or bchunk on PATH.

Steps (all manual; ask before I do any of them):
1. Make a persistent mountable image. Use the converted `.iso` (I can re-create it next to the BIN on request), or open the `.cue` with CrossOver's "Install from disc image" option.
2. Mount it: `hdiutil attach -readonly "<path>/sharktale.iso"` → `/Volumes/<label>`.
3. CrossOver → **Install a Windows Application** → "Unlisted application" → new bottle, template **Windows XP (32-bit)** or **Windows 10 (64-bit)**, named `SharkTale`.
4. Installer source: choose `setup.exe` on the mounted volume. Let the MSI install run. **Decline the DirectX 9.0c redist step** if offered (CrossOver provides its own D3D; the redist often fails or hangs under Wine).
5. Skip electronic registration (`ereg.exe`).
6. Run `C:\Program Files\Activision\Shark Tale\System\Game.exe` [I: path from the MSI layout].
7. If anything breaks, try these in order:
   - Graphics: bottle settings → toggle **DXVK** (d3d8 goes through Wine's d8vk/wined3d path) [I].
   - Audio crash or silence: in the user `SharkTale.ini`/`Default.ini` [System]/[Engine.Engine], set `AudioDevice=ALAudio.ALAudioSubsystem` with `UseEAX=False`, or as a last resort use `AudioDevice=StubAudio.StubAudioSubsystem` [I: key names follow UE2 conventions].
   - Resolution: edit `FullscreenViewportX/Y` or set `StartupFullscreen=False`.
   - Bink FMV hang: rename the `.bik` intro files to skip them.
   - Log: `System/*.log` (UE2 writes a detailed log).

## 3. Route recommendation

| Route | What it is | Effort | Likelihood |
|---|---|---|---|
| **A. Compatibility layer (recommended)** | CrossOver (installed) running the original binaries. A D3D8 UE2 title with no DRM is a good fit. | **1–3 hours** | High [I] |
| B. Targeted shim | Only if A hits one blocker: e.g. drop in `d3d8to9`/DXVK for rendering glitches, swap OpenAL DLL for audio, an ini or launcher tweak for a disc check. | **0.5–3 days** | Covers most remaining failure modes [I] |
| C. Native ARM64 port | No public source for this UE2 licensee build. You'd have to decompile or reimplement Core/Engine/D3DDrv/WinDrv/KWGame (~8 MB of x86 code) and add Metal/OpenAL/SDL backends. UnrealScript `.u` bytecode could be reused if the VM is reproduced exactly. | **Many person-months to years** | Low; only worth it as a hobby project [I] |

**Recommendation:** Route A first, then B for the specific symptom if needed. Route C isn't worth it for playability.

## Open unknowns
- [U] Whether the game checks for the disc at runtime.
- [U] Final install path and user ini filename (confirm after install).
- [U] Actual behavior under CrossOver's D3D8 path on Apple Silicon (crashes, rendering glitches, frame pacing).

---

## 4. Native Apple Silicon port — plan (Route C chosen 2026-10-24)

### Size of the job [C]
| Module | .text | Named exports | Notes |
|---|---|---|---|
| Engine.dll | 3,416 KB | 6,429 | Actors, levels, rendering front-end, physics, Karma? [U] |
| Core.dll | 576 KB | 2,410 | UObject, packages/serialization, UnrealScript VM |
| D3DDrv.dll | 473 KB | 38 | D3D8 render device. **Replace**, don't port |
| Window.dll | 249 KB | 1,058 | Win32 UI framework (mostly editor/dialogs). Stub or skip [I] |
| GUI.dll | 194 KB | 857 | In-game menus |
| IFC23.DLL | 153 KB | 719 | Immersion force-feedback lib. **Stub** [I] |
| IpDrv.dll | 138 KB | 364 | Networking (Winsock). Likely unused in single-player; stub or port [I] |
| Game.exe | 130 KB | 1 | Launcher / main loop |
| WinDrv.dll | 117 KB | 92 | Window + DirectInput 8. **Replace** with SDL |
| KWGame.dll | 92 KB | 222 | Shark Tale game-specific native code |
| Fire.dll | 78 KB | 147 | Procedural textures |
| ALAudio.dll | 66 KB | 53 | OpenAL. Port to OpenAL Soft (native on macOS) |
| Editor.dll | — | — | Not needed |
| binkw32 / vorbis / ogg | — | — | Third-party: FFmpeg (decodes Bink 1) + libvorbis |
| PrivateProfileString / StrUtils | 1–4 KB | 0 | .NET assemblies (mscoree). Installer helpers, not needed [I] |

- About **5.6 MB of x86 game code** to rebuild. Roughly 20–30k functions [I].
- Big advantage [C]: about 12k **MSVC-mangled C++ exports**. These give class names, method names and full parameter types for free. UE2's modular DLL design also lets you swap modules one at a time.
- UnrealScript `.u` bytecode and all asset packages are reused as data. Only the C++ runtime that executes and loads them has to be rebuilt.

### Strategy
1. **Ground truth first.** Install under CrossOver (Route A) as the reference, and capture `.log` files, screenshots and saves from the original binaries.
2. **Swap modules one at a time, still as 32-bit Windows DLLs.** Reimplement each DLL in C++ and build it with clang-cl for `i686-pc-windows-msvc`, so the MSVC 7 ABI matches: name mangling, vtables, `this`-call convention. Drop it in next to the original binaries and test under CrossOver. Each step is then verifiable against the original game.
   Order (fewest dependents first): ALAudio → WinDrv → D3DDrv → Fire → KWGame → GUI → IpDrv/Window (stubs) → Core → Engine.
3. **Retarget to macOS arm64** once every module exists as source:
   - SDL2 for window and input.
   - New render device implementing the UE2 `URenderDevice` interface on Metal (or OpenGL 4.1 as a quicker first pass).
   - OpenAL Soft, libvorbis, FFmpeg for `.bik` playback.
   - Force feedback stubbed.

### Main risks
- [I] **32-bit → 64-bit.** UE2 casts pointers to `INT`, and script-declared structs and classes must match native C++ `sizeof` (the "class size mismatch" check). Some script properties hold pointers. Options: keep pointer-sized fields 32-bit through a handle table, or patch the affected script and native layouts. Treat this as the hardest problem.
- [I] **Unreal VM exactness.** Native functions are bound by index (`iNative`), so every native function the `.u` files reference must exist with identical semantics.
- [U] Whether the game uses Karma physics (MathEngine), which would be another large proprietary library to replace.
- [U] Whether UT2003/UT2004 public native-mod headers are close enough to this branch to recover class layouts. They'd save months if so.
- Legal/scope: keep the decompiled code and assets private, for your own use with your own copy.

### Effort estimate (one experienced developer, non-matching decomp) [I]
| Milestone | Scope | Estimate |
|---|---|---|
| M0 | Tooling, CrossOver baseline, demangled symbol database | 1–2 weeks |
| M1 | ALAudio, WinDrv, D3DDrv reimplemented (still Windows), verified in CrossOver | 1–2 months |
| M2 | KWGame, Fire, GUI; stubs for IpDrv, Window, IFC23 | 1–2 months |
| M3 | Core: UObject, linker, UnrealScript VM | 2–4 months |
| M4 | Engine | 4–10 months |
| M5 | macOS arm64 build: 64-bit cleanup, SDL, Metal/GL, FFmpeg | 2–4 months |
| **Total** | | **~1–2 person-years** |

### Tooling needed (nothing installed yet; each needs approval)
- [C] Present: CrossOver, Apple clang, `/usr/bin/objdump`. No decompiler is available.
- Needed:
  - **Ghidra** + a JDK 21 (decompiler, MSVC demangling, headless batch export).
  - **LLVM** via Homebrew, for `clang-cl`, `lld-link` and `llvm-undname`, to build i686 MSVC-ABI DLLs.
  - Later: SDL2, OpenAL Soft, libvorbis, FFmpeg, CMake.

---

## 5. M0 results (2026-10-24)

### Tooling installed (Homebrew, with user approval)
- [C] `ghidra` 12.2. It pulled in `openjdk@25` as a required dependency.
- [C] `openjdk@21`, `llvm` (clang-cl, llvm-undname), `lld` (lld-link).
- [C] Headless analysis works: `/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless`. Core.dll takes about 24 s; all 11 modules take a few minutes.

### Project layout
```
orig/System/        original exe/dll/.u/.int/.ini (21 MB, gitignored), SHA256 in orig/SHA256SUMS
symbols/symbols.db  SQLite: modules, exports (demangled, class/member split), imports,
                    native_classes, views cross_module_use + os_imports
symbols/raw/        per-module exports/imports TSV (all 25 PE files)
symbols/*.functions.tsv  Ghidra function lists (entry, size, thunk, name) for 11 modules
ghidra/             Ghidra project "SharkTale" with all 11 modules analyzed (gitignored)
tools/              bin2iso.py, pesize.py, pe_symbols.py, build_symdb.py,
                    ghidra_scripts/ExportFunctions.java
src/, docs/         empty (M1+)
```
To rebuild the symbol DB, run from `orig/System`: `python3 -I ../../tools/build_symdb.py ../../symbols/symbols.db <modules…>`

### Findings
- [C] **All 11,743 MSVC-mangled exports demangle cleanly** (0 failures). The rest are C symbols such as `autoclass*`, `GPackage` and `_DllMain@12`.
- [C] **373 native classes** (`autoclass*`): Engine 248, GUI 48, Core 36, KWGame 16, IpDrv 13, Fire 7, others 1–2.
- [C] **674 script-callable natives** (`exec*`): Engine 322, Core 286, IpDrv 31, GUI 25, KWGame 5.
- [C] Function totals from Ghidra:

  | Module | Functions (non-thunk) | Unnamed (`FUN_`) |
  |---|---|---|
  | Engine | 22,379 | 11,056 |
  | Core | 6,005 | 2,720 |
  | Window | 2,965 | 1,168 |
  | GUI | 1,970 | 889 |
  | D3DDrv | 1,587 | 1,180 |
  | IpDrv | 1,233 | 550 |
  | Game | 622 | 351 |
  | KWGame | 536 | 286 |
  | Fire | 395 | 170 |
  | WinDrv | 307 | 101 |
  | ALAudio | 281 | 119 |
  | **Total** | **~38.3k** | |

  The modules that have to be ported rather than replaced (Core, Engine, GUI, KWGame, Fire, ALAudio, Game) hold about **32k functions**. Many are small template instances (`TArray<T>` etc.) [I].
- [C] **Engine.dll is almost OS-free.** Its only OS imports are 5 registry calls plus `MessageBoxA`. It also uses 11 Bink calls and 10 IFC23 (force-feedback) calls. Engine's code is therefore portable C++.
- [C] Core's OS surface is about 60 Win32 calls (files, threads, critical sections, timers, DLL loading, clipboard, `CoCreateGuid`). That's small enough to shim with POSIX.
- [C] The only Bink calls are `BinkOpen/Close/DoFrame/NextFrame/CopyToBuffer/Goto/Pause/GetSummary/OpenTrack/CloseTrack/GetTrackData`. A shim over FFmpeg is straightforward.
- [C] **Karma physics IS present, statically linked into Engine.dll**: classes `AKActor` and `UKarmaParams*`, plus 58 MathEngine strings, and no separate DLL. The Karma SDK code is mixed into Engine's 22k functions. How much the game actually relies on Karma is [U]. Next step: find which maps/actors use KActor/Karma params.
- [C] Ghidra logged 2 harmless `CreateFunctionCmd` errors (overlapping thunk bodies) across the batch.

### Next (M1 candidates)
1. Write the UE2 core headers (`UObject`, `FName`, `TArray`, `FArchive`, `UClass`), recovering field layouts from Ghidra plus the demangled signatures.
2. First drop-in module: **ALAudio.dll** (281 functions, 53 exports), built with clang-cl i686 and tested under CrossOver against the original.
3. CrossOver baseline install (Route A) for ground truth.

---

## 6. M1 progress (2026-10-24)

### CrossOver baseline (ground truth)
- [C] **Bottle `SharkTaleBaseline`** (CrossOver 26.3, winxp template, 32-bit prefix on WoW64) holds an **unmodified** install. All exe/dll/.u hashes match `orig/SHA256SUMS`, and `Default.ini` matches the disc.
- [C] How it was built: InstallShield silent mode doesn't work under Wine. `setup.exe /s` exits 252, and `msiexec /qn` hangs in the `OnCheckSilentInstall` custom action. So the MSI-installed tree from the existing `SharkTale` bottle was copied in, and its two modified files were reverted.
- [C] The game needs **no installer registry keys**. The engine reads only `Software\NVIDIA Corporation\Tools\` and `Software\Microsoft\Direct3D`.
- [C] The existing user bottle `SharkTale` was **left untouched**. It holds a custom-resolution mod: `STGame.u` has 30 bytes patched, turning the `1024x768` menu entry into `1470x956`, and `Default.ini` viewport sizes are changed. The disc copy is kept as `STGame.u.orig`.
- [C] **Result: the game boots to the main menu** under CrossOver. The Activision Bink intro plays, and the menu (New Game / Load Game / Options / Credits) renders through `d3d8` → wined3d. Screenshots: `docs/baseline/run4_window*.png` (game window only).
- [C] **No disc check** was hit: the disc image was mounted during the runs, but the game never prompts for it. Running with it unmounted hasn't been tested [U].
- [C] Loaded modules, confirmed by a `+loaddll` trace in `docs/baseline/run3_cx.log`: Game, Core, Engine, Window, WinDrv, D3DDrv, GUI, KWGame, **Editor**, Fire, ALAudio, DefOpenAL32, binkw32, IFC23, ogg, vorbis, vorbisfile. Wine supplies d3d8, ddraw, dinput8, dsound, winmm.
  - **Editor.dll loads at runtime.** Something references it, so it can't simply be dropped [I: package dependency].
  - IpDrv is not loaded at boot.
- [C] At startup the game throws 6 C++ exceptions of type `TCHAR*` (`appThrowf`, ThrowInfo `Core!0x1019A668`). The engine catches them and continues. What they report is [U].
- [C] **No log file or user ini is written**, even with `-log LOG=…`. Only `Running.ini` appears. The shipping build probably has logging off, or keeps its config somewhere we haven't found [U]. Next: trace `CreateFileW` to find where config is read and written.
- [C] Debugging tip: CrossOver ignores `WINEDEBUG`. Use `wine --bottle X --cx-log FILE --debugmsg +loaddll,+seh`.
- Launch the baseline yourself:
  `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine --bottle SharkTaleBaseline -- 'C:\Program Files\Activision\Shark Tale\System\Game.exe' -windowed`

### Core layout (started)
- [C] Native class sizes for 367 of 373 classes are in `symbols.db` table `class_sizes`, read from the `UClass` registration calls by `tools/ghidra_scripts/ExportClassSizes.java`. Examples: Object 56, Field 68, Struct 144, Function 164, State 1192, Class 1340, Actor 1096, Pawn 1708, Controller 1476, LevelInfo 1736, Level 4552.
- [C] **UObject = 56 bytes; UT2003's is 40.** The first 40 bytes follow UT2003 order, confirmed from the decompiled functions shown:

  | Offset | Field | Confirmed by |
  |---|---|---|
  | 0x04 | Index | `AddObject` |
  | 0x10 | `_Linker` | `GetLinker` |
  | 0x14 | `_LinkerIndex` | `SetLinker` |
  | 0x18 | Outer | `IsIn` |
  | 0x20 | Name | `GetFullName` |
  | 0x24 | Class | `IsA` |

  - The **licensee extension is at 0x28–0x37**. 0x2C–0x37 is zeroed in the constructor, which looks like an inlined `TArray`; 0x28 is [U].
  - `UField::SuperField` sits at 0x38.
  - Linker detail: `ULinkerLoad::ExportMap` is at linker+0x94, `sizeof(FObjectExport)` is 0x24, and `_Object` is at +0x1C.
- [C] `src/Core/Inc/UnObjLayout.h` encodes this layout with `static_assert`s. They pass under `clang-cl --target=i686-pc-windows-msvc`.
- Tools added:
  - `tools/ghidra_scripts/DecompileMatching.java`: decompile functions by name into `docs/decomp/`.
  - `tools/winid.swift`: find the game window's ID so `screencapture -l` captures only that window.

### Next
1. Identify the UObject fields at 0x28–0x37, plus HashNext, StateFrame and ObjectFlags (from `StaticAllocateObject`, `ConditionalDestroy`, `SetFlags`).
2. Lay out UStruct, UState and UClass, then FName/FNameEntry, FArchive and FString.
3. Find where config and logging actually go (`CreateFileW` trace).

### Core layouts (continued, 2026-10-24)
Header: `src/Core/Inc/UnObjLayout.h`. All `static_assert`s pass under clang-cl i686.
- [C] **Struct packing is 4 bytes** (`#pragma pack(4)`). UClass registers as 1340 bytes; with default 8-byte packing the QWORD masks would pad it to 1344. Any rebuilt module must use `/Zp4` or `pack(4)`.
- [C] **UObject (56):** HashNext 0x08 (`HashObject`, 4096 buckets keyed on Name), StateFrame 0x0C (`IsProbing`, `GotoState`), ObjectFlags 0x1C (`GotoState`). The remaining unknowns are 0x28 and a TArray at 0x2C, both licensee additions.
- [C] **UObject vtable:** 27 slots. It starts with FUnknown's COM slots (QueryInterface, AddRef, Release), then the deleting destructor, then ProcessEvent… up to `ImportPostProcess`. Vtables for UObject, UField, UStruct, UState, UClass, UProperty, UFunction and FArchive are in `symbols/vtables/`, generated by `tools/vtable.py`.
- [C] **UField (68):** SuperField 0x38, Next 0x3C, HashNext 0x40.
- [C] **UStruct (144):**

  | Offset | Field |
  |---|---|
  | 0x44 | ScriptText |
  | 0x48 | CppText (serialized only when ArVer > 0x77) |
  | 0x4C | Children |
  | 0x50 | PropertiesSize |
  | 0x54 | FriendlyName |
  | 0x58 | Script (TArray) |
  | 0x64 | TextPos |
  | 0x68 | Line |
  | 0x78 | ConfigLink |
  | 0x7C | ConstructorLink |

  - [I] PropertyLink is 0x74.
  - [U] 0x6C, 0x70 and 0x80–0x8C.
- [C] **UState (1192):** ProbeMask 0x90, IgnoreMask 0x98, StateFlags 0xA0, LabelTableOffset 0xA4 (WORD). [I] VfHash[256] at 0xA8, which makes the size exact.
- [C] **UClass (1340):**

  | Offset | Field |
  |---|---|
  | 0x4A8 | ClassFlags |
  | 0x4AC | ClassUnique |
  | 0x4B0 | ClassGuid |
  | 0x4C0 | ClassWithin |
  | 0x4C4 | ClassConfigName |
  | 0x4E0 | Dependencies |
  | 0x4EC | PackageImports |
  | 0x4F8 | Defaults |
  | 0x504 | an FName array (likely HideCategories) |
  | 0x528 | ClassConstructor |
  | 0x52C | ClassStaticConstructor |

  - [I] ClassReps 0x4C8, NetFields 0x4D4.
  - [U] TArrays at 0x510, 0x51C and 0x530.
- [C] **UProperty (partial):** ArrayDim 0x44, ElementSize 0x48, PropertyFlags 0x4C, RepOffset 0x54, Offset 0x58, ConfigLinkNext 0x60, ConstructorLinkNext 0x64. Total size still [U].
- [C] **FNameEntry:** HashNext 0x08, Name[64] (TCHAR) 0x0C. [I] Index 0x00, Flags 0x04.
- [C] **FArchive (0x40):**
  - Default values: ArVer=0x81 (129) and ArLicenseeVer=3, matching the `.u` package headers. ArNetVer=2000.
  - Flag fields: ArIsLoading 0x10, ArForEdit/ArForClient/ArForServer = 1.
  - [U] Fields 0x30–0x3C (the last defaults to -1).
  - Vtable: 22 slots in UT2003 order.
- [C] **FFrame / FStateFrame:**
  - FFrame: Node 0x04, Code 0x0C.
  - FStateFrame: StateNode 0x1C, ProbeMask 0x20, LatentAction 0x28.
  - [U] One extra field at 0x18 compared with UT2003.
  - [I] Size 0x2C.
- [C] **FString** is TArray<TCHAR>, where TCHAR is a 2-byte UTF-16 code unit. The TArray growth rule is Max = Num + 3·Num/8 + 32.
- Tools: `tools/offsets.py` lists the `this+offset` accesses per decompiled function; `tools/vtable.py` dumps a vtable with slot names.

Next: identify the licensee fields (UObject 0x28/0x2C, UStruct 0x6C/0x70/0x80+, UClass 0x510/0x51C/0x530) by finding which functions read them, and recover UProperty's full size and its subclasses. Then turn the layouts into real class declarations whose vtables match.

---

## 7. Header generation and first rebuilt module (2026-10-24)

### Script layouts: every native class's fields, validated against the binary
- [C] `tools/upkg.py` reads UE2 packages (v129, licensee 3): names, imports, exports, and the Class/Struct/Property serializers recovered from Core. It then links properties exactly as `UStruct::Link` does.
  - Licensee change: when LicenseeVer > 2, `UProperty` serializes an extra FString.
- [C] **257 of 259 native classes match their registered C++ size byte for byte.** The two that don't are genuine script/C++ differences:
  - `UMovie`: the script declares `HudParent`, which the C++ class lacks.
  - `UHashTable`: it has 20 bytes of C++-only storage (the size of a TMap).
- [C] **UObject's licensee fields resolved:** 0x28 `LastStateName` (FName), 0x2C `EventCallBacks` (`TArray<FuncNameDelegate>`).
- [C] Tables in `symbols.db`:
  - `script_structs` and `script_members`: every class and struct, with field offsets and bitmasks
  - `classes`: C++ name, size and base. Bases come from UClass registrations, script supers, and 11 set from UE2's known hierarchy, confirmed by size and slot counts.
- [C] Sizes recovered by other means:

  | Class | Size | Evidence |
  |---|---|---|
  | UPackageMap | 128 | UPackageMapLevel's constructor stores Connection at 0x80 |
  | UAudioSubsystem | 60 | constructor only sets the FExec vptr |
  | USound | 140 | unnamed registration |
  | UFactory | 120 | UTextBufferFactory adds nothing |
  | UStubAudioSubsystem | 84 | copy constructor |

### Generated, verified headers for all 11 modules
`tools/gen_headers.py` writes `src/<Module>/Inc/<Module>Classes.h` from `symbols.db`, the script layouts and the binaries' vtables. `tools/check_headers.py` verifies them. Results:

| Module | Exports with identical MSVC mangling | Vtables matching slot for slot |
|---|---|---|
| Core | 95.8% | 55/55 |
| Engine | 93.2% | 316/316 |
| Window | 92.5% | 44/44 |
| GUI | 93.3% | 48/48 |
| Fire | 94.7% | 7/7 |
| IpDrv | 93.6% | 18/18 |
| KWGame | 91.3% | 16/16 |
| ALAudio | 97.9% | 1/1 |
| WinDrv | 96.3% | 2/2 |
| D3DDrv | 97.0% | 1/1 |
| StubAudio | 97.0% | 1/1 |

- [C] Every remaining mangling miss is one of: an explicit template instantiation (`TArray<T>` members, which come with the Core rewrite), a compiler-generated helper (`default ctor closure`), a function-local static string, or a destructor. Destructors were checked by real codegen, which emits the exact `??1X@@…` names.
- [C] Everything compiles with all size `static_assert`s passing. **Code compiled against these headers is ABI-compatible with the original DLLs.**
- How the generator handles the hard cases:
  - [C] **Multiple inheritance**: `??_7X@@6BFExec@@@` etc. adds the interface base, and the script's placeholder vptr int (e.g. `ExecVtbl`) is skipped.
  - [C] **Pure virtual functions that also have bodies**: e.g. `UClient::Init`/`Exec` have `_purecall` slots but exported bodies.
  - [C] **Identical-code folding**: several exports share one address, so names are resolved preferring the class's own hierarchy.
  - [C] **Shared image bases**: D3DDrv, ALAudio, KWGame, GUI, IFC23 and StubAudio all use base 0x10000000, so lookups are keyed per module.
  - [C] **Overloads**: MSVC places overloaded virtual functions in reverse declaration order; handled.
  - [C] **Unexported slots**: named from a descendant's override or from the derived class's interface vtable; otherwise a `_SlotN` placeholder. `tools/slot_overrides.json` holds hand-known interfaces (FMalloc).
- [C] `src/Core/Inc/UnNames.h`: the full EName table (274 hardcoded names), extracted from `FName::StaticInit`. Examples: NAME_Init=762, NAME_Exit=763, NAME_DevSound=782.

### Ghidra project improvements (saved)
- `PropagateThunkNames.java`: copied export names onto about 9,100 function bodies that sat behind incremental-link `jmp` stubs.
- `ApplyLayouts.java` + `tools/export_layouts.py`: 515 class/struct layouts applied, so decompiled code shows `this->Field` names.

### Build pipeline for drop-in modules (Windows x86, tested under CrossOver)
- **Import libraries:** `build/implib/*.lib` for every original DLL plus msvcr70, generated from the export tables with `llvm-dlltool`.
- **`src/Core/Inc/SharkCore.h`** provides:
  - `TEXT()`, the object and class flags, and `check()`
  - global `new`/`delete` routed through `GMalloc`
  - `IMPLEMENT_PACKAGE`, `IMPLEMENT_CLASS_EX` and `DECLARE_CLASS_BOILERPLATE`, which mirror the shipped registration call: zero GUID, object flags 0x4084004, `StaticConfigName()`
- **`src/Runtime/DllCrt.cpp`:** a minimal DLL entry point that runs static initializers (this is what registers the classes), keeps an atexit list, and calls DllMain.
- **`tools/build_module.sh <Module> <imports…>`:** compiles with clang-cl i686 and links with lld-link. Flags: `/Zp4 /Zc:wchar_t- /Zc:sizedDealloc- /GR- /GS- /EHs-c- -fno-access-control`.
- **`tools/run_dev.sh <label>`:** runs the game in the **`SharkTaleDev`** bottle. That bottle copies `System/` and symlinks the asset folders to the baseline, so `SharkTaleBaseline` stays pristine. The run records DLL loads, C++ exceptions and the game files opened in order (a progress trace). Screenshots are taken with `tools/wincap.swift` (ScreenCaptureKit, game window only).
  - [U] macOS currently refuses the capture (SCK error -3811). The Screen Recording permission probably needs re-approval in System Settings.

### Milestone: StubAudio.dll rebuilt from source and running in the game
- [C] `src/StubAudio/Src/StubAudio.cpp` (about 200 lines) is a rewrite from the decompiled original. Its **export table is identical: 38/38 names**. It imports only Core.dll (50) and Engine.dll (10).
- [C] Test with `AudioDevice=StubAudio.StubAudioSubsystem` in the dev bottle:
  - **The rebuilt DLL reaches the same point as the original**: entry map `Entry.unr`, menu package `KWMenu.u`, the logo movies.
  - The C++ exception profile is the same (4 startup throws).
  - A diagnostic build (`SHARK_CFLAGS=-DSHARK_TRACE SHARK_LIBS=kernel32_min`) proves the engine **constructed our class and called our `Init`**. The `[shark]` markers appear via `+debugstr` in `docs/runs/stubaudio_traced/cx.log`.
- Current dev bottle state: rebuilt StubAudio.dll installed (original kept as `StubAudio.dll.orig`), and `Default.ini` has `AudioDevice=StubAudio.StubAudioSubsystem`.

---

## 8. Change of method: static recompilation (2026-10-24)

**Why.** The StubAudio rewrite proved the hand-port pipeline end to end, but it also measured its cost. Porting about 32k functions by hand doesn't fit the time available, and the 32-bit → 64-bit pointer problem would still remain afterwards. **Static recompilation** reaches the same goal: a native arm64 macOS executable, with no Wine or emulator at runtime. It does so by mechanically translating every x86 function to C, compiled ahead of time for Apple Silicon. Prior art includes N64Recomp and XenonRecomp.

### Approach (what we're building)
1. **Emitter (`tools/ghidra_scripts/EmitC.java`).** Each function is turned into a C function, `void F_<addr>(Ctx*)`, by translating Ghidra's **raw P-code** (SLEIGH semantics: exact flags, x87, string ops).
   - Registers live in a per-thread register file; guest memory in a reserved 4 GB address space (32-bit guest pointers). This removes the pointer-width problem entirely.
   - Calls: direct calls become C calls, indirect calls go through an O(1) address→function table, and `switch` jumps become C `switch`es.
2. **Runtime.**
   - A PE loader. The six DLLs that share base 0x10000000 are rebased.
   - IAT patching: imports point either at other recompiled modules or at native implementations.
   - Per-thread contexts and guest stacks.
   - MSVC C++ exception support: `_CxxThrowException` and `__CxxFrameHandler` reimplemented, driven by the binaries' own FuncInfo tables, with setjmp/longjmp for catch continuations. This is needed: the game catches 4 exceptions on every boot.
3. **Native API layer** (replacing what Wine/CrossOver does):
   - msvcr70 (C runtime, UTF-16 formatting), kernel32, advapi32/ole32/shell32 stubs
   - user32/gdi32/DirectInput8 → AppKit window and input
   - Direct3D 8 → Metal
   - audio (OpenAL API)
   - stubs for Bink (movies) and IFC23 (force feedback) at first. vorbis and ogg are recompiled like the game.

### Feasibility census [C] (`tools/ghidra_scripts/PcodeCensus.java`, `symbols/pcode/`)
- **1.16M instructions** in the 12 programs → about 6.8M P-code ops. Engine alone is 734k instructions.
- **Indirect jumps:** 5,393 total, of which ~5,250 are resolved by Ghidra. The rest get a per-function label table.
- **Special instructions:** about 60 kinds, all manageable:
  - 3DNow! (`PF*`, `FEMMS`, ~2.4k uses) runs only on AMD 3DNow! CPUs. Our CPUID will report Intel without 3DNow!, so it's dead code.
  - x87 transcendentals (`fsin`/`fcos`/`fpatan`/`fyl2x`/`f2xm1`/`fscale`) map to libm.
  - A few MMX/SSE ops.
  - `cpuid` and `rdtsc` return values we define.

### Milestones
- **R1.** Emitter + runtime core; all of Core compiles; individual functions tested.
- **R2.** Loader + C runtime/kernel32 layer; Core's DLL init and class registration run natively.
- **R3.** All game modules; boot headless to the menu with the null renderer (`UNullRenderDevice`) and StubAudio.
- **R4.** Direct3D 8 → Metal; window and input through AppKit.
- **R5.** Audio (OpenAL), music (recompiled vorbis), Bink movies.

---

## 9. Static recompilation: native boot to the front-end menu (2026-10-25)

**Result: [C]** `build/sharktale` is a native arm64 macOS executable, 53 MB. It contains all 16 game modules recompiled from x86: about 49,000 functions and 264 MB of generated C. It **boots Shark Tale headless to the front-end menu and runs the game loop**, executing the engine, game logic and UnrealScript. The engine's own log (echoed by `src/Recomp/hooks.c`) shows:

```
Init: Unreal engine initialized / Client initialized
(Karma): Using SSE Optimizations
LoadMap: Entry -> Browse: FrontEnd.unr?...Class=SharksGame.Oscar
Game class is 'STGame' ... Bringing Level FrontEnd.myLevel up for play
PC GUI System Initialized / StubAudio: subsystem initialized
Init: Input system initialized for WindowsViewport0 / Opened viewport
Init: Game engine initialized / Startup time: 1.75 seconds
```
The configuration for this run is `game/System/Game.ini`: `RenderDevice=Engine.NullRenderDevice`, `AudioDevice=StubAudio.StubAudioSubsystem`.

### Pipeline (all in this repo)
| Step | Tool |
|---|---|
| Ghidra fixes (saved) | `CreateDataReferencedFunctions.java`: functions reached only from data (about 4,600) · `CreateStubTargets.java`: incremental-link thunk tables (about 5,200) · `PropagateThunkNames.java` · `FixVarargs.java` |
| P-code → C | `tools/ghidra_scripts/EmitC.java` (described below) |
| Layout | `tools/recomp_layout.txt` (guest base per module; relocation deltas → `src/Recomp/recomp_modules.h`) |
| Build | `tools/recomp_build_all.sh` (emit + compile + link, about 2–4 min on this Mac) · `tools/recomp_compile.sh` · `tools/recomp_link.sh` |
| Debug | `tools/symbolize.py` (guest address → module!function) · `SHARK_TRACE_HLE=1` (every API call) · `SHARK_TRACE_EH=1` (throws) · `SHARK_SAMPLE=<s>` (stack sampler) · `SHARK_QUIET_LOG=1` |

What `EmitC.java` does:
- Each function body is the code reachable from its entry by control flow. Ghidra's function bodies get split by data-referenced labels, so the emitter follows flow itself.
- It applies relocations at translation time (exact, from the relocation table).
- Extra entry points are created for code addresses used as immediates (EH handler thunks).
- `switch` dispatch covers indirect jumps; unresolved ones fall back to every label in the function.
- User ops take and return 128-bit values (two 64-bit halves) for SSE.
- Exception frames get host resume points: `_setjmp` after each `MOV FS:[0],ESP` and after `__SEH_prolog`, plus a continuation switch.

### Runtime (`src/Recomp/`)
- `runtime.c`:
  - a 4 GB guest address space with 32-bit pointers
  - an O(1) guest address → function table, with a fallback that follows `E9`/`FF25` jump stubs through the mapped image
  - x87 80-bit ↔ double conversion
- `loader.c`: maps the original PE images (data, vtables, IATs, exports), relocates them, binds imports to recompiled exports or native thunks, and runs DLL entry points. `LoadLibrary` of a recompiled module (WinDrv, StubAudio …) maps it on demand.
- `host.c`: threads (per-thread Ctx, guest stack, TEB for `FS:`), host→guest calls, and the guest heap.
- `uops.c`: CPUID (GenuineIntel family 6, MMX/SSE/SSE2, **no 3DNow!**), RDTSC, x87 transcendentals, SSE reciprocal estimates, `movmskps`, MMX pack/madd/shift ops.
- `hle_eh.c`: **MSVC C++ and SEH exceptions** (`_CxxThrowException`, `__CxxFrameHandler`, `_except_handler3`), driven by the binaries' own FuncInfo/try/unwind/ThrowInfo tables. The boot's four `Can't find file for package 'GeneralImpacts'` exceptions are thrown and caught exactly as on Windows.

### Native API layer (the part CrossOver/Wine normally provides)
| File | Covers |
|---|---|
| `hle_kernel32.c` | Files with path mapping (`C:\Shark Tale\…` → `game/`), find, mappings, modules/`GetProcAddress`, memory, critical sections, threads, time, system info, code pages |
| `hle_msvcrt.c` | The CRT: a UTF-16-aware printf engine on guest memory, strings, math, x87 helpers (`_ftol`, `_CIpow`), stdio, `_initterm`/atexit |
| `hle_user32.c` | A small window manager (classes, guest WndProcs, SendMessage, a posted queue, foreground/focus) plus inert GDI |
| `hle_d3d8.c` | `IDirect3D8` (adapter "GeForce FX 5900 Ultra", caps with fixed-function T&L and no shaders, modes) and `IDirectDraw7::GetAvailableVidMem` |
| `hle_dinput8.c` | DirectInput 8 keyboard/mouse fed by `input.h` state |
| `hle_other.c` | Registry (none, so .ini defaults apply), winmm, sockets (network off), Bink (no movies yet), IFC23 force feedback (none) |
| `hle_com.c` | COM objects in guest memory with native vtables |

### Next
1. **R4 graphics and input:**
   - an AppKit window (`hle_platform_pump`), feeding keyboard/mouse into `input.h` and the message queue
   - `IDirect3DDevice8` on Metal (`hle_d3d8_device.c`, currently a stub that returns NOTAVAILABLE): resources (textures incl. DXT, vertex/index buffers), render states, the fixed-function texture-stage pipeline as a Metal shader, then switch `RenderDevice` back to `D3DDrv.D3DRenderDevice`
2. **R5 audio:**
   - OpenAL via `OpenAL.framework`: map `DefOpenAL32`'s procs (ALAudio fetches them by name with `GetProcAddress`) to native OpenAL, then use `AudioDevice=ALAudio.ALAudioSubsystem`
   - Bink movies
3. Performance pass: registers/flags in C locals between calls (not yet measured).

## 10. R4: Direct3D 8 on Metal, window and input (2026-10-25)

**Result [C]:** natively on Apple Silicon (no Wine), the game:
- renders the intro screens and the **main menu** through Metal
- takes keyboard and mouse input
- starts a **New Game**, loads `LowerCity` and plays the opening tutorial cutscene with the HUD

Frame dumps are in `build/frames*/`.

### Pieces
| File | What it does |
|---|---|
| `gfx.h` | Interface between the D3D8 front end and the Metal back end: raw D3D render/texture-stage state snapshot per draw |
| `hle_d3d8_device.c` | `IDirect3DDevice8` (all 97 methods) plus Texture, CubeTexture, Surface, VertexBuffer and IndexBuffer. FVF codes and vertex declarations, D3D-style reference counting (bindings hold references, texture level surfaces share their container's count), resources freed on last Release. |
| `gfx_metal.m` | AppKit window and `CAMetalLayer`; offscreen back buffer at game resolution, scaled into the window at Present. Cached pipeline, depth-stencil and sampler states; per-frame vertex ring buffer; macOS key codes mapped to DirectInput scan codes plus Win32 VK/WM_* messages; PNG frame dump. |
| `gfx_shaders.metal` | The **fixed-function pipeline as one uber-shader**, compiled at run time and embedded by the build. Covers transforms, D3D lighting (material sources, point/spot/directional), fog, texture coordinate generation and transforms, the 8-stage texture combiner (all D3DTOPs incl. bump-env), and alpha test. |

### Memory layout fixes needed for real levels
- Texture and static vertex/index buffer contents live in host memory. The guest gets a temporary copy only while a level or range is locked. Dynamic buffers keep a persistent guest copy.
- `VirtualAlloc` was the real problem: it leaked and wasted up to 4× through the power-of-two heap, and UE2's allocator gets its pools there. It now has its own **64 KB-granule page allocator** with real `VirtualFree`, growing down from the top of the heap range.

### Other fixes
- Window messages with no target window go to the focused window (the viewport), not the first window created (the log window).
- Bink: opening a movie file returns a one-frame silent movie, so intros end at once (`eventMovieEnded`) until real playback (R5).

### Translator bug found and fixed in `EmitC.java`
Within one instruction, p-code can write a wide temporary and then read its lanes as smaller temporaries at inner offsets. Example: the 16-byte memory operand of `MULPS xmm, [mem]`. These pieces were emitted as unrelated C variables, so the lanes read as zero or stale values.
- **Symptom:** `FConvexVolume::SphereCheck`'s SSE path culled about 5% of visible spheres, so most BSP and static meshes were missing in levels.
- **Fix:** each partial temporary is now a slice of its container (extract on read, insert on write).
- **Check:** the self-test reproduced it against a C reference: x87 path 0/2000 wrong, SSE path 104/2000 wrong.
- **After the fix and a full rebuild (2:39) [C]:** the SSE path is 0/2000 wrong. LowerCity now renders completely: BSP, static meshes, Oscar, fog and lighting, about 330 draws per frame. Screenshot: `docs/native_lowercity.png`.

### Debug tools added
| Variable | Effect |
|---|---|
| `SHARK_HEADLESS=1` | No window |
| `SHARK_DUMP=<dir>` + `SHARK_DUMP_EVERY=<n>` | PNG frames |
| `SHARK_SCRIPT="<frame>:<action>,..."` | Scripted input: `k<dik>` key, `c<x>/<y>` click, `m<x>/<y>` move, `t` trace the next frame's draws, `p` screenshot, `x<cmd>` engine console |
| `SHARK_TRACE_DRAWS=<frame>` | Describe that frame's draws |
| `SHARK_TRACE_FILES=1` | Log file opens |
| `SHARK_TRACE_MATERIALS=1` | Log D3DDrv `SetMaterial` results |
| `SHARK_TRACE_SLOTS=1` | Call counts per render-interface vtable slot |
| `SHARK_SELFTEST=1` | Recompiled-kernel checks against C references; native replacements run against the recompiled code |
| `SHARK_RECOMP_LIGHTING=1` | Recompiled static-mesh lighting instead of the native one |

Example run to gameplay: `SHARK_SCRIPT="23000:k28,25990:m135/175,26000:c135/175,28990:m320/345,29000:c320/345"` (New Game → slot 1 → Yes).

### Observations, not yet resolved
- [I] Each `DrawPrimitive` arrives twice with identical state and material. Both calls come from the engine with the same stack frame, and `Clear` runs twice per frame. Likely engine behaviour; it costs 2× draws.
- [U] Many actors log `playanim with no mesh` and `Sequence '…' not found` (still 107 lines after the EmitC fix). Probably shipped-content noise; no CrossOver log exists to compare against.
- `stat render` through the host console crashes inside the stat code (calls a null pointer). Not investigated.

## 11. R5: audio (2026-10-25)

**Result [C]:** the game runs on `AudioDevice=ALAudio.ALAudioSubsystem` with native macOS OpenAL. Menu music and in-game sound effects play: 64+ sounds started in a menu-to-gameplay run, with no "is not a Bink file" warnings.

### Pieces
| File | What it does |
|---|---|
| `hle_openal.c` | The about 75 `al*`/`alc*` entry points ALAudio fetches by name from DefOpenAL32/OpenAL32, bridged (cdecl) to `OpenAL.framework`. Devices and contexts are small guest handles. Windows device names map to the default output. EAX is reported absent. |
| `bink.c` / `bink.h` | Native Bink 1 container parser and **Bink audio decoder** (DCT-III and inverse-RDFT variants, band quantisation, RLE, 1/16 crossfade). No third-party code or libraries. |
| `hle_bink.c` | binkw32 API over it: `BinkOpen` (from memory), `OpenTrack`, `GetTrackData`, `NextFrame`, `GetSummary`, `Goto`, `Close`. Guest BINK/BINKTRACK/BINKSUMMARY layouts recovered from Engine's `FBinkModInfo::GetInfo`, `FSoundData::GetPeriod` and `FBinkMovie::GetWavSoundTrack`. |
| `tools/binkdec.c` | Test tool: decodes a `.bik` audio track to WAV. |

### How sound is stored [C]
- **Every sound effect is a Bink audio file inside the `.uax` packages.** A census of 1,715 embedded blobs: 1,649 are 22,050 Hz mono and 54 are 44,100 Hz mono; a few are stereo and a few 48 kHz. All use the DCT variant (flag 0x1000).
- **Music** is Ogg Vorbis, played through the recompiled vorbis/vorbisfile.

### Checks
- `AVLogo_800.bik` audio: 10.41 s, which equals 312 frames at 29.97 fps. No clipping at the 2/N DCT-III normalisation. The sample-to-sample jump at block boundaries is the same as elsewhere (143.6 vs 147.7 mean), so the transform and crossfade are right.
- 400 effects extracted from the packages all decode, with under 0.2% clipped samples in the worst file and median RMS about 4,500.
- [U] The inverse-RDFT variant's sign and scale are unverified; no Shark Tale data uses it.

### Test switches
| Variable | Effect |
|---|---|
| `SHARK_MUTE=1` | Listener gain 0, for automated runs |
| `SHARK_DUMP_AUDIO=<dir>` | Writes the first 32 OpenAL buffers as WAV |

### Bink video (2026-10-25)
**Result [C]:** all intro movies play natively, with their soundtracks, then the game continues to the menu:
- DreamWorks (800x600, 651 frames)
- Activision (312)
- KnowWonder (640x480 at 12 fps, centred)

GameIntro (2,968 frames) plays after New Game. It is followed by the in-engine story cutscene (about 526 draws per frame) and gameplay. Screenshots: `docs/native_movie_dwlogo.png`, `docs/native_movie_kwlogo.png`, `docs/native_cutscene.png`.

How it works:
- FFmpeg was installed via Homebrew (`brew install ffmpeg`, 9.0.2) with the user's approval.
- `bink_video.c` **dlopens** libavcodec/libavutil at run time. Without FFmpeg the game still runs and the movies are skipped as before. Building needs the headers in `/opt/homebrew/include`.
- `bink.c` supplies the packets. The codec tag is `BIK<rev>`, the extradata is the 4-byte video flags (as FFmpeg's demuxer sets them), and keyframes come from bit 0 of the index. Seeks restart at the previous keyframe.
- YUV 4:2:0 (BT.601, limited range) is converted to BGRA.
- [C] The engine shows movies by locking the **back buffer**, calling `BinkCopyToBuffer` into it, and unlocking. So back-buffer `LockRect` without `D3DLOCK_READONLY` now reads the Metal frame back, and `UnlockRect` uploads it with a blit ordered within the frame (`gfx_backbuffer_read/write`). Read-only locks (ReduceMouseLag) stay cheap.

## 12. Performance pass (2026-10-25)

**Baseline [C]:** headless, uncapped, in LowerCity gameplay and cutscenes, the main thread runs at **about 140–290 fps**. Each 5 s sample varies with the scene; the mean of the last 12 samples was 194 fps. That is already 2–4× the 60 fps target. In a window the frame rate is capped by vsync (`CAMetalLayer.displaySyncEnabled`).

The profile (`sample`, 10 s in gameplay):
- The main thread never waits; it is CPU-bound in translated code.
- Translated code was about 74% of samples, `setjmp`/EH binding about 5%, dispatch about 3.5%, and the D3D/Metal layer about 1–2%.
- The hot spots are ordinary engine maths: `FPlane::TransformByUsingAdjointT`, `FConvexVolume::BoxCheck/SphereCheck`, `FMatrix::Determinant`, skeletal animation and the D3DDrv state cache.

### Changes
1. **Generated code at `-O2`** (was `-O1`). A full compile now takes about 1.5 min.
2. **Fixed guest base:** the 4 GB guest space is mapped at `0x10000000000` (`GUEST_HOST_BASE`, 1 TB). Every guest load or store becomes a constant-base access instead of reloading the global `g_mem`. macOS ignores mmap hints below 0x7000000000.
3. **`Ctx* restrict c`** in every generated function. Guest memory never overlaps the register file, so stores to guest memory no longer force registers to be reloaded.
4. **No more `setjmp` for exceptions.** Before, each function that registered an SEH/C++ frame (about 4% of functions, but half of the hottest ones) called `_setjmp`. Clang then treats the whole function as returns_twice and stops optimising it. On `FUN_104f30a0` that meant 29,998 instructions versus 11,674 without, and 3.4× the loads.
   - Now a throw records the target (`Ctx::unwind_record`, continuation, ESP/EBP) and returns. Every call site runs `UNWIND_CHECK()`, which returns early until the function whose `eh_rec` matches resumes at its continuation.
   - `rt_call_guest` runs nothing while unwinding. `__CxxFrameHandler` stops if the catch block itself throws or rethrows; this was found by the boot's rethrow chain.
   - The thread-local bind table is gone.
   - The executable shrank from 53 MB to 43 MB.

### Results [C]
- `tools/bench.sh` (scripted New Game → LowerCity, mean of the last 12 five-second samples) gives **222 fps**, against 194 at baseline.
- The bench is noisy (each sample ranges 140–290 with the scene), so the measured gain is modest and within scene variation. The code itself is much leaner.
- No regressions: the SphereCheck self-test passes, boot exceptions are thrown and caught, and the movies, menu, story cutscene and gameplay render as before (`build/frames15/`).

### Not done
Promoting x86 registers and flags to C locals across call sites (the translator still spills every register to `Ctx::r` around calls). That is the next big lever if more speed is ever needed; it is not needed for 60 fps.

### Frame rate cap (2026-10-26)
Uncapped, the game burns a full core rendering 140–290 fps, and vsync only caps it at the display's refresh rate (120 Hz on ProMotion). `gfx_present` now ends with `limit_frame_rate()`, which sleeps (`mach_wait_until`) to an absolute 1/60 s deadline. `SHARK_MAXFPS=<n>` changes the cap and `0` removes it. `SHARK_SCRIPT` counts frames, so scripts timed for an uncapped run fire much later when capped; `tools/bench.sh` sets `SHARK_MAXFPS=0`.

## 13. Playable app (2026-10-25)

**`Shark Tale Native.app`** (project root; rebuild with `tools/make_app.sh` after `tools/recomp_link.sh`):
- `Contents/MacOS/SharkTale` is the native executable, ad-hoc signed.
- `Contents/Resources/game` is a symlink to the project's `game/`. Saves and `Game.ini` live there; the movies and level data are linked from the CrossOver baseline bottle.
- The icon is the original Oscar icon from `Game.exe` (48×48, the only size in the exe).
- Without `--game-dir`, the executable finds its data in `./game/System`, `<exe>/../Resources/game/System` or `<exe>/../game/System`.
- Launched from Finder, the log goes to `~/Library/Logs/SharkTaleNative.log`.

### Gameplay settings
- **Resolution:** 1600×1200 (`[WinDrv.WindowsClient]` Windowed/FullscreenViewport in `game/System/Game.ini`; the 800×600 original is saved in `build/Game.ini.before_res`). Menus and HUD scale correctly. Use the window's green button for full screen.
- **Movies** are authored for 800×600. At higher resolutions `BinkCopyToBuffer` scales them up (bilinear) by screen/800 into the whole back buffer instead of a small centred picture. The engine passes the movie's height, not the screen's, so the back-buffer size comes from the device.
- **Mouse, menu cursor [C]:** the GUI cursor follows the absolute position in WM_MOUSEMOVE, while camera look uses DirectInput deltas. Freezing the macOS cursor froze the menu cursor (first playtest: it "doesn't appear most times and doesn't move"). While captured, a **virtual cursor** moved by the relative motion (scaled to game pixels and clamped to the screen) supplies WM_MOUSEMOVE/GetCursorPos. DirectInput deltas keep their fractional remainder between events.
- **Mouse:** the game captures it once at startup (`SetCapture`, `ShowCursor(FALSE)`, `ClipCursor`) and reads relative motion via DirectInput, menus included. While the window is focused, the macOS cursor is hidden and frozen (`CGAssociateMouseAndMouseCursorPosition`). Cmd-Tab or minimising releases it; clicking the window captures it again (that click is not passed to the game).
- **Keys:** the standard Windows key mapping. Cmd-Q quits through the game (WM_CLOSE).

### Verified [C], headless and muted
- Launching the bundled executable from `/` finds the data, creates the 1600×1200 device, plays the intro movies full-screen and reaches the menu.
- New Game reaches the story cutscene at 1600×1200.

### Not yet verified
First playtest (user): picture, full screen, movies, sound, keys and camera look are all good. The menu cursor did not show or move; fixed with the virtual cursor and confirmed working by the user in the window.

### Quit crash fixed (2026-10-25)
**User report:** the app crashed on quit. The log showed a call to `0x105a9f80` during Karma shutdown (`KTermGameKarma` ← `ULevel::Destroy`).

Causes and fixes:
- **Missing entry:** `0x105a9f80` is an empty callback (a lone `ret` after padding) whose address is pushed as a function pointer. Ghidra never disassembled it, so EmitC skipped it as an extra entry. EmitC now accepts immediate code addresses that Ghidra didn't reach, unless they point inside an existing instruction; `reachable()` disassembles them on the fly. All modules were re-emitted.
- **Closing the window:** `DefWindowProc` ignored `WM_CLOSE`. On Windows it destroys the window, and that is how the engine notices it should exit. It now destroys the window (WM_DESTROY/WM_NCDESTROY, guarded against re-entry). Cmd-Q and the red close button now shut the game down cleanly: exit 0, with the engine, client, OpenAL and Karma torn down. Verified [C] from the menu, through the same Karma teardown that crashed.
- **Log redirect:** `~/Library/Logs/SharkTaleNative.log` is now used only when launched by launchd (Finder/Dock), so logs from scripted runs are no longer swallowed.
- **Script action:** `q` (posts WM_CLOSE).

**User retest (2026-10-25) [C]:** the menu cursor works, and quitting works without a crash. Shark Tale is now natively playable on Apple Silicon: graphics, input, music, sound effects, movies, saving and quitting are all confirmed in the window.

### Full screen by default, free mouse in windowed mode (2026-10-25)
- **Start-up:** the app starts in macOS full screen when the game asks for it (`StartupFullscreen=True` in `Game.ini`, passed through `CreateDevice`'s windowed flag). `SHARK_WINDOWED=1` forces a window. The green button or Ctrl-Cmd-F toggles.
- **Mouse in full screen:** captured whenever focused, as before.
- **Mouse in a window** (user request, so the window can be resized):
  - Free by default, so you can drag, resize and use the edges.
  - A click inside the game captures it; that click is not passed to the game.
  - **Command** releases it.
- Focus loss always releases the mouse.
- Over the game area the system cursor is invisible (a blank cursor rect), because the game draws its own in menus. It stays normal on the title bar and resize edges.
- Each capture restarts the virtual cursor from the current position.
- [U] Window behaviour is not testable headless; awaiting the user's check.

## 14. Self-contained app (2026-10-26)

`tools/make_app.sh` now builds a **self-contained `Shark Tale.app`** (887 MB) that needs neither CrossOver nor Homebrew:
- `Contents/Resources/game` is a full copy of the game data. Links into the CrossOver baseline bottle are resolved, and player data is left out.
- `Contents/Frameworks` holds 14 libraries: libavcodec, libavutil and their whole Homebrew dependency closure (x264/x265/vpx/dav1d/opus/lame/mpg123/ssl/crypto/lzma/swresample/SvtAv1). They are relinked to `@loader_path`, ad-hoc signed, and reached through the unversioned `libavcodec.dylib`/`libavutil.dylib` symlinks. `bink_video.c` tries `@executable_path/../Frameworks` first and logs which copy it used.
- The executable links only system libraries.

**Player data** lives in `~/Library/Application Support/Shark Tale/`. This is a writable overlay over the read-only game files (`g_user_root`, set when running from a bundle; override with `SHARK_USER_DIR`):
- **Reads** prefer the overlay copy when one exists; `System/Save` always maps there.
- **Writes** (CreateFile for writing, CreateDirectory, Delete, Move, Copy) go to the overlay, copying the bundled original first unless the file is being truncated.
- The first `make_app.sh` seeds it with the project's `Save/` and `User.ini`.
- The bundle is never modified, so its signature stays valid and saves survive rebuilds.

When running from a bundle, the executable always uses its own `Contents/Resources/game/System` (canonicalised), even if launched from a directory that contains `game/`.

**Verified [C]:** launched from the project root (the worst case), headless and muted, with every file open traced:
- Every open was inside the bundle or the overlay; nothing touched the project's `game/` or the CrossOver bottle.
- Writes landed in the overlay.
- No bundle file changed, and `codesign --verify --deep` passes.
- FFmpeg loaded from the bundle; movies, menu and a clean exit all worked.

[U] Not tried: deleting the CrossOver bottle or Homebrew's FFmpeg for real. The trace shows neither is used.

The bundle contains Activision's game data and GPL FFmpeg builds. It is for personal use only; don't distribute it.

## 15. Reproducible setup (2026-10-26)

`tools/setup_ghidra.sh [dir]` recreates the Ghidra project from `orig/System`:
- It checks the hashes against `docs/SHA256SUMS`.
- It imports the 16 modules with default analysis.
- It runs PropagateThunkNames, ApplyLayouts, FixVarargs, CreateDataReferencedFunctions, CreateStubTargets and PropagateThunkNames again, the order the original project was built in (recovered from the session history).
- It takes 5 minutes.

`recomp_emit_all.sh` honours `GHIDRA_PROJECT`.

**Verified [C]:**
- A fresh project in a scratch directory, followed by `GHIDRA_PROJECT=… tools/recomp_build_all.sh`, produced generated C **byte-identical** to the known-good build: all 247 `.c` files and `recomp_modules.h`.
- The self-test passes, and the movies, main menu and a clean exit all work.
- The "unresolved indirect jumps" counts in EmitC's log summary differ slightly between runs, but the emitted code does not.

## 16. Game controllers (2026-10-26)

`src/Recomp/gamepad.m` reads the current extended gamepad (GameController.framework) once per frame and turns it into the game's own **keyboard and mouse** input, not a DirectInput joystick:
- WinDrv does support a DirectInput joystick (`EnumDevices(DI8DEVCLASS_GAMECTRL)`, `c_dfDIJoystick2`, buttons → `IK_Joy1..12`, POV → `IK_JoyPov*`, axes → `IK_JoyX/Y/Z/R/U/V/Slider*`), but the default `Joy*` bindings in `DefUser.ini` expect an old pad: swimming is a button and both sticks turn and look. Changing them would mean editing each player's `User.ini`.
- The menus are mouse- and keyboard-driven, so a joystick alone could not use them.

**Two layouts**, switched by `rt_menu_active()` (`hooks.c`): a hook on `UWindowsViewport::UpdateInput` keeps the viewport pointer, and `UPlayer::bShowWindowsMouse` (+0x48, bit 1) is set exactly while a menu wants the cursor: front end and pause menu, not gameplay, cutscenes or the intro movies. (Traced: the GUIController's `bActive` and its `MenuStack` count follow it too. No Win32 call marks the switch.)
- Gameplay: left stick → W A S D (press past 0.5, release below 0.35); right stick → relative mouse motion; A/B → left/right mouse button; X → Space; Y → F; RT/LT → Shift/Ctrl; d-pad → arrows; Menu → Escape; Options → Backspace.
- Menus: left stick → mouse (the cursor); d-pad → arrows, which move the highlight (save slots included); Cross → Enter on the highlighted item, or a left click if the stick moved the cursor more recently than the d-pad; Circle and Menu → Escape (back).
- Buttons held across a layout switch stay inert until released, so the Menu press that opens the pause menu does not also close it.
- Sticks: radial dead zone 0.25 (a worn DualSense rested at −0.19 on the right X axis and crept the cursor), squared response, `SHARK_PAD_SPEED` points/s at full tilt (default 1200).

**Bug found in the first version:** it polled on every event pump, and the game pumps on every `PeekMessage`, so each check posted another WM_MOUSEMOVE and the game's message loop never saw an empty queue; the menu cursor never moved. Polling now happens once per frame in `gfx_present`.

`gfx_metal.m` exposes `host_key`, `host_mouse_button` and `host_mouse_motion` (the old `mouse_position` is split into `mouse_motion`). Using the pad captures the mouse, like a click in the game. When the window is not key every held control is released. `SHARK_PAD_TRACE=1` logs raw element changes, control changes and layout switches.

[C] With a DualSense: camera and swimming in gameplay, the stick-driven cursor, d-pad highlighting, L2 reaching the game. Scripted: Enter selects the highlighted main-menu item and save slot, reaching LowerCity; Escape steps back through the menu stack.

## 17. Faster loading after missions (2026-10-26)

**User report:** the loading screen after a mission is long.

**Where the time goes [C]** (the player's `Game.log`, plus timed headless runs). A mission returns to LowerCity: `LoadMap` (about 2.4 s), then autosaves. Each `UGameEngine::SaveGame` draws the level through `PaintProgress` with precaching on, both before and after `SavePackage` (`Save=` about 1.06 s). Each precache spent about **3 s in "precaching geometry"**. Profile: `FStaticMeshBatchVertexStream::GetStreamData` → `FUN_104b48a0`, which relights a static mesh's vertices into its `UStaticMeshInstance` colour cache.
- `GetStreamData` calls it once per batch element. Each call zeroes the cache, bumps the revision (+0x60), and for every light in the instance's list (+0x38/+0x3c, 0x14-byte entries: light, visibility bits, lit flag at +0x10) loops over all vertices (`mesh+0x78`, 0x18 bytes: position, normal; count `mesh+0x7c`).
- It transforms each position and normal by `FDynamicActor::LocalToWorld` and calls `SafeNormal` **before** testing the vertex's visibility bit. For unlit vertices the result is unused, and this x87 work was the self time.
- Lit vertices: `SampleIntensity(pos, normal)` × (actor `+0x2c0` × const × light colour), then `FColor(FPlane)` and a saturating `FColor +=`. A final pass modulates by the mesh's vertex colours (`+0x98`, when `+0x154`) and copies their alpha.

**Native replacement:** `src/Recomp/native_lighting.c` does the same work in the same order and precision (doubles, as the recompiled x87 code keeps them, rounding to float where the original stores). It transforms only lit vertices. `SafeNormal`, `SampleIntensity` and the `FColor` calls still run as guest code through `rt_call_guest`, and the scratch they need sits below the guest stack pointer.
- **Override mechanism:** the caller reaches it by a direct C call (thunk `F_1130ae66` → `F_113b48a0`), not through dispatch, so `rt_register` can't hook it. `tools/native_overrides.txt` lists `<Module> <function>`. `tools/recomp_compile.sh` compiles the file that defines each one with `-D<fn>=<fn>_recomp`, so the native definition links in its place and can still call the recompiled one.
- `SHARK_SELFTEST=1` runs native and then recompiled on every call (resetting the revision), and compares the colour cache, the revision and the lit flags. `SHARK_RECOMP_LIGHTING=1` uses the recompiled version only.

**Verified [C]**, headless, uncapped, New Game → LowerCity → autosave (the same steps as a mission return):
- Self-test: **0/3026 calls differ** (bit-identical); exit 0.
- Geometry precache: 3.07 s → **1.41 s**, and 2.90 s → **1.31 s**.
- From `LoadMap: LowerCity` to the end of the autosave's second precache: **10.8 s → 7.4 s**.
- What is left is spread out: `FMemCache::Create`, `FUN_104af570/afae0/af830`, property serialisation for the save, and the guest calls for lit vertices.

**Not verified:** an actual mission end in the window (the steps above are the same work, measured headless).

**Found in passing:** the `savegame <n>` console command crashes the dev build (a call through a null pointer, from `0x1041cab1`). The game's own autosaves are unaffected.

