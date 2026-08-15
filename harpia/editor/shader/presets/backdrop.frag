// Backdrop — a drifting noise field with tumbling rectangles.
//
// A GENERATOR, unlike every other bundled shader here: the rest read iChannel0
// and grade what they are given, this one paints every pixel and only consults
// the input to blend against it. Put it on an effect clip on a track BELOW a
// caption and the caption sits on moving colour.
//
// Each colour is one swatch you pick by looking at it, with a separate
// Brightness beside it. The brightness is NOT folded into the swatch: it runs
// past 1, and the glow this shader is built around lives up there -- a swatch
// alone tops out at white and the field goes flat.
//
//@param uColor     color #0661FF Background colour
//@param uVal       float 0.0 2.0 0.42  Background brightness
//@param uRectColor color #0575FF Shape colour
//@param uRectVal   float 0.0 2.0 0.57  Shape brightness
//@param uSpeed     float 0.0 4.0 1.0   Speed
//@param uCount     float 0.0 200.0 60.0 Shapes
//@param uMinSize   float 0.005 0.3 0.03 Min size
//@param uMaxSize   float 0.01 0.5 0.08  Max size
//@param uSpread    float 0.0 2.0 0.5    Vertical spread
//@param uOpacity   float 0.0 1.0 1.0    Opacity

// The loop runs to a COMPILE-TIME bound and breaks early. uCount is a uniform,
// and while GLSL 3.30 allows a non-constant loop bound, a constant one with a
// break is the shape every driver optimises well.
const float kMaxShapes = 200.0;

const float noiseIntensity = 2.8;
const float noiseDefinition = 0.6;
const vec2 glowPos = vec2(-2.0, 0.0);

float random(vec2 co)
{
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

float noise(in vec2 p)
{
    p *= noiseIntensity;
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(random(i + vec2(0.0, 0.0)),
                   random(i + vec2(1.0, 0.0)), u.x),
               mix(random(i + vec2(0.0, 1.0)),
                   random(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(in vec2 uv)
{
    uv *= 5.0;
    mat2 m = mat2(1.6, 1.2, -1.2, 1.6);
    float f  = 0.5000 * noise(uv); uv = m * uv;
    f += 0.2500 * noise(uv); uv = m * uv;
    f += 0.1250 * noise(uv); uv = m * uv;
    f += 0.0625 * noise(uv);
    return 0.5 + 0.5 * f;
}

// `t` is passed in rather than read from iTime, so Speed scales every motion in
// the picture from one place and the rates stay in the proportion they were
// tuned at.
vec3 bg(vec2 uv, float t, vec3 tint)
{
    float velocity = t / 1.6;
    float intensity = sin(uv.x * 3.0 + velocity * 2.0) * 1.1 + 1.5;
    uv.y -= 2.0;
    vec2 bp = uv + glowPos;
    uv *= noiseDefinition;

    // Ripple.
    float rb = fbm(vec2(uv.x * 0.5 - velocity * 0.03, uv.y)) * 0.1;
    uv += rb;

    // Colouring.
    float rz = fbm(uv * 0.9 + vec2(velocity * 0.35, 0.0));
    rz *= dot(bp * intensity, bp) + 1.2;

    // The original divides by (0.1 - rz), which blows up as rz approaches 0.1.
    // That is where the glow comes from, so it is kept -- but held off exactly
    // zero, because a division by it is an infinity, and an infinity here is a
    // block of undefined colour rather than a bright spot.
    float d = 0.1 - rz;
    d = sign(d) * max(abs(d), 1e-4);
    return sqrt(abs(tint / d));
}

float rectangle(vec2 uv, vec2 pos, float width, float height, float blur)
{
    pos = (vec2(width, height) + 0.01) / 2.0 - abs(uv - pos);
    pos = smoothstep(0.0, max(blur, 1e-4), pos);
    return pos.x * pos.y;
}

mat2 rotate2d(float a)
{
    return mat2(cos(a), -sin(a), sin(a), cos(a));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv0 = fragCoord.xy / iResolution.xy;
    vec2 uv = uv0 * 2.0 - 1.0;
    uv.x *= iResolution.x / iResolution.y;

    float t = iTime * uSpeed;
    vec3 bgTint = uColor.rgb * uVal;
    vec3 rectTint = uRectColor.rgb * uRectVal;

    vec3 color = bg(uv, t, bgTint) * (2.0 - abs(uv.y * 2.0));

    // Min and Max are a real min and max here. The original's `maxSize` was the
    // RANGE above the minimum (0.08 - minSize), which reads as a maximum and is
    // not one -- so a slider labelled Max would have moved the wrong quantity.
    float minSize = min(uMinSize, uMaxSize);
    float range = max(uMaxSize - minSize, 1e-4);
    float count = max(uCount, 1.0);

    float velX = -t / 8.0;
    float velY = t / 10.0;
    for (float i = 0.0; i < kMaxShapes; i++) {
        if (i >= count)
            break;
        float index = i / count;
        float rnd = random(vec2(index));
        vec3 pos = vec3(0.0);
        pos.x = fract(velX * rnd + index) * 4.0 - 2.0;
        pos.y = sin(index * rnd * 1000.0 + velY) * uSpread;
        pos.z = range * rnd + minSize;
        vec2 uvRot = uv - pos.xy + pos.z / 2.0;
        uvRot = rotate2d(i + t / 2.0) * uvRot;
        uvRot += pos.xy + pos.z / 2.0;
        float rect = rectangle(uvRot, pos.xy, pos.z, pos.z, (minSize + range - pos.z) / 2.0);
        color += rectTint * rect * pos.z / (minSize + range);
    }

    // Opacity 1 is a solid backdrop that replaces whatever is beneath it, which
    // is what it is for. Below 1 it becomes a wash OVER the picture instead --
    // and the alpha travels with it, so at 0 the frame is returned untouched
    // rather than as an opaque rectangle the size of the clip.
    vec4 under = texture(iChannel0, uv0);
    fragColor = vec4(mix(under.rgb, color, uOpacity), mix(under.a, 1.0, uOpacity));
}
