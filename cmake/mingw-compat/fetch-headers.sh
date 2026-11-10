#!/usr/bin/env bash
# Creates the header overlay for the MinGW cross-build: current Direct2D / DirectWrite / UI Automation
# headers from the official MinGW-w64 repository (older distro packages lack the ones JUCE 8 needs),
# plus the small compatibility headers in cmake/mingw-compat/include.
#   cmake/mingw-compat/fetch-headers.sh <output-dir>
set -euo pipefail
out="$(realpath -m "${1:?usage: fetch-headers.sh <output-dir>}")"
here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d)"
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/mingw-w64/mingw-w64.git "$tmp/mingw-w64"
git -C "$tmp/mingw-w64" sparse-checkout set mingw-w64-headers/include
inc="$tmp/mingw-w64/mingw-w64-headers/include"
mkdir -p "$out"
cp "$inc"/d2d1_2.h "$inc"/d2d1_2helper.h "$inc"/d2d1_3.h "$inc"/d2d1_3helper.h "$inc"/d2d1effects_1.h "$inc"/d2d1effects_2.h "$inc"/d2d1svg.h "$out"/
cp "$inc"/dwrite.h "$inc"/dwrite_1.h "$inc"/dwrite_2.h "$out"/
cp "$inc"/dwrite_3.h "$out"/dwrite_3_upstream.h
cp "$inc"/uiautomation*.h "$out"/
cp "$here"/include/* "$out"/
echo "MinGW header overlay written to $out"
