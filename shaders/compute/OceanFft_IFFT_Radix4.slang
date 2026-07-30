implementing OceanFft;
__include OceanFftCommon;

// renaming these slightly from radix-2 impl, as either the linter is paranoid and thinks we have
// ambiguous names, or its going to actually be a problem. not worth the risk so removing this
internal groupshared half4 s_DataA[FFT_SIZE];
internal groupshared half4 s_DataB[FFT_SIZE];

uniform RWStructuredBuffer<half4> Radix4_IFFT_Input;
uniform bool Radix4_IFFT_Transpose;
uniform bool Radix4_IFFT_BonusWaveOpsRound;

// Base-4 digit-reversal of the low FFT_LOG2_SIZE bits of idx.
internal uint DigitReverse4(uint idx)
{
    if (FFT_LOG4_REMAINDER == 0)
    {
        // Unchanged - this is correct as-is for even FFT_LOG2_SIZE.
        uint x = idx;
        x = ((x >> 2) & 0x33333333) | ((x & 0x33333333) << 2);
        x = ((x & 0x0F0F0F0F) << 4) | ((x & 0xF0F0F0F0) >> 4);
        x = ((x & 0x00FF00FF) << 8) | ((x & 0xFF00FF00) >> 8);
        x = (x << 16) | (x >> 16);
        return x >> (32u - FFT_LOG2_SIZE);
    }
    else
    {
        // Odd FFT_LOG2_SIZE (e.g. 512): bit-position permutation pi(i), verified
        // against the actual radix-4 x4 + radix-2 stage order in DoIFFT_Radix4.
        const uint L = FFT_LOG2_SIZE;
        uint result = 0u;
        [unroll(FFT_LOG2_SIZE)]
        for (uint i = 0; i < L; ++i)
        {
            if (bool((idx >> i) & 1u))
            {
                uint pi;
                if (i == 0)
                {
                    pi = L - 1u;
                }
                else if ((i & 1u) == 0u)
                {
                    pi = L - i;
                }
                else
                {
                    pi = L - 2u - i;
                }
                result |= (1u << pi);
            }
        }
        return result;
    }
}

internal void Radix4Twiddles(uint logStep,
                    uint outputIndex,
                    out half2 twiddle1,
                    out half2 twiddle2,
                    out half2 twiddle3)
{
    const uint groupSize = logStep << 2; // 4 * logStep
    const uint j = outputIndex & (logStep - 1); // outputIndex % logStep
    const half theta = (-half(OceanFft_2PI) / half(groupSize)) * half(j);
    twiddle1 = ComplexExp(theta);
    twiddle2 = ComplexExp(2.0h * theta);
    twiddle3 = ComplexExp(3.0h * theta);
    twiddle1.y *= -1.0h;
    twiddle2.y *= -1.0h;
    twiddle3.y *= -1.0h;
}

internal void ButterflyValues_Radix4(uint logStep,
                            uint outputIndex,
                            out uint4 inputIndices,
                            out half2 twiddle1,
                            out half2 twiddle2,
                            out half2 twiddle3)
{
    Radix4Twiddles(logStep, outputIndex, twiddle1, twiddle2, twiddle3);
    const uint groupSize = logStep << 2;
    const uint j = outputIndex & (logStep - 1); // outputIndex % logStep
    // outputIndex - (outputIndex % groupSize)
    const uint groupBase = outputIndex & ~(groupSize - 1); 
    inputIndices = uint4(
        groupBase + j,
        groupBase + j + logStep,
        groupBase + j + 2 * logStep,
        groupBase + j + 3 * logStep);
}

internal void Radix4Combine(half4 a0,
                   half4 a1,
                   half4 a2,
                   half4 a3,
                   out half4 y0,
                   out half4 y1,
                   out half4 y2,
                   out half4 y3)
{
    const half4 s02p = a0 + a2;
    const half4 s02m = a0 - a2;
    const half4 s13p = a1 + a3;
    const half4 s13m = a1 - a3;

    y0 = s02p + s13p;
    y2 = s02p - s13p;

    const half4 rotP = half4(ComplexMulI(s13m.xy),    ComplexMulI(s13m.zw));
    const half4 rotM = half4(ComplexMulNegI(s13m.xy), ComplexMulNegI(s13m.zw));

    y1 = s02m + rotP;
    y3 = s02m + rotM;
}

