// CRT — adjustable beam lines, phosphor masks and glass for Harpia.
//@name CRT
// Original implementation; design references (not copied shader code):
// https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-easymode.slang
// https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-lottes.slang
// https://github.com/libretro/slang-shaders/blob/master/crt/shaders/crt-geom.slang
//
// Density is a virtual line count. Mask size and glow radius use reference
// pixels at 1080p, so reducing the preview does not change their screen size.
// Derivative filtering fades detail too fine for the output to resolve.
//
//@param uCurvature          float 0.0 0.5 0.08 Screen curvature
//@param uScanline           float 0.0 1.0 0.35 Scanline strength
//@param uScanDensity        float 60.0 1080.0 360.0 Scanline count
//@param uScanWidth          float 0.05 0.95 0.5 Scanline width
//@param uScanSoftness       float 0.0 1.0 0.0 Scanline softness
//@param uPreserveBrightness bool 1 Preserve brightness
//@param uAberration         float 0.0 4.0 1.0 Chromatic aberration
//@param uMaskType           choice 1 Off|Aperture_grille|Shadow_mask Phosphor mask
//@param uMaskStrength       float 0.0 1.0 0.08 Mask strength
//@param uMaskSize           float 1.0 6.0 1.0 Mask size (1080p px)
//@param uGlow               float 0.0 1.0 0.0 Phosphor glow
//@param uGlowRadius         float 0.5 8.0 2.0 Glow radius (1080p px)
//@param uBrightness         float 0.5 1.5 1.0 Brightness
//@param uGamma              float 0.5 2.5 1.0 Display gamma
//@param uVignette           float 0.0 1.0 0.40 Vignette
//@param uBorderOpacity      float 0.0 1.0 1.0 Curved border opacity

const float CRT_PI = 3.141592653589793;
const float CRT_TAU = 6.283185307179586;

vec2 curve(vec2 uv, float amount)
{
    uv = uv * 2.0 - 1.0;
    vec2 offset = abs(uv.yx);
    return (uv + uv * offset * offset * amount) * 0.5 + 0.5;
}

// Integral of a periodic stripe: each unit interval is dark for `width`.
// Integrating across the pixel footprint avoids the phase-dependent full-frame
// darkening that an undersampled sine can produce in a reduced preview.
float stripeIntegral(float position, float width)
{
    return floor(position) * width + min(fract(position), width);
}

float stripeCoverage(float position, float width, float footprint)
{
    footprint = max(footprint, 0.0001);
    float coverage = (stripeIntegral(position + footprint * 0.5, width)
                    - stripeIntegral(position - footprint * 0.5, width)) / footprint;
    float resolved = 1.0 - smoothstep(0.5, 1.0, footprint);
    return mix(width, clamp(coverage, 0.0, 1.0), resolved);
}

float filteredCos(float position, float footprint)
{
    float x = CRT_PI * max(footprint, 0.0001);
    float resolved = 1.0 - smoothstep(0.5, 1.0, footprint);
    return cos(CRT_TAU * position) * (sin(x) / x) * resolved;
}

