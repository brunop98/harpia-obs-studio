// Sharpen — unsharp mask using the 4 nearest neighbours.
//
//@param uAmount float 0.0 3.0 0.6 Amount

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    vec2 px = 1.0 / iResolution.xy;
    vec3 c = texture(iChannel0, uv).rgb;
    vec3 sum = texture(iChannel0, uv + vec2(px.x, 0.0)).rgb +
               texture(iChannel0, uv - vec2(px.x, 0.0)).rgb +
               texture(iChannel0, uv + vec2(0.0, px.y)).rgb +
               texture(iChannel0, uv - vec2(0.0, px.y)).rgb;
    // Laplacian sharpen: emphasise the difference from the neighbourhood.
    vec3 sharp = c + uAmount * (4.0 * c - sum);
    fragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