// "Radix-4" IFFT - not actually pure radix-4, it decomposes as needed into radix-4 stages
half4 DoIFFT_Radix4(in const uint threadIdx, in const half4 input)
{
    const uint digitReversedIdx = DigitReverse4(threadIdx);
    s_DataA[digitReversedIdx] = half4(input);
    s_DataB[digitReversedIdx] = 0.0h; // zero out the other
    GroupMemoryBarrierWithGroupSync();

    bool pingPong = false;
    uint startStage = 0;

    if (FFT_USE_WAVE_OPS)
    {
        half4 threadData = s_DataA[threadIdx];
        uint bitOffset = 0;
        // 2 radix-4 stages: bits 0-3, i.e. 16 lanes
        [unroll(FFT_LOG4_WAVE_SIZE)]
        for (; bitOffset + 2 < FFT_LOG2_WAVE_SIZE; bitOffset += 2)
        {
            const uint logStep = 1u << bitOffset;
            half2 tw1, tw2, tw3;
            Radix4Twiddles(logStep, threadIdx, tw1, tw2, tw3);

            const uint lane = WaveGetLaneIndex();
            // outputIndex - (outputIndex % groupSize)
            const uint groupBase = lane & ~(3u << bitOffset);

            const half4 v0 = WaveReadLaneAt(threadData, groupBase | (0u << bitOffset));
            const half4 v1 = WaveReadLaneAt(threadData, groupBase | (1u << bitOffset));
            const half4 v2 = WaveReadLaneAt(threadData, groupBase | (2u << bitOffset));
            const half4 v3 = WaveReadLaneAt(threadData, groupBase | (3u << bitOffset));

            const half4 a0 = v0;
            const half4 a1 = half4(ComplexMul(tw1, v1.xy), ComplexMul(tw1, v1.zw));
            const half4 a2 = half4(ComplexMul(tw2, v2.xy), ComplexMul(tw2, v2.zw));
            const half4 a3 = half4(ComplexMul(tw3, v3.xy), ComplexMul(tw3, v3.zw));

            half4 y0, y1, y2, y3;
            Radix4Combine(a0, a1, a2, a3, y0, y1, y2, y3);

            // get relative index of this digit in current group
            const uint myDigit = (lane >> bitOffset) & 3u;
            switch (myDigit)
            {
                case 0: threadData = y0; break;
                case 1: threadData = y1; break;
                case 2: threadData = y2; break;
                case 3: threadData = y3; break;
            }
            threadData *= FFT_RADIX4_SCALE_FACTOR;
        }

        // don't run the bonus wave ops round if param not set, because this changes
        // our derivation of the bit reversal function (since that depends on stage ordering)
        if (Radix4_IFFT_BonusWaveOpsRound && bitOffset < FFT_LOG2_WAVE_SIZE)
        {
            threadData = IFFT_WaveOpsRadix2Step(
                threadData,
                threadIdx,
                bitOffset);
        }

        s_DataA[threadIdx] = half4(threadData);
        GroupMemoryBarrierWithGroupSync();
        // 2 stages per radix-4 stage + 1 stage for radix-2 tail
        startStage = FFT_LOG4_WAVE_SIZE * 2u + (Radix4_IFFT_BonusWaveOpsRound ? 1u : 0u);
    }

    // Remaining radix-4 stages via LDS, ping-ponging exactly like the radix-2 path
    uint bitOffset = startStage;
    for (; bitOffset + 2 <= FFT_LOG2_SIZE; bitOffset += 2)
    {
        const uint logStep = 1u << bitOffset;
        uint4 inputIndices;
        half2 tw1, tw2, tw3;
        ButterflyValues_Radix4(logStep, threadIdx, inputIndices, tw1, tw2, tw3);

        const half4 v0 = pingPong ? s_DataB[inputIndices.x] : s_DataA[inputIndices.x];
        const half4 v1 = pingPong ? s_DataB[inputIndices.y] : s_DataA[inputIndices.y];
        const half4 v2 = pingPong ? s_DataB[inputIndices.z] : s_DataA[inputIndices.z];
        const half4 v3 = pingPong ? s_DataB[inputIndices.w] : s_DataA[inputIndices.w];

        const half4 a0 = v0;
        const half4 a1 = half4(ComplexMul(tw1, v1.xy), ComplexMul(tw1, v1.zw));
        const half4 a2 = half4(ComplexMul(tw2, v2.xy), ComplexMul(tw2, v2.zw));
        const half4 a3 = half4(ComplexMul(tw3, v3.xy), ComplexMul(tw3, v3.zw));

        half4 y0, y1, y2, y3;
        Radix4Combine(a0, a1, a2, a3, y0, y1, y2, y3);

        const uint myDigit = (threadIdx >> bitOffset) & 3u;
        half4 result;
        switch (myDigit)
        {
            case 0:
                result = y0;
                break;
            case 1:
                result = y1;
                break;
            case 2:
                result = y2;
                break;
            case 3:
                result = y3;
                break;
        }

        if (pingPong)
        {
            s_DataA[threadIdx] = half4(result) * FFT_RADIX4_SCALE_FACTOR;
        }
        else
        {
            s_DataB[threadIdx] = half4(result) * FFT_RADIX4_SCALE_FACTOR;
        }

        pingPong = !pingPong;
        GroupMemoryBarrierWithGroupSync();
    }

    // Trailing radix-2 stage for the one leftover bit. Only fires when wave
    // ops are enabled -- without them FFT_LOG2_SIZE is consumed exactly by
    // radix-4 stages (guaranteed even), so this compiles away to nothing.
    if (bitOffset < FFT_LOG2_SIZE)
    {
        const uint logStep = 1u << (FFT_LOG2_SIZE - 1u);
        const uint groupSize = logStep << 1;
        const uint j = threadIdx & (logStep - 1);
        const half theta = -half(OceanFft_2PI) * half(j) / half(groupSize);
        half2 phaseRotation = ComplexExp(theta);
        phaseRotation.y *= -1.0h; // we are always doing the IFFT, so we need to conjugate the phase rotation

        const uint partnerIdx = threadIdx ^ logStep;
        half4 sourceData = pingPong ? s_DataB[threadIdx] : s_DataA[threadIdx];
        half4 partnerData = pingPong ? s_DataB[partnerIdx] : s_DataA[partnerIdx];

        const bool isLow = (threadIdx & logStep) == 0;
        half4 a = isLow ? sourceData : partnerData;
        half4 b = isLow ? partnerData : sourceData;
        half4 rotated = half4(ComplexMul(phaseRotation, b.xy), ComplexMul(phaseRotation, b.zw));

        if (pingPong)
        {
            s_DataA[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated);
            s_DataA[threadIdx] *= FFT_RADIX2_SCALE_FACTOR;
        }
        else
        {
            s_DataB[threadIdx] = isLow ? half4(a + rotated) : half4(a - rotated);
            s_DataB[threadIdx] *= FFT_RADIX2_SCALE_FACTOR;
        }

        pingPong = !pingPong;

        GroupMemoryBarrierWithGroupSync();
    }

    return pingPong ? half4(s_DataB[threadIdx]) : half4(s_DataA[threadIdx]);
}

