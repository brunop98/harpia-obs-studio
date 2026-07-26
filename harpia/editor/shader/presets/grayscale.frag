// Grayscale — desaturate toward Rec.709 luma.
//
//@param uAmount float 0.0 1.0 1.0 Amount

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(mix(c, vec3(l), uAmount), 1.0);
}
