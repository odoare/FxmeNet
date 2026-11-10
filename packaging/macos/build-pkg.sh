#!/usr/bin/env bash
# Builds FxmeNet-<version>-macOS.pkg from a staged tree.
# Usage: build-pkg.sh <version> <stage-dir> <output.pkg>
# <stage-dir> holds: FxmeNet-manual.pdf, fxmenet-server, fxmenet-top, FxmeNet.vst3, FxmeNet.component
set -euo pipefail
version="$1"; stage="$2"; out="$3"
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"

mkdir -p "$work/r-server/usr/local/bin" "$work/r-vst3/Library/Audio/Plug-Ins/VST3" "$work/r-au/Library/Audio/Plug-Ins/Components" "$work/pkgs" "$work/r-server/usr/local/share/doc/fxmenet"
cp "$stage/FxmeNet-manual.pdf" "$work/r-server/usr/local/share/doc/fxmenet/"
cp "$stage/fxmenet-server" "$stage/fxmenet-top" "$work/r-server/usr/local/bin/"
cp -R "$stage/FxmeNet.vst3" "$work/r-vst3/Library/Audio/Plug-Ins/VST3/"
cp -R "$stage/FxmeNet.component" "$work/r-au/Library/Audio/Plug-Ins/Components/"

pkgbuild --root "$work/r-server" --identifier com.FXMechanics.FxmeNet.server --version "$version" --install-location / "$work/pkgs/server.pkg"
pkgbuild --root "$work/r-vst3"   --identifier com.FXMechanics.FxmeNet.vst3   --version "$version" --install-location / "$work/pkgs/vst3.pkg"
pkgbuild --root "$work/r-au"     --identifier com.FXMechanics.FxmeNet.au     --version "$version" --install-location / "$work/pkgs/au.pkg"

productbuild --distribution "$here/distribution.xml" --package-path "$work/pkgs" "$out"
rm -rf "$work"
