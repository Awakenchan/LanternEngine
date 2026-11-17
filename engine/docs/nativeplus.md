# Native+ HDR

`hdr_mode=nativeplus` (the launcher's HDR source "Native+", applied on restart) keeps the light of
the game's scene all the way to the screen and tone maps it to the display's peak, with an in-game
menu laid out like a RenoDX mod. Two tone mappers:

- **Vanilla+** (default), by **souperman9**, from his RenoDX Bloodborne mod: the game's own image,
  with brighter highlights up to the display's peak.
- **PsychoV**: RenoDX's PsychoV-30 with the game's bloom kept: deeper blacks, softer highlights
  with the most detail near the peak. Its Vanilla+ Blend (Vanilla+ Highlights, Vanilla+ Shadows,
  Hue Correction) takes it towards Vanilla+'s brightness, blacks and hues.

It works with the upscalers and with DLSS and FSR 3.1 frame generation. How each tone mapper is
built and measured: [nativeplus-tonemappers.md](nativeplus-tonemappers.md). `native` and `expand`
([hdr.md](hdr.md)) are unchanged by it; with Native+, they are off.

Native+ is written by LRF17, largely with an AI coding assistant (Claude), and tested by a few
players. It reuses RenoDX's MIT-licensed code (PsychoV-30, its grading, hue correction, roll-off
and gamut bridge) and souperman9's Vanilla+; it is not a RenoDX mod and is not affiliated with or
endorsed by RenoDX or its authors.

## Use

1. HDR on in Windows (the Windows **HDR Calibration** app, run once, gives the display's real
   peak), **HDR output** on in the launcher's Display page, **HDR source** Native+. Entering or
   leaving Native+ needs a game restart.
2. The game's own **Brightness** option at **5**: it still changes the whole image, shadows first.
3. The in-game menu (Insert or L3+R3): the HDR section's first line says **Native+: active**.
   Every setting there applies live and is saved in `lantern.ini`.

