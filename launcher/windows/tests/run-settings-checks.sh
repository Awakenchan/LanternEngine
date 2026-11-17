#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
launcher=$(cd "$(dirname "$0")/.." && pwd)
writer=${1:-$launcher/../engine/out/settings-writes-test}
cycles=${SETTINGS_STRESS_CYCLES:-300}
if [[ -d /c/msys64/clang64/bin ]]; then PATH="/c/msys64/clang64/bin:$PATH"; fi
if [[ -f $writer.exe ]]; then writer=$writer.exe; fi
if ! command -v dotnet >/dev/null 2>&1; then
    echo "SKIPPED: launcher settings checks need dotnet (.NET 8 SDK)"
    exit 0
fi

timeout 300 dotnet build "$launcher/tests/SettingsChecks" --nologo -v q
timeout 300 dotnet run --no-build --project "$launcher/tests/SettingsChecks"
if [[ -x $writer ]]; then
    timeout 300 dotnet run --no-build --project "$launcher/tests/SettingsChecks" -- --ini-stress "$writer" "$cycles"
else
    echo "SKIPPED: ini-stress (no engine writer at $writer; engine/build.sh --test builds it)"
fi
