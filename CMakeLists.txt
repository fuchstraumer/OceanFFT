cmake_minimum_required(VERSION 3.22)
project(OceanFFT C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /MP /Gy")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /Oi /MP /Gy")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Gy")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /JMC /bigobj")
include(cmake/ConfigureSlang.cmake)
add_subdirectory(third_party/slang)
organize_slang_targets()

SET(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation")
SET(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs")
SET(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs")
set(GLFW_LIBRARY_TYPE "STATIC" CACHE STRING "Library type override for GLFW (SHARED, STATIC, OBJECT, or empty to follow BUILD_SHARED_LIBS)")
add_subdirectory(third_party/glfw)
set_target_properties(glfw PROPERTIES FOLDER "third_party")

include(cmake/ConfigureDawn.cmake)
add_subdirectory(third_party/dawn EXCLUDE_FROM_ALL)

# rope together source files for imgui
set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_wgpu.cpp)


add_executable(OceanFFT main.cpp ${IMGUI_SOURCES})
target_include_directories(OceanFFT PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dawn/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/slang/include)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if (ipo_supported)
    set_target_properties(OceanFFT PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(WARNING "IPO is not supported: ${ipo_error}")
endif()

if (EMSCRIPTEN)
    target_compile_options(OceanFFT PRIVATE
        "-O3"                    # Maximum optimization
        "-flto"                  # Link-time optimization (needed at compile stage)
        "-msimd128"              # SIMD support for better performance
        "-fno-rtti"              # Disable RTTI if not needed (size reduction)
        "-fno-exceptions"        # Disable exceptions if not needed (size reduction)
    )
    target_link_options(OceanFFT PRIVATE
        "--use-port=emdawnwebgpu"
        "-O3"                                    # Maximum optimization
        "-flto"                                  # Link-time optimization
        "-sUSE_GLFW=3"                           # Use Emscripten's GLFW port
        "-sALLOW_MEMORY_GROWTH=0"                # Static memory allocation
        "-sFILESYSTEM=0"                         # No filesystem support
        "-sENVIRONMENT=web"                      # Target web only
        "-sMINIMAL_RUNTIME=0"                    # Full runtime (needed for WebGPU)
        "-sMALLOC=emmalloc"                      # Smaller malloc implementation
        "-sASSERTIONS=0"                         # Disable assertions in release
        "-sSTACK_SIZE=1MB"                       # Adjust if needed
        "-sINITIAL_MEMORY=64MB"                  # Adjust based on your needs
        "-sMAXIMUM_MEMORY=512MB"                 # Cap memory usage
        "-sSIMD=1"                               # Enable SIMD support
        "--closure=1"                            # Enable Closure Compiler for JS minification
        "--closure-args=--externs=node_modules/webgpu/webgpu-closure-externs.js")
    target_link_libraries(OceanFFT PRIVATE emdawnwebgpu_cpp webgpu_glfw)
    set_target_properties(OceanFFT PROPERTIES SUFFIX ".html")
else()

    target_compile_options(OceanFFT PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/MP>
        $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>
        $<$<CXX_COMPILER_ID:MSVC>:/fp:fast>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>)

    target_link_libraries(OceanFFT PRIVATE webgpu glfw webgpu_dawn webgpu_glfw webgpu_cpp slang)
    # similar link options locally
endif()