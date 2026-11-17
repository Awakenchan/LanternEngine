#!/usr/bin/env python3
"""Check gamut_bridge.glsl (the color table's gamut bridge) against RenoDX's HLSL.

Both are compiled with glslang (RenoDX's color/gamut.hlsl through its HLSL front end), turned
into C++ by SPIRV-Cross and run on the CPU over grey, BT.709 and BT.2020 colors from dark to
well above white: the compression scale, the compressed color, compressed then decompressed,
and Neutwo's max-channel scale must agree within 1e-3 of the largest channel.

Needs glslangValidator, spirv-cross, g++, GLM and SPIRV-Cross' CPU support headers. Set
RENODX_DIR to a RenoDX checkout, SPIRV_CROSS_INCLUDE to <SPIRV-Cross checkout>/include;
GLM_INCLUDE is optional. GLSLANG, SPIRV_CROSS and CXX can override the executable names.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


here = Path(__file__).resolve().parent
hdr = here.parents[2] / 'gpu/shadps4/video_core/host_shaders/hdr'
glslang = os.environ.get('GLSLANG', 'glslangValidator')
cxx = os.environ.get('CXX', 'g++')
with tempfile.TemporaryDirectory(prefix='gamut-bridge-') as tmp:
    work = Path(tmp)
    (work / 'hlsl' / 'color').mkdir(parents=True)
    run(sys.executable, '-I', here / 'prepare_gamut.py', os.environ['RENODX_DIR'], work / 'hlsl')
    # SPIR-V 1.3: SPIRV-Cross' CPU backend needs a literal LocalSize.
    run(glslang, '-V', '--target-env', 'vulkan1.3', '--target-env', 'spirv1.3', f'-I{hdr}',
        here / 'port.comp', '-o', work / 'port.spv')
    run(glslang, '-V', '-D', '-e', 'main', '-S', 'comp', '--target-env', 'vulkan1.3',
        '--target-env', 'spirv1.3', '-UVULKAN', f'-I{work / "hlsl"}', here / 'orig.hlsl',
        '-o', work / 'orig.spv')
    includes = ['-I' + str(here)]
    for key in ['SPIRV_CROSS_INCLUDE', 'GLM_INCLUDE']:
        if os.environ.get(key): includes.append('-I' + os.environ[key])
    objects = []
    for name in ['orig', 'port']:
        cpp = work / f'{name}.cpp'
        run(os.environ.get('SPIRV_CROSS', 'spirv-cross'), work / f'{name}.spv', '--cpp', '--output', cpp)
        source = cpp.read_text()
        source = source.replace('#include <stdint.h>', '#include <stdint.h>\n#include "glob.h"', 1)
        for resource, pointer in [('inputs', 'g_in'), ('outputs', 'g_out')]:
            source, count = re.subn(rf'_\d+\.{resource}\[', pointer + '[', source)
            source, count2 = re.subn(rf'{resource}\._data\[', pointer + '[', source)
            if count + count2 == 0:
                raise RuntimeError(f'SPIRV-Cross resource {resource} not found in {cpp.name}')
        cpp.write_text(source)
        # Two SPIRV-Cross modules in one program: their entry points renamed apart.
        run(cxx, '-std=c++17', '-O1', '-w', '-DGLM_FORCE_SWIZZLE', *includes,
            '-DSPIRV_CROSS_BRANCH=', '-DSPIRV_CROSS_FLATTEN=', '-DSPIRV_CROSS_UNROLL=',
            '-DSPIRV_CROSS_LOOP=', f'-DImpl=Impl_{name}',
            f'-Dspirv_cross_get_interface={name}_iface', f'-Dspirv_cross_construct={name}_construct',
            f'-Dspirv_cross_destruct={name}_destruct', f'-Dspirv_cross_invoke={name}_invoke',
            '-c', cpp, '-o', work / f'{name}.o')
        objects.append(work / f'{name}.o')
    linker = ['-Wl,--allow-multiple-definition']
    run(cxx, '-std=c++17', '-O1', '-w', '-DGLM_FORCE_SWIZZLE', *includes, here / 'main.cpp',
        *objects, '-o', work / 'check', *linker)
    run(work / 'check')
