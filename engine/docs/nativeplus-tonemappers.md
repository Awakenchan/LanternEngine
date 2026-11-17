# Native+ tone mappers

Native+ ([nativeplus.md](nativeplus.md)) has two tone mappers: **Vanilla+** (the default) and **PsychoV**, with its Vanilla+ Blend. This
page says how each is built, why, what was measured (on a model with RenoDX's own code, and in the game), and what
was tried and not kept. The references are RenoDX `b6b32f4` (`test30.hlsl`,
`test17.hlsl`, `DICE.hlsl`, `color/correct.hlsl`, the Cyberpunk 2077, Order of the Sinking Star,
Starfield and FromSoftware Engine mods, `.agents/skills/handle-sdr-tonemap-lut`) and souperman9's
`renodx-bloodborne.addon64` (its shaders, decompiled; its Bloodborne branch is not public).

Each point is marked **[contract]** (required by the tone mapper's own code or comments),
**[RenoDX]** (what RenoDX's mods do), **[deduction]** (this page's reasoning or model),
**[measured]** (an HDR capture in the game) or **[to measure]**.

## The chain

Both tone mappers run in Native+'s replacement of the game's final composite
(`bb_composite_hdr.frag`), on the game's own inputs:

1. **Scene**: the game's HDR scene (RGBA16F, before its tone curve) times its tint, gain and
   vignettes: `x`, linear BT.709, exactly as the game computes it (`bb_composite.glsl`, written
   from the game's shader code for all 1380 variants of its composite).
2. **Curve**: the tone mapper's signal in place of the game's curve (below).
3. **Bloom** `g` (the game's bloom texture, B10G11R11 float, with its tint), added to that signal.
4. **Color matrix** (the game's 3x4 matrix, rows 840..851; desaturation that varies by place).
5. **Roll-off** to the display's peak, in units of the game's white (peak / Game Brightness).
6. The result encoded as the game encodes (its gamma), written as the difference with the game's
   own output into the composite's RGBA16F target, so the target holds the HDR color plus the
   game's grain.
7. **Color table** (`bb_color_grading.frag`): the game's 16^3 table, exactly as the game applies
   it up to white; above white the color divided by its largest channel, graded, scaled back; a
   color outside BT.709 through RenoDX's gamut bridge.
8. Copies, the game's DLAA, the upscaler: all in 16-bit (RGBA16F) in Native+.
9. **Final pass** (`hdr_output.frag`): decode (pure 2.2), BT.709 to BT.2020, what is still below
   0 dropped, times Game Brightness, each channel clipped at the peak, PQ. The HUD is told apart
   by luminance against a copy taken before the game draws it.

## Vanilla+

By **souperman9**, from his RenoDX Bloodborne mod, confirmed in his addon's shaders
**[contract: the addon]**:

- **Curve**: the game's curve below a *scene* value of 0.18, its tangent above, per color
  channel: the scene's range from 0.5 to 2 (lanterns, flames, lit walls) is 1.4 to 4 times the
  game's.
- **Bloom** added unbounded (`+ bloom`, no clamp), then the color matrix.
- **Roll-off**: the menu's grading (RenoDX's user grading), then `CInfinityRollOff` (anchor 0.18,
  strength 1.5) on each channel to the peak.

The game's shadows and hues (its curve works per channel, as the game's own SDR image), bright
mid-highlights; the brightest lights reach the peak early and turn white.

Native+ differs from the addon in where it rolls off (before the color table, the addon after
it, in its LUT pass), in its color table bridge (Exact up to white; the addon compresses the max
channel with `CInfinityRollOff` around the table, in the encoded domain) and in its decoding (2.2;
the addon sRGB or 2.2). These change saturated highlights through a strong table, not the
brightness **[to measure]**.

## PsychoV

RenoDX's **PsychoV-30** (`psychotm_test30`, ported to GLSL and checked against RenoDX's HLSL
below), `hdr_tonemapper=4`, built in four steps, each from a difference seen in the game, and a
fifth for its blacks. Three sliders take it towards Vanilla+: **Vanilla+ Highlights** (step 1,
`hdr_vplus_highlights`), **Hue Correction** (step 4, `hdr_hue_correction`) and **Vanilla+
Shadows** (step 5, `hdr_vplus_shadows`), all 0 to 100 %, 0 by default; in the measurements below,
"both sliders" are Vanilla+ Highlights and Hue Correction, Vanilla+ Shadows at 0. Those two at 100 %
is Native+ 0.9.38.2's PsychoV: Vanilla+'s curve above mid grey, an idea
taken from the FromSoftware Engine RenoDX mod's structure (a game curve continued above mid
grey, then a PsychoV as the display mapping), not its operator.

1. **Signal.** On the scene's luminance `y` (the color's ratios kept), `v = 0.18 (y / grey_in)^cone`,
   `grey_in` where the game's curve gives mid grey (0.18), `cone` the game's log-log slope there.
   PsychoV-30 anchored at `grey_in` with that cone maps a grey exactly as it maps this `v` from mid
   grey with a cone of 1 **[contract: test30's F]**: PsychoV's greys are PsychoV-30's own. Above
   `grey_in`, Vanilla+ Highlights mixes that `v`, in the log, with Vanilla+'s extended curve,
   scaled to meet at `grey_in`. PsychoV-30 tends to the peak only as the light grows without bound
   (half the peak around x = 3); on Vanilla+'s curve its highlights have Vanilla+'s brightness.
   Below `grey_in` (the shadows) the slider changes nothing.
2. **Bloom** added unbounded to that signal, then the color matrix, in Vanilla+'s order. PsychoV-30
   alone took the game's screen blend, `t + clamp(g) (1 - min(t, 1))`: its per-channel clamp turned
   an orange glow yellow-green. Measured on the spires before the sunset, green to red 1.03 against
   Vanilla+'s 0.53, saturation 0.44 against 0.76 **[measured]**.
3. **PsychoV-30** on the scene color whose signal that is (`grey_in (v / 0.18)^(1 / cone)` on the
   luminance), anchored at `grey_in` with the game's slope as `cone_response_exponent`, as
   RenoDX's mods pass a vanilla slope **[RenoDX]**: its compression to the peak, purity, BT.2020
   target and gamut compression, and the menu's sliders as its own parameters.
4. **Hue correction** to Vanilla+'s output for the same pixel, by Hue Correction. As RenoDX's
   `HueOKLab` (`colorcorrect.hlsl`): the (a, b) of PsychoV-30's color moved towards the
   reference's by the strength, then scaled back to its chroma, so only the hue moves; its final
   AP1 clamp is left out (the final pass's BT.2020 makes it moot). RenoDX's mods expose the same as
   a Hue Correction slider (default 100 % in most, through RenoDRT); the Cyberpunk 2077 mod skips
   it for PsychoV, the FromSoftware Engine mod has its own hue emulation inside PsychoV.
   PsychoV-30 compresses each cone's response, so a bright orange (L far above M) moves towards
   yellow and a violet towards pink, as the eye sees a strong light; the game's curve, per
   channel, keeps the artists' hues. In the game the fires had turned yellow: OKLab hue 66 to 68
   degrees between 100 and 400 nits, against Vanilla+'s 55 to 59; with the correction 56 to 61
   against 54 to 58 **[measured]**. Against the game's own SDR image of the same fire (59 degrees
   in the flame body), PsychoV gives 59 at 0 % and 51 at 100 % **[measured]**: 0 % is the game's
   hue in the body, 100 % Vanilla+'s more orange one. The reference is Vanilla+'s *final* color:
   its color before the roll-off made the brightest flames redder than Vanilla+ itself (46 degrees
   at x = 2 against 76) and the red channel passed the peak **[deduction]**.
5. **Shadows** towards the same reference, by Vanilla+ Shadows: below mid grey (fading out between
   0.09 and 0.18 of luminance), the output's luminance moves, in the log, towards Vanilla+'s for
   the same pixel, the color's ratios kept (PsychoV's hue and chroma). PsychoV continues the game's
   slope at mid grey down, so its blacks are deeper or lighter than the game's depending on the
   curve of the place. It works on the output: PsychoV-30's own toe darkens what it is given, so
   moving its input towards Vanilla+'s only went about 40 % of the way **[measured]**. With **Keep
   Deeper Blacks** (`hdr_vplus_shadows_deeper`) it only takes the shadows down: where PsychoV's are
   already deeper than the game's, they stay.

