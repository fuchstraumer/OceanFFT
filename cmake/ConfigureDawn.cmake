set(DAWN_FETCH_DEPENDENCIES ON)
set(DAWN_BUILD_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(VulkanHeaders_DIR $ENV{VULKAN_SDK}/Include)
# disable desktop targets we won't be using
set(DAWN_ENABLE_DESKTOP_GL OFF)
set(DAWN_ENABLE_OPENGLES OFF)
set(DAWN_ENABLE_D3D11 OFF)
set(DAWN_ENABLE_NULL OFF)
set(DAWN_BUILD_SAMPLES OFF)
# we won't be baking IR: embed WGSL into generated C++
set(DAWN_BUILD_PROTOBUF OFF)
# using GLFW
set(DAWN_ENABLE_WINDOWS_UI OFF)
# not using fuzzers
set(TINT_BUILD_FUZZERS OFF)
set(TINT_BUILD_DOCS OFF)
set(TINT_BUILD_TESTS OFF)
set(TINT_BUILD_SAMPLES OFF)
set(TINT_BUILD_CMD_TOOLS OFF)
# compiling in slang: don't need extra validation
set(TINT_BUILD_GLSL_VALIDATOR OFF)
# feeding in WGSL: don't need any of these
set(TINT_BUILD_GLSL_WRITER OFF)
set(TINT_BUILD_HLSL_WRITER OFF)
set(TINT_BUILD_SPV_READER OFF)
set(TINT_BUILD_MSL_WRITER OFF)
set(TINT_BUILD_SPV_READER OFF)
set(TINT_BUILD_SPV_WRITER OFF)
set(TINT_BUILD_NULL_WRITER OFF)
# this requires DAWN_BUILD_PROTOBUF, which we don't want to do, so disable it
set(TINT_BUILD_IR_BINARY OFF)
# no IR dumping, we validate via slang
set(TINT_ENABLE_IR_DUMPING OFF)
set(BUILD_SHARED_LIBS OFF)
