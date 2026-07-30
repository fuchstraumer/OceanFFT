implementing OceanFft;
__include OceanFftCommon;

internal groupshared half4 s_DataAf16[FFT_SIZE];
internal groupshared half4 s_DataBf16[FFT_SIZE];

internal void ButterflyValues_Radix2(uint stage, uint outputIndex, out uint2 inputIndices, out half2 phaseRotation)
{
    const uint logStep = 1u << stage; // 2^stage
    const uint groupSize = logStep << 1;
    const uint j = outputIndex & (logStep - 1);            // outputIndex % logStep
    const uint groupBase = outputIndex & ~(groupSize - 1); // outputIndex with its low bits cleared
    const uint lowerInput = groupBase + j;
    const half theta = (-half(OceanFft_2PI) / half(groupSize)) * half(j);
    phaseRotation = ComplexExp(theta);
    phaseRotation.y *= -1.0h; // we are always doing the IFFT, so we need to conjugate the phase rotation
    inputIndices = uint2(lowerInput, lowerInput + logStep);
}

half4 DoIFFT_Radix2_f16(in const uint threadIdx, in const half4 input)
{
    const uint bitReversedIdx = reversebits(threadIdx) >> (32 - FFT_LOG2_SIZE);
    s_SharedDataA[bitReversedIdx] = input;
    GroupMemoryBarrierWithGroupSync();

    uint startStage = 0;
    bool pingPong = false;
    const half scaleFactor = (half)rsqrt(2.0h);

    if (FFT_USE_WAVE_OPS)
    {
        half4 threadData = s_SharedDataA[threadIdx];

        [unroll(FFT_LOG2_WAVE_SIZE)]
        for (uint stage = 0; stage < FFT_LOG2_WAVE_SIZE; ++stage)
        {
            uint logStep = 1u << stage;
            // compute butterfly values for this stage/thread
            uint2 inputIndices;
            half2 phaseRotation;
            ButterflyValues_Radix2(stage, threadIdx, inputIndices, phaseRotation);
            // grab other data from neighboring thread
            half4 partnerData = WaveReadLaneAt(threadData, WaveGetLaneIndex() ^ logStep);
            const bool isLow = (threadIdx & logStep) == 0;
            half4 a = isLow ? threadData : partnerData;
            half4 b = isLow ? partnerData : threadData;
            half4 rotated = half4(ComplexMul(phaseRotation, b.xy), ComplexMul(phaseRotation, b.zw));

            threadData = isLow ? a + rotated : a - rotated;
            threadData *= scaleFactor;
        }

        // write wave data back to the LDS
        s_SharedDataA[threadIdx] = half4(threadData);
        GroupMemoryBarrierWithGroupSync();
        startStage = FFT_LOG2_WAVE_SIZE;
    }

    for (uint stage = startStage; stage < FFT_LOG2_SIZE; ++stage)
    {
        uint2 inputIndices;
        half2 phaseRotation;
        ButterflyValues_Radix2(stage, threadIdx, inputIndices, phaseRotation);

        half4 a = pingPong ? s_SharedDataB[inputIndices.x] : s_SharedDataA[inputIndices.x];
        half4 b = pingPong ? s_SharedDataB[inputIndices.y] : s_SharedDataA[inputIndices.y];

        half4 rotated = half4(ComplexMul(phaseRotation, b.xy), ComplexMul(phaseRotation, b.zw));
        const bool isLow = (threadIdx & (1u << stage)) == 0;

        if (pingPong)
        {
            s_SharedDataA[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated);
            s_SharedDataA[threadIdx] *= scaleFactor;
        }
        else
        {
            s_SharedDataB[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated);
            s_SharedDataB[threadIdx] *= scaleFactor;
        }

        pingPong = !pingPong;

        GroupMemoryBarrierWithGroupSync();
    }

    return pingPong ? s_SharedDataB[threadIdx] : s_SharedDataA[threadIdx];
}

uniform RWStructuredBuffer<half4> Radix2_IFFT_Input_f16;
uniform bool Radix2_IFFT_Transpose_f16;
uniform bool Radix2_IFFT_DoScale_f16;
uniform bool Radix2_IFFT_DoPermute_f16;

[numthreads(FFT_SIZE, 1, 1)]
void Radix2_IFFT_f16(in uint3 dispatchThreadId : SV_DispatchThreadID,
                          in uint3 groupThreadId : SV_GroupThreadID,
                          in uint3 groupId : SV_GroupID)
{
    uint threadId = dispatchThreadId.x;
    uint2 targetIndex;
    if (Radix2_IFFT_Transpose_f16)
    {
        targetIndex = dispatchThreadId.yx;
    }
    else
    {
        targetIndex = dispatchThreadId.xy;
    }

    // linearize targetIndex to a single index for the buffer
    // z = cascade index, so it's FFT_SIZE * FFT_SIZE * groupID.z
    // x = targetIndex.x, y = targetIndex.y, so it's FFT_SIZE * targetIndex.y + targetIndex.x
    // total = FFT_SIZE * FFT_SIZE * groupID.z + FFT_SIZE * targetIndex.y + targetIndex.x
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * groupId.z + FFT_SIZE * targetIndex.y + targetIndex.x;

    half4 inputValue = Radix2_IFFT_Input_f16[linearizedIndex];
    half4 outputValue = DoIFFT_Radix2_f16(threadId, inputValue);
    Radix2_IFFT_Input_f16[linearizedIndex] = outputValue;
}

// apply checkerboard to the final output of the IFFT, to recenter it for rendering
[numthreads(8, 8, 1)]
void Radix2_IFFT_Scale(in uint3 dispatchThreadId: SV_DispatchThreadID,
                       in uint3 groupId: SV_GroupID)
{
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * dispatchThreadId.z + FFT_SIZE * dispatchThreadId.y + dispatchThreadId.x;
    half4 value = Radix2_IFFT_Input_f16[linearizedIndex];
    if (Radix2_IFFT_DoPermute_f16)
    {
        value *= 1.0h - 2.0h * half4((dispatchThreadId.x + dispatchThreadId.y) % 2);
    }
    Radix2_IFFT_Input_f16[linearizedIndex] = value;
}
