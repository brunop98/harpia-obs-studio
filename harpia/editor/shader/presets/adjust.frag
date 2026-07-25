// Adjustments — basic color grading for Harpia.
//
// ShaderToy-style: write mainImage(out vec4, in vec2). The harness provides
// iResolution, iTime, iFrame and iChannel0 (the video frame). Any `//@param`
// line below becomes a slider/checkbox in the editor's Inspector. Every control
// defaults to a no-op, so the picture is untouched until you move a slider.
//
//@param uExposure    float -3.0 3.0  0.0 Exposure (stops)
//@param uTemperature float -1.0 1.0  0.0 Temperature (cool ↔ warm)
//@param uContrast    float  0.0 2.0  1.0 Contrast
//@param uSaturation  float  0.0 2.0  1.0 Saturation
//@param uGamma       float  0.2 3.0  1.0 Gamma
//@param uBrightness  float -0.5 0.5  0.0 Brightness

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 texel = texture(iChannel0, uv);
    vec3 col = texel.rgb;

    // Exposure (photographic stops).
    col *= pow(2.0, uExposure);

    // White balance: warm pushes red up / blue down, cool the reverse.
    col.r += uTemperature * 0.10;
    col.b -= uTemperature * 0.10;

    // Contrast around mid-grey.
    col = (col - 0.5) * uContrast + 0.5;

    // Saturation toward/away from Rec.709 luma.
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, uSaturation);

    // Gamma.
    col = pow(max(col, 0.0), vec3(1.0 / uGamma));

    // Brightness (additive lift).
    col += uBrightness;

    fragColor = vec4(clamp(col, 0.0, 1.0), texel.a);
}
