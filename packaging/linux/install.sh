#!/usr/bin/env bash
# Installs FxmeNet from this archive.
#   ./install.sh            for the current user (~/.local/bin, ~/.vst3)
#   sudo ./install.sh --system   for everybody (/usr/local/bin, /usr/lib/vst3)
#   ./install.sh --uninstall [--system]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$HOME/.local/bin"; vst="$HOME/.vst3"; doc="$HOME/.local/share/doc/fxmenet"
uninstall=0
for a in "$@"; do
  case "$a" in
    --system) bin=/usr/local/bin; vst=/usr/lib/vst3; doc=/usr/local/share/doc/fxmenet ;;
    --uninstall) uninstall=1 ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
if [ "$uninstall" = 1 ]; then
  rm -fv "$bin/fxmenet-server" "$bin/fxmenet-top"; rm -rfv "$vst/FxmeNet.vst3" "$doc"; exit 0
fi
mkdir -p "$bin" "$vst" "$doc"
install -m 644 "$here/FxmeNet-manual.pdf" "$doc/"
install -m 755 "$here/bin/fxmenet-server" "$here/bin/fxmenet-top" "$bin/"
rm -rf "$vst/FxmeNet.vst3"; cp -R "$here/vst3/FxmeNet.vst3" "$vst/"
echo "Installed: $bin/fxmenet-server, $bin/fxmenet-top, $vst/FxmeNet.vst3, $doc/FxmeNet-manual.pdf"
case ":$PATH:" in *":$bin:"*) ;; *) echo "Note: $bin is not in your PATH." ;; esac
