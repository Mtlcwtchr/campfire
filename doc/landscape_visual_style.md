# Landscape visual style

A restrained, natural medieval-RTS direction: warm direct sunlight, cool sky
fill, earthy vegetation, quiet blue-green water and readable distant terrain.
No assets or shader code from another game are used.

## Shared contract

`assets/shaders/landscape_look.hlsli` is shared by terrain, foliage, water and
world sprites. UI, menus and the streaming curtain are deliberately excluded.

- The current texture arrays and offscreen target are RGBA8 **UNORM**, including
  water normal data. This pass preserves that display-referred workflow; it is
  not an HDR/linear-PBR conversion and does not apply a second gamma transform.
- One sun direction and sky palette replace independent lighting constants.
- Opaque terrain and grass receive hemisphere sky fill plus warm direct light.
  Cavity/root occlusion mainly reduces ambient light rather than painting holes black.
- A luminance-based pigment adjustment reduces excessive saturation, especially
  vegetation, without flattening different biome palettes into the same grey.
- A continuous highlight shoulder preserves midtones and scales RGB together to
  retain hue. There is no bloom, sharpening filter or blanket contrast curve.
- A bounded aerial-perspective approximation uses orthographic view span and
  signed depth relative to the focus. It is independent of the world origin,
  almost absent close up and capped at 32% so strategic views remain readable.
  Water and foliage use their actual world heights, not a screen-space gradient.

## Materials

Terrain keeps its existing blended materials, triplanar cliffs and nested sand
ripples. Broad texture modulation is restrained to avoid painted-looking stains.
A smooth wetness band darkens exposed banks without changing wave or foam timing.

Grass keeps its mesh, placement, density, wind and cutout/depth contracts. An
upward canopy normal follows the bend; roots are shaded, tips transmit a little
warm light, and each instance gets a small stationary tint variation. The far
meadow and near cards share the same pigment and daylight treatment.

Water keeps the existing waves, shoreline, foam, waterfall and alpha formulas.
Its depth-colour transition is exponential rather than a clamped nine-metre
ramp. A muted blue-green palette, reflected sky gradient and softer warm glints
replace the saturated blue and sharp white glints. No scene-refraction or SSR
is claimed: neither is available in the current pipeline.

World sprites receive only the common highlight shoulder and atmosphere:
lighting already painted into their artwork is not applied a second time.

## Cost and limitations

No new textures, passes, draw calls, CPU world queries or uniform layouts.
Additional work is bounded shader arithmetic and a few interpolated values.
Normal data stays untouched by the colour treatment.

These are local shading and art-direction improvements, not true terrain or
object cast shadows, SSAO, volumetric fog, GI, or a replacement for detailed 3D
assets. Canopy normals and aerial perspective are deliberate approximations
for the orthographic RTS camera. Profile representative hardware before adding
more effects; screenshots alone do not establish a GPU performance budget.

## Validation

`asr_ring_tests` compiles the scalar highlight, haze, water-depth, wetness and
root profiles directly from the HLSL include. Tests cover bounds, monotonicity,
close-view clarity and smooth highlight transitions, alongside the sand,
foliage, streaming and shoreline regression tests.

Reference captures use `asr_client --explore --world 256 --seed 11 --zoom 6
--shot-time 4 --at NAME --shot FILE`. Places: `temperate forest`, `mountains`,
`the coast`, `desert`. Also check vegetation at `--zoom 24` and the landscape
at `--zoom 0.8`; do not judge one biome or one camera scale in isolation.