| Setting | Values (default) | What it does |
|---|---|---|
| Preset | Off, #1 to #3 (#1) | Off is the game's own SDR look, shown in HDR, without HDR highlights. Each numbered preset keeps its own settings (`hdr_preset`, `hdr_preset_N`). |
| Settings Mode | Simple, Intermediate, Advanced (Simple) | How many settings the menu shows, as in RenoDX's mods; a hidden setting keeps its value. |
| Tone Mapper | Vanilla+, PsychoV (Vanilla+) | `hdr_tonemapper` 5 or 4. |
| Peak Brightness | 400 to 4000 nits (the display's) | `hdr_peak` / `hdr_peak_auto`, as the other modes. |
| Game Brightness | 80 to 500 nits (203) | `hdr_paper_white`. |
| UI Brightness | 48 to 500 nits (203) | `hdr_ui_nits`: the HUD and menus' white, whatever Game Brightness. |
| Exposure, Highlights, Shadows *(Intermediate)*; Contrast, Saturation | 1.00, 50, 50, 50, 50 | Vanilla+: RenoDX's user grading. PsychoV: PsychoV-30's own parameters. Neutral at these values. |
| Vanilla+ Highlights *(PsychoV, Vanilla+ Blend, Intermediate)* | 0 to 100 % (0) | PsychoV's highlights from its own curve (0) to Vanilla+'s brightness (100). Shadows and colors unchanged. |
| Vanilla+ Shadows *(PsychoV, Vanilla+ Blend, Intermediate)* | 0 to 100 % (0) | PsychoV's shadows from its own curve (0) to the game's (100: Vanilla+'s blacks). Highlights and colors unchanged. `hdr_vplus_shadows`. |
| Keep Deeper Blacks *(PsychoV, Vanilla+ Blend, Intermediate)* | Off, On (Off) | With Vanilla+ Shadows above 0, it only darkens: where PsychoV's blacks are deeper than the game's, they stay. `hdr_vplus_shadows_deeper`. |
| Hue Correction *(PsychoV, Vanilla+ Blend, Intermediate)* | 0 to 100 % (0) | How far the colors take Vanilla+'s hues (RenoDX's hue correction): 0 keeps PsychoV-30's, 100 Vanilla+'s, more orange fires. |
| Color Grade Strength *(Advanced)* | 0 to 100 % (100) | The game's own color table: 100 is the game's look, 0 the image before it. Some places have none. |
| Black Floor *(Advanced)* | 0 to 2 nits (0) | Takes the game's darkest greys down to true black (OLED). |
| Bloom | 0 to 100 (50) | 50 is the game's, 0 removes it. |
| Compare *(Advanced)* | HDR, SDR | The game's own SDR picture; **F9** with the menu closed. Not saved. |

In Native+, the HDR settings replace the Picture section's Brightness, Contrast, Saturation and
Bloom (hidden there) and the game's PC Colour menu's (`graphics.cpp` leaves the game's own picture
as drawn). The launcher hides Highlight boost and shows the Tone mapper.

## How it works

- **16-bit chain.** The game draws its final image in 8 bits, clipped at white. Native+ replaces
  the game's final composite (`bb_composite_hdr.frag`, from the game's shader code for all 1380
  variants of its composite, `bb_tonemap_variants.h`): the scene is tone mapped there, and every
  pass after it (the color table, the game's DLAA, the upscaler, the display copy) runs in
  RGBA16F, so the light above white reaches the screen (`vk_hdr_channel.cpp`). The final pass
  (`hdr_output.frag`) converts to HDR10, or scRGB where the display offers no HDR10.
- **Mid grey.** Both tone mappers start from where the game's own curve (constants 832..834 of the
  composite's constant buffer, read each frame) gives mid grey, and its slope there; the game
  changes its curve by place, the tone mappers follow it.
- **Gamma.** The chain decodes the game's image with a pure 2.2 power, as RenoDX's Gamma Correction
  2.2.
- **Color table.** Applied exactly as the game does up to white (`bb_color_grading.frag`). Above
  white, and for colors outside BT.709 (RenoDX's gamut bridge, `gamut_bridge.glsl`), the color is
  brought inside the table's range, graded, then taken back out.
- **HUD.** Told apart from the scene by comparing the frame with a copy taken just before the game
  draws its HUD; shown at UI Brightness.
- **Upscalers and frame generation.** The upscalers get the 16-bit scene, decoded to linear
  light and encoded again after, through Lantern's own path (`RunScaled`, its edge clamp against
  the jitter included; `vk_temporal_upscaler.cpp`, `hdr_upscale_transfer.comp`). The game pads its render targets'
  width to a multiple of 128 (the PS4's macro tile): the size checks allow it, else some outputs
  (1440p Quality, 13 of the launcher's 75) stayed 8-bit. DLSS and FSR 3.1 frame generation
  interpolate the HDR frames; FSR 3.1's generated frames tell their HUD from FSR's interpolated
  frame without it, so it is at UI Brightness too. The co-op portrait reads Native+'s RGBA16F frame.
- **Push constants.** Native+'s passes need 176 bytes (`PushData`, `bb_hdr` and `bb_hdr2` after
  the first 128); checked at startup.

### Shaders

Native+'s replacements of the game's composite, color table and copies are built for each way the
game shader can be compiled: inputs read per vertex (fragment-shader barycentrics, NVIDIA) or
interpolated (AMD, Intel), and the composite's constants as a storage buffer (legacy compiler) or a
uniform buffer (native compiler). Each replacement takes the game shader's layout (its bindings,
which inputs are read per vertex, its render target) from what the shader was compiled from (Info,
RuntimeInfo, the first Bindings), as `EffectShaders::Fits` does, not from SPIR-V names: the lifter
writes none. That layout is what a cache record keeps (InfoPersistent, the spec's runtime info and
start), so a second start, from the cache, finds the same; per-vertex inputs come from the SPIR-V's
`PerVertexKHR` decorations, render target 0 is location 0. The composite variants and the chain
passes keep the recompiler's SPIR-V.

The shader cache holds the game's own shaders; Native+ swaps its replacements in when they load
(`vk_pipeline_serialization.cpp`), so every mode, the shader catalog and `bb-shader-lab.exe` share
it and switching the HDR source does not rebuild shaders. With `BB_UBO_DWORDS=0` or
`BB_UBO_EXACT=1`, Native+ keeps the legacy compiler. In the log, "left in SDR: no constant
buffer", "has another interface" or "its names and its layout differ" mean a game shader Native+
could not match.

## Log

`Native+:` and `HDR pass:` lines: which replacement each game shader got, which targets are
RGBA16F, and why one was left in SDR. `BB_HDR_DEBUG=1` shows the pixels taken as the HUD in red.

## Settings carried over

Settings from earlier versions of the Native+ fork are read so that its players keep their look:
`hdr_mode=psychov30` as `nativeplus`; `hdr_tonemapper` 7 (0.9.38.2's PsychoV with Vanilla+'s
curve) as PsychoV with Vanilla+ Highlights and Hue Correction at 100 %, 0, 2, 3 and 6 as Vanilla+;
Target gamut, Gamut Compression, Compression and Color Table Bridge ignored.

## Tests

`settings-test` covers the HDR settings (defaults, presets, limits, the settings carried over).
`docs/hdr-tools/psychov_check/run.sh` checks the GLSL PsychoV-30 against RenoDX's HLSL compiled
unchanged (791 colors, max difference 2e-4 of the largest channel), `docs/hdr-tools/gamut_bridge_check`
the gamut bridge against RenoDX's: both need a RenoDX checkout.

## Known issues

- The HUD over very bright areas (the sun, a lamp) is brighter than UI Brightness: over
  highlights the HUD and the scene blend. RenoDX's mods show the same.
- AMD with FSR 4: a thin light line on the right and bottom edges was seen before Native+ went
  through Lantern's edge clamp; if it shows again, Sub-pixel jitter off removes it.
- ReShade add-ons ReLimiter and Display Commander crash Lantern 0.9.39 and later at startup (in every mode).
  ReShade presets made for SDR give wrong colors on the HDR image.

## Credits

- **Native+ HDR** by LRF17.
- **Vanilla+** by **souperman9** (RenoDX Bloodborne mod).
- **RenoDX** by Carlos Lopez (ShortFuse) and contributors, MIT: PsychoV-30, the user color grading,
  the hue correction, `CInfinityRollOff`, the gamut and LUT bridge, ported to GLSL
  ([clshortfuse/renodx](https://github.com/clshortfuse/renodx)).
