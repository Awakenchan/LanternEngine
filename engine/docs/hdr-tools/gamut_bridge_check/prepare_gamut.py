"""Prepare RenoDX's gamut.hlsl for glslang's HLSL front end: namespaces removed, qualified calls
renamed to the shim's functions (copied from macleod_boynton.hlsl, rgb.hlsl and neutwo.hlsl).
The gamut code itself is unchanged. Usage: python -I prepare_gamut.py <renodx> <out dir>
(standalone: the shim has the matrices it needs)."""
import os, re, sys

src_root, out = sys.argv[1], sys.argv[2]
shaders = os.path.join(src_root, 'src', 'shaders')
gamut = open(os.path.join(shaders, 'color', 'gamut.hlsl')).read()
gamut = gamut[:gamut.index('float3 GamutCompressLMS(')] + '#endif\n'  # the include guard's
for a, b in [('renodx::color::macleod_boynton::from::WeightedLMS', 'mb_from_WeightedLMS'),
             ('renodx::color::macleod_boynton::WeightedLMSFromMacleodBoynton', 'mb_WeightedLMSFromMacleodBoynton'),
             ('renodx::color::macleod_boynton::from::D65XY', 'mb_D65XY'),
             ('renodx::color::macleod_boynton::WeighLMS', 'mb_WeighLMS'),
             ('renodx::color::macleod_boynton::UnweighLMS', 'mb_UnweighLMS'),
             ('renodx::color::macleod_boynton::BT709_TO_LMS_WEIGHTED_MAT', 'MB_BT709_TO_LMS_WEIGHTED_MAT'),
             ('renodx::color::lms::from::BT709', 'lms_from_BT709'),
             ('renodx::color::bt709::from::LMS', 'bt709_from_LMS'),
             ('renodx::math::DivideSafe', 'DivideSafe'),
             # glslang's HLSL front end crashes on array sizes given by a constant
             ('[CIE1702_EDGE_COUNT] =', '[7] ='), ('[CIE1702_BOUNDARY_POINT_COUNT] =', '[89] ='),
             ('renodx::math::FLT_MAX', 'FLT_MAX'),
             ('#include "./macleod_boynton.hlsl"', '#include "color/mb_shim.hlsl"')]:
    gamut = gamut.replace(a, b)
gamut = re.sub(r'^namespace \w+ \{\n', '', gamut, flags=re.M)
if 'renodx::' in gamut:
    sys.exit('unresolved: ' + re.findall(r'renodx::[\w:]+', gamut)[0])
open(os.path.join(out, 'color', 'gamut.hlsl'), 'w').write(gamut)

# Matrices as literals (RenoDX's rgb.hlsl values; the LMS inverse in double precision): glslang's
# HLSL front end crashes on the shim's function-call initializers (Invert3x3) in this shader.
shim = r'''static const float3x3 BT709_TO_XYZ_MAT = float3x3(
    0.4123907993f, 0.3575843394f, 0.1804807884f,
    0.2126390059f, 0.7151686788f, 0.0721923154f,
    0.0193308187f, 0.1191947798f, 0.9505321522f);
static const float3x3 XYZ_TO_BT709_MAT = float3x3(
    3.2409699419f, -1.5373831776f, -0.4986107603f,
    -0.9692436363f, 1.8759675015f, 0.0415550574f,
    0.0556300797f, -0.2039769589f, 1.0569715142f);
static const float3x3 STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT = float3x3(
    0.2670502842655792, 0.8471990148492798, -0.03470416612462053,
    -0.38706882411220156, 1.165429935890458, 0.10302286696614202,
    0.026727793989083093, -0.02729131667566509, 0.5333267257603284);
static const float3x3 STOCKMAN_CVRL_LMS_TO_XYZ_2DEG_FIT = float3x3(
    1.81146296369, -1.30814926125, 0.370569464061,
    0.606912405731, 0.415906259283, -0.0408482553314,
    -0.0597250591702, 0.0868409009978, 1.8543617735);
static const float3 CIE1702_MB_CIE_WEIGHTS = float3(0.68990272f, 0.34832189f, 0.0371597069161f);
float Max(float3 v) { return max(v.x, max(v.y, v.z)); }
static const float FLT_MAX = 3.402823466e+38f;
float DivideSafe(float a, float b, float f) { return b == 0.f ? f : a / b; }
float3 DivideSafe(float3 a, float3 b, float f) {
  return float3(DivideSafe(a.x, b.x, f), DivideSafe(a.y, b.y, f), DivideSafe(a.z, b.z, f));
}
float3 mb_WeighLMS(float3 lms) { return lms * CIE1702_MB_CIE_WEIGHTS; }
float3 mb_UnweighLMS(float3 w) { return w / CIE1702_MB_CIE_WEIGHTS; }
float3 mb_WeightedLMSFromMacleodBoynton(float2 mb, float l_plus_m) {
  return float3(mb.x, (1.f - mb.x), mb.y) * l_plus_m;
}
float3 mb_from_WeightedLMS(float3 weighted_lms) {
  float y_mb = max(weighted_lms.x + weighted_lms.y, 0);
  float inv = DivideSafe(1.f, y_mb, 0.f);
  return float3(weighted_lms.x * inv, weighted_lms.z * inv, y_mb);
}
static const float3x3 MB_BT709_TO_LMS_WEIGHTED_MAT = mul(float3x3(
    CIE1702_MB_CIE_WEIGHTS.x, 0.f, 0.f, 0.f, CIE1702_MB_CIE_WEIGHTS.y, 0.f, 0.f, 0.f,
    CIE1702_MB_CIE_WEIGHTS.z), mul(STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT, BT709_TO_XYZ_MAT));
float3 xyz_from_xyY(float3 xyY) {
  float3 XYZ;
  XYZ.xz = float2(xyY.x, (1.f - xyY.x - xyY.y)) / xyY.y * xyY[2];
  XYZ.y = xyY[2];
  return XYZ;
}
float2 mb_D65XY() {
  float3 xyz = xyz_from_xyY(float3(0.31272f, 0.32903f, 1.f));
  return mb_from_WeightedLMS(mb_WeighLMS(mul(STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT, xyz))).xy;
}
float3 lms_from_BT709(float3 bt709) { return mul(STOCKMAN_CVRL_XYZ_TO_LMS_2DEG_FIT, mul(BT709_TO_XYZ_MAT, bt709)); }
float3 bt709_from_LMS(float3 lms) { return mul(XYZ_TO_BT709_MAT, mul(STOCKMAN_CVRL_LMS_TO_XYZ_2DEG_FIT, lms)); }
float Neutwo(float x) { return x * rsqrt(mad(x, x, 1.0)); }
float ComputeMaxChannelScale(float3 color) {
  float max_channel = Max(abs(color.rgb));
  float new_max = Neutwo(max_channel);
  return max_channel != 0 ? (new_max / max_channel) : 1.f;
}
'''
open(os.path.join(out, 'color', 'mb_shim.hlsl'), 'w').write(shim)
