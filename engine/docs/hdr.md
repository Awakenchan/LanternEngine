# HDR output

`hdr=1` in `lantern.ini` (`BB_HDR=1`, the menu's "HDR output", applied on restart) presents the
game in an HDR swapchain when the display offers one (Windows HDR on): HDR10 (BT.2020, PQ,
`A2B10G10R10`) or scRGB (linear BT.709 in FP16). The game itself only makes an 8-bit SDR frame;
`hdr_output.frag` encodes it. SDR white is shown at `hdr_paper_white` nits (10-500, default 203, BT.2408's reference white),
nothing goes above the peak. The settings menu is drawn into its own layer and composed at paper
white. SDR is decoded with a 2.2 power curve (the piecewise sRGB curve lifts Bloodborne's shadows
into grey).

The peak is the display's by default (`hdr_peak_auto=1`, the menu's and launcher's "Peak
brightness from the display"): the window asks DXGI about the output its monitor is on
(`IDXGIOutput6::GetDesc1` MaxLuminance: the Windows HDR Calibration app's value once it has run,
else the display's EDID), at start and when the window moves to another display (`window.cpp`
`UpdateDisplayPeak`; `dxgi.dll` is loaded, not linked). The log says `HDR output: the display
reports a peak of N nits`. With auto off, or a display that reports nothing, `hdr_peak` (10-4000,
default 1000) is used; `BB_HDR_PEAK` sets it for one run and turns auto off.
`BbSettings::HdrPeakNits` is the one place that decides.

How the highlights get above paper white is `hdr_mode` (`BB_HDR_MODE`, the menu's "HDR mode",
live):

| mode | highlights from | midtones | UI |
|---|---|---|---|
| `native` (default) | the game's own linear HDR scene, before its tonemap | exactly the SDR picture | paper white |
| `expand` | the clipped SDR picture (a guess) | exactly the SDR picture | expanded like the rest |
| `nativeplus` (restart) | the game's scene, tone mapped in its place (Vanilla+ or PsychoV), a 16-bit chain to the screen | the game's curve up to mid grey | `hdr_ui_nits` |

The HUD, menus and subtitles are shown at `hdr_ui_nits` (48-500, default 203) in `native` and
`nativeplus`, whatever paper white; an older `hdr_ui_brightness` (times paper white) is read
converted. Native+ has its own page: [nativeplus.md](nativeplus.md).

## Expand

Above a knee (linear 0.8) the brightest channel of the SDR frame is stretched so that SDR white
reaches `1 + (peak / paper - 1) * hdr_boost` times paper white (boost 0..1, default 0.25: 400
nits with the defaults). Hue and saturation stay. Anything near SDR white is expanded, a lamp and
a white menu text alike.

## Native

The game's tonemap compresses its HDR scene into SDR: midtones roughly by an exposure, highlights
by a shoulder that ends at SDR white. Native mode keeps the game's SDR frame (its grading,
contrast, saturation, vignette, fog colour, bloom and UI) and multiplies its highlights by how
much brighter the scene really is there, measured every frame on the GPU:

1. **Scene luma** (`hdr_native.comp` LUMA): before the post-processing combine (compute shader
   `9a9cf8a9`, the upscaler's hook; after FSR/DLSS/TAA wrote its output into the scene color,
   so output-sized at 1080p output). The RGBA16F scene color's brightest channel at half size:
   the 2x2 maximum (highlights) and the brightest channel of the 2x2 average (exposure). A
   reduced scene (scaled presets) is resolved into the guest image first, as the game's own
   compute passes reading it do.
2. **UI-free copy and exposure** (SNAPSHOT, EXPOSURE): at the first UI pass (at 1080p output
   the first Scaleform draw: vertex shader `34e8a281` or another of
   `UiComposition::MovieShader`, into the game's RGBA8 UI target; with scaled presets the
   output-size UI image right after the upscaler or background copy wrote it) the tonemapped
   frame is copied bit for bit, and
   `log2(SDR / scene)` is averaged over the SDR midtones (linear brightest channel 0.02-0.45, at
   least 1/200 of the pixels): what the game's curve does to the scene there. Bloom lights up
   SDR midtones around lamps where the scene is dark; samples far from the last exposure count
   less (Gaussian weight, one stop), so a uniform change still goes through. Smoothed over
   frames (10% a frame); after frames without a scene it starts over.

   The same pass measures two more things from the same samples:
   - **Tiles**: the exposure of each of 8x6 tiles of the frame, from the STATS groups inside it,
     smoothed the same way, with a confidence that builds up over 10 frames (and fades without
     samples). The vignette and local grading darken the SDR picture but not the scene; with
     one exposure for the frame a lamp in a dark corner lost the vignette.
   - **The game's tone curve**: in 16 bins of SDR brightness from 0.5 to 0.97 (linear brightest
     channel: the shoulder), the scene's brightness on the SDR scale (exposure x scene, log2)
     that the game shows there, Gaussian-weighted around each bin's last value (one stop). A
     pixel brighter in SDR than the scene can make it (by more than a quarter stop: bloom,
     glare) is left out. Summed per group with shared-memory integer atomics (fixed point),
     added up by the EXPOSURE pass, smoothed (10% a frame), with a confidence per bin (20
     frames to full), then made monotonic and never below the SDR value itself. It is the
     game's curve, not the area's, so it is kept across loading screens.
