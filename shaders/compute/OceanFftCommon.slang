implementing OceanFft;

internal const static float OceanFft_PI = 3.1415926535897932384626433832795f;
internal const static float OceanFft_2PI = 6.283185307179586476925286766559f;
internal const static float OceanFft_G = 9.81f;
internal const static float OceanFft_SigmaOverRho = 0.074e-3f; // surface tension over density of water

internal const static uint FFT_SIZE_MIN = 256u;
internal const static uint FFT_SIZE_REGULAR = 512u;
internal const static uint FFT_SIZE_LARGE = 1024u;
internal const static uint FFT_SIZE = FFT_SIZE_REGULAR;
internal const static uint FFT_NUM_WAVE_CASCADES = 4u;
// FFT_SIZE_MIN: 8, FFT_SIZE_REGULAR: 9, FFT_SIZE_LARGE: 10
internal const static uint FFT_LOG2_SIZE = firstbithigh(FFT_SIZE);
internal const static uint FFT_LOG4_SIZE = FFT_LOG2_SIZE / 2u;
// get remainder so we know how many radix-2 stages we need after radix-4 stages
internal const static uint FFT_LOG4_REMAINDER = FFT_LOG2_SIZE % 2u;
// Pass -DOCEAN_FFT_DISABLE_WAVE_OPS to slangc to compile the fallback WGSL variant
#ifdef OCEAN_FFT_DISABLE_WAVE_OPS
internal const static bool FFT_USE_WAVE_OPS = false;
#else
internal const static bool FFT_USE_WAVE_OPS = true;
#endif
internal const static uint FFT_WAVE_SIZE = 32u;
// FFT_LOG2_WAVE_SIZE: 5, FFT_LOG4_WAVE_SIZE: 2
internal const static uint FFT_LOG2_WAVE_SIZE = FFT_USE_WAVE_OPS ? firstbithigh(FFT_WAVE_SIZE) : 0u;
internal const static uint FFT_LOG4_WAVE_SIZE = FFT_USE_WAVE_OPS ? 2u : 0u;
internal const static half FFT_RADIX2_SCALE_FACTOR = 0.5h;
internal const static half FFT_RADIX4_SCALE_FACTOR = 0.25h;

internal const static uint FFT_COMMON_BIND_LOC = 0;
internal const static uint FFT_RADIX2_BIND_LOC = 1;
internal const static uint FFT_RADIX4_BIND_LOC = 2;

public struct SpectrumParameters
{
    uint NoiseSeed;
    uint DirectionalSpreadingMode;
    float WindSpeed;
    float Fetch;
    float Depth;
    float SwellAmount;
    float Alignment;
    float LocalWindDirection;
    float SwellDirection;
    float FadeLength;
    float PeakingFactor;
};

public struct CascadeParameters
{
    float4 LengthScales;
    float4 CutoffsHigh;
    float4 CutoffsLow;
    uint2 Dimensions;
    float2 Pad0;
};

public struct IfftParameters
{
    bool Transpose;
    uint WaveSize;
    uint Log4WaveSize;
    uint Pad0;
}

internal int LinearizeThreadIndex(uint3 threadIdx, in uniform uint2 dimensions)
{
    return threadIdx.x + (dimensions.x * (threadIdx.y + (dimensions.y * threadIdx.z)));
}

internal float2 ComplexAdd(in float2 a, in float2 b)
{
    return a + b;
}

internal half2 ComplexAdd(in half2 a, in half2 b)
{
    return a + b;
}

internal float2 ComplexSub(in float2 a, in float2 b)
{
    return a - b;
}

internal half2 ComplexSub(in half2 a, in half2 b)
{
    return a - b;
}

internal float2 ComplexExp(in float x)
{
    float2 result;
    sincos(x, result.y, result.x);
    return result;
}

internal half2 ComplexExp(in half x)
{
    half2 result;
    sincos(x, result.y, result.x);
    return result;
}

