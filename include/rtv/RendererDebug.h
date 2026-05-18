#pragma once

#include <cstdint>
#include <string_view>

namespace rtv {

enum class RendererDebugView : uint32_t {
    Beauty = 0,
    Variance = 1,
    Normals = 2,
    ReprojectionConfidence = 3,
    DenoiserRejection = 4,
    Depth = 5,
    Roughness = 6,
    DirectLighting = 7,
    IndirectLighting = 8,
    EmissiveContribution = 9,
    EnvironmentContribution = 10,
    TraversalSteps = 11,
    BvhDepth = 12,
    InstanceId = 13,
    MeshId = 14,
    TlasSteps = 15,
    TraversalMismatch = 16,
    LightPdf = 17,
    BsdfPdf = 18,
    MisWeight = 19,
    DirectSampleType = 20,
    Albedo = 21,
    ClayMaterial = 22,
    FirstBounceThroughput = 23,
    SecondaryEnvironmentMiss = 24,
    BounceCount = 25,
    SecondaryEnvironmentRadiance = 26,
    WhiteEnvironmentTransport = 27,
};

struct RendererDebugParams {
    uint32_t view = static_cast<uint32_t>(RendererDebugView::Beauty);
    uint32_t flags = 0;
    float scale = 1.0f;
    float padding0 = 0.0f;
};

[[nodiscard]] RendererDebugView parseRendererDebugView(std::string_view value);
[[nodiscard]] const char* rendererDebugViewName(RendererDebugView view);

} // namespace rtv
