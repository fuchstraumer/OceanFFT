implementing OceanFft;
__include OceanFftCommon;

[[vk::binding(FFT_PERMUTE_BIND_LOC, 0)]] uniform RWStructuredBuffer<half4> Permute_Input;

// apply checkerboard to the final output of the IFFT, to recenter it for rendering
[numthreads(8, 8, 1)]
void IFFT_Permute(
    in uint3 dispatchThreadId: SV_DispatchThreadID,
    in uint3 groupId: SV_GroupID)
{
    uint linearizedIndex = FFT_SIZE * FFT_SIZE * dispatchThreadId.z + FFT_SIZE * dispatchThreadId.y + dispatchThreadId.x;
    half4 value = Permute_Input[linearizedIndex];
    value *= 1.0h - 2.0h * half4((dispatchThreadId.x + dispatchThreadId.y) % 2);
    Permute_Input[linearizedIndex] = value;
}