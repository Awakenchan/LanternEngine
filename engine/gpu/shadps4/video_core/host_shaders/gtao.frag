// SPDX-License-Identifier: GPL-2.0-or-later

// GTAO_SPATIAL_HQ (graphics_ao_quality=1): GTAO_SLICES evenly rotated directions, rejects samples
// off the screen and keeps sub-texel steps along the slice. GTAO_COMPUTE (compiled as a compute
// shader, HQ only) runs the same math at the full internal resolution after the game's bilateral
// upscale: img0 is the full-resolution depth the upscale samples, the rotation comes from a
// per-pixel hash and the result goes to a storage image.

#version 450
#extension GL_EXT_scalar_block_layout : require

#ifdef GTAO_COMPUTE
layout(local_size_x = 8, local_size_y = 8) in;
layout(scalar, set = 0, binding = 0) uniform Cb0 { uint d[16384]; } ubo_0;
layout(scalar, set = 0, binding = 1) uniform Cb2 { uint d[16384]; } ubo_2;
layout(set = 0, binding = 2) uniform texture2D img0;
layout(set = 0, binding = 3) uniform texture2D img1;
layout(set = 0, binding = 4) uniform sampler samp0;
#define samp1 samp0
layout(rgba8, set = 0, binding = 5) uniform writeonly image2D ao_output;

// buf_off: byte 0 is Cb0's offset, byte 2 Cb2's (the same packing as the fragment shader's).
layout(push_constant) uniform BbPush {
    uvec2 extent;
    float ao_k;
    uint buf_off;
};
#else
layout(scalar, set = 0, binding = 0) uniform Cb0 { uint d[16384]; } ubo_0;
layout(scalar, set = 0, binding = 1) uniform Cb1 { uint d[16384]; } ubo_1;
layout(scalar, set = 0, binding = 2) uniform Cb2 { uint d[16384]; } ubo_2;
layout(set = 0, binding = 3) uniform texture2D img0;
layout(set = 0, binding = 4) uniform texture2D img1;
layout(set = 0, binding = 5) uniform texture2D img2;
layout(set = 0, binding = 6) uniform sampler samp0;
layout(set = 0, binding = 7) uniform sampler samp1;
layout(set = 0, binding = 8) uniform sampler samp2;

layout(location = 0) out vec4 frag_color0;

layout(push_constant) uniform BbPush {
    layout(offset = 80) uint buf_off;
    layout(offset = 120) float ao_k;
    layout(offset = 124) uint scene_size;
};
#endif

const float PI = 3.14159265358979;
const float HALF_PI = 1.57079632679490;
#if defined(GTAO_SLICES) && defined(GTAO_STEPS)
const int SLICES = GTAO_SLICES;
const int STEPS = GTAO_STEPS;
#else
const int SLICES = 1;
const int STEPS = 5;
#endif
const float MIN_RADIUS_PX = 28.0;
const int SECTORS = 32;
const float SECTOR_WEIGHT[32] = float[32](
    0.00240764, 0.00719972, 0.01192247, 0.01653040, 0.02097913, 0.02522583, 0.02922958, 0.03295184,
    0.03635675, 0.03941153, 0.04208675, 0.04435665, 0.04619938, 0.04759718, 0.04853659, 0.04900857,
    0.04900857, 0.04853659, 0.04759718, 0.04619938, 0.04435665, 0.04208675, 0.03941153, 0.03635675,
    0.03295184, 0.02922958, 0.02522583, 0.02097913, 0.01653040, 0.01192247, 0.00719972, 0.00240764);

const float THICKNESS_NEAR = 0.10;
const float THICKNESS_FAR = 2.0;

const float MAX_WORLD_RADIUS = 0.3;

const float DISTANCE_BOOST = 2.0;

const float CONTRAST = 0.5;
const float THICKNESS_BIAS = 0.02;

float cb0(int i) { return uintBitsToFloat(ubo_0.d[((buf_off & 255u) >> 2u) + uint(i)]); }
float cb2(int i) { return uintBitsToFloat(ubo_2.d[(((buf_off >> 16u) & 255u) >> 2u) + uint(i)]); }

