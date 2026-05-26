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

const char* toneMapperName(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::Linear: return "linear";
    case ToneMapper::Reinhard: return "reinhard";
    case ToneMapper::ReinhardWhite: return "reinhard-white";
    case ToneMapper::ACES: return "aces";
    case ToneMapper::PBRNeutral: return "pbr-neutral";
    case ToneMapper::AgX: return "agx";
    }
    return "aces";
}

const char* restirModeName(RestirMode mode) {
    switch (mode) {
    case RestirMode::ClassicNee: return "classic-nee";
    case RestirMode::RestirOnly: return "restir-only";
    case RestirMode::HybridCompare: return "hybrid-compare";
    }
    return "classic-nee";
}

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
    if (key == "emissivecontinuation" || key == "emissivecontinue" || key == "continuedemissive") {
        return RendererDebugView::EmissiveContinuation;
    }
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
    if (key == "sunmis" || key == "sunmisweight") { return RendererDebugView::SunMisWeight; }
    if (key == "sunpdf" || key == "sunlightpdf") { return RendererDebugView::SunLightPdf; }
    if (key == "sunbsdfpdf" || key == "sunpreviousbsdfpdf" || key == "sunprevbsdfpdf") {
        return RendererDebugView::SunPreviousBsdfPdf;
    }
    if (key == "risrawpdf" || key == "risrawlightpdf") { return RendererDebugView::RisRawLightPdf; }
    if (key == "riseffectivepdf" || key == "riseffectivelightpdf") { return RendererDebugView::RisEffectiveLightPdf; }
    if (key == "rispdfratio" || key == "risratio") { return RendererDebugView::RisPdfRatio; }
    if (key == "sampledimension" || key == "sampledimensions" || key == "samplingdimension") { return RendererDebugView::SampleDimension; }
    if (key == "samplescramble" || key == "samplescrambling" || key == "scramble") { return RendererDebugView::SampleScramble; }
    if (key == "pathdirectdiffuse" || key == "directdiffuse") { return RendererDebugView::PathDirectDiffuse; }
    if (key == "pathdirectspecular" || key == "directspecular") { return RendererDebugView::PathDirectSpecular; }
    if (key == "pathindirectdiffuse" || key == "indirectdiffuse") { return RendererDebugView::PathIndirectDiffuse; }
    if (key == "pathindirectspecular" || key == "indirectspecular") { return RendererDebugView::PathIndirectSpecular; }
    if (key == "pathdataalbedo" || key == "pathalbedo") { return RendererDebugView::PathDataAlbedo; }
    if (key == "pathdatametrics" || key == "pathmetrics" || key == "hitconfidence" || key == "hitdistance") { return RendererDebugView::PathDataMetrics; }
    if (key == "denoiserkernelradius" || key == "kernelradius" || key == "filterradius") { return RendererDebugView::DenoiserKernelRadius; }
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
    if (key == "motion" || key == "motionvectors" || key == "velocity" || key == "velocitybuffer") {
        return RendererDebugView::MotionVectors;
    }
    if (key == "atmospheresky" || key == "atmosphereskyview" || key == "skyviewlut") {
        return RendererDebugView::AtmosphereSkyView;
    }
    if (key == "atmospheretransmittance" || key == "transmittancelut") {
        return RendererDebugView::AtmosphereTransmittance;
    }
    if (key == "atmosphereaerial" || key == "aerialperspective" || key == "aerialperspectivelut") {
        return RendererDebugView::AtmosphereAerialPerspective;
    }
    if (key == "atmospheremultiscatter" || key == "multiscatter" || key == "multiscatterlut") {
        return RendererDebugView::AtmosphereMultiScatter;
    }
    if (key == "reactive" || key == "reactivemask" || key == "temporalreactive" || key == "temporalreactivemask") {
        return RendererDebugView::TemporalReactiveMask;
    }
    if (key == "historyweight" || key == "temporalhistory" || key == "temporalhistoryweight") {
        return RendererDebugView::TemporalHistoryWeight;
    }
    if (key == "restirage" || key == "restirreservoirage" || key == "reservoirage") {
        return RendererDebugView::RestirReservoirAge;
    }
    if (key == "restirconfidence" || key == "restirreservoirconfidence" || key == "reservoirconfidence") {
        return RendererDebugView::RestirReservoirConfidence;
    }
    if (key == "restirm" || key == "restirreservoirm" || key == "reservoirm" || key == "restirsamplecount") {
        return RendererDebugView::RestirReservoirM;
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
    case RendererDebugView::MotionVectors: return "motion-vectors";
    case RendererDebugView::AtmosphereSkyView: return "atmosphere-sky-view";
    case RendererDebugView::AtmosphereTransmittance: return "atmosphere-transmittance";
    case RendererDebugView::AtmosphereAerialPerspective: return "atmosphere-aerial-perspective";
    case RendererDebugView::AtmosphereMultiScatter: return "atmosphere-multi-scatter";
    case RendererDebugView::TemporalReactiveMask: return "temporal-reactive-mask";
    case RendererDebugView::TemporalHistoryWeight: return "temporal-history-weight";
    case RendererDebugView::RestirReservoirAge: return "restir-reservoir-age";
    case RendererDebugView::RestirReservoirConfidence: return "restir-reservoir-confidence";
    case RendererDebugView::RestirReservoirM: return "restir-reservoir-m";
    case RendererDebugView::EmissiveContinuation: return "emissive-continuation";
    case RendererDebugView::SunMisWeight: return "sun-mis-weight";
    case RendererDebugView::SunLightPdf: return "sun-light-pdf";
    case RendererDebugView::SunPreviousBsdfPdf: return "sun-previous-bsdf-pdf";
    case RendererDebugView::RisRawLightPdf: return "ris-raw-light-pdf";
    case RendererDebugView::RisEffectiveLightPdf: return "ris-effective-light-pdf";
    case RendererDebugView::RisPdfRatio: return "ris-pdf-ratio";
    case RendererDebugView::SampleDimension: return "sample-dimension";
    case RendererDebugView::SampleScramble: return "sample-scramble";
    case RendererDebugView::PathDirectDiffuse: return "path-direct-diffuse";
    case RendererDebugView::PathDirectSpecular: return "path-direct-specular";
    case RendererDebugView::PathIndirectDiffuse: return "path-indirect-diffuse";
    case RendererDebugView::PathIndirectSpecular: return "path-indirect-specular";
    case RendererDebugView::PathDataAlbedo: return "path-data-albedo";
    case RendererDebugView::PathDataMetrics: return "path-data-metrics";
    case RendererDebugView::DenoiserKernelRadius: return "denoiser-kernel-radius";
    }
    return "beauty";
}

} // namespace rtv
