implementing OceanFft;
__include OceanFftCommon;

internal groupshared half4 s_SharedDataA[FFT_SIZE];
internal groupshared half4 s_SharedDataB[FFT_SIZE];

internal half4 DoIFFT_Radix2(in const uint threadIdx, in const half4 input)
{
    const uint bitReversedIdx = reversebits(threadIdx) >> (32 - FFT_LOG2_SIZE);
    s_SharedDataA[bitReversedIdx] = input;
    s_SharedDataB[bitReversedIdx] = 0.0h; // zero out the other
    GroupMemoryBarrierWithGroupSync();
    uint startStage = 0;
    bool pingPong = false;

#if defined(FFT_USE_WAVE_OPS)
    {
        half4 threadData = s_SharedDataA[threadIdx];

        [unroll(FFT_LOG2_WAVE_SIZE)]
        for (uint stage = 0; stage < FFT_LOG2_WAVE_SIZE; ++stage)
        {
            threadData = IFFT_WaveOpsRadix2Step(threadData, threadIdx, stage);
        }

        // write wave data back to the LDS
        s_SharedDataA[threadIdx] = half4(threadData);
        GroupMemoryBarrierWithGroupSync();
        startStage = FFT_LOG2_WAVE_SIZE;
    }
#endif

    for (uint stage = startStage; stage < FFT_LOG2_SIZE; ++stage)
    {
        const uint logStep = 1u << stage;
        const uint groupSize = logStep << 1;
        const uint j = threadIdx & (logStep - 1);
        const half theta = -half(OceanFft_2PI) * half(j) / half(groupSize);
        half2 phaseRotation = ComplexExp(theta);
        phaseRotation.y *= -1.0h;

        const uint partnerIdx = threadIdx ^ logStep;
        half4 sourceData = pingPong ? s_SharedDataB[threadIdx] : s_SharedDataA[threadIdx];
        half4 partnerData = pingPong ? s_SharedDataB[partnerIdx] : s_SharedDataA[partnerIdx];
        
        const bool isLow = (threadIdx & logStep) == 0;
        half4 a = isLow ? sourceData : partnerData;
        half4 b = isLow ? partnerData : sourceData;
        half4 rotated = half4(ComplexMul(phaseRotation, b.xy),
                              ComplexMul(phaseRotation, b.zw));
        
        if (pingPong)
        {
            s_SharedDataA[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated) ;
            s_SharedDataA[threadIdx] *= FFT_RADIX2_SCALE_FACTOR;
        }
        else
        {
            s_SharedDataB[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated);
            s_SharedDataB[threadIdx] *= FFT_RADIX2_SCALE_FACTOR;
        }
        pingPong = !pingPong;

        GroupMemoryBarrierWithGroupSync();
    }

    return pingPong ? s_SharedDataB[threadIdx] : s_SharedDataA[threadIdx];
}

[[vk::binding(0, 0)]] RWStructuredBuffer<half4> Radix2_IFFT_Input;
[[vk::binding(0, 1)]] Texture1D<half2> Radix2_IFFT_PhaseRotationLUT;
[[vk::binding(0, 2)]] bool Radix2_IFFT_Transpose;

[numthreads(FFT_SIZE, 1, 1)]
void Radix2_IFFT(
    in uint3 dispatchThreadId : SV_DispatchThreadID,
    in uint3 groupId : SV_GroupID)
{
    uint threadId = dispatchThreadId.x;
    uint2 targetIndex;
    if (Radix2_IFFT_Transpose)
    {
        targetIndex = dispatchThreadId.yx;
    }
    else
    {
        targetIndex = dispatchThreadId.xy;
    }

    uint linearizedIndex = FFT_SIZE * FFT_SIZE * groupId.z + FFT_SIZE * targetIndex.y + targetIndex.x;

    half4 inputValue = Radix2_IFFT_Input[linearizedIndex];
    Radix2_IFFT_Input[linearizedIndex] = DoIFFT_Radix2(threadId, inputValue);
}

// apply checkerboard to the final output of the IFFT, to recenter it for rendering
[numthreads(8, 8, 1)]
void Radix2_IFFT_Permute(
    in uint3 dispatchThreadId : SV_DispatchThreadID,
    in uint3 groupId : SV_GroupID)
{
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * dispatchThreadId.z + FFT_SIZE * dispatchThreadId.y + dispatchThreadId.x;
    half4 value = Radix2_IFFT_Input[linearizedIndex];
    value *= 1.0h - 2.0h * half4((dispatchThreadId.x + dispatchThreadId.y) % 2);
    Radix2_IFFT_Input[linearizedIndex] = value;
}