// Only dispatch this for FFT_SIZE_MIN (256) or FFT_SIZE_LARGE (1024) --
// FFT_SIZE_REGULAR (512) will fail to compile here (see #error above).
[numthreads(FFT_SIZE, 1, 1)]
void Radix4_IFFT(
    in uint3 dispatchThreadID : SV_DispatchThreadID,
    in uint3 groupID : SV_GroupID)
{
    uint threadIdx = dispatchThreadID.x;
    uint2 targetIndex;
    if (Radix4_IFFT_Transpose)
    {
        targetIndex = dispatchThreadID.yx;
    }
    else
    {
        targetIndex = dispatchThreadID.xy;
    }

    // linearize targetIndex to a single index for the buffer
    // z = cascade index, so it's FFT_SIZE * FFT_SIZE * groupID.z
    // x = targetIndex.x, y = targetIndex.y, so it's FFT_SIZE * targetIndex.y + targetIndex.x
    // total = FFT_SIZE * FFT_SIZE * groupID.z + FFT_SIZE * targetIndex.y + targetIndex.x
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * groupID.z + FFT_SIZE * targetIndex.y + targetIndex.x;

    half4 input = Radix4_IFFT_Input[linearizedIndex];
    Radix4_IFFT_Input[linearizedIndex] = DoIFFT_Radix4(threadIdx, input);
}

[numthreads(8,8,1)]
void Radix4_IFFT_Permute(
    in uint3 dispatchThreadId : SV_DispatchThreadID,
    in uint3 groupID : SV_GroupID)
{
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * dispatchThreadId.z + FFT_SIZE * dispatchThreadId.y + dispatchThreadId.x;
    half4 value = Radix4_IFFT_Input[linearizedIndex];
    value *= 1.0h - 2.0h * half4((dispatchThreadId.x + dispatchThreadId.y) % 2);
    Radix4_IFFT_Input[linearizedIndex] = value;
}