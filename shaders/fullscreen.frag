#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D inputTexture;
layout(set = 0, binding = 1) uniform sampler inputSampler;

layout(push_constant) uniform FullscreenParams {
    float exposure;
    uint debugView;
    float pad0;
    float pad1;
} params;

vec3 aces_fitted(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

bool is_hdr_view(uint debugView) {
    return debugView == 0u ||
           debugView == 7u ||
           debugView == 8u ||
           debugView == 9u ||
           debugView == 10u ||
           debugView == 22u ||
           debugView == 27u;
}

void main() {
    vec3 color = texture(sampler2D(inputTexture, inputSampler), vUv).rgb;
    if (is_hdr_view(params.debugView)) {
        color = aces_fitted(max(color, vec3(0.0)));
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    outColor = vec4(color, 1.0);
}
