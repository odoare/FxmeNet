#!/usr/bin/env bash
# Builds fxmenet_<version>_amd64.deb from a staged tree.
# Usage: build-deb.sh <version> <stage-dir> <output.deb>
# <stage-dir> holds: bin/ (fxmenet-server, fxmenet-top), vst3/FxmeNet.vst3, fxmenet-server.service, FxmeNet-manual.pdf
set -euo pipefail
version="${1#v}"; stage="$2"; out="$3"
root="$(mktemp -d)"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/lib/vst3" "$root/usr/share/doc/fxmenet"
install -m 755 "$stage/bin/fxmenet-server" "$stage/bin/fxmenet-top" "$root/usr/bin/"
cp -R "$stage/vst3/FxmeNet.vst3" "$root/usr/lib/vst3/"
cp "$stage/FxmeNet-manual.pdf" "$root/usr/share/doc/fxmenet/"
cp "$stage/fxmenet-server.service" "$root/usr/share/doc/fxmenet/"
cat > "$root/DEBIAN/control" <<CTL
Package: fxmenet
Version: $version
Architecture: amd64
Maintainer: Olivier Doaré <olivier.doare@ensta.fr>
Section: sound
Priority: optional
Depends: libasound2t64 | libasound2, libfreetype6, libfontconfig1, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1
Homepage: https://www.fx-mechanics.com
Description: FxmeNet: effect plugins processed on a server on the network
 The fxmenet-server and fxmenet-top programs, and the FxmeNet VST3 plugin.
 The PDF manual and an example systemd unit are in /usr/share/doc/fxmenet.
CTL
dpkg-deb --root-owner-group --build "$root" "$out"
rm -rf "$root"
