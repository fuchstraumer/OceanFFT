implementing OceanFft;
__include OceanFftCommon;
#define FFT_SIZE_MIN 128
#define FFT_SIZE_REGULAR 256
#define FFT_SIZE_LARGE 512
#define FFT_SIZE_XLARGE 1024
#define FFT_SIZE_XXLARGE 2048

#ifndef FFT_SIZE
#define FFT_SIZE FFT_SIZE_MIN
#endif

#if FFT_SIZE == FFT_SIZE_MIN
#define FFT_LOG2_SIZE 7
#elif FFT_SIZE == FFT_SIZE_REGULAR
#define FFT_LOG2_SIZE 8
#elif FFT_SIZE == FFT_SIZE_LARGE
#define FFT_LOG2_SIZE 9
#elif FFT_SIZE == FFT_SIZE_XLARGE
#define FFT_LOG2_SIZE 10
#elif FFT_SIZE == FFT_SIZE_XXLARGE
#define FFT_LOG2_SIZE 11
#endif

internal groupshared half4 s_difDataA[FFT_SIZE];
internal groupshared half4 s_difDataB[FFT_SIZE];

uniform RWStructuredBuffer<half4> Radix2_DIF_Input;
uniform bool Radix2_DIF_Transpose;
uniform bool Radix2_DIF_DoScale;
uniform bool Radix2_DIF_DoPermute;

internal void ButterflyValues(in const uint stage, in const uint index, out uint2 indices, out float2 twiddle)
{
    const uint b = FFT_SIZE >> (stage + 1);
    const uint w = b * (index / b);
    const uint i = (w + index) % FFT_SIZE;
    sincos(-OceanFft_2PI/FFT_SIZE * w, twiddle.y, twiddle.x);
    twiddle.y *= -1.0f; // we are always doing the IFFT
    indices = uint2(i, i + b);
}

float4 DoFFT_DIF(in const uint threadIdx, in const half4 input)
{
    s_difDataA[threadIdx] = input;
    GroupMemoryBarrierWithGroupSync();
    bool pingPong = false;

    for (uint stage = 0; stage < FFT_LOG2_SIZE; ++stage)
    {
        uint2 indices;
        float2 twiddle;
        ButterflyValues(stage, threadIdx, indices, twiddle);
        float4 v = pingPong ? s_difDataB[indices.y] : s_difDataA[indices.y];
        float4 u = pingPong ? s_difDataB[indices.x] : s_difDataA[indices.x];
        float4 result = u + float4(ComplexMul(twiddle, v.xy), ComplexMul(twiddle, v.zw));
        if (pingPong)
        {
            s_difDataA[threadIdx] = half4(result);
        }
        else
        {
            s_difDataB[threadIdx] = half4(result);
        }
        pingPong = !pingPong;
    }

    return pingPong ? s_difDataB[threadIdx] : s_difDataA[threadIdx];
}

[numthreads(FFT_SIZE, 1, 1)]
void Radix2_DIF(in uint3 dispatchThreadId : SV_DispatchThreadID,
                in uint3 groupThreadId : SV_GroupThreadID,
                in uint3 groupId : SV_GroupID)
{
    uint threadId = dispatchThreadId.x;
    uint2 targetIndex;
    
    if (Radix2_DIF_Transpose)
    {
        targetIndex = dispatchThreadId.yx;
    }
    else
    {
        targetIndex = dispatchThreadId.xy;
    }
    
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * groupId.z + FFT_SIZE * targetIndex.y + targetIndex.x;
    float4 input = Radix2_DIF_Input[linearizedIndex];
    float4 output = DoFFT_DIF(threadId, half4(input));
    Radix2_DIF_Input[linearizedIndex] = half4(output);
}

[numthreads(8, 8, 1)]
void Radix2_DIF_Scale(
    uint3 dispatchThreadId: SV_DispatchThreadID,
    uint3 groupID: SV_GroupID)
{
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * dispatchThreadId.z + FFT_SIZE * dispatchThreadId.y + dispatchThreadId.x;
    half4 value = Radix2_DIF_Input[linearizedIndex];
    if (Radix2_DIF_DoScale)
    {
        value /= half(FFT_SIZE * FFT_SIZE);
    }
    if (Radix2_DIF_DoPermute)
    {
        value *= 1.0h - 2.0h * half4((dispatchThreadId.x + dispatchThreadId.y) % 2);
    }
    Radix2_DIF_Input[linearizedIndex] = value;
}