# Linux build environment of the Lantern Engine (packaging/linux/build.sh runs the build in it).
# Ubuntu 24.04: the package runs on glibc 2.39 and later (Ubuntu 24.04, Debian 13, Fedora 40,
# Arch, SteamOS 3.6, Bazzite). The libraries the Windows build takes from MSYS2 are built here at
# the same versions, static where they can be; SDL3 is the one shared library shipped
# (engine/out/gpu/libSDL3.so.0). Vulkan, X11 and OpenSSL 3 come from the player's system.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git xz-utils file patchelf \
        clang-20 lld-20 llvm-20 g++-14 cmake ninja-build pkg-config python3 cargo rustc nasm \
        glslang-tools libvulkan-dev libboost-dev libxxhash-dev libssl-dev zlib1g-dev \
        libexpat1-dev nlohmann-json3-dev libx11-dev \
        libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgles-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
        libdrm-dev libgbm-dev libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*
RUN for tool in clang clang++ lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-profdata llvm-symbolizer; do \
        ln -sf /usr/bin/$tool-20 /usr/local/bin/$tool; done
ENV CC=clang CXX=clang++ PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig:/opt/deps/share/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/deps
WORKDIR /deps
# One CMake project per library: static and position independent (linked into libbbgpu.so).
ARG CMAKE_DEPS="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/deps -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF"
RUN git clone -q --depth 1 -b v1.4.363 https://github.com/KhronosGroup/Vulkan-Headers.git \
    && cmake -S Vulkan-Headers -B Vulkan-Headers/b $CMAKE_DEPS && cmake --install Vulkan-Headers/b \
    && git clone -q --depth 1 -b v3.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git vma \
    && cmake -S vma -B vma/b $CMAKE_DEPS && cmake --install vma/b \
    && git clone -q --depth 1 -b 12.0.0 https://github.com/fmtlib/fmt.git \
    && cmake -S fmt -B fmt/b $CMAKE_DEPS -DFMT_TEST=OFF -DFMT_DOC=OFF && cmake --build fmt/b && cmake --install fmt/b \
    && git clone -q --depth 1 -b v0.9.7 https://github.com/Neargye/magic_enum.git \
    && cmake -S magic_enum -B magic_enum/b $CMAKE_DEPS -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF \
    && cmake --install magic_enum/b \
    && git clone -q --depth 1 -b v1.4.0 https://github.com/Tessil/robin-map.git \
    && cmake -S robin-map -B robin-map/b $CMAKE_DEPS && cmake --install robin-map/b \
    && git clone -q --depth 1 -b 3.1.0 https://github.com/richgel999/miniz.git \
    && cmake -S miniz -B miniz/b $CMAKE_DEPS -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF \
    && cmake --build miniz/b && cmake --install miniz/b \
    && git clone -q --depth 1 -b v4.1.1 --recursive https://github.com/zyantific/zydis.git \
    && cmake -S zydis -B zydis/b $CMAKE_DEPS -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF \
    && cmake --build zydis/b && cmake --install zydis/b \
    && rm -rf /deps/*
# Protobuf (bbnet's shadNet messages), with its Abseil.
RUN git clone -q --depth 1 -b v29.5 --recursive https://github.com/protocolbuffers/protobuf.git \
    && cmake -S protobuf -B protobuf/b $CMAKE_DEPS -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_ABSL_PROVIDER=module \
       -DABSL_PROPAGATE_CXX_STD=ON -Dprotobuf_INSTALL=ON -Dutf8_range_ENABLE_INSTALL=ON \
    && cmake --build protobuf/b && cmake --install protobuf/b && rm -rf /deps/*
# SDL3: shared (its video, audio and input backends are loaded at run time from the system).
RUN git clone -q --depth 1 -b release-3.4.18 https://github.com/libsdl-org/SDL.git sdl \
    && cmake -S sdl -B sdl/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/deps \
       -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build sdl/b && cmake --install sdl/b && rm -rf /deps/*
# FFmpeg: only what the game's movies need (H.264/HEVC video, AAC audio in MP4), no system
# libraries.
RUN git clone -q --depth 1 -b n7.1.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg && cd ffmpeg \
    && ./configure --prefix=/opt/deps --cc=clang --enable-pic --enable-static --disable-shared \
       --disable-autodetect --disable-programs --disable-doc --disable-network --disable-everything \
       --disable-avdevice --disable-avfilter --disable-postproc \
       --enable-decoder=h264,hevc,aac,aac_latm --enable-parser=h264,hevc,aac \
       --enable-demuxer=mov,mpegts,h264,hevc,aac --enable-protocol=file --enable-swscale --enable-swresample \
    && make -j"$(nproc)" && make install && cd / && rm -rf /deps/*
WORKDIR /build