float readDepth(vec2 uv) { return textureLod(sampler2D(img0, samp0), uv, 0.0).x; }

vec4 gR0, gR1, gR2, gR3;

vec3 viewPos(vec2 uv, float depth) {
    vec4 h = vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    vec4 r = vec4(dot(gR0, h), dot(gR1, h), dot(gR2, h), dot(gR3, h));
    return r.xyz / r.w;
}

float fastAcos(float x) {
    float a = abs(x);
    float r = sqrt(1.0 - a) * (1.5707963267948966 + a * (-0.2146018 + a * (0.0889789 + a * -0.0501743)));
    return x >= 0.0 ? r : PI - r;
}

uint sectorMask(vec2 uvS, vec3 pos, vec3 v, float side, float lo, float thick, float reach, float bias) {
#ifdef GTAO_SPATIAL_HQ
    if (any(lessThan(uvS, vec2(0.0))) || any(greaterThanEqual(uvS, vec2(1.0)))) return 0u;
#endif
    vec3 d = viewPos(uvS, readDepth(uvS)) - pos;
    float l = length(d);
    if (l > reach || l < bias) return 0u;

    thick *= 1.0 - smoothstep(0.3, 1.0, l / reach);
    vec3 db = d - v * thick;
    float af = side * fastAcos(clamp(dot(d, v) / l, -1.0, 1.0));
    float ab = side * fastAcos(clamp(dot(db, v) / max(length(db), 1e-4), -1.0, 1.0));
    float s0 = clamp((min(af, ab) - lo) * (1.0 / PI), 0.0, 1.0) * float(SECTORS);
    float s1 = clamp((max(af, ab) - lo) * (1.0 / PI), 0.0, 1.0) * float(SECTORS);
    uint a = uint(s0 + 0.5);
    uint n = uint(s1 + 0.5) - a;
    if (n == 0u || a >= uint(SECTORS)) return 0u;
    uint m = n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u);
    return m << a;
}

void main() {
    gR0 = vec4(cb0(0), cb0(1), cb0(2), cb0(3));
    gR1 = vec4(cb0(4), cb0(5), cb0(6), cb0(7));
    gR2 = vec4(cb0(8), cb0(9), cb0(10), cb0(11));
    gR3 = vec4(cb0(12), cb0(13), cb0(14), cb0(15));
#ifdef GTAO_COMPUTE
    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, extent))) return;
    const vec2 fragCoord = vec2(gl_GlobalInvocationID.xy) + 0.5;
    vec2 uv = fragCoord / vec2(extent);
#else
    vec2 size = vec2(cb0(16), cb0(17));
    const uvec2 reduced = uvec2(scene_size & 65535u, scene_size >> 16u);
    const vec2 raster = reduced.x != 0u && reduced.y != 0u ? vec2(reduced) : vec2(1920.0, 1080.0);
    vec2 uv = gl_FragCoord.xy * (vec2(1920.0, 1080.0) / raster) / size;
#endif
    float depth = readDepth(uv);

    float visibility = 1.0;
    vec3 pos = viewPos(uv, depth);

    if (depth < 1.0) {
        vec3 wn = textureLod(sampler2D(img1, samp1), uv, 0.0).xyz * 2.0 - 1.0;
        wn = normalize(wn);
        vec3 n = vec3(dot(vec3(cb0(20), cb0(21), cb0(22)), wn),
                      dot(vec3(cb0(24), cb0(25), cb0(26)), wn),
                      dot(vec3(cb0(28), cb0(29), cb0(30)), wn));
        n = normalize(n);

        vec3 v = -normalize(pos);
        float scale = max(cb0(41), min(cb0(42), cb0(40) / pos.z));
        vec2 texel = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));