Where the composite has no curve to extend (its curve 3), PsychoV falls back to PsychoV-30 on the
scene.

### Model

A model with RenoDX's own test30 compiled unchanged (`docs/hdr-tools/psychov_check/psychov_model.cpp`): interiors (curve k 2.30, c 0.46: `grey_in` 0.19475, cone 1.1656), peak 1000 nits, Game
Brightness 203, the color matrix left out. PsychoV-30 alone (Native+ 0.9.38.1's tone mapper, with
the game's screen-blended bloom and no hue correction) for reference **[deduction]**. VH: Vanilla+
Highlights.

| Scene x | Game (SDR) | Vanilla+ | PsychoV-30 alone | PsychoV VH 0 % | VH 25 % | VH 50 % | VH 75 % | VH 100 % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.002 | | 0.27 nits | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 |
| 0.01 | 1.4 | 1.40 | 1.04 | 1.04 | 1.04 | 1.04 | 1.04 | 1.04 |
| 0.18 | 33 | 33 | 33 | 33 | 33 | 33 | 33 | 33 |
| 0.5 | 101 | 142 | 106 | 106 | 113 | 119 | 126 | 134 |
| 1 | 166 | 439 | 215 | 215 | 250 | 288 | 329 | 373 |
| 2 | 199 | 819 | 388 | 388 | 483 | 579 | 669 | 748 |
| 4 | 203 | 965 | 595 | 595 | 734 | 839 | 907 | 948 |
| 16 | 203 | 999 | 887 | 887 | 964 | 989 | 997 | 999 |
| Nits gained from x = 4 to 8 | | +29 | +178 | +178 | +160 | +116 | +74 | +44 |
| from x = 8 to 16 | | +5 | +115 | +115 | +69 | +34 | +15 | +7 |

Colors (nits / OKLab hue / chroma per lightness), PsychoV at both sliders' ends:

| | Vanilla+ | PsychoV-30 alone | PsychoV (VH 0, hue 0) | PsychoV (VH 100, hue 100) |
|---|---|---|---|---|
| Flame orange (1, 0.45, 0.08) x 1 | 179 / 55 / 0.223 | 115 / 71 / 0.187 | 115 / 71 / 0.187 | 148 / 55 / 0.185 |
| x 2 | 446 / 76 / 0.192 | 231 / 72 / 0.178 | 231 / 72 / 0.178 | 413 / 76 / 0.160 |
| x 5 | 831 / 104 / 0.177 | 477 / 76 / 0.152 | 477 / 76 / 0.152 | 876 / 104 / 0.071 |
| Violet glass (0.45, 0.3, 1) x 1 | 103 / 290 / 0.330 | 79 / 295 / 0.217 | 79 / 295 / 0.217 | 91 / 290 / 0.212 |
| x 2 | 277 / 300 / 0.246 | 166 / 295 / 0.180 | 166 / 295 / 0.180 | 256 / 300 / 0.148 |
| Dark spire under a strong orange glow (green / red) | 213, 0.66 | 178, 0.96 | 198, 0.62 | 198, 0.64 |

No color is above the peak or outside BT.2020 in PsychoV's output. At both sliders' 0 and
without bloom, PsychoV is PsychoV-30 alone exactly; it differs where the bloom adds light (the
glow stays orange).

### In the game

HDR captures of PsychoV with both sliders at 100 % (Native+ 0.9.38.2's PsychoV) against Vanilla+ (Xbox
Game Bar JXR, 30/10/2026; peak 1037 nits, the same settings at 50 in both presets)
**[measured]**:

- **Moon, Yharnam at night**: 786 nits (median of the disc) against Vanilla+'s 870. The moon is
  between 1 and 3 times the game's white in the scene, where PsychoV's shoulder sits below
  Vanilla+'s (373 against 439 nits at x = 1 in the model).
- **Sun at sunset**: 1017 nits against 915. Vanilla+ rolls off each channel: an orange sun's
  blue stays low and its luminance tops out under the peak; PsychoV compresses the luminance itself
  to the peak.
- **Deep shadows** depend on the place. PsychoV continues the game's slope at mid grey downwards
  (its own log-log slope is about 1.04 times that in the blacks); the game's own curve is a
  straight line at the bottom (slope 1). Where the slope at mid grey is above 1 the blacks are
  deeper than Vanilla+'s (Yharnam at night: 25th percentile 2.0 against 2.8 nits); where it is
  below 1, lighter (a dark interior: median 0.017 against 0.012 nit) **[measured]**. The game
  changes its curve by place and by progress; a test build's curve log found two so far
  (`y = 1 - exp(-k x)`, `f = y (c (y - 1) + 1)^2`) **[measured]**:

  | Curve | Places | Mid grey at scene | Slope there | PsychoV's blacks |
  |---|---|---:|---:|---|
  | k 1, c 0 | Byrgenwerth; Yharnam after Father Gascoigne | 0.1985 | 0.904 | lighter than the game's |
  | k 2.222, c 0.428 | Hunter's Dream, Forbidden Woods; Yharnam before Gascoigne | 0.1917 | 1.133 | deeper |

  This is PsychoV-30 anchored as RenoDX's skill says (output mid grey and the cone there), which
  does not force PsychoV to the game's own crush in the shadows **[RenoDX]**.
- **Fires and lanterns**: the hue is Vanilla+'s (above); the chroma stays PsychoV-30's, about 18 %
  lower around 100 to 400 nits (fire 0.139 against 0.168). That is PsychoV-30's purity near the
  peak; the Saturation slider raises it.

So, against Vanilla+: at Vanilla+ Highlights 100 %, PsychoV-30's shadows, Vanilla+'s brightness
for the brightest lights, mid-bright sources (the moon, lamps) about 10 % lower, more gradation
near the peak (+44 against +29 nits from x = 4 to 8), softer colors near the peak. At 0 %,
PsychoV-30's whole curve: half Vanilla+'s nits around the game's white (215 against 439 at x = 1),
much more gradation near the peak (+178 nits from x = 4 to 8). In between, the table above; the
Highlights slider (PsychoV-30's own highlight grading) at 60 to 70 gives 290 to 377 nits at x = 1
too, through PsychoV-30's shoulder instead of Vanilla+'s curve.

## PsychoV-30's integration

Kept in PsychoV, which calls the same port:

- **The game's slope as `cone_response_exponent`**. test30 takes its cone
  power as `contrast * cone_response_exponent` but its purity as `purity_scale / contrast`
  **[contract]**. Native+ passed the game's slope (1.05 to 1.17 in the scenes measured) as
  `contrast`, so it also took purity down by that much at neutral settings. RenoDX's mods pass a
  vanilla slope as `cone_response_exponent` (Cyberpunk 2077: `psychovConeResponse`; Order of the
  Sinking Star: `SinkingStarResolvePsychoVConeResponse`) **[RenoDX]**. Measured on RenoDX's own
  code: the luminance unchanged, the colors about 10 % more saturated (flame orange 0.787 to
  0.870, blood red 0.871 to 0.927, moon blue 0.603 to 0.648).
- Anchor and slope: `V(grey_in) = 0.18` and the game's `x V'(x) / V(x)` there, the skill's
  output-midgray anchor and cone formula **[RenoDX]**; measured on the output: V = 0.18000, slope
  1.1657 for the game's 1.1656 (interiors), 1.0461 for 1.0459 (outdoors).
- Compression `h = 1`: test30's default and Order of the Sinking Star's **[RenoDX]** (Cyberpunk
  2077 uses the automatic h). Not changed.
- BT.2020 target, full gamut compression, BT.709 source boundary: test30's defaults and the HDR
  mods' **[RenoDX]**. Negative BT.709 channels kept signed to the final pass, which converts to
  BT.2020 before dropping what is still negative.
- Gamma: the chain encodes with the game's gamma and decodes with 2.2, as RenoDX's Gamma
  Correction 2.2; the peak needs no pre-correction **[deduction]**.
- Not a cause of any difference: the color table bridge returns the color unchanged with an
  identity table (largest relative change 4e-16 on a grey ramp up to 6x white), the 16-bit chain
  keeps values above white through every pass, and the final pass only encodes.

## Tried and not kept

- **PsychoV-30 alone** (Native+ 0.9.38.1's tone mapper, kept as PsychoV's curve and colors): its bloom
  clamped per channel (glows turned yellow-green). PsychoV is the same with the bloom fixed.
- **Vanilla+'s hues by default in PsychoV**: fires orange, but PsychoV-30's own hue is the
  game's in the flame body (above); it is the Hue Correction slider's 100 % instead.
- **PsychoV+ as a third tone mapper** (Native+ 0.9.38.3 before its release): PsychoV with Vanilla+'s
  curve and hues. It is PsychoV with both sliders at 100 %, so the sliders replaced it.
- **PsychoV-17** (RenoDX's `psychotm_test17`, ported and checked within 7e-6, set as RenoDX's
  Starfield mod sets it): on greys PsychoV-30's curve exactly, its colors a little paler in the
  brightest saturated lights; the same as PsychoV-30 in the game. Removed before release.
- **DICE** (RenoDX `DICE.hlsl`) in place of Vanilla+'s roll-off: about as bright as Vanilla+,
  flat at the peak from x = 4, saturated colors' channels 2 to 3 times the peak (clipped).
- **The FromSoftware Engine mod's own operator** (Ritsu; Elden Ring, Nightreign, Sekiro, Armored
  Core VI: a per-cone Reinhard with the luminance restored and Neutwo on the largest channel),
  compiled unchanged on Vanilla+'s extended curve: brighter than Vanilla+ up to x = 4 (927 against
  819 nits at x = 2), flat at the peak above it, flame saturation 0.49 at x = 5. PsychoV (Vanilla+ Highlights) keeps its
  structure with PsychoV-30.
- **PsychoV-30 after Vanilla+'s bloom and matrix with the game's curve below mid grey**, and
  **PsychoV-30 driven to Vanilla+'s luminance**: Vanilla+'s shadows (not PsychoV-30's), whiter
  colors (flame saturation 0.37 and 0.35 at x = 5).

None was taken as better for reaching the peak more often.

## Checks run

- Engine build, settings tests (`hdr_tonemapper` 4 kept, 7 read as 4 with both sliders at 100 %,
  0, 2, 3 and 6 read as Vanilla+; `hdr_vplus_highlights` and `hdr_hue_correction` 0 by default,
  kept per preset, clamped to 0..100, a Hue Correction below 0 ignored; `hdr_vplus_shadows` and
  `hdr_vplus_shadows_deeper` 0 and off by default, kept per preset), the host shader
  configurations compiled and validated.
- `docs/hdr-tools/psychov_check/run.sh`: the GLSL PsychoV-30 against RenoDX's HLSL compiled
  unchanged, on 791 colors (grey ramp 1e-4..1e4, 18 hues at 40 levels, out-of-gamut, extremes):
  max difference 2e-4 of the color's largest channel over 9 configurations; no NaN or infinite
  value; the grey ramp monotonic and continuous. Then `psychov_model.cpp`: the tables above.
- HDR captures in the game, Vanilla+ against PsychoV (both sliders at 100 %) at each step (above).
- The game's color table, read back from the GPU (a test build): a 16x16x16
  `R32G32B32A32Sfloat` table that grades the image a little, mean change 4.6/255 against a neutral
  table, largest 18/255, slightly warm (white 255, 251, 243; mid grey 137, 138, 134 for 136)
  **[measured]**. The game stops drawing its grading pass at times (loading screens, menus, some
  places); Color Grade Strength then has nothing to change.

**Not run:** RenderDoc captures of each stage; the curve's parameters (832, 833) of every place
(those of the table above only).

## Capture procedure

Same save, same place, same camera, the game's Brightness at 5, the same Native+ settings except
the tone mapper; no upscaler first, then the one you use.

1. Places: a lantern in a dark street (Central Yharnam), a fire, the moon (Hunter's Dream /
   Cathedral Ward), the sun at the Grand Cathedral, a dark interior.
2. For Vanilla+ and PsychoV (its sliders at 0 then 100 %), Bloom 50 then 0:
   an HDR screenshot (Xbox Game Bar, Win+Alt+PrtScn: a JXR) for nits; read the luminance and the
   largest channel separately (they differ for colors: a red at 1000 nits in its red channel is
   about 260 nits of luminance), and the hue in OKLab.
3. One frame with RenderDoc (Vulkan) per tone mapper: the composite draw (`bb_composite_hdr`: its
   scene input `x` and bloom input `g` texels on the lantern, its RGBA16F target), the color table
   draw (input / output), the final pass (PQ output). Note the composite's constants 832, 833
   (curve) and 840..851 (matrix) for that place.
4. Send `logs/last_session.log`.
