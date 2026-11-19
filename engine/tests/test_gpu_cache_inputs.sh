#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
engine=$PWD
build=$(realpath "${1:-build.sh}")
required=$(realpath "${2:-tests/gpu_cache_required_inputs.txt}")
legacy_fixture_id=545d4cb7d29e5d31
functions=(gpu_cache_sources gpu_cache_inputs gpu_cache_versions gpu_cache_stream gpu_cache_id gpu_cache_legacy_id)
script=$(tr -d '\r' < "$build")
eval "$(sed -n -e '/^gpu_cache_[a-z_]*() {/,/^}/p' <<< "$script")"
for f in "${functions[@]}"; do
    declare -F "$f" > /dev/null || { echo "gpu-cache-inputs-test: $build has no $f" >&2; exit 1; }
done
failures=0
fail() {
    echo "gpu-cache-inputs-test: $1" >&2
    failures=$((failures + 1))
}
grep -qx 'gpu_cache_id > out/gpu-cache.id' <<< "$script" || fail "build.sh does not write gpu_cache_id to out/gpu-cache.id"
grep -qx 'gpu_cache_legacy_id > out/gpu-cache.legacy.id' <<< "$script" || fail "build.sh does not write gpu_cache_legacy_id to out/gpu-cache.legacy.id"

entries=$(tr -d '\r' < "$required")
drops=$(sed -n 's/^drop //p' <<< "$entries" | paste -sd'|' -)
excluded=$(sed -n 's/^!//p' <<< "$entries")
must=()
while IFS= read -r entry; do
    case $entry in
        ''|'!'*|'drop '*|'version '*) ;;
        */)
            files=$(find "${entry%/}" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' \) | LC_ALL=C sort)
            [[ -n $files ]] || fail "$entry holds no sources"
            while IFS= read -r file; do
                [[ -n $file ]] && ! grep -qxF -- "$file" <<< "$excluded" && must+=("$file")
            done <<< "$files" ;;
        *)
            [[ -f $entry ]] || fail "$entry does not exist"
            must+=("$entry") ;;
    esac
done <<< "$entries"

hashed=$(gpu_cache_inputs)
for file in "${must[@]}"; do
    grep -qxF -- "$file" <<< "$hashed" || fail "$file is not hashed"
done
while IFS= read -r file; do
    [[ -n $file ]] && grep -qxF -- "$file" <<< "$hashed" && fail "$file is hashed but must not be"
done <<< "$excluded"

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
gpu_cache_stream > "$scratch/stream"
missing=$(DROPS="$drops" awk '
    FNR == NR { seen[$0]; lines[++n] = $0; next }
    {
        sub(/\r$/, ""); sub(/[[:space:]]+$/, "")
        if ($0 == "" || $0 ~ /^[[:space:]]*\/\//) next
        if (ENVIRON["DROPS"] != "" && $0 ~ ENVIRON["DROPS"]) next
        if ($0 in seen) next
        for (i = 1; i <= n; ++i) if (index(lines[i], $0) == 1) next
        print FILENAME ": " $0; exit
    }' "$scratch/stream" "${must[@]}")
[[ -z $missing ]] || fail "a line of a required input does not reach the id: $missing"
while IFS= read -r name; do
    [[ -n $name ]] || continue
    grep -qE "^$name [0-9a-f]{40,64}$" "$scratch/stream" || fail "no $name version in the id"
done < <(sed -n 's/^version //p' <<< "$entries")
[[ $(gpu_cache_id) == "$(sha256sum < "$scratch/stream" | cut -c1-16)" ]] || fail "gpu_cache_id is not the sha256 of the whole gpu_cache_stream"
accepted_ids=$(tr -d '\r' < gpu-cache.accepted.id)
current_id=$(gpu_cache_id)
grep -qxF -- "$current_id" <<< "$accepted_ids" && fail "the current id $current_id is in gpu-cache.accepted.id"
grep -qvE '^[0-9a-f]{16}$' <<< "$accepted_ids" && fail "gpu-cache.accepted.id holds a line that is not an id"
if sirit_tree=$(git rev-parse HEAD:./gpu/third_party/sirit 2>/dev/null) && git diff --quiet HEAD -- gpu/third_party/sirit; then
    grep -qx "sirit $sirit_tree" "$scratch/stream" || fail "the sirit line is not the clean tree's id $sirit_tree"
fi

if (( failures == 0 )); then
    copy=$scratch/copy
    mkdir -p "$copy"
    { gpu_cache_sources; printf '%s\n' "${must[@]}"; sed -n 's/^!//p' <<< "$entries"; find gpu/third_party/sirit -type f; } |
        LC_ALL=C sort -u | while IFS= read -r file; do
            mkdir -p "$copy/$(dirname -- "$file")"
            cp -- "$file" "$copy/$file"
        done
    cd "$copy"
    base=$(gpu_cache_id)
    probe() {
        cp -- "$1" "$scratch/saved"
        printf x >> "$1"
        local id
        id=$(gpu_cache_id)
        cp -- "$scratch/saved" "$1"
        [[ $id != "$base" ]]
    }
    for file in "${must[@]}"; do
        probe "$file" || fail "one byte appended to $file leaves the id as it was"
    done
    while IFS= read -r file; do
        [[ -n $file ]] || continue
        probe "$file" && fail "one byte appended to excluded $file changes the id"
    done <<< "$excluded"
    sirit_file=$(find gpu/third_party/sirit/src -type f -name '*.cpp' | LC_ALL=C sort | sed -n 1p)
    probe "$sirit_file" || fail "one byte appended to $sirit_file leaves the id as it was"
    [[ $(gpu_cache_id) == "$base" ]] || fail "the id changed after the probes restored every file"
fi

fixture=$scratch/fixture
for file in gpu/shadps4/shader_recompiler/ir/value.cpp gpu/shadps4/shader_recompiler/native/capture.cpp \
    gpu/shadps4/shader_recompiler/notes.txt gpu/shadps4/video_core/amdgpu/liverpool.h gpu/shadps4/common/serdes.h \
    gpu/shadps4/common/hash.h gpu/shadps4/video_core/cache_storage.cpp gpu/shadps4/video_core/cache_storage.h \
    gpu/shadps4/video_core/renderer_vulkan/liverpool_to_vk.cpp gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp \
    gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp gpu/shadps4/video_core/renderer_vulkan/pipeline_cache_blob.h \
    gpu/shadps4/video_core/renderer_vulkan/vk_object_motion.h gpu/shadps4/video_core/renderer_vulkan/vk_effect_shaders.cpp \
    gpu/shadps4/video_core/renderer_vulkan/vk_pom_patch.h gpu/shadps4/video_core/renderer_vulkan/vk_ssr_composite_patch.h \
    gpu/shim/engine/pipeline_memo.h gpu/shim/core/emulator_settings.h gpu/shim/lifter/lift.cpp gpu/third_party/sirit/src/sirit.cpp; do
    mkdir -p "$fixture/$(dirname -- "$file")"
    printf '// %s\r\nint value = 1;  \r\n\r\n' "$file" > "$fixture/$file"
done
cd "$fixture"
legacy=$(gpu_cache_legacy_id)
[[ $legacy == "$legacy_fixture_id" ]] || fail "the legacy id of the fixture is $legacy, earlier engines give $legacy_fixture_id"
cd "$engine"

if (( failures )); then
    echo "gpu-cache-inputs-test: $failures failed" >&2
    exit 1
fi
echo "gpu-cache-inputs-test: $(wc -l <<< "$hashed") files hashed, ${#must[@]} required inputs each change the id, the legacy id is pinned"
