#!/usr/bin/env bash
# Numerical check of host_shaders/hdr/psychov30.glsl against RenoDX's test30.hlsl, on the CPU. The
# original is unchanged, with a shim of the few RenoDX helpers it uses (prepare30.py). Both are
# compiled to SPIR-V by glslang, turned into C++ by SPIRV-Cross (--cpp) and run on the same colors
# and parameters (main.cpp, which also prints the measurements of the tone mapper audit,
# LanternEngine-NativePlusHDR's docs/TONEMAPPERS.md). Needs glslangValidator, spirv-cross (with
# its include/), glm, python3, a C++ compiler and a RenoDX checkout:
#   RENODX=<renodx> SPIRV_CROSS_INCLUDE=<SPIRV-Cross/include> GLM=<glm> [CXX=clang++] ./run.sh
# Then psychov_model.cpp models the tone mapper PsychoV with that original.
# Result on 30/10/2026 (RenoDX b6b32f4): LanternEngine-NativePlusHDR's docs/TONEMAPPERS.md.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
hdr="$here/../../../gpu/shadps4/video_core/host_shaders/hdr"
CXX=${CXX:-clang++}
work=$(mktemp -d)
python3 -I "$here/prepare30.py" "$RENODX" "$work/hlsl30"
glslangValidator -V -D -e main -S comp --target-env vulkan1.3 -UVULKAN -I"$work/hlsl30" "$here/orig30.hlsl" -o "$work/orig30.spv" > /dev/null
glslangValidator -V --target-env vulkan1.3 -I"$hdr" "$here/port30.comp" -o "$work/port30.spv" > /dev/null
for t in orig30 port30; do
  spirv-cross "$work/$t.spv" --cpp --output "$work/$t.cpp"
  python3 -I - "$work/$t.cpp" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = s.replace('#include <stdint.h>\n', '#include <stdint.h>\n#include "glob.h"\n', 1)
s = s.replace('inputs._data[', 'g_in[').replace('outputs._data[', 'g_out[')
s = re.sub(r'_\d+\.inputs\[', 'g_in[', s); s = re.sub(r'_\d+\.outputs\[', 'g_out[', s)
s = re.sub(r'_\d+\.pc\.', 'g_pc.', s)
s = s.replace('#define pc __res->pc.get()', '#define pc g_pc').replace('s.register_push_constant(pc__);', '')
open(p, 'w').write(s)
PY
done
flags="-std=c++17 -O1 -w -DGLM_FORCE_SWIZZLE -DGLM_FORCE_RADIANS -I$SPIRV_CROSS_INCLUDE -I$GLM -I$here"
for t in orig30 port30; do
  $CXX $flags -DSPIRV_CROSS_BRANCH= -DSPIRV_CROSS_FLATTEN= -DSPIRV_CROSS_UNROLL= -DSPIRV_CROSS_LOOP= \
    -DImpl=Impl_$t -Dspirv_cross_get_interface=${t}_iface -Dspirv_cross_construct=${t}_construct \
    -Dspirv_cross_destruct=${t}_destruct -Dspirv_cross_invoke=${t}_invoke -c "$work/$t.cpp" -o "$work/$t.o"
done
$CXX $flags "$here/main.cpp" "$work"/orig30.o "$work"/port30.o -o "$work/check" -Wl,--allow-multiple-definition
"$work/check"
# The tone mapper PsychoV (test30 inside it), against Vanilla+ and PsychoV-30 alone.
$CXX $flags "$here/psychov_model.cpp" "$work"/orig30.o -o "$work/model"
"$work/model"
