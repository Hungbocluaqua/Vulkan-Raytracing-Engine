#include "rtv/RendererDebug.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace rtv {

namespace {

[[nodiscard]] std::string normalized(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '_'), result.end());
    return result;
}

} // namespace

RendererDebugView parseRendererDebugView(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "variance") { return RendererDebugView::Variance; }
    if (key == "normal" || key == "normals") { return RendererDebugView::Normals; }
    if (key == "reprojection" || key == "reprojectionconfidence") { return RendererDebugView::ReprojectionConfidence; }
    if (key == "rejection" || key == "denoiserrejection") { return RendererDebugView::DenoiserRejection; }
    if (key == "depth") { return RendererDebugView::Depth; }
    if (key == "roughness") { return RendererDebugView::Roughness; }
    if (key == "direct" || key == "directlighting") { return RendererDebugView::DirectLighting; }
    if (key == "indirect" || key == "indirectlighting") { return RendererDebugView::IndirectLighting; }
    if (key == "emissive" || key == "emissivecontribution") { return RendererDebugView::EmissiveContribution; }
    if (key == "environment" || key == "env" || key == "environmentcontribution") { return RendererDebugView::EnvironmentContribution; }
    if (key == "traversal" || key == "traversalsteps") { return RendererDebugView::TraversalSteps; }
    if (key == "bvh" || key == "bvhdepth") { return RendererDebugView::BvhDepth; }
    if (key == "instance" || key == "instanceid") { return RendererDebugView::InstanceId; }
    if (key == "mesh" || key == "meshid") { return RendererDebugView::MeshId; }
    if (key == "tlas" || key == "tlassteps") { return RendererDebugView::TlasSteps; }
    if (key == "mismatch" || key == "traversalmismatch" || key == "tlasmismatch") { return RendererDebugView::TraversalMismatch; }
    if (key == "lightpdf" || key == "directpdf") { return RendererDebugView::LightPdf; }
    if (key == "bsdfpdf" || key == "brdfpdf") { return RendererDebugView::BsdfPdf; }
    if (key == "mis" || key == "misweight") { return RendererDebugView::MisWeight; }
    if (key == "directsample" || key == "directsampletype" || key == "sampletype") { return RendererDebugView::DirectSampleType; }
    if (key == "albedo" || key == "basecolor" || key == "basecolour") { return RendererDebugView::Albedo; }
    if (key == "clay" || key == "claymaterial" || key == "balancedclay" || key == "balancedclaymaterial" ||
        key == "white" || key == "whitematerial" || key == "whitematerialmode") {
        return RendererDebugView::ClayMaterial;
    }
    if (key == "firstbounce" || key == "firstbouncethroughput" || key == "throughput" || key == "firstbounceweight") {
        return RendererDebugView::FirstBounceThroughput;
    }
    if (key == "secondaryenvmiss" || key == "secondaryenvironmentmiss" || key == "envmiss" || key == "skyescape") {
        return RendererDebugView::SecondaryEnvironmentMiss;
    }
    if (key == "bouncecount" || key == "bounces") { return RendererDebugView::BounceCount; }
    if (key == "secondaryenvradiance" || key == "secondaryenvironmentradiance" || key == "envradiance") {
        return RendererDebugView::SecondaryEnvironmentRadiance;
    }
    if (key == "whiteenv" || key == "whiteenvironment" || key == "whiteenvironmenttransport" || key == "whitetransport") {
        return RendererDebugView::WhiteEnvironmentTransport;
    }
    return RendererDebugView::Beauty;
}

const char* rendererDebugViewName(RendererDebugView view) {
    switch (view) {
    case RendererDebugView::Beauty: return "beauty";
    case RendererDebugView::Variance: return "variance";
    case RendererDebugView::Normals: return "normals";
    case RendererDebugView::ReprojectionConfidence: return "reprojection-confidence";
    case RendererDebugView::DenoiserRejection: return "denoiser-rejection";
    case RendererDebugView::Depth: return "depth";
    case RendererDebugView::Roughness: return "roughness";
    case RendererDebugView::DirectLighting: return "direct-lighting";
    case RendererDebugView::IndirectLighting: return "indirect-lighting";
    case RendererDebugView::EmissiveContribution: return "emissive-contribution";
    case RendererDebugView::EnvironmentContribution: return "environment-contribution";
    case RendererDebugView::TraversalSteps: return "traversal-steps";
    case RendererDebugView::BvhDepth: return "bvh-depth";
    case RendererDebugView::InstanceId: return "instance-id";
    case RendererDebugView::MeshId: return "mesh-id";
    case RendererDebugView::TlasSteps: return "tlas-steps";
    case RendererDebugView::TraversalMismatch: return "traversal-mismatch";
    case RendererDebugView::LightPdf: return "light-pdf";
    case RendererDebugView::BsdfPdf: return "bsdf-pdf";
    case RendererDebugView::MisWeight: return "mis-weight";
    case RendererDebugView::DirectSampleType: return "direct-sample-type";
    case RendererDebugView::Albedo: return "albedo";
    case RendererDebugView::ClayMaterial: return "clay-material";
    case RendererDebugView::FirstBounceThroughput: return "first-bounce-throughput";
    case RendererDebugView::SecondaryEnvironmentMiss: return "secondary-environment-miss";
    case RendererDebugView::BounceCount: return "bounce-count";
    case RendererDebugView::SecondaryEnvironmentRadiance: return "secondary-environment-radiance";
    case RendererDebugView::WhiteEnvironmentTransport: return "white-environment-transport";
    }
    return "beauty";
}

} // namespace rtv
