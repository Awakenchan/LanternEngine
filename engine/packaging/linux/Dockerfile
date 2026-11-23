# Linux build environment of the Lantern Engine (Ubuntu 24.04: the package runs on glibc 2.39 and
# later — Ubuntu 24.04, Debian 13, Fedora 40, Arch, SteamOS 3.6, Bazzite).
#
# The environment itself is setup-ubuntu.sh, so the image and a direct install (WSL, a VM, a CI
# runner) cannot drift apart: the libraries the Windows build takes from MSYS2 are built at the
# same versions into /opt/deps, static where they can be; SDL3 is the one shared library shipped.
#
#   docker build -t lantern-build engine/packaging/linux
#
# docker-build.sh builds this image and runs engine/build.sh (or the packaging) in it.
FROM ubuntu:24.04
COPY setup-ubuntu.sh /tmp/setup-ubuntu.sh
RUN BB_DEPS_WORK=/var/tmp/lantern-deps BB_DEPS_LOG=1 bash /tmp/setup-ubuntu.sh \
    && rm -rf /var/tmp/lantern-deps /var/lib/apt/lists/* /tmp/setup-ubuntu.sh
ENV CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld \
    PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig:/opt/deps/share/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/deps
WORKDIR /src/engine
