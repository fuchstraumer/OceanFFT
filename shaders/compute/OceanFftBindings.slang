implementing OceanFft;
// Single source of truth for the binding locations for the IFFT compute shaders:
// they can each refer to them by the same set of names, we can coordinate the locations
// ourselves (which is somewhat important with WebGPU, since that's so much more manual
// and we aren't doing runtime reflection at all!)

internal const static uint FFT_COMMON_BIND_LOC = 0;

[[vk::binding(FFT_COMMON_BIND_LOC, 0)]] uniform RWStructuredBuffer<half4> IfftInput;
[[vk::binding(FFT_COMMON_BIND_LOC, 1)]] ConstantBuffer<IfftParameters> IfftParams;
// LUT has to be 2D because WGSL does not like 1D textures.
[[vk::binding(FFT_COMMON_BIND_LOC, 0)]] uniform Texture1D<half2> FFT_SinCos_LUT;
[[vk::binding(FFT_COMMON_BIND_LOC, 1)]] uniform SamplerState FFT_LUT_Sampler;