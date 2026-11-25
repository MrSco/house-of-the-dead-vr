#!/bin/bash
# iCloud sync's local rules (tests/apple/icloud_native.mm, src/platform/apple/icloud_apple.mm), on the Mac: built for
# the iOS simulator and run in a booted one (no iCloud account needed).
#   bash tests/apple/test_icloud.sh [simulator-udid]   (default: the booted one)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mktemp -d)"
SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
xcrun --sdk iphonesimulator clang++ -std=c++17 -fobjc-arc -O1 -Wall -Wno-unused-function -target arm64-apple-ios17.0-simulator \
  -isysroot "$SDK" -DHOTD_ICLOUD=1 -I"$ROOT/src" -I"$ROOT/src/core" \
  -framework Foundation -framework CloudKit -framework UIKit \
  "$ROOT/tests/apple/icloud_native.mm" -o "$OUT/icloud_native"
xcrun simctl spawn "${1:-booted}" "$OUT/icloud_native"
rm -rf "$OUT"
