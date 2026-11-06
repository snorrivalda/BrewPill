#!/bin/bash
# Build BrewPill.app with the Command Line Tools' swiftc (no Xcode project needed)
set -e
cd "$(dirname "$0")"
APP=build/BrewPill.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp Info.plist "$APP/Contents/"
cp web/index.html "$APP/Contents/Resources/"
swiftc -O -swift-version 5 -target arm64-apple-macos14 Sources/*.swift -o "$APP/Contents/MacOS/BrewPill"
codesign --force --sign - "$APP"
echo "Built $PWD/$APP"
