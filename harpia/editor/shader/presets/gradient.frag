// Gradient — two colours, at any angle, blending where you put the seam.
//
// A GENERATOR, like Solid and Backdrop: it paints every pixel and reads the
// input only to blend against it.
//
// Three controls describe the blend, and they are independent on purpose.
// Angle turns the whole ramp. Position slides the seam along it without
// changing the angle. Softness sets how wide the blend is around that seam --
// at 1 it is a classic edge-to-edge gradient, at 0 a hard two-colour split, and
// anything between is a band. Position alone could not give you the split, and
// Softness alone could not tell you where to put it.
//
//@param uColorA   color #4A7FE0 Colour A
//@param uColorB   color #B94FD4 Colour B
//@param uAngle    float 0.0 360.0 90.0 Angle
//@param uPosition float 0.0 1.0 0.5  Transition position
//@param uSoftness float 0.0 1.0 1.0  Transition softness
//@param uOpacity  float 0.0 1.0 1.0  Opacity

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord.xy / iResolution.xy;
    float aspect = iResolution.x / iResolution.y;

    // Centre the frame and undo the aspect, so Angle is a real angle ON SCREEN:
    // at 45 the seam runs at one pixel across per pixel down, on a 16:9 output
    // and on any other. Without this it would tilt with the output shape, and
    // the same project would read differently at a different aspect. (45 is a
    // true diagonal, not corner-to-corner -- corner-to-corner on 16:9 is about
    // 29 degrees, and depends on the shape, which is exactly what Angle should
    // not do.)
    vec2 p = uv - 0.5;
    p.x *= aspect;

    float a = radians(uAngle);
    vec2 dir = vec2(cos(a), sin(a));
    float t = dot(p, dir);

    // How far the frame reaches along that direction: the projection of the
    // half-frame onto it. Dividing by this puts 0 at the first corner the ramp
    // meets and 1 at the last, WHATEVER the angle -- so Position 0.5 is the
    // middle of the picture at 30 degrees just as it is at 90, and the ramp
    // always spans the frame instead of running out early on the diagonal.
    float halfExtent = abs(dir.x) * (0.5 * aspect) + abs(dir.y) * 0.5;
    float t01 = 0.5 + t / (2.0 * max(halfExtent, 1e-4));

    // smoothstep's edges must differ, hence the floor: at Softness 0 they would
    // be equal and smoothstep(e, e, x) is undefined. A hair's width apart reads
    // as the hard edge that Softness 0 is asking for.
    float w = max(uSoftness, 1e-4);
    float g = smoothstep(uPosition - w * 0.5, uPosition + w * 0.5, t01);

    vec3 col = mix(uColorA.rgb, uColorB.rgb, g);

    // Each swatch carries its own alpha, so the ramp can fade from a solid
    // colour into nothing as well as from one colour into another; Opacity
    // then scales the whole thing on top.
    float amount = uOpacity * mix(uColorA.a, uColorB.a, g);

    // Opacity 1 replaces whatever is beneath; below 1 it is a wash over the
    // picture, alpha included, so at 0 the frame comes back untouched.
    vec4 under = texture(iChannel0, uv);
    fragColor = vec4(mix(under.rgb, col, amount), mix(under.a, 1.0, amount));
}
