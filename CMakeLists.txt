project(OceanFFT)
cmake_minimum_required(VERSION 3.10)

add_executable(OceanFFT main.cpp)



if (EMSCRIPTEN)
    target_link_options(OceanFFT PRIVATE
        "-sUSE_WEBGPU=1"
        "-sUSE_GLFW=3"
        "-sWASM=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sUSE_PTHREADS=1")
    set_target_properties(OceanFFT PROPERTIES SUFFIX ".html")
else()
    # link GLFW, Vulkan, and Dawn
endif()