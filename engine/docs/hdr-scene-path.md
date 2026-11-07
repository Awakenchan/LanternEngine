# HDR from the scene: the next HDR mode (plan, 2026-10-21)

Players who compared HDR mods rank the RenoDX style first. What they see is not a particular
tone curve but where the HDR comes from: the scene itself, tone mapped once for the display,
carried in 16 bits to the screen. Native mode (`hdr.md`) works the other way round: it takes the
game's finished 8-bit SDR frame and estimates how much brighter each highlight should be (scene
luma, a learned tone curve, 8x6 exposure tiles, UI found by difference). That keeps the game's
look exactly, but its limits come from the estimate:

- highlights are 8-bit values multiplied: steps in gradients near lamps and the sky widen with the
  gain (banding), and a highlight clipped to white in SDR stays white (its colour is gone);
- bloom, glare and depth of field are only in the SDR frame, so their brightness comes from the
  learned curve, which needs frames to learn and ends where SDR clips;
- the UI is found by comparing two copies of the frame; exposure is per tile, not per pixel.

This plan is a mode of our own ("scene") that removes the estimate: the game's composite shader
makes the HDR picture itself, and the frame stays 16-bit from there to the display.

## What a fork does (looked at, not used)

Cham17-dev/LanternEngine (branch `lantern-psychov30`, a patch over 0.9.36) does this with RenoDX's
PsychoV30 tone mapper:
- the 1418 variants of Bloodborne's final composite pixel shader in `eboot.bin` (bloom, tone
  curve, colour matrix, grain, gamma) were modelled offline with a GCN emulator script into a
  1418-entry table (curve family, pre-exposure, vignettes, matrices, which texture is which), and
  one hand-written GLSL composite reproduces each variant and adds PsychoV30's HDR on top;
- the targets after the composite (its copy for the AA, the colour grading copy, the display
  buffers) become RGBA16F, found by shader hash and address;
- the game's colour table is applied above white by normalising to the brightest channel;
  colours outside BT.709 are carried signed to the final pass (BT.2020 output);
- native shaders (uniform constant buffers) are turned off in that mode, and push constants grow
  to 176 bytes.

We take the lesson (scene-referred, 16-bit chain), not the code or the tone mapper. Our way
differs where our engine is stronger: the shader compiler and the shader captures.

## Census: which composites players actually run

The shader captures (`bb-shader-lab`, `logs/shaders`) against that table of 1418 composite hashes:

| capture set | pixel shaders | composite variants run |
|---|---|---|
| a player's (857 captures) | 285 | 5: `5c02eaf8`, `5e67d866`, `925d5712`, `cbe2dd84`, `ccb08339` |
| ours (411 captures) | 175 | 2: `5e67d866`, `ccb08339` |

All five are branch-free, 212-243 GCN dwords, and apply the curve per channel without
pre-exposure: an exponential curve (`1 - exp(-k x)`, constant buffer words 832-833) in four, an
extended Reinhard curve in `cbe2dd84`. The rest of the frame's chain is in the captures too:
colour grading `ed4c7668`, the second glow `8c08444c`, the upscaler hook `9a9cf8a9`, the UI's
vertex shader `34e8a281`. So a scene mode has to handle a handful of composites, not 1418, and
the lab can check every one it meets. A composite it does not recognise keeps native mode for that
frame (logged), so an area nobody has captured yet is never worse than today.

## Design

### 1. The curve, changed in the compiler (native compiler pass)

A pass of the native compiler (`shader_recompiler/native/`, see `native_shader_compiler.md`)
recognises the tone curve in a composite's IR: the scene sample's colour, scaled by a constant,
into `exp2` of its negation and `1 -` that (exponential), or `x / (1 + x)` with the white point
(Reinhard). It replaces only that expression with our curve:

- identical to the game's curve below a knee (the same constant buffer words, the same operations:
  the midtones and shadows are bit for bit the game's, as in native mode);
- above the knee, continuous in value and slope, an extension that reaches the display's peak
  (on the paper-white scale: `peak / paper`) with the roll-off native mode already uses (linear in
  log2 to 70% of the stops to the peak, then each doubling gets a smaller share of the room left);
- its parameters (knee, peak over paper white, strength) in the push data, so the menu changes
  them live without a recompile.

Bloom is added to the scene before the curve in these composites, so halos rise with their lamps
on their own: no learned curve. Vignette, colour matrix, grain and gamma after the curve stay the
game's; the composite's output may now exceed 1.

Not recognised (another curve family, pre-exposure, a branch): the shader stays as the game made
it, and the frame uses native mode.

### 2. The 16-bit chain

