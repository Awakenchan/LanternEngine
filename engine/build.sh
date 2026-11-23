#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
case ${BB_GATE:-} in
    '') ;;
    fast) BB_LTO=OFF BB_PGO=off BB_CCACHE=1; export CCACHE_NOHASHDIR=${CCACHE_NOHASHDIR:-1} ;;
    full) BB_LTO=ON ;;
    *) echo "BB_GATE is fast or full, not $BB_GATE" >&2; exit 1 ;;
esac
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
if ! command -v cargo >/dev/null 2>&1; then
    echo 'Install the Rust toolchain (cargo) to build the MOVBE/BMI1/LZCNT scanner.' >&2
    exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# Windows: MSYS2 CLANG64 shell (README "Windows": clang, libc++ and lld, whose threads and locks
# are the Win32 ones). The GPU library is static there (a DLL cannot leave the runtime_* symbols
# to the executable), the runtime adds its Win32 files (src/win32_*.c), runs without pthreads,
# and the executable keeps a console and bottom-up addresses (guest-visible host memory must
# stay below 1 TiB).
windows=
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
# GNU C11: glibc's POSIX declarations (pthread_rwlock_t in src/host_sync.h, which every runtime
# file includes) are hidden by strict -std=c11 in the files that do not define _GNU_SOURCE.
cstd=(-std=gnu11)
threads=(-pthread)
if [[ -n $windows ]]; then
    threads=()
    libraries=("${libraries[@]/-mwindows/-mconsole}")
    cstd=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00)
fi
cargo_home=${CARGO_HOME:-${USERPROFILE:-$HOME}/.cargo}
cargo_src=$PWD
if [[ -n $windows ]]; then cargo_home=$(cygpath -w "$cargo_home"); cargo_src=$(cygpath -w "$cargo_src"); fi
cargo_flags=(${RUSTFLAGS:-} "--remap-path-prefix=$cargo_home=cargo" "--remap-path-prefix=$cargo_src=engine")
CARGO_ENCODED_RUSTFLAGS=$(IFS=$'\x1f'; printf '%s' "${cargo_flags[*]}") cargo build --manifest-path tools/isa-scan/Cargo.toml --release
if [[ -n $windows ]]; then
    install -m755 tools/isa-scan/target/release/bb-isa-scan.exe out/bb-isa-scan.exe
else
    install -m755 tools/isa-scan/target/release/bb-isa-scan out/bb-isa-scan
fi
for scanner in out/bb-isa-scan out/bb-isa-scan.exe; do
    if [[ -f $scanner ]] && grep -qaF "$cargo_home" "$scanner"; then echo "$scanner still names the cargo home ($cargo_home)" >&2; exit 1; fi
