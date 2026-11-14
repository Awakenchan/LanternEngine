"""Prepare test30.hlsl for glslang (orig30.hlsl): copy test30.hlsl and a minimal shim of the RenoDX
color/math helpers it uses, without namespaces (glslang's HLSL front end does not resolve
unqualified names inside namespaces). The PsychoV30 code itself is unchanged.
Usage: python -I prepare30.py <renodx checkout> <out dir>"""
import os, re, sys

src_root, out = sys.argv[1], sys.argv[2]
shaders = os.path.join(src_root, 'src', 'shaders')
os.makedirs(os.path.join(out, 'tonemap', 'psychov'), exist_ok=True)
os.makedirs(os.path.join(out, 'color'), exist_ok=True)


def strip(text):
    text = text.replace('renodx::color::yf::from::LMS', 'yf_from_LMS')
    text = text.replace('renodx::color::', '').replace('renodx::math::', '')
    text = re.sub(r'^namespace \w+ \{\n', '', text, flags=re.M)
    return re.sub(r'^\}  // namespace \w+\n', '', text, flags=re.M)


test30 = open(os.path.join(shaders, 'tonemap', 'psychov', 'test30.hlsl')).read()
open(os.path.join(out, 'tonemap', 'psychov', 'test30.hlsl'), 'w').write(strip(test30))

rgb = open(os.path.join(shaders, 'color', 'rgb.hlsl')).read()
math = open(os.path.join(shaders, 'math.hlsl')).read()


def grab(name):
    i = rgb.index(f'static const float3x3 {name} = float3x3(')
    return rgb[i:rgb.index(';', i) + 1]


i = math.index('float3x3 Invert3x3(float3x3 m) {')
invert = math[i:math.index('\n}\n', i) + 3].replace('DivideSafe(1.0, det, 0.0)', '(det == 0.f ? 0.f : 1.0 / det)')
mats = ['BT709_TO_XYZ_MAT', 'XYZ_TO_BT709_MAT', 'BT2020_TO_XYZ_MAT', 'XYZ_TO_BT2020_MAT', 'AP1_TO_XYZ_MAT',
        'STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT', 'STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT', 'D60_TO_D65_MAT',
        'BT709_TO_AP1_MAT']
shim = '\n'.join([
    '// Test shim: RenoDX helpers used by test30.hlsl (matrices and Invert3x3 copied verbatim).',
    invert,
    'float3 Select(bool3 c, float3 a, float b) { return c ? a : b; }',
    'float Min(float3 v) { return min(v.x, min(v.y, v.z)); }',
    'float Max(float3 v) { return max(v.x, max(v.y, v.z)); }',
    'float CopySign(float mag, float sgn) { return asfloat((asuint(sgn) & 0x80000000u) | (asuint(mag) & 0x7FFFFFFFu)); }',
    'float3 CopySign(float3 mag, float3 sgn) { return float3(CopySign(mag.x, sgn.x), CopySign(mag.y, sgn.y), CopySign(mag.z, sgn.z)); }',
    'float3 SignPow(float3 x, float3 e) { return CopySign(pow(abs(x), e), x); }',
    *[grab(m) for m in mats],
    'static const float3x3 STOCKMAN_CVRL_LMS_TO_XYZ_2DEG_FIT = Invert3x3(STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT);',
    'static const float3 CIE1702_MB_CIE_WEIGHTS = float3(STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT[1][0], '
    'STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT[1][1], 0.0371597069161f);',
    'static const float3x3 BT709_TO_BT2020_MAT = mul(XYZ_TO_BT2020_MAT, BT709_TO_XYZ_MAT);',
    'float yf_from_LMS(float3 lms) { return mul(STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT, lms).y; }',
    ''])
open(os.path.join(out, 'color', 'rgb.hlsl'), 'w').write(shim)
