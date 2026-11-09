#!/usr/bin/env bash
# Linux build environment of the Lantern Engine on Ubuntu 24.04 (WSL, a VM, a CI runner): the same
# packages and libraries as packaging/linux/Dockerfile, installed directly. Run as root once;
# packaging/linux/build.sh then builds and packages the engine.
#
# The libraries the Windows build takes from MSYS2 are built at the same versions into /opt/deps,
# static and position independent where they can be (linked into libbbgpu.so); SDL3 is the one
# shared library shipped. Vulkan, X11/Wayland and OpenSSL 3 come from the player's system.
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'Run as root (sudo).' >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
jobs=${BB_JOBS:-$(nproc)}

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates curl git xz-utils file patchelf rsync \
    clang-20 lld-20 llvm-20 g++-14 cmake ninja-build make pkg-config python3 cargo rustc nasm \
    glslang-tools libvulkan-dev libboost-dev libxxhash-dev libssl-dev zlib1g-dev libopus-dev \
    libexpat1-dev nlohmann-json3-dev libx11-dev \
    libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgles-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
    libdrm-dev libgbm-dev libusb-1.0-0-dev >/dev/null
for tool in clang clang++ lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-profdata llvm-symbolizer; do
    ln -sf "/usr/bin/$tool-20" "/usr/local/bin/$tool"
done

export CC=clang CXX=clang++ PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig:/opt/deps/share/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/deps
deps=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/deps
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF)
work=${BB_DEPS_WORK:-/var/tmp/lantern-deps}
mkdir -p "$work" /opt/deps
cd "$work"

# One stamp per library: a rerun skips what is installed.
built() { [[ -f /opt/deps/.stamp-$1 ]]; }
stamp() { touch "/opt/deps/.stamp-$1"; }
fetch() { # name tag url [--recursive]
    rm -rf "$1"
    git clone -q --depth 1 -b "$2" ${4:-} "$3" "$1"
}
cmake_dep() { # name tag url [cmake args...]
    local name=$1 tag=$2 url=$3
    shift 3
    built "$name" && return
    echo "deps: $name $tag"
    fetch "$name" "$tag" "$url" --recursive
    cmake -S "$name" -B "$name/b" "${deps[@]}" "$@" >/dev/null
    cmake --build "$name/b" -j "$jobs" >/dev/null
    cmake --install "$name/b" >/dev/null
    rm -rf "$name"
    stamp "$name"
}

cmake_dep vulkan-headers v1.4.363 https://github.com/KhronosGroup/Vulkan-Headers.git
cmake_dep vma v3.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
cmake_dep fmt 12.0.0 https://github.com/fmtlib/fmt.git -DFMT_TEST=OFF -DFMT_DOC=OFF
cmake_dep magic_enum v0.9.7 https://github.com/Neargye/magic_enum.git \
    -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
cmake_dep robin-map v1.4.0 https://github.com/Tessil/robin-map.git
cmake_dep miniz 3.1.0 https://github.com/richgel999/miniz.git \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF
# Zydis with its Zycore installed beside it (zydis-config.cmake looks for Zycore's own config,
# which the bundled copy does not install; Zydis itself builds with the bundled copy).
# Build output goes to /dev/null: a failing step stops the script (set -e) without its errors,
# so rerun that step by hand to see them.
if ! built zydis-zycore; then
    echo 'deps: zydis v4.1.1 + zycore'
    fetch zydis v4.1.1 https://github.com/zyantific/zydis.git --recursive
    cmake -S zydis/dependencies/zycore -B zydis/zb "${deps[@]}" -DZYCORE_BUILD_TESTS=OFF \
        -DZYCORE_BUILD_EXAMPLES=OFF >/dev/null
    cmake --build zydis/zb -j "$jobs" >/dev/null
    cmake --install zydis/zb >/dev/null
    cmake -S zydis -B zydis/b "${deps[@]}" -DZYDIS_BUILD_TOOLS=OFF \
        -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF >/dev/null
    cmake --build zydis/b -j "$jobs" >/dev/null
    cmake --install zydis/b >/dev/null
    rm -rf zydis
    stamp zydis-zycore
fi
# miniupnpc (bbnet's port mapping), static as on Windows: its CMake project is a subfolder.
if ! built miniupnpc; then
    echo 'deps: miniupnpc 2.2.8'
    fetch miniupnp miniupnpc_2_2_8 https://github.com/miniupnp/miniupnp.git
    cmake -S miniupnp/miniupnpc -B miniupnp/b "${deps[@]}" -DUPNPC_BUILD_STATIC=ON \
        -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF >/dev/null
    cmake --build miniupnp/b -j "$jobs" >/dev/null
    cmake --install miniupnp/b >/dev/null
    rm -rf miniupnp
    stamp miniupnpc
fi
# Protobuf (bbnet's shadNet messages), with its Abseil.
cmake_dep protobuf v29.5 https://github.com/protocolbuffers/protobuf.git \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_ABSL_PROVIDER=module -DABSL_PROPAGATE_CXX_STD=ON \
    -Dprotobuf_INSTALL=ON -Dutf8_range_ENABLE_INSTALL=ON
# SDL3: shared (its video, audio and input backends are loaded at run time from the system).
if ! built sdl3; then
    echo 'deps: SDL3 release-3.4.18'
    fetch sdl release-3.4.18 https://github.com/libsdl-org/SDL.git
    cmake -S sdl -B sdl/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/deps \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
    cmake --build sdl/b -j "$jobs" >/dev/null
    cmake --install sdl/b >/dev/null
    rm -rf sdl
    stamp sdl3
fi
# FFmpeg: only what the game's movies need (H.264/HEVC video, AAC audio in MP4), no system
# libraries.
if ! built ffmpeg; then
    echo 'deps: FFmpeg n7.1.1'
    fetch ffmpeg n7.1.1 https://github.com/FFmpeg/FFmpeg.git
    (cd ffmpeg && ./configure --prefix=/opt/deps --cc=clang --enable-pic --enable-static --disable-shared \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-everything \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --enable-decoder=h264,hevc,aac,aac_latm --enable-parser=h264,hevc,aac \
        --enable-demuxer=mov,mpegts,h264,hevc,aac --enable-protocol=file --enable-swscale \
        --enable-swresample >/dev/null && make -j"$jobs" >/dev/null && make install >/dev/null)
    rm -rf ffmpeg
    stamp ffmpeg
fi
echo "Linux build environment ready (/opt/deps)"