The composite's colour target and every target the frame derives from it up to the display copy
(the AA copy, the colour grading's copy, the second glow's target, the UI composite, the display
buffer) are created RGBA16F instead of RGBA8. The chain is found from the frame's own passes, not
from a list of hashes: the texture cache follows which image a pass reads and which it writes,
starting at the composite's target (the draw the curve pass marked), and the result is kept per
guest address and size from the next frame on (a resize or a new area starts it again). Pipelines
of those passes get the 16-bit attachment format (part of the pipeline key, as today).

### 3. After the composite

- **Colour table** (`ed4c7668`): the game's 3D table grades the colour clamped to SDR; above white
  the ratio of the colour to its clamped version is multiplied back, so a highlight keeps the
  table's tint where it leaves SDR and its brightness from the scene.
- **Second glow** (`8c08444c`): a blur of the half-size glow targets added after the tone curve.
  It adds onto the 16-bit frame as it is; its own brightness stays SDR (a soft haze, measured in
  step 3 whether it needs the same ratio).
- **UI**: drawn by the game into the 16-bit frame at values up to 1, so it lands at paper white
  by itself: no UI detection, no UI-free copy.
- **Upscalers**: their input is the scene before the composite, so they are unchanged; the
  composite's output is already output-sized (the hook `9a9cf8a9` is before it).
- **Encode** (`hdr_output.frag`): the 16-bit frame times paper white, capped at the peak, in HDR10
  or scRGB. Frame generation stays off with HDR, as now.

Wide gamut (BT.2020 colours beyond BT.709) is left for later: the game grades in BT.709 and its
colour table is BT.709.

### 4. Settings

`hdr_mode=scene` beside `native` and `expand` (default unchanged until players have tried it),
with the same peak, paper white and strength; the menu's Compare split works as now. Native
shaders stay on: the curve pass works on our IR, not on a replacement shader.

## Checks

