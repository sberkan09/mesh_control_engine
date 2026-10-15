#pragma once
// GLSL color-vision correction (M-ACCESS step 2), shared by the 2D post pass (renderer.cpp kPostFS)
// and the 3D tonemap pass (shaders3d.h kTonemapFS). The same math as correct_color_vision
// (accessibility.cpp), which the GPU tests compare against: linear RGB, mode 1 protanopia,
// 2 deuteranopia, 3 tritanopia.

// clang-format off
#define MCE_GLSL_CVD R"(
vec3 cvd_simulate(int mode, vec3 c) {
    if (mode == 1) return vec3(dot(vec3(0.152286, 1.052583, -0.204868), c), dot(vec3(0.114503, 0.786281, 0.099216), c),
                               dot(vec3(-0.003882, -0.048116, 1.051998), c));
    if (mode == 2) return vec3(dot(vec3(0.367322, 0.860646, -0.227968), c), dot(vec3(0.280085, 0.672501, 0.047413), c),
                               dot(vec3(-0.011820, 0.042940, 0.968881), c));
    return vec3(dot(vec3(1.255528, -0.076749, -0.178779), c), dot(vec3(-0.078411, 0.930809, 0.147602), c),
                dot(vec3(0.004733, 0.691367, 0.303900), c));
}
vec3 cvd_correct(int mode, float strength, vec3 c) {
    vec3 e = c - cvd_simulate(mode, c);
    vec3 shift = mode == 3 ? vec3(e.x + 0.7 * e.z, e.y + 0.7 * e.z, 0.0) : vec3(0.0, 0.7 * e.x + e.y, 0.7 * e.x + e.z);
    return clamp(c + shift * strength, 0.0, 1.0);
}
)"
// clang-format on
