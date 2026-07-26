// Film grain — animated luminance noise.
//
//@param uAmount float 0.0 0.5 0.08 Amount

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;
    // Re-seed per frame so the grain shimmers.
    float n = hash(fragCoord.xy + fract(iTime) * vec2(13.37, 7.11)) - 0.5;
    fragColor = vec4(clamp(c + n * uAmount, 0.0, 1.0), 1.0);
}
