// HDR / color-management debug test pattern.
//
// Drop-in replacement for a single triangle-list draw of 3 vertices, same as
// your original colored-triangle shader (@builtin(vertex_index), no vertex
// buffer). Uses the standard fullscreen-triangle trick instead of an actual
// triangle, so the whole viewport becomes the test chart.
//
// Layout, bottom (uv.y=0) to top (uv.y=1):
//   Band 1 [0.0, 0.3333): Four stacked ramps (R, G, B, White, top to bottom
//                         within the band) from 0 to kExposureMax.
//                         A thin inverted-color tick mark is drawn at the
//                         x position where the ramp value crosses 1.0 —
//                         i.e. exactly where SDR/"standard" tone mapping
//                         would start clipping. In "extended" mode you
//                         should see the ramp keep climbing past that tick
//                         instead of flattening out.
//   Band 2 [0.3333, 0.6667): Discrete step chart — fixed swatches at
//                         {0, .25, .5, .75, 1, 1.25, 1.5, 2, 3, 4}. The
//                         swatch at exactly 1.0 gets a red border so you
//                         can find the SDR reference-white patch at a
//                         glance and see which neighbors are above/below it.
//   Band 3 [0.6667, 1.00): Live gamma-vs-linear interpolation test — a
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
const kBand1Height : f32 = 0.333333333;
const kBand2Height : f32 = 0.666666667;

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

fn agxDefaultContrastApprox(c : vec3<f32>) -> vec3<f32>
{
    let x2 = c * c;
    let x4 = x2 * x2;
    return 15.5 * x4 * x2
           - 40.14 * x4 * c
           + 31.96 * x4
           - 6.868 * x2 * c
           + 0.4298 * x2
           + 0.1191 * c
           - 0.00232;
}

fn AgXTonemap(c : vec3<f32>) -> vec3<f32>
{
    // declare rec709 to AgX transformation matrix
    let rec709ToAgX = mat3x3<f32>(
        vec3<f32>(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
        vec3<f32>(0.0784335999999992,  0.878468636469772,  0.0784336),
        vec3<f32>(0.0792237451477643, 0.0791661274605434, 0.879142973793104)
    );
    let minEV = -12.47393;
    let maxEV = 4.026069;
    var val : vec3<f32>;
    val = linearToSrgb(c);
    val = transpose(rec709ToAgX) * val;
    val = clamp(log2(val), vec3<f32>(minEV), vec3<f32>(maxEV));
    val = (val - minEV) / (maxEV - minEV);
    val = agxDefaultContrastApprox(val);
    let AgXToRec709 = mat3x3<f32>(
        vec3<f32>(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
        vec3<f32>(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
        vec3<f32>(-0.0990297440797205, -0.0989611768448433, 1.15107367264116)
    );
    val = transpose(AgXToRec709) * val;
    val = srgbToLinear(val);
    return val;
}

fn aces_tone_map(hdr: vec3<f32>) -> vec3<f32> {
    let m1 = mat3x3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777,
    );
    let m2 = mat3x3(
        1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602,
    );
    let v = m1 * hdr;
    let a = v * (v + 0.0245786) - 0.000090537;
    let b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return m2 * (a / b);
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
        result = aces_tone_map(c);
    }
    else if (mode == 3u)
    {
        result = AgXTonemap(c);
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
    let localY = fract(uv.y * 3.0);
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
    // c = tonemap(c, kTonemapMode);

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

fn srgbToDisplayP3(c : vec3<f32>) -> vec3<f32>
{
    let srgbToP3 = mat3x3<f32>(
        vec3<f32>(0.8224621, 0.0331941, 0.0170827), // Column 0 (X)
        vec3<f32>(0.1775379, 0.9668059, 0.0723973), // Column 1 (Y)
        vec3<f32>(0.0000000, 0.0000000, 0.9105200)  // Column 2 (Z)
    );
    return srgbToP3 * c;
}

@fragment
fn FsMain(in : VertexOutput) -> @location(0) vec4<f32>
{
    let uv = in.uv;
    var outColor : vec3<f32>;
    if (uv.y < kBand1Height)
    {
        outColor = channelRamps(uv, kExposureMax);
        outColor = outColor * kExposureMax; // scale up to see the effect of tonemapping
    }
    else if (uv.y < kBand2Height)
    {
        outColor = stepChart(uv);
    }
    else
    {
        outColor = interpolationTest(uv);
        outColor = outColor * kExposureMax; // scale up to see the effect of tonemapping
    }

    // Faint separator lines at each band boundary.
    let bandLocalY = fract(uv.y * 4.0);
    if (bandLocalY < 0.002 || bandLocalY > 0.998)
    {
        outColor = vec3<f32>(0.5);
    }

    outColor = tonemap(outColor, kTonemapMode);
    // convert to Display-P3 if the surface is in that color space, so we can see the gamut difference.
    outColor = srgbToDisplayP3(outColor);

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
