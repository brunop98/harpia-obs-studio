// CRT — a classic cathode-ray-tube look for Harpia.
//
// ShaderToy-style: write mainImage(out vec4, in vec2). The harness provides
// iResolution, iTime, iFrame and iChannel0 (the video frame). Any `//@param`
// line below becomes a slider/checkbox in the editor's Inspector.
//
//@param uCurvature  float 0.0 0.5 0.08 Screen curvature
//@param uScanline   float 0.0 1.0 0.35 Scanline strength
//@param uAberration float 0.0 4.0 1.0  Chromatic aberration
//@param uVignette   float 0.0 1.0 0.40 Vignette

// Barrel-distort the coordinates so the picture bulges like a glass tube.
vec2 curve(vec2 uv, float amount)
{
    uv = uv * 2.0 - 1.0;             // -1..1
    vec2 offset = abs(uv.yx) / vec2(1.0);
    uv = uv + uv * offset * offset * amount;
    return uv * 0.5 + 0.5;           // back to 0..1
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    uv = curve(uv, uCurvature);

    // Outside the curved screen: black border.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Chromatic aberration: pull the red/blue channels apart toward the edges.
    float ab = uAberration / iResolution.x;
    vec2 dir = uv - 0.5;
    float r = texture(iChannel0, uv - dir * ab * 2.0).r;
    float g = texture(iChannel0, uv).g;
    float b = texture(iChannel0, uv + dir * ab * 2.0).b;
    vec3 col = vec3(r, g, b);

    // Scanlines: darken alternating horizontal lines.
    float scan = sin(uv.y * iResolution.y * 3.14159) * 0.5 + 0.5;
    col *= 1.0 - uScanline * (1.0 - scan);

    // Aperture-grille tint: subtle per-column RGB emphasis.
    float col3 = mod(fragCoord.x, 3.0);
    vec3 mask = vec3(1.0);
    if (col3 < 1.0)      mask = vec3(1.05, 0.97, 0.97);
    else if (col3 < 2.0) mask = vec3(0.97, 1.05, 0.97);
    else                 mask = vec3(0.97, 0.97, 1.05);
    col *= mask;

    // Vignette: darken the corners.
    float vig = 1.0 - uVignette * dot(dir, dir) * 2.0;
    col *= clamp(vig, 0.0, 1.0);

    fragColor = vec4(col, 1.0);
}
