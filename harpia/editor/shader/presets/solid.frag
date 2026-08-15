// Solid — one flat colour, edge to edge.
//
// A GENERATOR, like Backdrop: it paints every pixel and reads the input only to
// blend against it. On an effect clip below a caption it is the plainest
// background there is, and at less than full Opacity it becomes a colour wash
// over the footage underneath.
//
// Hue/Saturation/Brightness rather than R/G/B, matching every other generator
// here: one Hue slider walks the whole spectrum, where R/G/B means solving for
// the mix you want. (The shader parameter system carries floats and bools;
// there is no colour swatch to declare.)
//
//@param uHue     float 0.0 1.0 0.60 Hue
//@param uSat     float 0.0 1.0 0.55 Saturation
//@param uVal     float 0.0 1.0 0.45 Brightness
//@param uOpacity float 0.0 1.0 1.0  Opacity

vec3 hsv2rgb(vec3 c)
{
    vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord.xy / iResolution.xy;
    vec3 col = hsv2rgb(vec3(uHue, uSat, uVal));

    // Opacity 1 replaces whatever is beneath, which is what a background is
    // for. Below 1 it is a wash over the picture instead, and the alpha travels
    // with it -- so at 0 the frame comes back untouched rather than as an opaque
    // rectangle the size of the clip.
    vec4 under = texture(iChannel0, uv);
    fragColor = vec4(mix(under.rgb, col, uOpacity), mix(under.a, 1.0, uOpacity));
}
