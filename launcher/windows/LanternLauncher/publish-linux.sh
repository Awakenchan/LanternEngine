#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
launcher=${LANTERN_LAUNCHER_SRC:-$(cd -- "$(dirname -- "$0")/.." && pwd)}
out=${1:?usage: publish-linux.sh <output folder> [version] [informational version]}
# The package version: the argument, else the nearest git tag of this checkout (an untagged
# build keeps 0.9.9, and the launcher then shows its package.json version instead).
version=${2:-}
if [[ -z $version ]]; then
    version=$(git -C "$(dirname -- "$0")/../.." describe --tags --abbrev=0 2>/dev/null || true)
    version=${version#v}
fi
version=${version:-0.9.9}
informational=${3:-$version}
tree=${LANTERN_LAUNCHER_TREE:-$HOME/lantern-launcher}
mkdir -p "$tree"
rsync -a --delete --exclude bin --exclude obj --exclude '*.user' \
    "$launcher/BloodborneLauncher" "$launcher/LanternLauncher" "$launcher/Shared" "$tree/"
find "$tree" -type f \( -name '*.cs' -o -name '*.csproj' -o -name '*.axaml' -o -name '*.json' -o -name '*.manifest' \) \
    -exec sed -i 's/\r$//' {} +
rm -rf "$out"
dotnet publish "$tree/LanternLauncher/LanternLauncher.csproj" -c Release -r linux-x64 \
    -p:Version="$version" -p:InformationalVersion="$informational" -o "$out" --nologo
rm -f "$out"/*.dbg "$out"/*.pdb
chmod 755 "$out/Lantern Engine"
