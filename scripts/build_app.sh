#!/bin/zsh
# Builds the Universal 2 app bundle.
#   build_app.sh <deps prefix> <extracted "ClueFinders 5th Grade.app"> <output dir>
# The deps prefix must hold universal (arm64 + x86_64) libunicorn.a and libSDL2.a.
# Needs ffmpeg (with libx264) to convert the Sorenson Video movies.
set -e
root=${0:A:h:h}
deps=${1:A}; game=${2:A}; out=${3:A}
name="ClueFinders 5th Grade"
[[ -x "$game/Contents/MacOS/$name" ]] || { echo "no game at $game"; exit 1; }
build=$out/build
for arch in arm64 x86_64; do
	make -C $root/src -j8 ARCH=$arch DEPS=$deps OUT=$build/$arch
done

app="$out/$name.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/Game"
lipo -create $build/arm64/cfrun $build/x86_64/cfrun -output "$app/Contents/MacOS/cfrun"
# The original game, kept intact but without the .app extension so that it
# isn't treated as nested code
ditto "$game" "$app/Contents/Resources/Game/$name"
$root/scripts/transcode_movies.sh "$app/Contents/Resources/Game/$name/Contents/Resources/Movies"
cp "$game/Contents/Resources/5th Grade.icns" "$app/Contents/Resources/"
version=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$game/Contents/Info.plist" 2>/dev/null || echo 1.0)
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>cfrun</string>
	<key>CFBundleIconFile</key><string>5th Grade</string>
	<key>CFBundleIdentifier</key><string>com.mackiev.cluefinders5.universal</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>$name</string>
	<key>CFBundleDisplayName</key><string>$name</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$version</string>
	<key>CFBundleVersion</key><string>$version</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.educational-games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$app"
lipo -info "$app/Contents/MacOS/cfrun"
echo "built $app"
