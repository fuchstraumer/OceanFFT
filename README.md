# VeloxRHI
Convenience wrapper around WebGPU functionality, to make deploying graphical demos to my Github Pages easier to do. Uses Slang shaders so that I can share source code with my other projects with much less conversion work, and uses GLFW so that I can debug on native.

This is *not* intended to be a graphics engine at all - there's no input, no push by me to include any kind of workflow tooling, it's just designed to package and compile small WebGPU demos into bundles I can easily get on my website. Oftentimes I want to show off my work, but my projects are either tied up in DiamondDogs or UE5 - neither of which a recruiter or hiring manager is likely to actually download and build and run themselves. Web demos make it much easier to show off things, while still allowing me to keep my full featured code separate. 

## Building

(Get into generated files, cooking shaders for webgpu, etc, this still needs work because we barely have it working for us locally)