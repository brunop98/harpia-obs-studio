// Pixelate — snap sampling to a coarse block grid (mosaic / retro look).
//
//@param uSize float 1.0 64.0 8.0 Block size (px)

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float s = max(1.0, uSize);
    vec2 snapped = (floor(fragCoord / s) + 0.5) * s;
    vec2 uv = snapped / iResolution.xy;
    fragColor = vec4(texture(iChannel0, uv).rgb, 1.0);
}
