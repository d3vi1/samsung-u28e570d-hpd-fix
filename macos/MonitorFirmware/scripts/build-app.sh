#!/bin/bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$project_dir"
swift build -c release --product MonitorFirmware
binary_dir="$(swift build -c release --show-bin-path)"
app_dir="$project_dir/build/Monitor Firmware.app"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources"
cp "$binary_dir/MonitorFirmware" "$app_dir/Contents/MacOS/MonitorFirmware"
cat > "$app_dir/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Monitor Firmware</string>
<key>CFBundleDisplayName</key><string>Monitor Firmware</string>
<key>CFBundleIdentifier</key><string>ro.vilt.monitorfirmware</string>
<key>CFBundleExecutable</key><string>MonitorFirmware</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
# Finder may attach metadata to an existing build bundle. Only this generated
# bundle is normalized; otherwise codesign rejects resource forks/FinderInfo.
xattr -cr "$app_dir"
codesign --force --sign - "$app_dir"
codesign --verify --strict "$app_dir"
# A metadata-free archive avoids File Provider/Finder attributes being attached
# to the .app directory in synced Documents workspaces after the build finishes.
ditto --norsrc --noextattr -c -k --keepParent "$app_dir" "$project_dir/build/MonitorFirmware-macos-arm64.zip"
printf '%s\n' "$app_dir"