float peakChannel(vec3 color)
{
    return max(color.r, max(color.g, color.b));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = curve(fragCoord / iResolution.xy, uCurvature);
    vec2 referenceSize = iResolution.xy * (1080.0 / iResolution.y);
    float linePosition = uv.y * max(1.0, uScanDensity);
    float lineFootprint = fwidth(linePosition) * (1.0 + 3.0 * clamp(uScanSoftness, 0.0, 1.0));
    vec2 maskPosition = (fragCoord / iResolution.xy) * referenceSize / (3.0 * max(1.0, uMaskSize));
    vec2 maskFootprint = fwidth(maskPosition);

    // Compute derivatives before the varying border branch, so a curved edge
    // does not put the filtering calculation in nonuniform control flow.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, clamp(uBorderOpacity, 0.0, 1.0));
        return;
    }

    vec4 original = texture(iChannel0, uv);
    vec2 dir = uv - 0.5;
    float ab = uAberration / referenceSize.x;
    vec3 col = vec3(texture(iChannel0, uv - dir * ab * 2.0).r,
                    original.g,
                    texture(iChannel0, uv + dir * ab * 2.0).b);

    // Optional glow stays off by default: ordinary CRT rendering still needs
    // just the three source samples used for chromatic aberration.
    if (uGlow > 0.0) {
        vec2 stepUV = vec2(max(0.0, uGlowRadius)) / referenceSize;
        vec3 soft = original.rgb * 0.25;
        soft += (texture(iChannel0, uv + vec2(stepUV.x, 0.0)).rgb
               + texture(iChannel0, uv - vec2(stepUV.x, 0.0)).rgb
               + texture(iChannel0, uv + vec2(0.0, stepUV.y)).rgb
               + texture(iChannel0, uv - vec2(0.0, stepUV.y)).rgb) * 0.125;
        soft += (texture(iChannel0, uv + stepUV).rgb
               + texture(iChannel0, uv - stepUV).rgb
               + texture(iChannel0, uv + vec2(stepUV.x, -stepUV.y)).rgb
               + texture(iChannel0, uv + vec2(-stepUV.x, stepUV.y)).rgb) * 0.0625;
        col = 1.0 - (1.0 - col) * (1.0 - clamp(uGlow, 0.0, 1.0) * soft * 0.5);
    }

    col = clamp(pow(clamp(col, 0.0, 1.0), vec3(1.0 / max(0.05, uGamma)))
                * max(0.0, uBrightness), 0.0, 1.0);

    float width = clamp(uScanWidth, 0.05, 0.95);
    float stripe = stripeCoverage(linePosition, width, lineFootprint);
    float strength = clamp(uScanline, 0.0, 1.0);
    if (uPreserveBrightness) {
        // Zero-mean modulation: dark stripes are balanced by brighter beam
        // areas. Limit the gain to the available highlight headroom instead
        // of clipping bright bands and losing their average brightness again.
        // A shared gain keeps the pixel's channel ratios intact. Near white,
        // contrast gently recedes; black stays black. This operates in the
        // renderer's display RGB space, matching Qt preview downscaling.
        float peak = peakChannel(col);
        float amplitude = min(1.0 / (1.0 - width),
                              (1.0 - peak) / max(peak * width, 0.0001));
        col *= 1.0 + strength * (width - stripe) * amplitude;
    } else {
        col *= 1.0 - strength * stripe;
    }

    if (uMaskType > 0.5 && uMaskStrength > 0.0) {
        vec3 coverage;
        float positivePeak;
        if (uMaskType < 1.5) {
            coverage = vec3(stripeCoverage(maskPosition.x, 1.0 / 3.0, maskFootprint.x),
                            stripeCoverage(maskPosition.x - 1.0 / 3.0, 1.0 / 3.0, maskFootprint.x),
                            stripeCoverage(maskPosition.x - 2.0 / 3.0, 1.0 / 3.0, maskFootprint.x));
            positivePeak = 2.0;
        } else {
            float stagger = mod(floor(maskPosition.y), 2.0) * 0.5;
            float vertical = filteredCos(maskPosition.y - 0.5, maskFootprint.y);
            vec3 horizontal = vec3(filteredCos(maskPosition.x + stagger, maskFootprint.x),
                                    filteredCos(maskPosition.x + stagger - 1.0 / 3.0, maskFootprint.x),
                                    filteredCos(maskPosition.x + stagger - 2.0 / 3.0, maskFootprint.x));
            coverage = (1.0 + horizontal) * (1.0 + vertical) * 0.25;
            positivePeak = 3.0;
        }
        float amount = clamp(uMaskStrength, 0.0, 1.0);
        if (uPreserveBrightness) {
            float peak = peakChannel(col);
            float amplitude = min(1.0, (1.0 - peak) / max(peak * positivePeak, 0.0001));
            vec3 emphasis = coverage * (uMaskType < 1.5 ? 3.0 : 4.0) - 1.0;
            col *= 1.0 + amount * amplitude * emphasis;
        } else {
            col *= mix(vec3(1.0), coverage, amount);
        }
    }

    float vignette = 1.0 - uVignette * dot(dir, dir) * 2.0;
    col *= clamp(vignette, 0.0, 1.0);
    fragColor = vec4(clamp(col, 0.0, 1.0), original.a);
}
