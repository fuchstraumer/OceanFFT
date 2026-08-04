implementing OceanFft;
// Single source of truth for the binding locations for the IFFT compute shaders:
// they can each refer to them by the same set of names, we can coordinate the locations
// ourselves (which is somewhat important with WebGPU, since that's so much more manual
// and we aren't doing runtime reflection at all!)
internal const static uint IFFT_COMMON_BIND_LOC = 0;

[[vk::binding(IFFT_COMMON_BIND_LOC, 0)]] uniform RWStructuredBuffer<half4> IfftInput;
[[vk::binding(IFFT_COMMON_BIND_LOC, 1)]] ConstantBuffer<IfftParameters> IfftParams;
[[vk::binding(IFFT_COMMON_BIND_LOC, 2)]] uniform Texture1D<half2> IfftSinCosLUT;
[[vk::binding(IFFT_COMMON_BIND_LOC, 3)]] uniform SamplerState IfftLUTSampler;
