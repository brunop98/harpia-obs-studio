// Color Overlay — recolour the clip itself, keeping its shape.
//
// Photoshop's "Color Overlay": a colour laid over the clip's own pixels and
// nowhere else. Where the clip is see-through -- around a logo, a cut-out, a
// sticker, a PNG with a transparent background -- it stays see-through, so a
// logo turns white (or your brand colour) and the footage under it is left
// alone. That is the difference from Solid, which paints the whole frame. On
// an effect clip it tints everything below it.
//
// Blend mode is how the colour meets the picture:
//   Normal      the colour, flat: at Opacity 1 the clip becomes a silhouette
//   Multiply    darkens: white parts take the colour, black stays black
//   Screen      lightens: black parts take the colour, white stays white
//   Overlay     adds contrast: darks multiply, lights screen
//   Soft light  a gentler Overlay -- a grade more than a paint job
//   Color       the colour's hue and saturation, the clip's own light and
//               shade: a tint that keeps every detail
//
// The swatch's alpha multiplies into Opacity, as in Solid and Gradient: two
// controls, and both count.
//
//@name Color Overlay
//@param uColor   color  #3F73B8 Colour
//@param uMode    choice 0 Normal|Multiply|Screen|Overlay|Soft_light|Color Blend mode
//@param uOpacity float  0.0 1.0 1.0 Opacity

// Luminance, and putting a colour at a given luminance without leaving the
// 0..1 cube -- the "Color" blend from the W3C compositing spec, which is what
// Photoshop's does.
float lum(vec3 c)
{
    return dot(c, vec3(0.3, 0.59, 0.11));
}

vec3 clipColor(vec3 c)
{
    float l = lum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    // The floors keep a grey (n == l == x) from dividing nothing by nothing.
    if (n < 0.0)
        c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0)
        c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}

vec3 setLum(vec3 c, float l)
{
    return clipColor(c + (l - lum(c)));
}

vec3 overlayBlend(vec3 b, vec3 s)
{
    return mix(2.0 * b * s, 1.0 - 2.0 * (1.0 - b) * (1.0 - s), step(0.5, b));
}

vec3 softLightBlend(vec3 b, vec3 s)
{
    vec3 d = mix(((16.0 * b - 12.0) * b + 4.0) * b, sqrt(b), step(0.25, b));
    return mix(b - (1.0 - 2.0 * s) * b * (1.0 - b), b + (2.0 * s - 1.0) * (d - b), step(0.5, s));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord.xy / iResolution.xy;
    vec4 under = texture(iChannel0, uv);
    vec3 b = under.rgb;
    vec3 s = uColor.rgb;

    int mode = int(uMode + 0.5);
    vec3 blended = s;                                   // Normal
    if (mode == 1)
        blended = b * s;                                // Multiply
    else if (mode == 2)
        blended = 1.0 - (1.0 - b) * (1.0 - s);          // Screen
    else if (mode == 3)
        blended = overlayBlend(b, s);                   // Overlay
    else if (mode == 4)
        blended = softLightBlend(b, s);                 // Soft light
    else if (mode == 5)
        blended = setLum(s, lum(b));                    // Color

    // The clip's own alpha, untouched: that is what keeps the overlay inside
    // its shape. Only the colour moves.
    float amount = clamp(uOpacity * uColor.a, 0.0, 1.0);
    fragColor = vec4(mix(b, clamp(blended, 0.0, 1.0), amount), under.a);
}
