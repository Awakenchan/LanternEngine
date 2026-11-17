#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
census=gpu/shim/engine/gx_census.h
table=tests/gx_readers.txt
fail() {
    echo "check-gx-readers: FAIL $1" >&2
    exit 1
}
[[ -f $census && -f $table ]] || fail "missing $census or $table"
fields=$(tr -d '\r' < "$census" | sed -n 's/^#define LANTERN_GX_READER_FIELDS(X) //p' | grep -oE 'X\([a-z_0-9]+\)' | sed -E 's/X\((.*)\)/\1/' | paste -sd'|' -)
[[ -n $fields ]] || fail "no LANTERN_GX_READER_FIELDS line in $census"
export GX_READER_PATTERN="(regs[.]|Regs[(][)][.])(${fields}|IsClipDisabled[(])"
scan() {
    local f
    while IFS= read -r f; do
        tr -d '\r' < "$f" | awk -v F="$f" '
            /^[A-Za-z].*\(/ && !/;[[:space:]]*$/ && !/^(if|for|while|return|else)[^A-Za-z_]/ {
                name = $0
                sub(/\(.*/, "", name)
                n = split(name, parts, /[[:space:]*&]+/)
                fn = parts[n]
            }
            $0 ~ ENVIRON["GX_READER_PATTERN"] { print F "|" fn }' || return 1
    done < <(find gpu/shadps4/video_core/renderer_vulkan gpu/shadps4/video_core/amdgpu gpu/shim/engine -type f -name '*.cpp' | LC_ALL=C sort)
}
scanned=$(scan) || fail "scan failed"
found=$(LC_ALL=C sort -u <<< "$scanned" | grep . || true)
listed=$(tr -d '\r' < "$table" | grep -vE '^[[:space:]]*$' | cut -d'|' -f1,2 | LC_ALL=C sort -u || true)
bad=0
while IFS= read -r line; do
    [[ -z $line ]] && continue
    grep -qxF "$line" <<< "$listed" || { echo "check-gx-readers: FAIL unlisted reader $line" >&2; bad=1; }
done <<< "$found"
while IFS= read -r line; do
    [[ -z $line ]] && continue
    grep -qxF "$line" <<< "$found" || { echo "check-gx-readers: FAIL listed reader not found $line" >&2; bad=1; }
done <<< "$listed"
while IFS='|' read -r file fn fix rest; do
    [[ -z $file ]] && continue
    case $fix in record|carried|fallback|off|none) ;; *) echo "check-gx-readers: FAIL $file|$fn has fix '$fix'" >&2; bad=1 ;; esac
done < <(tr -d '\r' < "$table")
[[ $bad == 0 ]] || exit 1
echo "check-gx-readers: ok, $(grep -c . <<< "$found") readers listed"
