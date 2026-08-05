cmake_minimum_required(VERSION 3.22)
project(VeloxRhi C CXX)

# cmake/ contains our own FindVulkanHeaders.cmake; must be set before any find_package calls
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# need this to generate various support files for the ocean fft
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if (NOT EMSCRIPTEN)
    find_package(Vulkan REQUIRED)
    set(DAWN_VULKAN_HEADERS_DIR ${Vulkan_INCLUDE_DIR})
    set(VulkanHeaders_INCLUDE_DIR ${Vulkan_INCLUDE_DIR} CACHE PATH "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# One should really not be modifying the CMAKE_CXX_FLAGS directly,
# but I need to make sure these are passed down to child projects
# and do keep things fairly low risk and minimal at least
if (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # MSVC specific flags
    # /MP: Tells cl.exe to multithread
    # /bigobj: Increases number of sections in .obj (slang can bump into this ime)
    # /arch:AVX2: Enables AVX2 instructions
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /bigobj /arch:AVX2 /Gy")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang specific flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
    if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # emulate bigobj flag as, again, slang can break on this
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
    elseif(MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mbig-obj")
    endif()
endif()

if (NOT EMSCRIPTEN)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation")
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs")
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs")
    set(GLFW_LIBRARY_TYPE "STATIC" CACHE STRING "Library type override for GLFW (SHARED, STATIC, OBJECT, or empty to follow BUILD_SHARED_LIBS)")
    add_subdirectory(third_party/glfw)
    set_target_properties(glfw PROPERTIES FOLDER "third_party")

    include(cmake/ConfigureSlang.cmake)
    add_subdirectory(third_party/slang SYSTEM EXCLUDE_FROM_ALL)
    organize_slang_targets()
    
    include(cmake/ConfigureDawn.cmake)
    add_subdirectory(third_party/dawn SYSTEM EXCLUDE_FROM_ALL)
endif()


set(MAGIC_ENUM_OPT_BUILD_EXAMPLES OFF)
set(MAGIC_ENUM_OPT_BUILD_TESTS OFF)
set(MAGIC_ENUM_OPT_INSTALL OFF)
add_subdirectory(third_party/magic_enum)

if(NOT EMSCRIPTEN)
    add_subdirectory(shaders)
endif()

# rope together source files for imgui
set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_wgpu.cpp)

set(OCEAN_FFT_GENERATED_OUTPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/generated/DonelanBannerNormLUT.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/DonelanBannerNormLUT.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/generated/SinCosLUT.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/SinCosLUT.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute/OceanFFT_Generated.slang")

add_custom_command(
    OUTPUT ${OCEAN_FFT_GENERATED_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ocean_fft_support.py"
        --config "${CMAKE_CURRENT_SOURCE_DIR}/scripts/OceanFftConfig.yaml"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ocean_fft_support.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/OceanFftConfig.yaml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating OceanFFT support files..."
    VERBATIM)

# Ensures slang file generates even if OceanFFT target isn't built first
add_custom_target(OceanFft_GeneratedSources ALL DEPENDS ${OCEAN_FFT_GENERATED_OUTPUTS})

set(VeloxRhi_Headers
    "${CMAKE_CURRENT_SOURCE_DIR}/include/generated/DonelanBannerNormLUT.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/generated/SinCosLUT.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Application.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Context.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Callbacks.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderUtils.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/VeloxErrors.hpp")

set(VeloxRhi_SOURCES
    main.cpp
    ${IMGUI_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Application.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Callbacks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderUtils.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/DonelanBannerNormLUT.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/SinCosLUT.cpp")

add_executable(VeloxRhi ${VeloxRhi_SOURCES} ${VeloxRhi_Headers})
add_dependencies(VeloxRhi OceanFft_GeneratedSources)
target_include_directories(VeloxRhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

if (NOT EMSCRIPTEN)
    target_include_directories(VeloxRhi PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dawn/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/slang/include)
endif()
# set required definition for imgui backend (dawn)
target_compile_definitions(VeloxRhi PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_DAWN)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if (ipo_supported)
    set_target_properties(VeloxRhi PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(WARNING "IPO is not supported: ${ipo_error}")
endif()

if (EMSCRIPTEN)
    target_compile_options(VeloxRhi PRIVATE
        "--use-port=emdawnwebgpu" # Use the emdawnwebgpu port for WebGPU support
        "-O3"                     # Maximum optimization
        "-flto"                   # Link-time optimization (needed at compile stage)
        "-msimd128"               # SIMD support
        "-fno-rtti"               # Disable RTTI if not needed (reduce binary size)
        "-fno-exceptions"         # Disable exceptions if not needed (reduce binary size)
    )
    target_link_options(VeloxRhi PRIVATE
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
    target_link_libraries(VeloxRhi PRIVATE emdawnwebgpu_cpp webgpu_glfw)
    set_target_properties(VeloxRhi PROPERTIES SUFFIX ".html")
else()
    target_link_libraries(VeloxRhi PRIVATE webgpu_dawn glfw webgpu_glfw webgpu_cpp)
    if (WIN32)
        target_link_libraries(VeloxRhi PRIVATE dawn_native magic_enum::magic_enum)
    endif()
endif()