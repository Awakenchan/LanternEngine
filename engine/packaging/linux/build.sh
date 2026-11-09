#!/usr/bin/env bash
# Linux package of the Lantern Engine: builds the engine (engine/build.sh) in the Ubuntu 24.04
# environment of setup-ubuntu.sh (or the Dockerfile) and writes
# engine/out/package/LanternEngine-v<version>-linux-x64.tar.gz.
#
#   packaging/linux/build.sh <version> [package-template folder] [launcher folder]
#
# The package mirrors the Windows one: the launcher ("Lantern Engine", the Avalonia launcher
# published for linux-x64, when its folder is given), lantern.sh (starts the game from a terminal),
# lantern.ini (shipped settings, also kept as engine/lantern.default.ini), and engine/ with run.sh,
# its Python start scripts, the patches and FSR 4 shaders, and engine/out with bb-start, bb-probe,
# its GPU library and SDL3. The player's system gives Vulkan, X11 or Wayland, Python 3 and
# OpenSSL 3; saves, mods and logs stay next to lantern.sh.
set -euo pipefail
engine=$(cd -- "$(dirname -- "$0")/../.." && pwd)
version=${1:?usage: packaging/linux/build.sh <version> [package-template folder] [launcher folder]}
template=$(cd -- "${2:-$engine/../package-template}" && pwd)
launcher=${3:-}
cd "$engine"
export CC=${CC:-clang} CXX=${CXX:-clang++}
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-/opt/deps/lib/pkgconfig:/opt/deps/share/pkgconfig}
export CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:-/opt/deps}
export LDFLAGS=${LDFLAGS:--fuse-ld=lld}
# The profiles in pgo/ come from Windows runs: built without one.
BB_PGO=${BB_PGO:-off} bash build.sh

root=out/package/LanternEngine
bin=$root/engine/out
rm -rf out/package
mkdir -p "$bin/gpu"
install -m755 out/bb-probe out/bb-gpu-capabilities out/bb-isa-scan packaging/linux/bb-start "$bin/"
for tool in bb-engine-assets bb-ui-4k bb-shader-lab; do
    [[ ! -f out/$tool ]] || install -m755 "out/$tool" "$bin/"
done
install -m755 out/gpu/libbbgpu.so "$bin/gpu/"
# SDL3 (the one shared library built for the package) beside the GPU library: bb-probe finds both
# through its $ORIGIN/gpu RUNPATH, the GPU library through its own $ORIGIN. bb-probe itself is not
# patched (patchelf breaks the non-PIE binary's symbol versions).
sdl=$(pkg-config --variable=libdir sdl3)/libSDL3.so.0
install -m755 "$(readlink -f "$sdl")" "$bin/gpu/libSDL3.so.0"
patchelf --set-rpath '$ORIGIN' "$bin/gpu/libbbgpu.so"
patchelf --set-rpath '$ORIGIN/gpu' "$bin/bb-gpu-capabilities"
strip --strip-debug "$bin/gpu/libbbgpu.so" "$bin/gpu/libSDL3.so.0"
cp out/gpu-cache.id out/gpu-cache.legacy.id "$bin/" 2>/dev/null || true
# NVIDIA's DLSS runtime (DLSS SDK license, tools/fetch_dlss.sh) beside bb-probe, where the engine
# looks for it, as the Windows package has nvngx_dlss.dll. Kept in out/ once downloaded.
bash tools/fetch_dlss.sh out
install -m644 out/libnvidia-ngx-dlss.so.* out/LICENSE-DLSS-SDK.txt "$bin/"

cp run.sh "$root/engine/"
cp -r scripts patches data licenses "$root/engine/"
for item in fsr4_shaders shader-catalog gamecontrollerdb.txt lamp2lamp.bin lamp2lamp-bosses.bin debug-font; do
    [[ ! -e $template/engine/$item ]] || cp -r "$template/engine/$item" "$root/engine/"
done
cp "$template/lantern.ini" "$root/engine/lantern.default.ini"
cp "$template/lantern.ini" "$template/LICENSE" "$template/NOTICE" "$root/"
install -m755 packaging/linux/lantern.sh "$root/lantern.sh"
cp packaging/linux/README-linux.txt "$root/README.txt"
if [[ -n $launcher ]]; then
    cp -r "$launcher"/. "$root/"
fi
find "$root" -name __pycache__ -prune -exec rm -rf {} +
# Files copied from a Windows drive come executable: only the programs and scripts are.
find "$root" -type d -exec chmod 755 {} +
find "$root" -type f -exec chmod 644 {} +
chmod 755 "$root/lantern.sh" "$root/engine/run.sh" "$bin"/bb-* "$bin"/gpu/*.so* "$root"/engine/scripts/*.py
[[ ! -f "$root/Lantern Engine" ]] || chmod 755 "$root/Lantern Engine"
# Files checked out on Windows: the scripts need Unix line ends.
find "$root" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.ini' -o -name '*.txt' -o -name '*.json' \
    -o -name bb-start \) -exec sed -i 's/\r$//' {} +
# The engine id is the Windows launcher's (Updater.EngineId), as build.ps1 writes it; BB_ENGINE_ID
# when the launcher's source is not beside the engine.
engine_id=${BB_ENGINE_ID:-$(sed -n 's/.*EngineId = "\([^"]*\)".*/\1/p' "$engine/../launcher/BloodborneLauncher/Core/Updater.cs" 2>/dev/null || true)}
printf '{ "version": "%s", "engine": "%s", "platform": "linux-x64" }\n' "$version" "${engine_id:-unknown}" \
    > "$root/package.json"

archive=out/package/LanternEngine-v$version-linux-x64.tar.gz
tar -C out/package -czf "$archive" LanternEngine
echo "Package: $engine/$archive ($(du -h "$archive" | cut -f1))"
