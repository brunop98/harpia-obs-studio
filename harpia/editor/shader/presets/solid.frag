// Solid — one flat colour, edge to edge.
//
// A GENERATOR, like Backdrop: it paints every pixel and reads the input only to
// blend against it. On an effect clip below a caption it is the plainest
// background there is, and at less than full Opacity it becomes a colour wash
// over the footage underneath.
//
// One colour swatch, not three sliders: picking a colour is a thing people do
// by looking at it, and Hue/Saturation/Brightness made you solve for the one
// you already had in mind. The swatch is live -- the frame follows the picker
// while you drag around it.
//
//@param uColor   color #3F73B8 Colour
//@param uOpacity float 0.0 1.0 1.0  Opacity

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord.xy / iResolution.xy;

    // The swatch's own alpha multiplies into Opacity rather than replacing it:
    // a colour picked at half alpha and an Opacity of 0.5 are two different
    // controls, and both should still count.
    vec3 col = uColor.rgb;
    float amount = uOpacity * uColor.a;

    // Opacity 1 replaces whatever is beneath, which is what a background is
    // for. Below 1 it is a wash over the picture instead, and the alpha travels
    // with it -- so at 0 the frame comes back untouched rather than as an opaque
    // rectangle the size of the clip.
    vec4 under = texture(iChannel0, uv);
    fragColor = vec4(mix(under.rgb, col, amount), mix(under.a, 1.0, amount));
}
