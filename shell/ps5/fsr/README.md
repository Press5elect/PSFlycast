# FidelityFX Super Resolution 1.0

`ffx_a.h` and `ffx_fsr1.h` are AMD's, unmodified, from
https://github.com/GPUOpen-Effects/FidelityFX-FSR at
a21ffb8f6c13233ba336352bdff293894c706575 (v1.0.2), under the MIT licence in
`LICENSE.txt`. `ps5_fsr.cpp` compiles them as GLSL, with the title's shader
compiler, into the two passes that upscale the game's picture.