- **Lab** (`bb-shader-lab`): the curve pass on every capture: which composites it recognises
  (5 of 5 on today's captures), and that it changes nothing in any other shader.
- **GPU ramp test** (beside `hdr-native-shader-test`): the game's composite and the changed one
  compiled from the same capture, fed a ramp of scene values: equal below the knee (bit for bit),
  monotonic above, continuous slope at the knee, reaching the peak only asymptotically; with the
  extension off, equal everywhere.
- **Chain identity**: with the extension off, the 16-bit chain must show the SDR picture within
  1/255 of the 8-bit one (frame capture compare): the chain alone changes nothing.
- **In game** (Compare split): Central Yharnam lamps and fire, the moon in Cathedral Ward, the
  sky gradient (banding), Ebrietas' particles, a menu over a bright scene.

## Steps

1. Lab: composite recognition by IR pattern, reported per capture (no code change in the game).
2. The curve pass and the GPU ramp test.
3. The 16-bit chain with the chain-identity check (extension off).
4. The colour table's ratio and the encode of the 16-bit frame.
5. `hdr_mode=scene` in the menu and launcher; player tests; then the default.

Steps 1-2 belong with the native compiler work (its IR, its lab); 3-4 are the texture cache and
the HDR passes.

## Steps 1-2: done (2026-10-21, off by default)

**Recognition** (`shader_recompiler/native/tone_curve.{h,cpp}`, `FindToneCurves`): per colour
channel, the clamp (`FPSaturate32`) of one of the two shapes below, from the scene value clamped
at 2^-14 (`FPMax32(#6.1035156e-05, x)`), with its constants read from the constant buffer:

- exponential: `A * B * B`, `A = 1 - exp2(#1.442695 * (-x * k))`, `B = 1 + c * (#-1 + A)`
  (k, c: words 832, 833);
- extended Reinhard: `(k x + (k x) (w (k x))) * recip(k x + 1)` (k, w: words 833, 834).

Operands are matched in either order. A composite is three such channels of one family with the
same words and three scene values.

`bb-shader-lab --curves <folder>` (each capture in a child process, `--native` as the engine):

| capture set | composites recognised | other shaders with curve channels | other shaders changed |
|---|---|---|---|
| player (856) | 5 of 5: `5c02eaf8`, `5e67d866`, `925d5712`, `ccb08339` (exponential), `cbe2dd84` (Reinhard) | 0 | 0 |
| development install (412) | 2 of 2: `5e67d866`, `ccb08339` | 0 | 0 |

"Changed" compares each shader's SPIR-V with the pass on and off. Two large compute shaders
(`2da7fe60`, `42f2a521`, the two the native compiler census found unstructured) give different
SPIR-V from one run of the inherited compiler to the next, with or without the pass. The lab
compiles them a third time to tell this apart from a change; they are reported, not failed. The 18
player captures that do not compile are hull and domain shaders of an older capture format.

**The pass** (`ExtendToneCurve`, in `TranslateProgram` before the shader info is collected; only
with `BB_HDR_SCENE_CURVE=1`): each channel's clamp is replaced in its uses by
`select(above, extension, game)`:

- the knee is given as the curve's output (push data, 0.05-0.99). The pass solves it per draw from
  the game's own constants: for the exponential curve, five Newton steps on `(1-e)(1-ce)^2 = knee`;
  for Reinhard, the quadratic `w v^2 + (1-knee) v - knee = 0`. From that come the knee's position on
  the curve's own scene axis, its value and its log2-log2 slope;
- `above` = enabled, peak over paper white > 1, and this pixel's scene value past the knee. Below
  it the select takes the game's instruction itself: bit for bit;
- above it: a straight line in log2-log2 with the curve's slope at the knee, then native mode's
  roll-off (unchanged to 70% of the stops to the peak, then towards it with the same slope, each
  doubling taking a share of the room left). Strength mixes that with the game's clamped value in
  log2, so the curve tends to `peak^strength`. Value and slope are continuous at the knee for any
  strength;
- the clamps the curve's values reach on their way to the output become `clamp(v, 0, upper)` with
  `upper` 1 unless a channel reaching it is above the knee: the same `FClamp` to 1 otherwise;
- in a screen blend with the glow (`a + g - a g`), the product takes `min(a, 1)`: above white the
  glow no longer darkens a highlight (for a <= 1 it is the same value).

Parameters (`ToneCurvePush`: knee, peak / paper white, strength, enabled) follow the 128 bytes of
push data. They are declared only by modules that use them (`Info::uses_tone_curve`, kept in the
cache record's tail), and pushed only for their pipelines, whose push range grows to 144 bytes.
Other pipelines are unchanged. Values come from the HDR settings at each draw (peak and paper
white, strength = sqrt(boost) as in native mode, enabled with an HDR output; knee
`BB_HDR_SCENE_KNEE`, 0.75), so the menu's sliders apply live. With the variable set, shader caches
are kept in their own folder (`<serial>-scene-curve`), so turning it on or off never mixes
modules. Nothing changes in the game yet: the composite's target is still 8-bit (step 3).

**GPU ramp test** (`tone-curve-shader-test`, `ninja -C out/gpu tone-curve-shader-test`):
`bb-shader-lab --ramp-modules <capture> <folder>` compiles a composite twice, as the game's and
extended. In both it feeds the curve a ramp of scene values over a 4096-pixel row (2^-10 to 2^14)
and writes the three channels out. The test draws both on the GPU (RTX 5060), with the curve's
constants set in the constant buffer: exponential k 1 and 2, c 0.1 and 0; Reinhard k 1, w 1/16
and 0. Knees 0.6, 0.75 and 0.85, peaks 2.5, 5 and 10, strengths 0.5 and 1:

- extension off: equal to the game's module everywhere, bit for bit;
- below the knee: bit for bit;
- above: monotonic, log-log slope continuous at the knee (within 5%, no step in value), below
  `peak^strength` over the whole ramp.

All checks pass for the five player composites and the two of the development install.

Steps 3-5 (16-bit chain, colour table and encode, settings) are not started.

## GT7 tonemapper (selectable, SDR)

`shader_recompiler/native/gt7_tonemap.{h,cpp}` (`ApplyGt7ToneMap`) ports the Gran Turismo 7
tonemapper (aletschfirn/gt7tonemapper, MIT) into the same composites, found by `FindToneCurves`:
`15fa3515` (seen since in the skin captures), `5c02eaf8`, `5e67d866`, `925d5712`, `cbe2dd84`,
`ccb08339`. It runs whenever `BB_HDR_SCENE_CURVE` is not set.

- Input: each channel's scene value in the curve's units (`k x`), times a gain that makes GT7 meet
  the game's curve at 0.3 (from the game's own constants), times the exposure setting.
- GT7 (SDR target 2.5, SDR correction 0.4): Rec.709 to Rec.2020, the curve per channel, the ICtCp
  chroma fade and blend, back to Rec.709, clamped 0-1. It uses the curve's ICtCp intensity for the
  scaled colour, where the ReShade port uses the input's.
- Channels: Rec.709/2020 and ICtCp are not symmetric in R, G and B, so each clamp needs its colour.
  It is the output component the clamp alone reaches. Where the composite mixes them before export
  (`5c02eaf8`: glow screen blend, then a 3x3 colour matrix), it is the clamps' order in the block
  (the shader's x, y, z) through the colour buffer's export swizzle, which gives the same answer
  for the composites that do not mix.
- Each clamp's uses become `select(enabled, gt7, game)`. `enabled` and exposure come from push
  data (`Gt7Push`, after the 128 bytes, as `ToneCurvePush`), set at each draw from
  `graphics_tonemapper` (`original`, `gt7`) and `graphics_gt7_exposure` (0.25-4), so the
  overlay's Tonemapper drop-down and GT7 exposure slider switch live. With Original the game's
  instruction is taken bit for bit.
- Cache: a module that went through the pass carries a tag; a cached known composite without it is
  compiled again once.