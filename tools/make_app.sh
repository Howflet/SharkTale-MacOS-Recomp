#!/bin/zsh
# Build a self-contained "Shark Tale.app" in the project root:
#   Contents/MacOS/SharkTale        the native executable
#   Contents/Resources/game         a full copy of the game data (links into the
#                                   CrossOver bottle resolved), minus player data
#   Contents/Frameworks             FFmpeg's libavcodec/libavutil and everything
#                                   they link, relinked to @loader_path
# Player data (saves, User.ini, written .ini files, logs) lives outside the
# bundle in ~/Library/Application Support/Shark Tale; the first build seeds it
# from the project's game/System.
#   tools/make_app.sh            (run after tools/recomp_link.sh)
set -euo pipefail
ROOT=${0:A:h:h}
APP="$ROOT/Shark Tale.app"
RES="$APP/Contents/Resources"
FW="$APP/Contents/Frameworks"
USERDIR="$HOME/Library/Application Support/Shark Tale"
mkdir -p "$APP/Contents/MacOS" "$RES" "$FW"

# Executable (new inode, so a running copy is not disturbed) and icon.
cp "$ROOT/build/sharktale" "$APP/Contents/MacOS/SharkTale.new" && mv -f "$APP/Contents/MacOS/SharkTale.new" "$APP/Contents/MacOS/SharkTale"
cp "$ROOT/build/icon/SharkTale.icns" "$RES/SharkTale.icns"

# Game data: replace an older symlink to the project with a real copy.
[[ -L "$RES/game" ]] && rm "$RES/game"
rsync -aL --delete \
  --exclude 'System/Save/' --exclude 'System/*.log' --exclude 'System/Running.ini' \
  "$ROOT/game/" "$RES/game/"

# Seed the player's data once (existing saves and settings from the project).
if [[ ! -d "$USERDIR/System" ]]; then
  mkdir -p "$USERDIR/System"
  [[ -d "$ROOT/game/System/Save" ]] && cp -R "$ROOT/game/System/Save" "$USERDIR/System/"
  [[ -f "$ROOT/game/System/User.ini" ]] && cp "$ROOT/game/System/User.ini" "$USERDIR/System/"
  echo "seeded player data in $USERDIR"
fi

# FFmpeg: copy the dependency closure of libavcodec + libavutil, relink to @loader_path.
python3 -I - "$FW" <<'PY'
import os, shutil, subprocess, sys
fw = sys.argv[1]
def deps(p):
    out = subprocess.run(['otool', '-L', p], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
    return [l.split()[0] for l in out]
def local(ref):
    return ref.startswith('/opt/homebrew/') or ref.startswith('/usr/local/')
def install_name(p):
    return subprocess.run(['otool', '-D', p], capture_output=True, text=True, check=True).stdout.splitlines()[-1].strip()
roots = {}                        # lib -> install name, e.g. .../libavcodec.63.dylib
for lib in ('libavcodec', 'libavutil'):
    for prefix in ('/opt/homebrew/lib', '/usr/local/lib'):
        p = f'{prefix}/{lib}.dylib'
        if os.path.exists(p):
            roots[lib] = install_name(os.path.realpath(p))
            break
if len(roots) < 2:
    sys.exit('FFmpeg not found (brew install ffmpeg)')
copied = {}                       # basename of install name -> source file
work = list(roots.values())
while work:
    ref = work.pop()
    name = os.path.basename(ref)
    if name in copied:
        continue
    copied[name] = os.path.realpath(ref)
    work += [d for d in deps(copied[name]) if local(d)]
for name, src in copied.items():
    dst = os.path.join(fw, name)
    if os.path.lexists(dst):
        os.remove(dst)
    shutil.copyfile(src, dst)
    os.chmod(dst, 0o644)
    args = ['install_name_tool', '-id', '@loader_path/' + name]
    for d in deps(src):
        if local(d):
            args += ['-change', d, '@loader_path/' + os.path.basename(d)]
    subprocess.run(args + [dst], check=True, capture_output=True)
    subprocess.run(['codesign', '--force', '--sign', '-', dst], check=True, capture_output=True)
# The unversioned names the runtime dlopens.
for lib, iname in roots.items():
    link = os.path.join(fw, lib + '.dylib')
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(os.path.basename(iname), link)
print(f'bundled {len(copied)} FFmpeg libraries')
PY

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Shark Tale</string>
	<key>CFBundleDisplayName</key><string>Shark Tale</string>
	<key>CFBundleIdentifier</key><string>local.sharktale.native</string>
	<key>CFBundleExecutable</key><string>SharkTale</string>
	<key>CFBundleIconFile</key><string>SharkTale</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.1</string>
	<key>CFBundleVersion</key><string>2</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP/Contents/MacOS/SharkTale" >/dev/null 2>&1 || true
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "built $APP ($(du -sh "$APP" | cut -f1))"