done
# BB_JOBS=N: at most N compile and link-time optimization jobs (less memory, e.g. while the
# game runs).
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
# Clang (Windows CLANG64) writes pgo/default_*.profraw: merged here into pgo/default.profdata,
# which the "use" build reads (GCC: pgo/*.gcda, read as they are).
mkdir -p pgo
if [[ ${BB_PGO:-} != generate && -n $(find pgo -name '*.profraw' -print -quit 2>/dev/null) &&
      ( ! -f pgo/default.profdata || -n $(find pgo -name '*.profraw' -newer pgo/default.profdata -print -quit) ) ]]; then
    profdata=$(command -v llvm-profdata || true)
    if [[ -n $profdata ]]; then
        "$profdata" merge -o pgo/default.profdata pgo/*.profraw
        echo "PGO: merged $(find pgo -name '*.profraw' | wc -l) raw profile(s) into pgo/default.profdata"
    else
        echo 'PGO: pgo/*.profraw found but no llvm-profdata in PATH; not merged' >&2
    fi
fi
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo \( -name '*.gcda' -o -name 'default.profdata' \) -print -quit 2>/dev/null) ]]; then
        pgo=use
    else
        pgo=off
    fi
fi
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# A plain source copy (no submodule checkout) already has them applied.
if [[ -e gpu/third_party/fsr-vulkan/.git ]]; then
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
fi
# Line tables only (not full debug info): crash offsets still resolve to function and
# file:line with llvm-symbolizer, and compiling and linking take much less time.
gpu_config=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" -DBB_LTO="${BB_LTO:-ON}"
            -DBB_PGO_DIR="$PWD/pgo" -DBB_MARCH="${BB_MARCH:-}"
            "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -gline-tables-only -DNDEBUG"
            "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -gline-tables-only -DNDEBUG")
launcher=
if [[ ${BB_CCACHE:-0} == 1 ]] && launcher=$(command -v ccache); then
    export CCACHE_DIR=${CCACHE_DIR:-E:/Temp/ccache} CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-10G} CCACHE_BASEDIR=${CCACHE_BASEDIR:-$(cd .. && (pwd -W 2>/dev/null || pwd))}
    echo "GPU library: ccache on ($CCACHE_DIR, cap $CCACHE_MAXSIZE)"
fi
gpu_config+=("-DCMAKE_C_COMPILER_LAUNCHER=$launcher" "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher")
# Configured once per set of options (ninja itself reruns CMake when a CMakeLists.txt changes).
if [[ ! -f out/gpu/build.ninja || $(cat out/gpu/bb-configure.txt 2>/dev/null) != "${gpu_config[*]}" ]]; then
    cmake -S gpu -B out/gpu "${gpu_config[@]}" >/dev/null
    printf '%s' "${gpu_config[*]}" > out/gpu/bb-configure.txt
fi
if build_commit=$(git rev-parse --short=10 HEAD 2>/dev/null); then
    git diff --quiet HEAD -- . 2>/dev/null || build_commit+="+changes"
else
    build_commit=unknown
fi
build_stamp="#define BB_BUILD_COMMIT \"$build_commit\""
if [[ $(cat out/gpu/bb_build_commit.h 2>/dev/null) != "$build_stamp" ]]; then
    printf '%s\n' "$build_stamp" > out/gpu/bb_build_commit.h
fi
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}, march ${BB_MARCH:-default}, build $build_commit"
slot_reference_gate() {
    local found
    found=$(grep -rnE --include='*.cpp' --include='*.h' --include='*.inc' \
        '((static|thread_local)[^(]*ThreadSlot *[&*])|(ThreadSlot *[&*] *[A-Za-z_][A-Za-z0-9_]* *(= *nullptr *)?(\{ *(nullptr)? *\})? *;)' "$1" |
        grep -vE '/lantern_steps\.(h|cpp|inc):' || true)
    if [[ -n $found ]]; then
        printf 'A ThreadSlot reference or pointer outside a local or a parameter:\n%s\n' "$found" >&2
        return 1
    fi
}
emulated_tls_gate() {
    if ! command -v llvm-nm > /dev/null; then
        echo 'Emulated TLS gate: llvm-nm not found, the gate cannot run' >&2
        return 1
    fi
    if llvm-nm "$@" 2>/dev/null | grep -E '__emutls_v\..*t_slot' >&2; then
        echo 'Emulated TLS for the step registry slot (t_slot): build the shim with -fno-emulated-tls' >&2
        return 1
    fi
}
switch_table_census() {
    local table=gpu/shim/lantern_switches.inc names missing frame_readers batch_names bad_batch
    names=$(grep -rhoE --include='*.c' --include='*.cpp' --include='*.h' '"(BB|LANTERN)_[A-Z0-9_]*[A-Z0-9]"' "$@" | tr -d '"' | LC_ALL=C sort -u)
    missing=$(LC_ALL=C comm -23 <(printf '%s\n' "$names") <(grep -oE '"(BB|LANTERN)_[A-Z0-9_]+"' "$table" | tr -d '"' | LC_ALL=C sort -u))
    if [[ -n $missing ]]; then
        printf 'Switch names read in the tree with no row in %s:\n%s\n' "$table" "$missing" >&2
        return 1
    fi
    frame_readers=$(grep -rnF --include='*.c' --include='*.cpp' --include='*.h' '"BB_FRAME_STATS"' "$@" | grep -v '/lantern_switches\.cpp:' || true)
    if [[ -n $frame_readers ]]; then
        printf 'BB_FRAME_STATS read outside the switch table:\n%s\n' "$frame_readers" >&2
        return 1
    fi
    batch_names=$(grep -rhoE --include='*.cpp' --include='*.h' 'Batch::Switch\("[A-Z0-9_]+"\)' "$@" | sed -E 's/.*\("([A-Z0-9_]+)"\)/\1/' | LC_ALL=C sort -u)
    bad_batch=$(for name in $batch_names; do grep -qE "^X\([a-z0-9_]+, \"$name\", Mode, .*, Step, " "$table" || echo "$name"; done)
    if [[ -n $bad_batch ]]; then
        printf 'Batch::Switch names that are not Mode step rows:\n%s\n' "$bad_batch" >&2
        return 1
    fi
}
no_dialog_gate() {
    local found
    found=$(grep -nE '("\$\{CXX:-clang\+\+\}"|"\$CC") .*(tests|tools)/[^ ]+\.(c|cpp)' "$1" | grep -vF '"${no_dialog[@]}"' || true)
    if [[ -n $found ]]; then
        printf 'Test or tool compiles without tests/no_dialog.h in %s:\n%s\n' "$1" "$found" >&2
        return 1
    fi
    found=$(awk '/^build / { object = ($2 ~ /(tests|tools)\/[^\/:]+\.(c|cpp)\.obj:$/) ? $2 : "" }
                 object != "" && /^  FLAGS = / && !/no_dialog\.h/ { print object }' "$2")
    if [[ -n $found ]]; then
        printf 'CMake test or tool objects without tests/no_dialog.h in %s:\n%s\n' "$2" "$found" >&2
        return 1
    fi
}
slot_reference_gate gpu || exit 1
switch_table_census src start gpu tools || exit 1
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
lab_target=
if [[ ${BB_SHADER_LAB:-} == 1 || ! -f out/gpu/bb-shader-lab.exe && ! -f out/gpu/bb-shader-lab ]]; then lab_target=bb-shader-lab; fi
if ! ninja -C out/gpu ${BB_JOBS:+-j "$BB_JOBS"} bbgpu bb-engine-assets bb-ui-4k $lab_target > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
cp -f out/gpu/bb-engine-assets* out/gpu/bb-ui-4k* out/
gpu_library=out/gpu/libbbgpu.so
[[ -n $windows ]] && gpu_library=out/gpu/libbbgpu.a
emulated_tls_gate "$gpu_library" || exit 1
# The cache recompile after a compiler change (start/main.cpp RecompileGpuCache).
cp -f out/gpu/bb-shader-lab* out/ 2>/dev/null || true
# The GPU cache's identity (start/main.cpp ResetStaleGpuCache): a hash of the sources that decide
# what the shader and pipeline caches hold. An engine update that leaves them alone keeps the
# players' compiled shaders; one that changes them starts the caches again (GitHub issue 30).
# Whole-line comments, blank lines, trailing spaces and line endings do not count.
gpu_cache_sources() {
    find gpu/shadps4/shader_recompiler gpu/shadps4/video_core/amdgpu gpu/shadps4/common/serdes.h \
        gpu/shadps4/video_core/cache_storage.cpp gpu/shadps4/video_core/cache_storage.h \
        gpu/shadps4/video_core/renderer_vulkan/liverpool_to_vk.* gpu/shadps4/video_core/renderer_vulkan/vk_*pipeline* \
        gpu/shadps4/video_core/renderer_vulkan/pipeline_cache_blob.h gpu/shadps4/video_core/renderer_vulkan/vk_object_motion.* \
        -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' \) | LC_ALL=C sort
}
# The shader capture (shader_recompiler/native/capture.*, BB_SHADER_CAPTURE) and the shader lab's
# override (Info::lab_flat) store nothing in the caches: their files and one-line hooks are left out.
gpu_cache_inputs() {
    { gpu_cache_sources
      find gpu/shadps4/common/hash.h gpu/shim/engine/pipeline_memo.h gpu/shim/engine/cache_settings.h \
          gpu/shim/core/emulator_settings.h gpu/shim/lifter gpu/shadps4/video_core/renderer_vulkan/vk_effect_shaders.* \
          gpu/shadps4/video_core/renderer_vulkan/vk_pom_patch.h gpu/shadps4/video_core/renderer_vulkan/vk_ssr_composite_patch.h -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' \)
    } | grep -v -E 'vk_compute_pipeline\.cpp$|vk_pipeline_common\.(cpp|h)$|pipeline_compile_queue\.h$|native/capture\.(cpp|h)$' |
        LC_ALL=C sort
}
gpu_cache_versions() {
    local sirit=gpu/third_party/sirit tree
    if tree=$(git rev-parse "HEAD:./$sirit" 2>/dev/null) && git diff --quiet HEAD -- "$sirit" 2>/dev/null; then
        echo "sirit $tree"
    else
        echo "sirit $(find "$sirit" -type f | LC_ALL=C sort | xargs cat | tr -d '\r' | sha256sum | cut -c1-64)"
    fi
}
gpu_cache_stream() {
    { gpu_cache_inputs | xargs cat; gpu_cache_versions; } | tr -d '\r' |
        grep -v -E 'Shader::Native::Capture|shader_recompiler/native/capture\.h|lab_flat|fetch_override|GxWholeCensus|engine/gx_whole_census|GxWhole::Owed|owed_check|on_mark|GxWholeKey|Lantern::Switches::(Get|Load)\(\)\.frame_stats|"lantern_switches\.h"' |
        sed -E 's#^[[:space:]]*//.*$##; s/[[:space:]]+$//' | grep -v '^$'
}
gpu_cache_id() {
    gpu_cache_stream | sha256sum | cut -c1-16
}
# The identity as earlier engines computed it (whole files): a cache stamped with it for these same
# sources is kept and stamped again with the new one.
gpu_cache_legacy_id() {
    gpu_cache_sources | xargs cat | sha256sum | cut -c1-16
}
gpu_cache_id > out/gpu-cache.id
gpu_cache_legacy_id > out/gpu-cache.legacy.id
cp -f gpu-cache.accepted.id out/gpu-cache.accepted.id
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/host.c src/wait_stats.c src/blocked.c src/sys/*.c)
sys_tests=(tests/sys_test_host.c src/sys/*.c)
no_dialog=(-include tests/no_dialog.h)
link=(-no-pie)
if [[ -n $windows ]]; then
    # out/gpu/bbgpu_link.txt (gpu/CMakeLists.txt): libbbgpu.a and its dependencies, one per line.
    mapfile -t gpu < out/gpu/bbgpu_link.txt
    runtime+=(src/win32_*.c)
    sys_tests+=(src/win32_*.c)
    # The static library's link-time optimization (ThinLTO in lld) runs here; on Linux it runs in
    # the .so link.
    # The ThinLTO cache keeps each module's optimized code: a rebuild after a small change
    # optimizes only the modules it touched (and what inlines from them), on every thread.
    link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin
          -Wl,--thinlto-cache-dir="$PWD/out/thinlto-cache" -Wl,--thinlto-jobs="${BB_JOBS:-all}"
          -Wl,--thinlto-cache-policy=cache_size_bytes=6g:prune_after=1209600s)
    # Windows names the executables by this (firewall prompts, Task Manager): Lantern Engine.
    llvm-windres src/lantern.rc -O coff -o out/lantern.res
    link+=(out/lantern.res)
fi
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# Windows: the link includes the GPU library's link-time optimization (a minute or more), and
# run.bat builds before every start, so an up-to-date executable is kept.
gpu_inputs=(out/gpu/libbbgpu.a out/gpu/bbgpu_link.txt)
if [[ -n $windows ]]; then
    # Native bridge dependencies can change without changing libbbgpu.a itself.
    for dependency in "${gpu[@]}"; do
        [[ ! -f $dependency ]] || gpu_inputs+=("$dependency")
    done
fi
if [[ -n $windows && -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe && -f out/isa_emulate.o &&
      -z $(find src gpu/bbgpu.h "${gpu_inputs[@]}" out/libatrac9.a build.sh \
               tools/gpu_capabilities.c -newer out/bb-probe.exe -print -quit) ]]; then
    echo "Up to date: $PWD/out/bb-probe.exe"
else
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" -mgeneral-regs-only "${includes[@]}" -I. -Isrc -c src/isa_emulate.c -o out/isa_emulate.o
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc src/probe.c src/isa_rewrite.c out/isa_emulate.o "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# GPU and controller inventory; keep the Windows C dialect and include SDL headers.
"$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
fi
# The native start (Windows): boot image, mods, patches, engine assets, then bb-probe. One static
# program: it runs before bb-probe's DLLs are found, and replaces the Python scripts.
start_stale() {
    [[ ! -f out/bb-start.exe ]] && return 0
    [[ -n $(find start src/lantern.rc -newer out/bb-start.exe -print -quit) ]] && return 0
    local depfiles=(out/start-obj/start/*.cpp.d) dep
    [[ -f ${depfiles[0]} ]] || return 0
    for dep in $(sed -e 's/^[^ ]*\.o: //' -e 's/\\\r\?$//' "${depfiles[@]}"); do
        [[ -f $dep && $dep -nt out/bb-start.exe ]] && return 0
    done
    return 1
}
if [[ -n $windows ]] && start_stale; then
    start_sources=(start/*.cpp)
    start_objects=("${start_sources[@]/#/out/start-obj/}")
    mkdir -p out/start-obj/start
    printf '%s\n' "${start_sources[@]}" | xargs -P "${BB_JOBS:-4}" -I{} $launcher "${CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra \
        -Wno-deprecated-declarations -Wno-missing-field-initializers -municode -MMD -c {} -o out/start-obj/{}.o
    "${CXX:-clang++}" -municode -static "${start_objects[@]/%/.o}" out/lantern.res -lz -lexpat -lbcrypt -o out/bb-start.exe
    echo "Built $PWD/out/bb-start.exe"
fi
# The game is launched outside MSYS2, so clang64/bin is not on its PATH.
# Bundle the transitive CLANG64 runtime DLLs beside every Windows executable in out/
# (notably libc++.dll, which bb-probe.exe needs through the statically linked GPU library).
if [[ -n $windows ]]; then
    for executable in out/bb-isa-scan.exe out/bb-probe.exe out/bb-gpu-capabilities.exe \
                      out/bb-engine-assets.exe out/bb-ui-4k.exe out/bb-shader-lab.exe; do
        [[ -f $executable ]] || continue
        while IFS= read -r dll; do
            [[ ! -f $dll ]] || cp -f "$dll" out/
        done < <(ldd "$executable" 2>/dev/null | awk '$3 ~ /clang64\/bin\/.*[.]dll$/ {print $3}')
    done
fi

if [[ ${1:-} == --test ]]; then
    export LANTERN_TEST_SANDBOX=1
    unset LANTERN_TEST_SANDBOX_LOOPBACK
    if [[ -f out/test-paths.env ]]; then source out/test-paths.env; fi
    export BB_BOOT_INSTALL_IMAGE=${BB_BOOT_INSTALL_IMAGE:-} BB_BOOT_INSTALL_ASSETS=${BB_BOOT_INSTALL_ASSETS:-}
    test_link=()
    if [[ -n $windows ]]; then test_link=(-lws2_32 -lwinpthread -Wl,--disable-dynamicbase,--disable-high-entropy-va); fi
    no_dialog_gate build.sh out/gpu/build.ninja || exit 1
    # Scratch of earlier runs goes first: tests name their folders by process id, and a fresh
    # container gives the same ids again.
    rm -rf out/test-tmp
    planted=out/test-tmp/no-dialog-$$
    mkdir -p "$planted" || { echo "test gate: cannot make $planted" >&2; exit 1; }
    printf '%s %s\n' '"$CC"' 'tests/test_planted.c -o out/planted-test' > "$planted/build.sh"
    printf 'build CMakeFiles/planted-test.dir/tests/test_planted.cpp.obj: CXX\n  FLAGS = -O2\n' > "$planted/build.ninja"
    if no_dialog_gate "$planted/build.sh" /dev/null 2> /dev/null; then echo 'no-dialog gate: a planted compile line passed' >&2; exit 1; fi
    if no_dialog_gate /dev/null "$planted/build.ninja" 2> /dev/null; then echo 'no-dialog gate: a planted CMake object passed' >&2; exit 1; fi
    rm -rf "$planted"
    # The C runtime's words: "Assertion failed" (Windows), "Assertion `...' failed" (glibc).
    assert_message='Assertion (failed|.* failed)'
    for language in c c++; do
        compiler=$CC
        if [[ $language == c++ ]]; then compiler=${CXX:-clang++}; fi
        "$compiler" "${no_dialog[@]}" -x $language -O1 -Wall -Wextra -Werror -Wno-deprecated tests/test_no_dialog_assert.c -o out/no-dialog-assert-$language-test || exit 1
        no_dialog_rc=0
        timeout 60 out/no-dialog-assert-$language-test > /dev/null 2> out/no-dialog-assert-$language.log || no_dialog_rc=$?
        if (( no_dialog_rc == 0 || no_dialog_rc == 124 )) || ! grep -qE "$assert_message" out/no-dialog-assert-$language.log; then
            echo "no-dialog-assert-$language-test: FAILED (exit $no_dialog_rc, a failed assert must print to stderr and exit)" >&2; cat out/no-dialog-assert-$language.log >&2; exit 1
        fi
        echo "no-dialog-assert-$language-test: ok (exit $no_dialog_rc, $(grep -m1 -E "$assert_message" out/no-dialog-assert-$language.log))"
    done
    gate_run=ALL
    if [[ ${BB_GATE:-} == fast ]]; then
        gate_python=python3
        "$gate_python" -c "" 2>/dev/null || gate_python=python
        gate_run=$("$gate_python" -I tools/gate_select.py) || gate_run=ALL
        [[ -n ${gate_run//[[:space:]]/} ]] || gate_run=ALL
    fi
    gate_skipped=()
    wanted() { [[ " $gate_run " == *" ALL "* || " $gate_run " == *" $1 "* ]]; }
    want() {
        if wanted "$1"; then echo "== test group $1"; return 0; fi
        [[ " ${gate_skipped[*]} " == *" $1 "* ]] || gate_skipped+=("$1")
        return 1
    }
    sys_objects() {
        local dir=out/test-obj/$1 source
        shift
        rm -rf "$dir"
        for source in "${sys_tests[@]}"; do mkdir -p "$dir/${source%/*}"; done
        printf '%s\n' "${sys_tests[@]}" | xargs -P "${BB_JOBS:-4}" -I{} $launcher "$CC" "$@" -c {} -o "$dir/{}.o"
    }
    sys_list() { local source; for source in "${sys_tests[@]}"; do printf '%s\n' "out/test-obj/$1/$source.o"; done; }
    sys_flags=("${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB)
    if wanted runtime-core || wanted runtime-sys || wanted server; then
        sys_objects plain "${sys_flags[@]}"
        mapfile -t sys_plain < <(sys_list plain)
    fi
    if want runtime-core; then
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_pad.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/pad-test
    out/pad-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_runtime.c src/host.c src/wait_stats.c src/blocked.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sema.c src/host.c src/wait_stats.c src/blocked.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sema-test
    out/sema-test
    fi
    if want runtime-sys; then
    sys_objects gnu -D_GNU_SOURCE "${sys_flags[@]}"
    mapfile -t sys_gnu < <(sys_list gnu)
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_content.c "${sys_gnu[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/content-test
    out/content-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_rtc.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-rtc-test
    out/sys-rtc-test
    fi
    if want toml-patch; then
    # start/common.cpp is the native start's (Windows only), as for online-profile-test.
    if [[ -n $windows ]]; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Wno-deprecated-declarations -I. tests/test_toml_patch.cpp start/toml_patch.cpp start/xml.cpp start/common.cpp -lexpat -lz -lbcrypt -o out/toml-patch-test
    out/toml-patch-test
    else
        echo 'toml-patch-test: SKIPPED: the native start (start/common.cpp) is Windows only'
    fi
    fi
    if want game-heaps; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_game_heaps.cpp -o out/game-heaps-test && timeout 300 out/game-heaps-test && "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DGAME_HEAPS_TRIALS -I. tests/test_game_heaps.cpp -o out/game-heaps-trials && timeout 300 out/game-heaps-trials || exit 1
    fi
    if want gx-records; then
    for mutants in "" "-DLANTERN_GX_RECORDS_MUTANTS -DLANTERN_GX_RECORD_HASH_MUTANTS"; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_records.cpp gpu/shim/engine/gx_bindings.cpp -o out/gx-records-test && timeout 300 out/gx-records-test || exit 1; done
    fi
    if want gx-whole; then
    for mutants in "" -DLANTERN_GX_WHOLE_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" $mutants -I. tests/test_gx_whole.cpp -o out/gx-whole-test && timeout 300 out/gx-whole-test || exit 1; done
    fi
    if want cache-settings; then
    for mutants in "" -DLANTERN_CACHE_SETTINGS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_cache_settings.cpp -o out/cache-settings-test && timeout 300 out/cache-settings-test || exit 1; done
    fi
    if want bottleneck-meter; then
    for mutants in "" -DLANTERN_METER_RULE_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror "${threads[@]}" $mutants -I. tests/test_bottleneck_meter.cpp gpu/shim/lantern_meter_threads.cpp -o out/bottleneck-meter-test && timeout 300 out/bottleneck-meter-test || exit 1; done
    fi
    if want gpu-stamps; then
    for mutants in "" -DLANTERN_GPU_STAMPS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gpu_stamps.cpp -o out/gpu-stamps-test && timeout 300 out/gpu-stamps-test || exit 1; done
    fi
    if want gx-whole-stageb; then
    for mutants in "" -DLANTERN_GX_STAGEB_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_whole_stageb.cpp -o out/gx-whole-stageb-test && timeout 300 out/gx-whole-stageb-test || exit 1; done
    fi
    if want gx-whole-owed-blocks; then
    for mutants in "" -DLANTERN_GX_OWED_BLOCKS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_whole_owed_blocks.cpp -o out/gx-whole-owed-blocks-test && timeout 300 out/gx-whole-owed-blocks-test || exit 1; done
    fi
    if want gx-whole-targets; then
    for mutants in "" -DLANTERN_GX_TARGETS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_whole_targets.cpp -o out/gx-whole-targets-test && timeout 300 out/gx-whole-targets-test || exit 1; done
    fi
    if want main-jobs-barrier; then
    for mutants in "" -DLANTERN_MAIN_JOBS_BARRIER_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" $mutants -I. tests/test_main_jobs_barrier.cpp "${test_link[@]}" -o out/main-jobs-barrier-test && timeout 300 out/main-jobs-barrier-test $([[ -n $mutants ]] && echo --mutants || echo --bench) || exit 1; done
    fi
    if want gx-stream-plan; then
    for mutants in "" -DLANTERN_STREAM_PLAN_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror $mutants -I. tests/test_gx_stream_plan.cpp -o out/gx-stream-plan-test && timeout 60 out/gx-stream-plan-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want utf8-wide; then
    for mutants in "" -DLANTERN_UTF8_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror $mutants -I. tests/test_utf8_wide.cpp -o out/utf8-wide-test && timeout 60 out/utf8-wide-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want ui-composition; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -UNDEBUG -I. tests/test_ui_composition.cpp -o out/ui-composition-test && timeout 300 out/ui-composition-test || exit 1
    fi
    if want upscaler-skips; then
    for mutants in "" -DLANTERN_UPSCALER_SKIPS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_upscaler_skips.cpp -o out/upscaler-skips-test && timeout 300 out/upscaler-skips-test || exit 1; done
    fi
    if want dlss-rules; then
    for mutants in "" -DLANTERN_DLSS_RULES_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_dlss_rules.cpp -o out/dlss-rules-test && timeout 60 out/dlss-rules-test || exit 1; done
    fi
    if want fog-scale; then
    for mutants in "" -DLANTERN_FOG_SCALE_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_fog_scale.cpp -o out/fog-scale-test && timeout 60 out/fog-scale-test || exit 1; done
    fi
    if want fps-box; then
    for mutants in "" -DLANTERN_FPS_BOX_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_fps_box.cpp -o out/fps-box-test && timeout 60 out/fps-box-test || exit 1; done
    fi
    if want lamp-fog-lights; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_lamp_fog_lights.cpp -o out/lamp-fog-lights-test && timeout 60 out/lamp-fog-lights-test || exit 1; for mutant in 1 2 3 4 5 6 7 8 9 10 11; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_LAMP_FOG_MUTANT=$mutant -I. tests/test_lamp_fog_lights.cpp -o out/lamp-fog-lights-mutant || exit 1; if timeout 60 out/lamp-fog-lights-mutant >/dev/null 2>&1; then echo "lamp-fog-lights mutant $mutant survived"; exit 1; fi; echo "lamp-fog-lights mutant $mutant caught"; done
    fi
    if want frame-gen-pacing; then
    for mutants in "" -DLANTERN_FG_PACING_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_frame_gen_pacing.cpp -o out/frame-gen-pacing-test && timeout 60 out/frame-gen-pacing-test || exit 1; done
    fi
    if want vblank-watch; then
    for mutants in "" -DLANTERN_VBLANK_WATCH_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_vblank_watch.cpp -o out/vblank-watch-test && timeout 120 out/vblank-watch-test || exit 1; done
    fi
    if want fsr-framegen-rules; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_fsr_framegen_rules.cpp -o out/fsr-framegen-rules-test
    out/fsr-framegen-rules-test
    fi
    if want gc-marks; then
    for mutants in "" -DLANTERN_GC_MARKS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gc_marks.cpp -o out/gc-marks-test && timeout 300 out/gc-marks-test || exit 1; done
    fi
    if want catalog-keys; then
    for mutants in "" -DLANTERN_CATALOG_KEYS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_catalog_keys.cpp -o out/catalog-keys-test && timeout 60 out/catalog-keys-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want motion-history; then
    for mutants in "" -DLANTERN_MOTION_HISTORY_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_motion_history.cpp -o out/motion-history-test && timeout 60 out/motion-history-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want p2p-split; then
    for mutants in "" -DLANTERN_P2P_SPLIT_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_p2p_split.cpp -o out/p2p-split-test && timeout 60 out/p2p-split-test || exit 1; done
    fi
    if want room-bundle; then
    for mutants in "" -DLANTERN_ROOM_BUNDLE_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_room_bundle.cpp -o out/room-bundle-test && timeout 60 out/room-bundle-test || exit 1; done
    fi
    if want room-calls; then
    for mutants in "" -DLANTERN_ROOM_CALLS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_room_calls.cpp -o out/room-calls-test && timeout 60 out/room-calls-test || exit 1; done
    fi
    if want rt-scene-rules; then
    for mutants in "" -DLANTERN_RT_RULES_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_rt_scene_rules.cpp -o out/rt-scene-rules-test && timeout 60 out/rt-scene-rules-test || exit 1; done
    fi
    if want net-counts; then
    for mutants in "" -DLANTERN_NET_COUNTS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_net_counts.cpp -o out/net-counts-test && timeout 60 out/net-counts-test || exit 1; done
    fi
    if want rt-lamps; then
    for mutants in "" -DLANTERN_RT_LAMPS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" $mutants -I. tests/test_rt_lamps.cpp -o out/rt-lamps-test && timeout 120 out/rt-lamps-test || exit 1; done
    fi
    if want pc-input; then
    for mutants in "" -DLANTERN_PAD_ACTIONS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants "${includes[@]}" -Igpu/shim tests/test_pc_input.cpp gpu/shim/input/bindings.cpp "${libraries[@]}" -o out/pc-input-test && timeout 300 out/pc-input-test || exit 1; done
    fi
    if want party-death; then
    for mutants in "" -DLANTERN_PARTY_DEATH_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_party_death.cpp -o out/party-death-test && timeout 300 out/party-death-test || exit 1; done
    fi
    if want trophy-rules; then
    for mutants in "" -DLANTERN_TROPHY_RULES_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_trophy_rules.cpp -o out/trophy-rules-test && timeout 60 out/trophy-rules-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want kept-travel; then
    for mutants in "" -DLANTERN_KEPT_TRAVEL_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -UNDEBUG $mutants -I. tests/test_kept_travel.cpp -o out/kept-travel-test && timeout 60 out/kept-travel-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want world-copy; then
    for mutants in "" -DLANTERN_WORLD_COPY_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -UNDEBUG $mutants -I. tests/test_world_copy.cpp -o out/world-copy-test && timeout 60 out/world-copy-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want insight-sync; then
    for mutants in "" -DLANTERN_INSIGHT_SYNC_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -UNDEBUG $mutants -I. tests/test_insight_sync.cpp -o out/insight-sync-test && timeout 60 out/insight-sync-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want torn-vsharps; then
    for mutants in "" -DLANTERN_TORN_VSHARPS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror $mutants -I. tests/test_torn_vsharps.cpp -o out/torn-vsharps-test && timeout 60 out/torn-vsharps-test $([[ -n $mutants ]] && echo --mutants) || exit 1; done
    fi
    if want gx-whole-binds; then
    for mutants in "" -DLANTERN_GX_BINDS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_whole_binds.cpp -o out/gx-whole-binds-test && timeout 300 out/gx-whole-binds-test || exit 1; done
    fi
    if want gx-owed-registers; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_gx_owed_registers.cpp -o out/gx-owed-registers-test && timeout 300 out/gx-owed-registers-test && "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_GX_OWED_MUTANTS -I. tests/test_gx_owed_registers.cpp -o out/gx-owed-registers-mutants && timeout 300 out/gx-owed-registers-mutants --mutants || exit 1
    fi
    if want gx-whole-stage-a; then
    for mutants in "" -DLANTERN_GX_STAGE_A_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_gx_whole_stage_a.cpp -o out/gx-whole-stage-a-test && timeout 300 out/gx-whole-stage-a-test || exit 1; done
    fi
    if want draw-tokens; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_draw_tokens.cpp -o out/draw-tokens-test
    out/draw-tokens-test
    fi
    if want const-census; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_const_census.cpp -o out/const-census-test
    out/const-census-test
    fi
    if want ring-constants; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -I. tests/test_ring_constants.cpp -o out/ring-constants-test
    timeout 300 out/ring-constants-test --bench
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_RING_MUTANTS -I. tests/test_ring_constants.cpp -o out/ring-constants-mutants
    timeout 300 out/ring-constants-mutants
    fi
    if want batch; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_batch.cpp gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/batch-test
    timeout 300 out/batch-test
    fi
    if want frame-gen-cap; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_frame_gen_cap.cpp -o out/frame-gen-cap-test
    out/frame-gen-cap-test
    fi
    if want present-policy; then
    for variant in plain faults; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. $([[ $variant == faults ]] && echo -DPRESENT_POLICY_FAULTS) tests/test_present_policy.cpp -o out/present-policy-$variant-test && timeout 300 out/present-policy-$variant-test || exit 1; done
    fi
    if want pacing-stats; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror -I. -Igpu/shim tests/test_pacing_stats.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/pacing-stats-test
    out/pacing-stats-test
    fi
    if want gx-sealed; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. tests/test_gx_sealed.cpp -o out/gx-sealed-test
    out/gx-sealed-test
    fi
    if want vram-evict; then
    for mutants in "" -DLANTERN_VRAM_EVICT_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_vram_evict.cpp -o out/vram-evict-test && timeout 300 out/vram-evict-test || exit 1; done
    fi
    if want texset-static; then
    for mutants in "" -DLANTERN_TEXSET_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. tests/test_texset_static.cpp -o out/texset-static-test && timeout 300 out/texset-static-test || exit 1; done
    fi
    if want vertex-census; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_vertex_census.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/vertex-census-test
    out/vertex-census-test
    fi
    if want pipe-census; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_pipe_census.cpp gpu/shim/engine/pipe_census.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/pipe-census-test
    out/pipe-census-test
    fi
    if want reg-window; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_reg_window.cpp -o out/reg-window-test
    out/reg-window-test
    fi
    if want decoder-walk; then
    for mutants in "" -DLANTERN_DECODER_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror $mutants -I. tests/test_decoder_walk.cpp -o out/decoder-walk-test && timeout 300 out/decoder-walk-test || exit 1; done
    fi
    if want gx-whole-census; then
    for mutants in "" -DLANTERN_GX_CENSUS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $mutants -I. -Igpu/shadps4 tests/test_gx_whole_census.cpp -o out/gx-whole-census-test && timeout 300 out/gx-whole-census-test || exit 1; done
    fi
    if want gx-readers; then
    bash tools/check-gx-readers.sh || exit 1
    fi
    if want gx-flush-census; then
    for mutants in "" -DLANTERN_GX_FLUSH_CENSUS_MUTANTS; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror $mutants -I. -Igpu/shim -isystem gpu/shadps4 tests/test_gx_flush_census.cpp -o out/gx-flush-census-test && timeout 300 out/gx-flush-census-test || exit 1; done
    fi
    if want pipeline-memo; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -I. tests/test_pipeline_memo.cpp -o out/pipeline-memo-test
    out/pipeline-memo-test --bench
    fi
    if want server; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -I. -iquote gpu/bbnet/vendor/bbhost_shim tests/test_thd_signal.cpp gpu/bbnet/vendor/bbhost/np_signaling.cpp $([[ -n $windows ]] && echo -lws2_32) -o out/thd-signal-test
    out/thd-signal-test
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_online_gate.cpp -o out/online-gate-test
    out/online-gate-test
    fi
    if want breadcrumbs-ring; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. tests/test_breadcrumbs_ring.cpp -o out/breadcrumbs-ring-test
    out/breadcrumbs-ring-test --bench
    fi
    if want crash-markers; then
    for mode in test mutants; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror $([[ $mode == mutants ]] && echo -DBB_MARKER_MUTANTS) -I. -Igpu/shim -Igpu/shadps4 tests/test_crash_markers.cpp gpu/shadps4/video_core/renderer_vulkan/vk_breadcrumbs_note.cpp gpu/shadps4/common/io_file.cpp gpu/shadps4/common/path_util.cpp gpu/shadps4/common/error.cpp gpu/shim/common/logging/log.cpp gpu/shim/common/assert.cpp -lfmt $([[ -n $windows ]] && echo -lonecore) -o out/crash-markers-$mode; timeout 300 out/crash-markers-$mode; done
    fi
    if want server; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_thd_resend.cpp -o out/thd-resend-test
    out/thd-resend-test
    fi
    if want gpu-cache-stamp; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_gpu_cache_stamp.cpp -o out/gpu-cache-stamp-test
    out/gpu-cache-stamp-test
    fi
    if want gpu-cache-inputs; then
    bash tests/test_gpu_cache_inputs.sh
    fi
    if want driver-cache-save; then
    for variant in plain mutants; do "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. $([[ $variant == mutants ]] && echo -DLANTERN_DECISION_MUTANTS) tests/test_driver_cache_save.cpp -o out/driver-cache-save-$variant-test; timeout 300 out/driver-cache-save-$variant-test; done
    fi
    if want scene-proxy-release; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. tests/test_scene_proxy_release.cpp "${test_link[@]}" -o out/scene-proxy-release-test
    out/scene-proxy-release-test
    fi
    if want submit-failures; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_submit_failures.cpp -o out/submit-failures-test; timeout 300 out/submit-failures-test; "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_TEST_MUTANTS -I. tests/test_submit_failures.cpp -o out/submit-failures-mutants; timeout 300 out/submit-failures-mutants --mutants
    fi
    if want settings; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -UNDEBUG -Wall -Wextra -Werror "${threads[@]}" $([[ -n $windows ]] && echo -include gpu/win_compat.h) -I. -Igpu -Igpu/shim tests/test_settings.cpp gpu/shim/bbport_settings.cpp gpu/shim/bbport_menu_i18n.cpp -o out/settings-test
    out/settings-test
    fi
    if wanted settings || wanted launcher; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" $([[ -n $windows ]] && echo -include gpu/win_compat.h) -I. -Igpu/shim tests/test_settings_writes.cpp gpu/shim/bbport_settings.cpp gpu/shim/bbport_menu_i18n.cpp -o out/settings-writes-test
    fi
    if want settings; then
    timeout 300 out/settings-writes-test
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" $([[ -n $windows ]] && echo -include gpu/win_compat.h) -DLANTERN_MUTANT_NO_EXIT_FLUSH -I. -Igpu/shim tests/test_settings_writes.cpp gpu/shim/bbport_settings.cpp gpu/shim/bbport_menu_i18n.cpp -o out/settings-writes-exit-mutant
    timeout 300 out/settings-writes-exit-mutant --exit-mutant
    fi
    early_skipped=()
    if want launcher; then
    # The launcher beside the engine (../launcher), or under windows/ as in the public repository.
    launcher_tests=../launcher/tests
    [[ -d $launcher_tests ]] || launcher_tests=../launcher/windows/tests
    bash "$launcher_tests/run-settings-checks.sh" "$PWD/out/settings-writes-test" | tee out/settings-checks.log
    if grep -q "SKIPPED:" out/settings-checks.log; then early_skipped+=(launcher-settings-checks); fi
    launcher_python=python3
    "$launcher_python" -c "" 2>/dev/null || launcher_python=python
    if "$launcher_python" -c "" 2>/dev/null; then
        "$launcher_python" -I tests/test_launcher_settings.py launcher
    else
        echo "launcher-settings-test: SKIPPED: needs Python (python3 or python)"
        early_skipped+=(launcher-settings-test)
    fi
    fi
    if want package-switches; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -I. tests/test_package_switches.cpp start/package_switches.cpp -o out/package-switches-test
    timeout 300 out/package-switches-test
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -DLANTERN_MUTANTS -I. tests/test_package_switches.cpp start/package_switches.cpp -o out/package-switches-mutants
    for mutant in $(out/package-switches-mutants --mutants); do
        if timeout 300 out/package-switches-mutants --mutant "$mutant" > /dev/null; then echo "package-switches-test: mutant $mutant survived" >&2; exit 1; fi
    done
    echo "package-switches-test: every mutant fails"
    if [[ -n $windows ]]; then
        check_dir=$(mktemp -d)
        printf 'LANTERN_RT_KEEP_PREVIOUS=1\r\nBB_FRAME_STATS=1\r\n' > "$check_dir/engine-switches.txt"
        if ! BB_LOG_FILE= out/bb-start.exe --check-switches "$check_dir" > /dev/null; then echo "package-switches-test: bb-start refused switches its own table has" >&2; exit 1; fi
        printf 'LANTERN_RT_KEEP_PREVIOUS=1\r\nLANTERN_PLANTED_UNKNOWN_SWITCH=1\r\n' > "$check_dir/engine-switches.txt"
        code=0; BB_LOG_FILE= out/bb-start.exe --check-switches "$check_dir" > /dev/null || code=$?; if [[ $code != 3 ]]; then echo "package-switches-test: a planted unknown switch gave exit $code, not 3" >&2; exit 1; fi
        rm -f "$check_dir/engine-switches.txt"
        code=0; BB_LOG_FILE= out/bb-start.exe --check-switches "$check_dir" > /dev/null || code=$?; if [[ $code != 4 ]]; then echo "package-switches-test: a package without engine-switches.txt gave exit $code, not 4" >&2; exit 1; fi
        rmdir "$check_dir"
        echo "package-switches-test: bb-start --check-switches applies known switches and fails a planted unknown one"
    fi
    fi
    if want switches; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror -I. -Igpu/shim tests/test_switches.cpp gpu/shim/engine/pipe_census.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/switches-test
    timeout 300 out/switches-test
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror -DLANTERN_MUTANTS -I. -Igpu/shim tests/test_switches.cpp gpu/shim/engine/pipe_census.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp -o out/switches-mutants
    for mutant in $(out/switches-mutants --mutants); do
        if timeout 300 out/switches-mutants --mutant "$mutant" > /dev/null; then echo "switches-test: mutant $mutant survived" >&2; exit 1; fi
    done
    echo "switches-test: every mutant fails"
    fi
    if want steps; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_steps.cpp gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp "${test_link[@]}" -o out/steps-test
    timeout 300 out/steps-test
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -DLANTERN_MUTANTS -I. -Igpu/shim tests/test_steps.cpp gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp "${test_link[@]}" -o out/steps-mutants
    for mutant in $(out/steps-mutants --mutants); do
        if timeout 300 out/steps-mutants --mutant "$mutant" > /dev/null; then echo "steps-test: mutant $mutant survived" >&2; exit 1; fi
    done
    echo "steps-test: every mutant fails"
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/bench_steps.cpp gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp "${test_link[@]}" -o out/steps-bench
    echo "steps-bench: report only (rule T3: timings are printed for people to compare, never asserted)"
    timeout 600 out/steps-bench
    fi
    if want scrub; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror -I. -Igpu/shim tests/test_scrub.cpp gpu/shim/lantern_scrub.cpp -o out/scrub-test
    timeout 300 out/scrub-test
    fi
    if want diag-gates; then
    planted=out/test-tmp/diag-gates-$$
    mkdir -p "$planted" || { echo "test gate: cannot make $planted" >&2; exit 1; }
    printf 'struct Holder {\n    Lantern::ThreadSlot& slot;\n};\n' > "$planted/planted.h"
    if slot_reference_gate "$planted" 2> /dev/null; then echo 'slot reference gate: a planted member passed' >&2; exit 1; fi
    printf 'struct ThreadSlot;\nthread_local ThreadSlot* t_slot;\nThreadSlot** Where() { return &t_slot; }\n' > "$planted/tls.cpp"
    "${CXX:-clang++}" -std=c++23 -O1 -femulated-tls -c "$planted/tls.cpp" -o "$planted/tls.o"
    if emulated_tls_gate "$planted/tls.o" 2> /dev/null; then echo 'emulated TLS gate: a planted __emutls_v.t_slot passed' >&2; exit 1; fi
    mkdir -p "$planted/census/gpu"
    printf 'const char* planted = "BB_PLANTED_NAME_WITH_NO_ROW";\n' > "$planted/census/gpu/planted.cpp"
    if switch_table_census "$planted/census" 2> /dev/null; then echo 'switch table census: a planted name passed' >&2; exit 1; fi
    printf 'const char* prefix = "BB_PLANTED_PREFIX_";\n' > "$planted/census/gpu/planted.cpp"
    if ! switch_table_census "$planted/census" 2> /dev/null; then echo 'switch table census: a name prefix ending in _ was taken for a switch name' >&2; exit 1; fi
    printf 'bool on = std::getenv("BB_FRAME_STATS") != nullptr;\n' > "$planted/census/gpu/planted.cpp"
    if switch_table_census "$planted/census" 2> /dev/null; then echo 'switch table census: a planted BB_FRAME_STATS reader passed' >&2; exit 1; fi
    printf 'auto m = Batch::Switch("BB_FRAME_STATS");\n' > "$planted/census/gpu/planted.cpp"
    if switch_table_census "$planted/census" 2> /dev/null; then echo 'switch table census: a planted Batch name passed' >&2; exit 1; fi
    rm -rf "$planted"
    echo "slot reference, emulated TLS and switch table census gates: planted faults are caught"
    fi
    if want later; then
    for later in vertex_memo:gpu/shim/engine/vertex_memo.cpp triage:gpu/shim/lantern_crash.cpp \
                 crash_writer:gpu/shim/lantern_crash.cpp guest_symbols:gpu/shim/lantern_guest_symbols.cpp; do
        test_name=${later%%:*} test_source=${later#*:}
        [[ -f tests/test_$test_name.cpp ]] || continue
        "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_$test_name.cpp "$test_source" gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp "${test_link[@]}" -o "out/${test_name//_/-}-test"
        timeout 300 "out/${test_name//_/-}-test"
    done
    fi
    if want runtime-sys; then
    sys_objects save "${sys_flags[@]}" -DLANTERN_SAVE_TEST_HOOKS
    mapfile -t sys_save < <(sys_list save)
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -DLANTERN_SWITCH_C_STUB -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc tests/test_sys_mutex.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-mutex-test
    out/sys-mutex-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_thread.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-thread-test
    out/sys-thread-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_clock.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-clock-test
    out/sys-clock-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_file.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-file-test
    out/sys-file-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -DLANTERN_SWITCH_C_STUB -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -DLANTERN_SAVE_TEST_HOOKS -I. -Isrc tests/test_save_data.c "${sys_save[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/save-data-test
    out/save-data-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -DLANTERN_SWITCH_C_STUB -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -DLANTERN_SAVE_TEST_HOOKS -I. -Isrc tests/test_file_root.c "${sys_save[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/file-root-test
    out/file-root-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_game_language.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/game-language-test
    out/game-language-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_memory.c src/host.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-memory-test
    out/sys-memory-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_video_out.c src/host.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-video-out-test
    out/sys-video-out-test
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc -DLANTERN_SWITCH_C_STUB tests/test_sys_av_player.c src/host.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/sys-av-player-test
    out/sys-av-player-test
    fi
    if want server; then
    "$CC" "${no_dialog[@]}" "${cstd[@]}" -DLANTERN_SWITCH_C_STUB -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc tests/test_offline_contract.c "${sys_plain[@]}" out/libatrac9.a -lm "${libraries[@]}" "${test_link[@]}" -o out/offline-contract-test
    out/offline-contract-test tests/golden/offline-contract.txt
    fi
    if want stageb-counts; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_stageb_counts.cpp -o out/stageb-counts-test && timeout 300 out/stageb-counts-test && "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_STAGEB_MUTANTS -I. tests/test_stageb_counts.cpp -o out/stageb-counts-mutants && timeout 300 out/stageb-counts-mutants --mutants || exit 1
    fi
    if want sealed-holds; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -I. tests/test_sealed_holds.cpp -o out/sealed-holds-test && timeout 300 out/sealed-holds-test && "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DLANTERN_SEALED_MUTANTS -I. tests/test_sealed_holds.cpp -o out/sealed-holds-mutants && timeout 300 out/sealed-holds-mutants --mutants || exit 1
    fi
    if want dynamic-state-skip; then
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror "${threads[@]}" -I. tests/test_dynamic_state_skip.cpp -o out/dynamic-state-skip-test; "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -DBB_DSS_VARIANTS "${threads[@]}" -I. tests/test_dynamic_state_skip.cpp -o out/dynamic-state-skip-variants; out/dynamic-state-skip-test; out/dynamic-state-skip-variants --variants
    fi
    if want main-jobs; then
    main_jobs_sources=(gpu/shim/engine/main_jobs.cpp gpu/shim/engine/engine_hooks.cpp gpu/shim/lantern_steps.cpp gpu/shim/lantern_switches.cpp gpu/shim/lantern_scrub.cpp)
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_main_jobs.cpp "${main_jobs_sources[@]}" "${test_link[@]}" -o out/main-jobs-test || exit 1
    timeout 300 out/main-jobs-test || exit 1
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -DLANTERN_JOBS_WRONG -I. -Igpu/shim tests/test_main_jobs.cpp "${main_jobs_sources[@]}" "${test_link[@]}" -o out/main-jobs-mutants || exit 1
    timeout 300 out/main-jobs-mutants --wrong 0 > out/main-jobs-mutants-0.log || { cat out/main-jobs-mutants-0.log; echo "main-jobs-test: the mutant build fails with no mutant" >&2; exit 1; }
    for wrong in $(seq 1 "$(out/main-jobs-mutants --count)"); do
        if timeout 300 out/main-jobs-mutants --wrong "$wrong" > /dev/null; then echo "main-jobs-test: mutant $wrong survived" >&2; exit 1; fi
    done
    echo "main-jobs-test: every mutant fails ($(out/main-jobs-mutants --count))"
    "${CXX:-clang++}" "${no_dialog[@]}" -std=c++23 -O2 -Wall -Wextra -Werror "${threads[@]}" -I. -Igpu/shim tests/test_main_jobs_image.cpp tools/engine/sha256.cpp "${main_jobs_sources[@]}" "${test_link[@]}" -o out/main-jobs-image-test || exit 1
    main_jobs_image=$(timeout 300 out/main-jobs-image-test) || { echo "$main_jobs_image"; exit 1; }
    echo "$main_jobs_image"
    if [[ $main_jobs_image == *SKIPPED:* ]]; then early_skipped+=(main-jobs-image-test); fi
    fi
    if want server; then
    if [[ -n $windows ]]; then
        "${CXX:-clang++}" "${no_dialog[@]}" -std=c++20 -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations -I. tests/test_online_profile.cpp start/online.cpp start/common.cpp -lexpat -lz -lbcrypt -o out/online-profile-test
        out/online-profile-test tests/golden/online-profile-cases.json tests/golden/online-profile-start.txt
    fi
    ninja -C out/gpu ${BB_JOBS:+-j "$BB_JOBS"} bbnet-registration-test thd-wire-test room-wire-test \
        room-crypto-test room-link-test room-link-errors-test room-host-test room-announce-test room-status-test \
        room-service-test sandbox-test sandbox-mutant-test
    timeout 300 out/gpu/bbnet/sandbox-test
    for mutant in 1 2 3 4; do
        if timeout 300 out/gpu/bbnet/sandbox-mutant-test "$mutant" > "out/sandbox-mutant-$mutant.log" 2>&1; then
            echo "sandbox-test mutant $mutant: FAILED (the mutant passes)"; cat "out/sandbox-mutant-$mutant.log"; exit 1
        fi
        if ! grep -q '^FAIL: ' "out/sandbox-mutant-$mutant.log"; then
            echo "sandbox-test mutant $mutant: FAILED (it fails, but on no check)"; cat "out/sandbox-mutant-$mutant.log"; exit 1
        fi
        echo "sandbox-test mutant $mutant: ok (the mutant fails: $(grep -m1 '^FAIL: ' "out/sandbox-mutant-$mutant.log"))"
    done
    skipped_tests=(${early_skipped[@]+"${early_skipped[@]}"})
    online_test() {
        local name=$1 output loopback=
        shift
        if [[ $1 == --loopback ]]; then loopback=1; shift; fi
        if ! output=$(LANTERN_TEST_SANDBOX_LOOPBACK=$loopback timeout 300 "$@" 2>&1); then
            printf '%s\n' "$output"
            echo "$name: FAILED"
            return 1
        fi
        printf '%s\n' "$output"
        if grep -q 'SKIPPED:' <<<"$output"; then skipped_tests+=("$name"); fi
    }
    online_test bbnet-registration-test-thd out/gpu/bbnet/bbnet-registration-test thd tests/golden/bbnet-registration-thd.txt
    online_test bbnet-registration-test-room out/gpu/bbnet/bbnet-registration-test room tests/golden/bbnet-registration-room.txt
    online_test thd-wire-test --loopback out/gpu/bbnet/thd-wire-test tests/golden/thd-wire.txt
    online_test thd-wire-test-refused-start --loopback out/gpu/bbnet/thd-wire-test --refused-start
    online_test thd-wire-test-refused-in-game --loopback out/gpu/bbnet/thd-wire-test --refused-in-game
    online_test room-wire-test --loopback out/gpu/bbnet/room-wire-test tests/golden/room-wire.txt
    for room_test in room-link-test room-announce-test room-status-test; do
        online_test "$room_test" "out/gpu/bbnet/$room_test"
    done
    for room_test in room-crypto-test room-link-errors-test room-host-test; do
        online_test "$room_test" --loopback "out/gpu/bbnet/$room_test"
    done
    if [[ -n ${BB_ROOM_SERVER:-} ]]; then
        LANTERN_TEST_SANDBOX_LOOPBACK=1 timeout 300 out/gpu/bbnet/room-service-test host & room_host_pid=$!
        room_guest_rc=0
        LANTERN_TEST_SANDBOX_LOOPBACK=1 timeout 300 out/gpu/bbnet/room-service-test guest > out/room-service-guest.log 2>&1 || room_guest_rc=$?
        cat out/room-service-guest.log
        room_host_rc=0
        wait "$room_host_pid" || room_host_rc=$?
        if (( room_guest_rc || room_host_rc )); then
            echo "room-service-test: FAILED (host $room_host_rc, guest $room_guest_rc)"
            exit 1
        fi
        if grep -q 'SKIPPED:' out/room-service-guest.log; then skipped_tests+=(room-service-test); fi
    else
        echo 'room-service-test: SKIPPED: needs BB_ROOM_SERVER'
        skipped_tests+=(room-service-test)
    fi
    fi
    if want gpu-hooks; then
    ninja -C out/gpu ${BB_JOBS:+-j "$BB_JOBS"} engine-hooks-test frame-rate-test widescreen-test engine-menu-memory-test native-local-shadows-test \
        frame-rate-mutant-test cheats-code-test cheats-code-mutant-test boot-install-test gx-whole-hooks-test gx-whole-hooks-mutant-test
    out/gpu/engine-hooks-test
    timeout 300 out/gpu/gx-whole-hooks-test || exit 1
    timeout 300 out/gpu/gx-whole-hooks-mutant-test > out/gx-whole-hooks-mutant.log 2>&1 || { cat out/gx-whole-hooks-mutant.log; echo "gx-whole-hooks-mutant-test: FAILED"; exit 1; }
    tail -1 out/gx-whole-hooks-mutant.log
    out/gpu/frame-rate-test
    timeout 300 out/gpu/cheats-code-test
    for mutant in frame-rate-mutant-test cheats-code-mutant-test; do
        if timeout 300 "out/gpu/$mutant" > "out/$mutant.log" 2>&1; then echo "$mutant: FAILED (the mutant passes)"; exit 1; fi
        echo "$mutant: ok (the mutant fails)"
    done
    mkdir -p out/test-tmp
    boot_install=$(timeout 300 out/gpu/boot-install-test out/test-tmp) || { echo "$boot_install"; exit 1; }
    echo "$boot_install"
    if [[ $boot_install == *SKIPPED:* ]]; then
        skipped_tests+=(boot-install-test)
    else
        for mutant in "expected-byte:keep echoes did not write its code at 0x13d2bd2" \
            "loading-restore-byte:the loading pace was not put back at 0x2034845" \
            "auto-rotation-byte:the auto-rotation store was not put back at 0x143c6e8"; do
            name=${mutant%%:*} expected=${mutant#*:}
            if BB_BOOT_INSTALL_MUTANT=$name BB_BOOT_INSTALL_ONLY=offline timeout 300 out/gpu/boot-install-test out/test-tmp > "out/boot-install-mutant-$name.log" 2>&1; then
                echo "boot-install-test mutant $name: FAILED (the mutant passes)"; exit 1
            fi
            if ! grep -q "$expected" "out/boot-install-mutant-$name.log" ||
                grep -q "could not start the child" "out/boot-install-mutant-$name.log"; then
                echo "boot-install-test mutant $name: FAILED (it fails, but not on \"$expected\")"; cat "out/boot-install-mutant-$name.log"; exit 1
            fi
            echo "boot-install-test mutant $name: ok (the mutant fails: $expected)"
        done
        if BB_BOOT_INSTALL_MUTANT=gx-whole-byte BB_BOOT_INSTALL_ONLY=offline-gx-whole timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-gx-whole-byte.log 2>&1; then
            echo "boot-install-test mutant gx-whole-byte: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "GX whole hooks off: GX's output-merger flush at 0x26d2230" out/boot-install-mutant-gx-whole-byte.log ||
            grep -q "could not start the child" out/boot-install-mutant-gx-whole-byte.log; then
            echo "boot-install-test mutant gx-whole-byte: FAILED (it fails, but not on the output-merger flush at 0x26d2230)"; cat out/boot-install-mutant-gx-whole-byte.log; exit 1
        fi
        echo "boot-install-test mutant gx-whole-byte: ok (the mutant fails on the output-merger flush at 0x26d2230)"
        if BB_BOOT_INSTALL_MUTANT=jobs-lock-body BB_BOOT_INSTALL_ONLY=offline-jobs timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-jobs-lock-body.log 2>&1; then
            echo "boot-install-test mutant jobs-lock-body: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "Job lock unavailable: the game's Lock is not 1.09's" out/boot-install-mutant-jobs-lock-body.log ||
            grep -q "could not start the child" out/boot-install-mutant-jobs-lock-body.log; then
            echo "boot-install-test mutant jobs-lock-body: FAILED (it fails, but not on the Lock's body)"; cat out/boot-install-mutant-jobs-lock-body.log; exit 1
        fi
        echo "boot-install-test mutant jobs-lock-body: ok (the mutant fails on the Lock's body)"
        if BB_BOOT_INSTALL_MUTANT=jobs-barrier-shape BB_BOOT_INSTALL_ONLY=offline-main-jobs-barrier timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-jobs-barrier-shape.log 2>&1; then
            echo "boot-install-test mutant jobs-barrier-shape: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "did not fall back to the game's step" out/boot-install-mutant-jobs-barrier-shape.log ||
            grep -q "could not start the child" out/boot-install-mutant-jobs-barrier-shape.log; then
            echo "boot-install-test mutant jobs-barrier-shape: FAILED (it fails, but not on the fallback)"; cat out/boot-install-mutant-jobs-barrier-shape.log; exit 1
        fi
        echo "boot-install-test mutant jobs-barrier-shape: ok (the mutant fails on the fallback to the game's step)"
        if BB_BOOT_INSTALL_MUTANT=jobs-barrier-flip BB_BOOT_INSTALL_ONLY=offline-main-jobs-barrier timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-jobs-barrier-flip.log 2>&1; then
            echo "boot-install-test mutant jobs-barrier-flip: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "runtime switch at 0 left the step on" out/boot-install-mutant-jobs-barrier-flip.log ||
            grep -q "could not start the child" out/boot-install-mutant-jobs-barrier-flip.log; then
            echo "boot-install-test mutant jobs-barrier-flip: FAILED (it fails, but not on the runtime switch)"; cat out/boot-install-mutant-jobs-barrier-flip.log; exit 1
        fi
        echo "boot-install-test mutant jobs-barrier-flip: ok (the mutant fails on the runtime switch at 0)"
        if BB_BOOT_INSTALL_MUTANT=jobs-barrier-steal BB_BOOT_INSTALL_ONLY=offline-main-jobs-barrier timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-jobs-barrier-steal.log 2>&1; then
            echo "boot-install-test mutant jobs-barrier-steal: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "a worker's deque holds 2 jobs, off: its own deque holds 1 jobs" out/boot-install-mutant-jobs-barrier-steal.log ||
            grep -q "could not start the child" out/boot-install-mutant-jobs-barrier-steal.log; then
            echo "boot-install-test mutant jobs-barrier-steal: FAILED (it fails, but not on the moved job)"; cat out/boot-install-mutant-jobs-barrier-steal.log; exit 1
        fi
        echo "boot-install-test mutant jobs-barrier-steal: ok (the mutant fails on the job the added sweep moved)"
        if BB_BOOT_INSTALL_MUTANT=jobs-barrier-census BB_BOOT_INSTALL_ONLY=offline-main-jobs-barrier timeout 300 out/gpu/boot-install-test out/test-tmp > out/boot-install-mutant-jobs-barrier-census.log 2>&1; then
            echo "boot-install-test mutant jobs-barrier-census: FAILED (the mutant passes)"; exit 1
        fi
        if ! grep -q "did not reach the registry at the frame tick" out/boot-install-mutant-jobs-barrier-census.log ||
            grep -q "could not start the child" out/boot-install-mutant-jobs-barrier-census.log; then
            echo "boot-install-test mutant jobs-barrier-census: FAILED (it fails, but not on the census)"; cat out/boot-install-mutant-jobs-barrier-census.log; exit 1
        fi
        echo "boot-install-test mutant jobs-barrier-census: ok (the mutant fails on the census of another thread)"
    fi
    out/gpu/widescreen-test
    out/gpu/engine-menu-memory-test
    out/gpu/native-local-shadows-test
    fi
    echo "fast tier: test groups not run, none of their inputs changed: ${#gate_skipped[@]}${gate_skipped[*]:+ (${gate_skipped[*]})}"
    echo "tests skipped: ${#skipped_tests[@]}${skipped_tests[*]:+ (${skipped_tests[*]})}"
fi
