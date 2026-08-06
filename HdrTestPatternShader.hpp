// HDR / color-management debug test pattern.
//
// Drop-in replacement for a single triangle-list draw of 3 vertices, same as
// your original colored-triangle shader (@builtin(vertex_index), no vertex
// buffer). Uses the standard fullscreen-triangle trick instead of an actual
// triangle, so the whole viewport becomes the test chart.
//
// Layout, bottom (uv.y=0) to top (uv.y=1):
//   Band 0 [0.00, 0.25): Gamut swatches — black, white, R, G, B, C, M, Y at
//                         full saturation. This is the band that will show
//                         a hue/appearance shift when you flip colorSpace
//                         between "srgb" and "display-p3", since fully
//                         saturated primaries are exactly where the two
//                         gamuts disagree most.
//   Band 1 [0.25, 0.50): Four stacked ramps (R, G, B, White, top to bottom
//                         within the band) from 0 to kExposureMax.
//                         A thin inverted-color tick mark is drawn at the
//                         x position where the ramp value crosses 1.0 —
//                         i.e. exactly where SDR/"standard" tone mapping
//                         would start clipping. In "extended" mode you
//                         should see the ramp keep climbing past that tick
//                         instead of flattening out.
//   Band 2 [0.50, 0.75): Discrete step chart — fixed swatches at
//                         {0, .25, .5, .75, 1, 1.25, 1.5, 2, 3, 4}. The
//                         swatch at exactly 1.0 gets a red border so you
//                         can find the SDR reference-white patch at a
//                         glance and see which neighbors are above/below it.
//   Band 3 [0.75, 1.00): Live gamma-vs-linear interpolation test — a
//                         red->green->blue crossfade computed either as a
//                         naive lerp of the encoded values (interpSpace=0,
//                         what your original triangle was always doing) or
//                         decode->lerp->encode in linear light
//                         (interpSpace=1). Toggle kInterpSpace at
//                         runtime to A/B them directly.
//
// Faint gray separator lines are drawn at each band boundary. 
#include <cstdint>

constexpr const char* const hdrTestPatternShaderSource = R"(
struct VertexOutput
{
    @builtin(position) Position : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

const kExposureMax : f32 = 2.0;
const kTonemapMode : u32 = 0u; // 0 = raw/hard-clip, 1 = Reinhard, 2 = crude ACES-ish
const kTransferFunctionMode : u32 = 2u; // 0 = linear, 1 = EOTF, 2 = OETF

@vertex
fn VsMain(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput
{
    // Fullscreen-triangle trick: 3 vertices, no vertex buffer needed.
    var uv = vec2<f32>(f32((vertexIndex << 1u) & 2u), f32(vertexIndex & 2u));
    var output : VertexOutput;
    output.Position = vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

fn tonemap(c : vec3<f32>, mode : u32) -> vec3<f32>
{
    var result : vec3<f32>;
    if (mode == 0u)
    {
        result = c;
    }
    else if (mode == 1u)
    {
        result = c / (c + vec3<f32>(1.0));
    }
    else if (mode == 2u)
    {
        result = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
    }
    return result;
}

fn srgbToLinear(c : vec3<f32>) -> vec3<f32>
{
    let cutoff = c <= vec3<f32>(0.04045);
    let lo = c / 12.92;
    let hi = pow((c + vec3<f32>(0.055)) / 1.055, vec3<f32>(2.4));
    return select(hi, lo, cutoff);
}

fn linearToSrgb(c : vec3<f32>) -> vec3<f32>
{
    let cutoff = c <= vec3<f32>(0.0031308);
    let lo = c * 12.92;
    let hi = vec3<f32>(1.055) * pow(c, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055);
    return select(hi, lo, cutoff);
}

fn gamutSwatches(uv : vec2<f32>) -> vec3<f32>
{
    let colors = array<vec3<f32>, 8>(
        vec3<f32>(0.0, 0.0, 0.0),
        vec3<f32>(1.0, 1.0, 1.0),
        vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(0.0, 0.0, 1.0),
        vec3<f32>(0.0, 1.0, 1.0),
        vec3<f32>(1.0, 0.0, 1.0),
        vec3<f32>(1.0, 1.0, 0.0),
    );
    let idx = u32(clamp(floor(uv.x * 8.0), 0.0, 7.0));
    return colors[idx];
}

fn channelRamps(uv : vec2<f32>, exposureMax : f32) -> vec3<f32>
{
    let localY = (uv.y - 0.25) / 0.25;
    let row = u32(clamp(floor(localY * 4.0), 0.0, 3.0));
    let value = uv.x * exposureMax;

    var c = vec3<f32>(0.0);
    if (row == 0u)
    {
        c = vec3<f32>(value, 0.0, 0.0);
    }
    else if (row == 1u)
    {
        c = vec3<f32>(0.0, value, 0.0);
    }
    else if (row == 2u)
    {
        c = vec3<f32>(0.0, 0.0, value);
    }
    else
    {
        c = vec3<f32>(value, value, value);
    }

    // tonemap the ramps to give them perceptual headroom, but don't tonemap the reference-white tick.
    c = tonemap(c, kTonemapMode);

    // Reference-white marker: thin inverted tick where value crosses 1.0.
    let markerX = 1.0 / exposureMax;
    if (markerX <= 1.0 && abs(uv.x - markerX) < 0.0015)
    {
        c = vec3<f32>(1.0) - clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
    }

    return c;
}

fn stepChart(uv : vec2<f32>) -> vec3<f32>
{
    let stops = array<f32, 10>(0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0);
    let n = 10u;
    let col = u32(clamp(floor(uv.x * f32(n)), 0.0, f32(n) - 1.0));
    let v = stops[col];
    let localX = fract(uv.x * f32(n));

    var c = vec3<f32>(v);
    if (abs(v - 1.0) < 0.001)
    {
        let edge = min(localX, 1.0 - localX);
        if (edge < 0.04)
        {
            c = vec3<f32>(1.0, 0.0, 0.0);
        }
    }
    return c;
}

fn interpolationTest(uv : vec2<f32>) -> vec3<f32>
{
    let red = vec3<f32>(1.0, 0.0, 0.0);
    let green = vec3<f32>(0.0, 1.0, 0.0);
    let blue = vec3<f32>(0.0, 0.0, 1.0);

    var c : vec3<f32>;
    if (uv.x < 0.5)
    {
        let t = uv.x / 0.5;
        c = mix(red, green, t);
    }
    else
    {
        let t = (uv.x - 0.5) / 0.5;
        c = mix(green, blue, t);
    }
    return c;
}

@fragment
fn FsMain(in : VertexOutput) -> @location(0) vec4<f32>
{
    let uv = in.uv;
    var outColor : vec3<f32>;

    if (uv.y < 0.25)
    {
        outColor = gamutSwatches(uv);
    }
    else if (uv.y < 0.50)
    {
        outColor = channelRamps(uv, kExposureMax);
    }
    else if (uv.y < 0.75)
    {
        outColor = stepChart(uv);
    }
    else
    {
        outColor = interpolationTest(uv);
    }

    // Faint separator lines at each band boundary.
    let bandLocalY = fract(uv.y * 4.0);
    if (bandLocalY < 0.002 || bandLocalY > 0.998)
    {
        outColor = vec3<f32>(0.5);
    }

    if (kTransferFunctionMode == 1u)
    {
        outColor = srgbToLinear(outColor);
    }
    else if (kTransferFunctionMode == 2u)
    {
        outColor = linearToSrgb(outColor);
    }

    return vec4<f32>(outColor, 1.0);
}
)";
