# CRT effect

The CRT component keeps its existing curvature, scanline, aberration and vignette controls. It now also offers:

| Control | Purpose |
| --- | --- |
| Scanline count | Number of beam lines across the image, independent of preview/export resolution. |
| Scanline width | Fraction of each line occupied by the dark gap. |
| Scanline softness | Soften beam edges; fine patterns fade when the preview cannot resolve them. |
| Preserve brightness | Enabled by default. Balance dark gaps with brighter beam areas, easing their contrast near highlights to avoid clipping. |
| Phosphor mask | Off, aperture grille, or staggered shadow mask. |
| Mask strength / size | Set the visibility and size of the RGB phosphor pattern. |
| Phosphor glow / radius | Add an optional soft glow; off by default. |
| Brightness / display gamma | Adjust the display's overall brightness and tonal response. Both default to neutral. |
| Curved border opacity | Keep black tube corners, or make them transparent for overlays. |

Mask size and glow radius use reference pixels at 1080p, so their relative size stays consistent in previews and exports. Existing source transparency is retained inside the curved screen.

The old scanline multiplier averaged `1 - strength / 2`: strength 1 removed half the average display RGB before the vignette. The new default uses zero-mean, headroom-limited modulation in the renderer's display RGB space. Dark and midtone footage retains its average brightness, whites avoid clipping, and black stays black. Disabling **Preserve brightness** restores darkening scanlines. Vignette remains an intentional corner darkener.

For a starting look, try strength 0.4, count 240–360, width 0.3–0.5, aperture grille at strength 0.1, and a small amount of glow. Lower vignette if the edges are too dark. Brightness preservation is about retaining the picture's overall level; a resolved scanline still has darker gaps and brighter beam areas.

The component ID and existing uniform keys are unchanged, so existing values and keyframes survive. New controls receive their declared defaults. On launch, Harpia upgrades only a recognized, untouched legacy stock `crt.frag`; custom shader edits remain untouched. Stock updates use atomic writes.

## GitHub references

These references informed the control choices and brightness/antialiasing design. Harpia's implementation is original; upstream shader code was not copied.

- [CRT EasyMode](https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-easymode.slang): beam width, scanline controls, brightness boost and highlight-dependent scanline relief.
- [CRT Lottes](https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-lottes.slang): phosphor masks, beam sharpness, gamma and bloom.
- [CRT-Geom / cgwg](https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-geom.slang): beam profiles, curvature and derivative-based scanline filtering.
- [ZFast CRT](https://github.com/libretro/slang-shaders/blob/master/crt/shaders/zfast_crt/zfast_crt_impl.inc): separate treatment of dark/bright scanlines and inexpensive brightness relief.