// LUT has to be 2D because WGSL does not like 1D textures. 
[[vk::binding(FFT_COMMON_BIND_LOC, 0)]] uniform Texture1D<half2> FFT_SinCos_LUT;
[[vk::binding(FFT_COMMON_BIND_LOC, 1)]] uniform SamplerState FFT_LUT_Sampler;
// LUT-based complex exponential, allowing us to stay entirely in half precision pipe for IDFT math
// RGA shows F32 ops showing up only around this function, so this should help knock those into f16
internal half2 ComplexExp(in const uint log2GroupSize, in const uint j, in const uint tapIndex)
{
    const float fftSizeInv = 1.0f / float(FFT_SIZE);
    const uint mask = (1u << log2GroupSize) - 1u;
    const uint p = (j * tapIndex & mask) << (FFT_LOG2_SIZE - log2GroupSize);
    const float uCoord = (float(p) + 0.5f) * fftSizeInv;
    // texture has y coord pre-conjugated, so one less mul as well
    return FFT_SinCos_LUT.SampleLevel(FFT_LUT_Sampler, uCoord, 0.0f);
}

internal float2 ComplexMul(in float2 a, in float2 b)
{
    // (a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x)
    float2 addend = a.yy * b.yx;
    return mad(a.xx, b, float2(-addend.x, addend.y));
}

internal half2 ComplexMul(in half2 a, in half2 b)
{
    // mad() not defined for halves, do it old fashioned way
    return half2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

internal float2 ComplexConj(in float2 a)
{
    return float2(a.x, -a.y);
}

internal half2 ComplexConj(in half2 a)
{
    return half2(a.x, -a.y);
}

internal float2 ComplexMulI(in float2 a)
{
    return float2(-a.y, a.x);
}

internal half2 ComplexMulI(in half2 a)
{
    return half2(-a.y, a.x);
}

internal float2 ComplexMulNegI(in float2 a)
{
    return float2(a.y, -a.x);
}

internal half2 ComplexMulNegI(in half2 a)
{
    return half2(a.y, -a.x);
}

internal float2 ComplexDiv(in float2 a, in float2 b)
{
    float2 denom = 1.0f / dot(a, b);
    return ComplexMul(a, ComplexConj(b)) * denom.xx;
}

internal half2 ComplexDiv(in half2 a, in half2 b)
{
    half2 denom = 1.0h / dot(a, b);
    return ComplexMul(a, ComplexConj(b)) * denom.xx;
}

internal half2 Radix2PhaseRotation(
    in const uint threadIdx,
    in const uint stage)
{
    const uint logStep = 1u << stage;
    const uint groupSize = logStep << 1;
    const uint j = threadIdx & (logStep - 1);
    const half theta = -half(OceanFft_2PI) * half(j) / half(groupSize);
    half2 phaseRotation = ComplexExp(theta);
    phaseRotation.y *= -1.0h; // we are always doing the IFFT, so we need to conjugate the phase rotation
    return phaseRotation;
}

internal half4 IFFT_WaveOpsRadix2Step(
    in half4 threadData,
    in const uint threadIdx,
    in const uint stage)
{
    const uint logStep = 1u << stage;
    const uint partnerIdx = WaveGetLaneIndex() ^ logStep;
    const uint groupSize = logStep << 1;
    const uint j = threadIdx & (logStep - 1);
    const half theta = -half(OceanFft_2PI) * half(j) / half(groupSize);
    half2 phaseRotation = ComplexExp(theta);
    phaseRotation.y *= -1.0h; // we are always doing the IFFT, so we need to conjugate the phase rotation
    const bool isLow = (threadIdx & logStep) == 0;
    half4 partnerData = WaveReadLaneAt(threadData, partnerIdx);
    half4 a = isLow ? threadData : partnerData;
    half4 b = isLow ? partnerData : threadData;
    half4 rotated = half4(ComplexMul(phaseRotation, b.xy), ComplexMul(phaseRotation, b.zw));
    return isLow ? (a + rotated) * FFT_RADIX2_SCALE_FACTOR : (a - rotated) * FFT_RADIX2_SCALE_FACTOR;
}