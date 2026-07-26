// Vignette — darken the edges of the frame.
//
//@param uAmount   float 0.0 1.0 0.5 Amount
//@param uSoftness float 0.1 1.5 0.6 Softness

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;
    // Distance from centre, corner-normalised.
    float d = length(uv - 0.5) * 1.41421356;
    float v = smoothstep(1.0, 1.0 - uSoftness, d);
    c *= mix(1.0, v, uAmount);
    fragColor = vec4(c, 1.0);
}
