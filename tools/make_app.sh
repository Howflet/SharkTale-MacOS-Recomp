#!/bin/zsh
# Build "Shark Tale.app" in the project root: the native executable,
# the game's icon, and a link to the game data (game/).
#   tools/make_app.sh            (run after tools/recomp_link.sh)
set -euo pipefail
ROOT=${0:A:h:h}
APP="$ROOT/Shark Tale.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/build/sharktale" "$APP/Contents/MacOS/SharkTale.new" && mv -f "$APP/Contents/MacOS/SharkTale.new" "$APP/Contents/MacOS/SharkTale"  # new inode: safe while running
cp "$ROOT/build/icon/SharkTale.icns" "$APP/Contents/Resources/SharkTale.icns"
ln -sfn "$ROOT/game" "$APP/Contents/Resources/game"
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
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP/Contents/MacOS/SharkTale" >/dev/null 2>&1 || true
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "built $APP"
