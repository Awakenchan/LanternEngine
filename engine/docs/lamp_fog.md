# Lamp fog falloff

Lamp fog (`effect_point_fog`, "Fog around lamps") integrates unshadowed volumetric
scattering along each camera ray through the spheres of the lights the game lists
(`point_volumetric.comp`). This note covers its two falloffs and the glow size setting.

## Choosing the falloff

`graphics_point_fog_falloff` (`lantern.ini`, applies live) picks the shader:

- `0`, Classic (the default): `point_volumetric.comp`, `(1 - d / r)^2` sampled at three
  Gauss-Legendre points per half-chord, 8 bytes of push constants (`count`, `intensity`).
  It is the lamp fog of earlier releases, unchanged.
- `1`, Inverse-square: `point_volumetric_falloff.comp`, below, a separate pipeline with
  12 bytes of push constants (`count`, `intensity`, `glow_size`). It costs more GPU time
  than classic. If this pipeline cannot be made on a GPU, classic is used.

It is "Lamp fog falloff" in the overlay's Picture section, both launchers' Lighting group
and the game's PC Lighting Detail page. A look carries it, and the glow size, only when it
is inverse-square, so looks made with classic stay readable by older launchers.

## Inverse-square falloff

Attenuation at distance `d` from a light of radius `r` is inverse-square:

```
core^2 / (d^2 + core^2) * saturate(1 - (d / r)^4)^2,   core = clamp(glow_size * r, 0.05, 1000)
```

The first term peaks at 1 at the centre and falls off as `1/d^2`; the window takes it
smoothly to zero at the light's radius. A constant 2.5 sets the peak at 2.5x the
classic `(1 - d / r)^2` profile's. At the default glow size, total scattered power over
a sphere is about 29% of the classic profile's: a tight glow at the source that falls
off quickly rather than brighter surrounding air. (Matching total power needs a 5.86x
peak, which made the fog noticeably brighter near lights.)

The ray/sphere chord is split at the point nearest the light, and each half is
integrated in six segments. The `1/d^2` term is integrated exactly per segment
(`atan`), with the window and view-ray transmittance (density 0.025) taken at the
segment midpoint, so the bright core cannot fall between fixed samples and flicker as
the camera moves. Colour bounding, saturation, the 0.35 scale and the
`graphics_point_fog_intensity` multiplier are unchanged.

## Glow size

`graphics_point_fog_glow_size` (`lantern.ini`, default 0.08, 0.03-0.20) is the
inverse-square core as a fraction of each light's radius, used only by the
inverse-square falloff (its slider is greyed out with classic). The core is capped at
1000 so that `core^2` stays finite for very large lights. Smaller is a tight, sharp
glow around the flame; larger a wider, softer haze. It changes the shape, not the peak.
It applies live and is in the overlay's Picture section and both launchers' Lighting
group as "Lamp fog glow size", under "Lamp fog falloff".

`point-volumetric-test` checks both shaders against a 20000-step reference: classic with
today's profile (and that the glow size does not change it), inverse-square at glow sizes
0.03, 0.08 and 0.20, and that very large lights stay finite.
