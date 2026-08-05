set(DAWN_FETCH_DEPENDENCIES ON)
set(DAWN_BUILD_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(VulkanHeaders_DIR $ENV{VULKAN_SDK}/Include)
# disable desktop targets we won't be using
set(DAWN_ENABLE_DESKTOP_GL OFF)
set(DAWN_ENABLE_OPENGLES OFF)
set(DAWN_ENABLE_D3D11 OFF)
# we *do* want to make sure we enable DXC, as it's required to enable and use
# both subgroups and shader-f16
set(DAWN_USE_BUILT_DXC ON)
# Vulkan currently broken because of the Nvidia fp16 bugs, disabling
set(DAWN_ENABLE_VULKAN OFF)
set(DAWN_ENABLE_SPIRV_VALIDATION OFF)
set(DAWN_BUILD_SAMPLES OFF)
set(DAWN_BUILD_PROTOBUF OFF)
# using GLFW
set(DAWN_USE_WINDOWS_UI OFF)
# not using fuzzers
set(TINT_BUILD_FUZZERS OFF)
set(TINT_BUILD_DOCS OFF)
set(TINT_BUILD_TESTS OFF)
set(TINT_BUILD_SAMPLES OFF)
set(TINT_BUILD_CMD_TOOLS OFF)
# disable a few components we don't need
set(TINT_BUILD_GLSL_VALIDATOR OFF)
set(TINT_BUILD_SPV_WRITER OFF)
set(TINT_BUILD_SPV_READER OFF)
set(TINT_ENABLE_IR_DUMPING OFF)
set(TINT_BUILD_IR_BINARY OFF)
