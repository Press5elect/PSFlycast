# NVIDIA Image Scaling

`NIS_Config.h` and `NIS_Scaler.h` are NVIDIA's, unmodified, from
https://github.com/NVIDIAGameWorks/NVIDIAImageScaling at
35e13ba316c98eeecf16f37eae70ce88019911f6 (v1.0.3), under the MIT licence in
LICENSE.txt. `ps5_fsr.cpp` runs `NIS_Scaler.h` as a compute shader (compiled
at run time as GLSL, in 32-bit floats, writing RGBA8; its two filter tables
are given to it as constant arrays made from `NIS_Config.h`'s) when
"Upscaling" is NIS.