#ifdef GTAO_COMPUTE
        // The minimum footprint stays in AO-resolution texels so the occlusion scale matches.
        vec2 radiusUv = max(vec2(cb0(18), cb0(19)) * scale,
                            MIN_RADIUS_PX / vec2(cb0(16), cb0(17)));
        float angle0 = 2.0 * PI * fract(52.9829189 * fract(dot(fragCoord, vec2(0.00583715, 0.06711056))));
        vec2 noise = vec2(cos(angle0), sin(angle0));
        float jitter = fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
#else
        vec2 radiusUv = max(vec2(cb0(18), cb0(19)) * scale, texel * MIN_RADIUS_PX);

        vec2 noise = textureLod(sampler2D(img2, samp2), gl_FragCoord.xy / 64.0, 0.0).xy * 2.0 - 1.0;
        noise = normalize(noise + vec2(1e-5));
        float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
#endif
        float thick = THICKNESS_NEAR + THICKNESS_FAR * clamp(pos.z / cb2(2), 0.0, 1.0);

        float accum = 0.0;
        for (int s = 0; s < SLICES; ++s) {
#ifdef GTAO_SPATIAL_HQ
            float angle = PI * float(s) / float(SLICES);
            vec2 dir = mat2(cos(angle), sin(angle), -sin(angle), cos(angle)) * noise;
#else
            vec2 dir = s == 0 ? noise : vec2(-noise.y, noise.x);
#endif
            vec2 stepUv = dir * radiusUv;

            vec3 full = viewPos(uv + stepUv, depth) - pos;
            float radius = max(length(full), 1e-4);
            vec3 dirV = full / radius;
            stepUv *= min(1.0, MAX_WORLD_RADIUS / radius);

            vec3 ortho = dirV - dot(dirV, v) * v;
            vec3 axis = normalize(cross(ortho, v));
            vec3 projN = n - axis * dot(n, axis);
            float projLen = length(projN);
            if (projLen < 1e-4) {
                accum += 1.0;
                continue;
            }
            float cosN = clamp(dot(projN, v) / projLen, -1.0, 1.0);
            float sgn = dot(ortho, projN) < 0.0 ? -1.0 : 1.0;
            float nAng = sgn * fastAcos(cosN);

            float lo = nAng - HALF_PI;
            float reach = min(radius, MAX_WORLD_RADIUS);
            float bias = THICKNESS_BIAS * reach;
            uint mask = 0u;

            for (int i = 0; i < STEPS; ++i) {
                float t = (float(i) + jitter) / float(STEPS);
                t = t * t;
#ifdef GTAO_SPATIAL_HQ
                // A scalar minimum step keeps the slice direction at sub-texel distances.
                vec2 off = stepUv * max(t, 1.0 / max(length(stepUv / texel), 1.0));
#else
                vec2 off = stepUv * t;
                off = sign(off) * max(abs(off), texel);
#endif
                mask |= sectorMask(uv + off, pos, v, 1.0, lo, thick, reach, bias);
                mask |= sectorMask(uv - off, pos, v, -1.0, lo, thick, reach, bias);
            }

            float open = 1.0;
            while (mask != 0u) {
                open -= SECTOR_WEIGHT[findLSB(mask)];
                mask &= mask - 1u;
            }
            accum += clamp(open, 0.0, 1.0);
        }
        visibility = accum / float(SLICES);

        float farFrac = clamp(pos.z / cb2(2), 0.0, 1.0);
        visibility = pow(clamp(visibility, 0.0, 1.0), 1.0 + DISTANCE_BOOST * sqrt(farFrac));

        visibility = mix(visibility, visibility * visibility * (3.0 - 2.0 * visibility), CONTRAST);
    }

    float ao = cb0(36) * ao_k * visibility + (cb0(37) * ao_k + (1.0 - ao_k));

    float lin = pos.z / cb2(2);
    float p = fract(65025.0 * lin);
    float q = fract(255.0 * lin);
    float b = q - p * (1.0 / 255.0);
    float a = lin - q * (1.0 / 255.0);
#ifdef GTAO_COMPUTE
    imageStore(ao_output, ivec2(gl_GlobalInvocationID.xy), vec4(ao, p, b, a));
#else
    frag_color0 = vec4(ao, p, b, a);
#endif
}
