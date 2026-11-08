#!/bin/zsh
# Builds the Universal 2 app bundle for the placement test.
#   build_assessment.sh <deps prefix> <extracted "Assessment 5.app"> <output dir>
# The Assessment shares the game's support folder (TSSupportName), so the game
# finds its results, and it opens the game's Universal 2 app when it's done.
set -e
root=${0:A:h:h}
deps=${1:A}; ass=${2:A}; out=${3:A}
game="ClueFinders 5th Grade"
name="$game Assessment"
exe="Assessment 5"
[[ -x "$ass/Contents/MacOS/$exe" ]] || { echo "no Assessment at $ass"; exit 1; }
build=$out/build
for arch in arm64 x86_64; do
	make -C $root/src -j8 ARCH=$arch DEPS=$deps OUT=$build/$arch
done

app="$out/$name.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/Game"
lipo -create $build/arm64/cfrun $build/x86_64/cfrun -output "$app/Contents/MacOS/cfrun"
ditto "$ass" "$app/Contents/Resources/Game/$exe"
cp "$ass/Contents/Resources/ASS.icns" "$app/Contents/Resources/"
version=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$ass/Contents/Info.plist" 2>/dev/null || echo 1.0)
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>cfrun</string>
	<key>CFBundleIconFile</key><string>ASS</string>
	<key>CFBundleIdentifier</key><string>com.mackiev.cluefinders5.assessment.universal</string>
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
	<key>TSSupportName</key><string>$game</string>
</dict>
</plist>
PLIST
codesign --force --sign - "$app"
lipo -info "$app/Contents/MacOS/cfrun"
echo "built $app"
