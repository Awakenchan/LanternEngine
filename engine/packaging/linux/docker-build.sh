#!/usr/bin/env bash
# Builds the Lantern Engine on Linux in the Ubuntu 24.04 environment of the Dockerfile beside this
# script, on any distribution with Docker or Podman: nothing but the container tool is installed
# on the host.
#
#   engine/packaging/linux/docker-build.sh [build.sh options]       engine/build.sh (out/ as usual)
#   engine/packaging/linux/docker-build.sh --package <version> ...  packaging/linux/build.sh
#   engine/packaging/linux/docker-build.sh --shell                  a shell in the environment
#
# --package needs what the repository does not hold: the package template (lantern.ini, FSR 4
# shaders, ...) inside the repository, as its second argument relative to engine/ (the container
# sees only the repository), BB_ENGINE_ID, and a published launcher folder for its third.
#
# The repository is mounted at /src and the build runs as the calling user, so engine/out is
# owned by them; Cargo's downloads are kept in engine/out/docker-home between runs. BB_* variables
# (BB_GATE, BB_LTO, BB_PGO, BB_JOBS, BB_MARCH, ...) are passed through.
# LANTERN_DOCKER: the container tool (docker by default, podman when there is no docker).
# LANTERN_IMAGE: the image's name (lantern-build). LANTERN_SKIP_IMAGE=1: use the image as it is.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
tool=${LANTERN_DOCKER:-$(command -v docker || command -v podman || true)}
[[ -n $tool ]] || { echo 'Install Docker or Podman (or set LANTERN_DOCKER).' >&2; exit 1; }
image=${LANTERN_IMAGE:-lantern-build}

if [[ ${LANTERN_SKIP_IMAGE:-} != 1 ]]; then
    # Cached layer by layer: a rebuild only happens when setup-ubuntu.sh or the Dockerfile change.
    "$tool" build -t "$image" "$here"
fi

case ${1:-} in
    --package) shift; command=(bash packaging/linux/build.sh "$@") ;;
    --shell) shift; command=(bash "$@") ;;
    *) command=(bash build.sh "$@") ;;
esac

home=/src/engine/out/docker-home
mkdir -p "$repo/engine/out/docker-home"
# label=disable: on SELinux hosts (Fedora, Bazzite) the repository is readable in the container
# without relabelling it (:z would relabel the user's whole checkout).
run=(run --rm --security-opt label=disable -v "$repo:/src" -w /src/engine -e HOME="$home"
     -e CARGO_HOME="$home/.cargo")
# No display or sound server in the container: the tests that start SDL use its dummy drivers.
run+=(-e SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-dummy}" -e SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}"
      -e XDG_RUNTIME_DIR=/tmp)
# The calling user's files stay theirs: rootful Docker or Podman run as their ids; rootless Podman
# keeps their id inside (keep-id); rootless Docker's root already is them outside. "docker" may be
# Podman's (podman-docker).
if "$tool" --version 2>/dev/null | grep -qi podman; then
    if [[ $("$tool" info --format '{{.Host.Security.Rootless}}' 2>/dev/null) == true ]]; then
        run+=(--userns=keep-id)
    else
        run+=(-u "$(id -u):$(id -g)")
    fi
elif ! "$tool" info --format '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    run+=(-u "$(id -u):$(id -g)")
fi
[[ ! -t 0 || ! -t 1 ]] || run+=(-it)
while IFS='=' read -r name _; do
    run+=(-e "$name")
done < <(env | grep -E '^BB_[A-Za-z0-9_]*=' || true)
exec "$tool" "${run[@]}" "$image" "${command[@]}"
