#!/bin/sh
# Builds MacGlass.app, installs it to /Applications and (re)starts it as a LaunchAgent, which keeps
# it running in the login session from then on. Log: ~/Library/Logs/MacGlass.log
# Usage: ./build.sh [codesign identity]   (default: ad-hoc, which loses the Screen
# Recording permission on every rebuild; a real identity keeps it)
set -e
cd "$(dirname "$0")"

app=build/MacGlass.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp Info.plist "$app/Contents/"
swiftc -O -swift-version 5 \
    -import-objc-header Sources/VirtualDisplayPrivate.h \
    Sources/*.swift \
    -framework AppKit -framework ScreenCaptureKit -framework VideoToolbox -framework CoreMedia -framework IOKit -framework CoreAudio \
    -o "$app/Contents/MacOS/MacGlass"
codesign --force --timestamp=none --sign "${1:--}" "$app"

label=dev.faruk.macglass
domain=gui/$(id -u)
agent=~/Library/LaunchAgents/$label.plist
launchctl bootout "$domain/$label" 2>/dev/null || true
pkill -x MacGlass || true  # a copy started by hand
while launchctl print "$domain/$label" >/dev/null 2>&1; do sleep 0.2; done

rm -rf /Applications/MacGlass.app
cp -R "$app" /Applications/
echo "installed /Applications/MacGlass.app"

mkdir -p ~/Library/LaunchAgents ~/Library/Logs
sed "s|__HOME__|$HOME|g" $label.plist > "$agent"
launchctl bootstrap "$domain" "$agent"
echo "started $label"
