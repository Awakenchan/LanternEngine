#!/usr/bin/env python3
"""Run the production mode 5 transfer functions through glslang -> SPIR-V -> CPU C++.

Needs glslangValidator, spirv-val, spirv-cross, g++, GLM, and SPIRV-Cross' CPU support headers.
Set SPIRV_CROSS_INCLUDE to <SPIRV-Cross checkout>/include; GLM_INCLUDE is optional.
GLSLANG, SPIRV_VAL, SPIRV_CROSS and CXX can override the executable names.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


here = Path(__file__).resolve().parent
hdr = here.parents[2] / 'gpu/shadps4/video_core/host_shaders/hdr'
with tempfile.TemporaryDirectory(prefix='hdr-transfer-') as tmp:
    work = Path(tmp)
    spv, cpp = work / 'check.spv', work / 'check.cpp'
    # SPIRV-Cross' packaged CPU backend needs literal LocalSize (SPIR-V 1.3), not the
    # LocalSizeId emitted for SPIR-V 1.6. Both modules are valid on a Vulkan 1.3 device.
    run(os.environ.get('GLSLANG', 'glslangValidator'), '-V', '--target-env', 'vulkan1.3',
        '--target-env', 'spirv1.3',
        f'-I{hdr}', here / 'check.comp', '-o', spv)
    run(os.environ.get('SPIRV_VAL', 'spirv-val'), '--target-env', 'vulkan1.3', spv)
    run(os.environ.get('SPIRV_CROSS', 'spirv-cross'), spv, '--cpp', '--output', cpp)
    source = cpp.read_text()
    source = source.replace('#include <stdint.h>', '#include <stdint.h>\n#include "glob.h"', 1)
    for name, pointer in [('inputs', 'g_in'), ('outputs', 'g_out')]:
        source, count = re.subn(rf'_\d+\.{name}\[', pointer + '[', source)
        source, count2 = re.subn(rf'{name}\._data\[', pointer + '[', source)
        if count + count2 == 0:
            raise RuntimeError(f'SPIRV-Cross resource {name} not found')
    cpp.write_text(source)
    includes = ['-I' + str(here)]
    for key in ['SPIRV_CROSS_INCLUDE', 'GLM_INCLUDE']:
        if os.environ.get(key): includes.append('-I' + os.environ[key])
    run(os.environ.get('CXX', 'g++'), '-std=c++17', '-O1', '-DGLM_FORCE_SWIZZLE',
        *includes, cpp, here / 'main.cpp', '-o', work / 'check')
    run(work / 'check')