3. **Gain map** (GAIN): at the display pass that copies the finished frame (UI included) into a
   display buffer, the first pass of the next frame. Per pixel of the finished frame:
   - UI: where the frame differs from the UI-free copy (HUD, menus, subtitles, their translucent
     backgrounds), gain 1;
   - below the knee (linear brightest channel 0.5) gain 1: the SDR picture exactly; above it the
     gain fades in (smoothstep to SDR white);
   - the scene's brightness on the SDR scale is `exposure * scene`, with the brightest scene
     pixel of the 2x2 luma texels around (4x4 scene pixels: the footprint does not shimmer with
     the upscalers' sub-pixel jitter), at the local exposure: the frame's, moved by the tiles
     around (bilinear) as far as they are sure, at most a stop either way;
   - bloom and glare are added after the scene color, so the scene cannot say how bright a halo
     is; the game's measured curve, inverted at the pixel's SDR value, can (linear in log2
     between bin centres, mixed with the identity by each bin's confidence). The target is the
     larger of the two: a halo around a lamp rises with the lamp, a clipped highlight still
     comes from the scene. The SDR pixel goes towards it:
     `gain = (target / sdr) ^ (sqrt(boost) * fade)`;
   - the result rolls off into the peak in log2: unchanged up to 70% of the stops from SDR white
     to the peak, then each further doubling gets a smaller share of the room left (with the
     same slope where it starts), so the moon, a lamp and fire keep apart instead of all
     clipping at the peak.

   The map (R16F, the finished frame's size, negative where UI was found) belongs to that
   display buffer. The flip of a display buffer comes a frame later in the command stream; the
   presenter takes the buffer's map when it prepares the flip (`Presenter::PrepareFrame`,
   exchanged with the frame's previous map, no copy), so a frame and its map always match.
4. **Encode** (`hdr_output.frag`): the letterboxed SDR frame times the map (filtered, mapped
   over the frame's rectangle in the window), capped at the peak.

A frame without a scene (title, menus, loading screens, pre-rendered movies), or whose display
pass reads another image than the UI-free copy came from (or whose UI target was not found),
has no map: it is shown as plain SDR at paper white. A frame with a scene but no UI pass (in-engine
cutscenes without subtitles) is its own UI-free copy.

`hdr_boost` in native mode is the share of the scene's highlight range shown, in log2:
`sqrt(boost)`. The default 0.25 shows half of it (a lamp the scene has 10x brighter than SDR
white comes out about 3.2x), 1 all of it (up to the peak), 0 none (the SDR picture).

**Compare** (the menu's "Compare: SDR | HDR", `hdr_compare`, not saved): the encode shows the left
half of the screen as an SDR display shows the game (the frame at paper white, no gain, sRGB
colours) and the right half as set, with a grey line between them.

HDR metadata (`VK_EXT_hdr_metadata`): max mastering luminance is the peak, MaxFALL paper white;
MaxCLL is the peak in native mode and expand's SDR white level in expand mode. It changes with
the settings, not per frame.

### Cost

Per frame, a handful of commands recorded on the recording threads (`Scheduler::Record`), none
per draw, no readback, no wait: one dispatch at each of the three points (and the sums: one
group for the frame and the curve, one per tile), the encode samples one more image. GPU: the
scene read once (RGBA16F), the frame read twice and copied once (RGBA8), a half-size RG16F and a
full-size R16F written, about 55 MB at 1080p, plus 144 bytes of partial sums per 16x16 pixels.
Measured (`hdr-native-shader-test --bench <w> <h>`, all passes with their barriers, RTX 5060):
0.23 ms at 1920x1080, 0.42 ms at 2560x1440, 0.94 ms at 3840x2160 (0.18, 0.37 and 0.79 before
the curve and the tiles; in the game the scene luma stays at the guest's 1920x1080 for larger
outputs, so a little less). Native mode off (or HDR off): nothing.

### Debug view

`BB_HDR_DEBUG=1` shows the gain instead of the picture: the frame in grey at a third of paper
white, the gain over it from blue (a little) through green and yellow to red (the peak), UI that
native mode keeps at paper white in magenta. The square in the top left corner is green while
native mode has the frame's gain map and orange while the frame has none and is shown as SDR (in
expand mode the heat map shows expand's gain).

The log says which mode runs (`HDR output: HDR10 (PQ, BT.2020), mode native (...)`, again when
it is changed in the menu) and, in native mode, when frames stop or start having a gain map
(`HDR native: ...`, at most every 5 s, with the number of switches in between).

### Tests

`ninja -C out/gpu hdr-native-shader-test` runs the passes on a synthetic frame (plain Vulkan,
any 1.3 device; `BB_TEST_GPU=<index>` picks one): the luma's 2x2 max/average with NaN and inf
texels, the bit-exact copy, the exposure of a known tonemap (exact to 0.03 stops) and its
resistance to bloom (0.27 stops off on the first frame, 0.001 after 60), empty frames, and the
gain map: exactly 1 in the midtones and on UI, the scene's ratio to the power of the strength
rolled off into the peak on an 8x highlight, 1 on bloom while no curve is learned. On a larger
frame: the tone curve learned from a ramp through the shoulder (every bin within 0.12 stops of
the synthetic curve's inverse, monotonic, the bloom halo left out) and the halo then lifted by
it (to 4%); the tiles under a half-stop vignette on one side (each within 0.06 stops), and a
highlight there keeping the vignette against the same highlight on the other side (to 3%).
`settings-test` covers `hdr_mode`, `hdr_peak_auto` and `HdrPeakNits` (the display's peak, none
reported, the limits, auto off, `BB_HDR_PEAK`) and that the comparison is not saved.

### Limits

- The scene color is the pre-post scene: bloom, depth of field and motion blur are only in the
  SDR frame. Halos get their brightness from the game's curve, which ends where the SDR frame
  clips (0.97): a halo clipped in SDR is as bright as the curve's last bin says, not as its
  lamp; a blurred highlight gets the gain where its sharp source is (the 4x4 footprint softens
  that).
- The curve needs frames with pixels in the shoulder to learn (dark areas take longer); until a
  bin is sure it counts as the identity (no extra gain), as before.
- The tiles are 8x6: a vignette or grading change sharper than a tile is smoothed; a tile with
  too few midtones keeps the frame's exposure.
- Scaled presets (output other than 1080p): the scene luma is at the guest resolution, jittered
  when an upscaler runs; the map is interpolated to the output.
- `BB_UPSCALER=none` disables the frame analysis the passes rely on: native mode shows every frame
  as SDR (use expand mode instead).
