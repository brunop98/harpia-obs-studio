// Hue Cycle — rotates every colour around the colour wheel over time, in a
// loop. Speed is full turns per second (0.25 = one lap every four seconds);
// Saturation scales how vivid the colours are while they travel (1 = as
// shot, above 1 = more intense, 0 = grey). Offset starts the wheel somewhere
// else, so two clips side by side can cycle out of step.
//
// iTime is clip-relative, so the cycle starts from Offset at the clip's first
// frame, and it loops seamlessly: a full turn lands back on the original hue.
//
//@name Hue Cycle
//@param uSpeed      float 0.0 4.0 0.25 Speed (turns per second)
//@param uSaturation float 0.0 3.0 1.5  Saturation
//@param uOffset     float 0.0 1.0 0.0  Offset (fraction of a turn)
//@param uAmount     float 0.0 1.0 1.0  Amount

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec4 texel = texture(iChannel0, uv);
    vec3 hsv = rgb2hsv(texel.rgb);
    // fract keeps the angle small however long the clip runs, so precision
    // does not drift an hour in.
    hsv.x = fract(hsv.x + fract(iTime * uSpeed) + uOffset);
    hsv.y = clamp(hsv.y * uSaturation, 0.0, 1.0);
    vec3 cycled = hsv2rgb(hsv);
    fragColor = vec4(mix(texel.rgb, cycled, uAmount), texel.a);
}
