#include "rtv/PathTracerRenderer.h"

#include "rtv/AtmosphereLutSystem.h"
#include "rtv/AtmosphereSamplingSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/ComputePipeline.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/DescriptorWriter.h"
#include "rtv/GraphicsPipeline.h"
#include "rtv/ImageBarrier.h"
#include "rtv/PipelineCache.h"
#include "rtv/RayTracingPipeline.h"
#include "rtv/RayTracingScene.h"
#include "rtv/RenderGraph.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderCompiler.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"
#include "rtv/TemporalSystem.h"
#include "rtv/VulkanContext.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace rtv {

namespace {

constexpr uint32_t kHistogramBinCount = 128;
constexpr VkDeviceSize kFrameCameraUniformOffset = 0;
constexpr VkDeviceSize kFramePrevCameraUniformOffset = 4096;
constexpr VkDeviceSize kFrameDenoiserParamsOffset = 8192;
constexpr VkDeviceSize kFrameDebugParamsOffset = 12288;
constexpr VkDeviceSize kFrameTaaParamsOffset = 16384;
constexpr VkDeviceSize kFrameRestirSpatialParamsOffset = 20480;
constexpr VkDeviceSize kFrameFogParamsOffset = 24576;
constexpr uint32_t kRendererFramesInFlight = 3;

float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

std::filesystem::path glslangPath() {
#if defined(_MSC_VER)
    char* sdk = nullptr;
    size_t sdkLength = 0;
    _dupenv_s(&sdk, &sdkLength, "VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    std::filesystem::path result = std::filesystem::path(sdk) / "Bin" / "glslangValidator.exe";
    std::free(sdk);
    return result;
#else
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    return std::filesystem::path(sdk) / "Bin" / "glslangValidator";
#endif
}

VkDescriptorSetLayoutBinding descriptorBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages, uint32_t count = 1) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = count;
    result.stageFlags = stages;
    return result;
}

std::vector<VkDescriptorSetLayoutBinding> rayTracingBindings() {
    constexpr VkShaderStageFlags allRt =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    return {
        descriptorBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(13, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(33, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(35, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(36, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(37, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(41, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, allRt, 1024),
        descriptorBinding(42, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
    };
}

} // namespace

const char* accumulationResetReasonName(AccumulationResetReason reason) {
    switch (reason) {
    case AccumulationResetReason::Startup: return "Startup";
    case AccumulationResetReason::Resize: return "Resize";
    case AccumulationResetReason::CameraMoved: return "CameraMoved";
    case AccumulationResetReason::Manual: return "Manual";
    case AccumulationResetReason::RenderSettingsChanged: return "RenderSettingsChanged";
    case AccumulationResetReason::LightingChanged: return "LightingChanged";
    case AccumulationResetReason::EnvironmentChanged: return "EnvironmentChanged";
    case AccumulationResetReason::DenoiserSettingsChanged: return "DenoiserChanged";
    case AccumulationResetReason::DebugViewChanged: return "DebugViewChanged";
    case AccumulationResetReason::SceneChanged: return "SceneChanged";
    case AccumulationResetReason::MaterialChanged: return "MaterialChanged";
    case AccumulationResetReason::ShaderReloaded: return "ShaderReloaded";
    }
    return "unknown";
}

PathTracerRenderer::PathTracerRenderer(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    VkFormat swapchainFormat,
    const std::filesystem::path& shaderDirectory,
    const std::filesystem::path& shaderOutputDirectory,
    RendererDebugView debugView,
    const SceneAsset* importedScene,
    const AssetManager* assets,
    std::optional<std::filesystem::path> environmentPath,
    std::optional<std::filesystem::path> sceneCachePath)
    : context_(context),
      allocator_(allocator),
      uploader_(uploader),
      scene_(allocator, uploader, importedScene, assets, std::move(environmentPath), std::move(sceneCachePath)) {
    temporalSystem_ = std::make_unique<TemporalSystem>();
    if (!context_.supportsHardwareRayTracing()) {
        throw std::runtime_error("Hardware ray tracing is required but this Vulkan device does not support the required KHR ray tracing features/extensions");
    }
    std::cout << "Renderer backend: hardware ray tracing\n";

    shaderCompiler_ = std::make_unique<ShaderCompiler>(glslangPath());
    shaderOutputDirectory_ = shaderOutputDirectory;
    ShaderCompiler& compiler = *shaderCompiler_;
    const auto denoiserSpv = compiler.compileIfNeeded(shaderDirectory / "denoiser.comp", shaderOutputDirectory);
    const auto taaSpv = compiler.compileIfNeeded(shaderDirectory / "taa.comp", shaderOutputDirectory);
    const auto restirSpatialSpv = compiler.compileIfNeeded(shaderDirectory / "restir_spatial.comp", shaderOutputDirectory);
    const auto fogSpv = compiler.compileIfNeeded(shaderDirectory / "fog_integrate.comp", shaderOutputDirectory);
    const auto transmittanceSpv = compiler.compileIfNeeded(shaderDirectory / "transmittance_lut.comp", shaderOutputDirectory);
    const auto multiScatterSpv = compiler.compileIfNeeded(shaderDirectory / "multi_scatter_lut.comp", shaderOutputDirectory);
    const auto skyViewSpv = compiler.compileIfNeeded(shaderDirectory / "sky_view_lut.comp", shaderOutputDirectory);
    const auto skyReprojectSpv = compiler.compileIfNeeded(shaderDirectory / "sky_reproject.comp", shaderOutputDirectory);
    const auto aerialPerspectiveSpv = compiler.compileIfNeeded(shaderDirectory / "aerial_perspective_lut.comp", shaderOutputDirectory);
    const auto skyCdfSpv = compiler.compileIfNeeded(shaderDirectory / "sky_cdf.comp", shaderOutputDirectory);
    const auto selectionSpv = compiler.compileIfNeeded(shaderDirectory / "selection_outline.comp", shaderOutputDirectory);
    const auto histogramSpv = compiler.compileIfNeeded(shaderDirectory / "luminance_histogram.comp", shaderOutputDirectory);
    const auto exposureSpv = compiler.compileIfNeeded(shaderDirectory / "exposure_reduce.comp", shaderOutputDirectory);
    const auto toneMapSpv = compiler.compileIfNeeded(shaderDirectory / "tone_map.comp", shaderOutputDirectory);
    const auto vertSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.vert", shaderOutputDirectory);
    const auto fragSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.frag", shaderOutputDirectory);

    denoiserShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(denoiserSpv), "temporal denoiser compute");
    taaShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(taaSpv), "taa compute");
    restirSpatialShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(restirSpatialSpv), "restir spatial compute");
    fogShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fogSpv), "height fog integrate compute");
    transmittanceShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(transmittanceSpv), "atmosphere transmittance lut compute");
    multiScatterShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(multiScatterSpv), "atmosphere multi scatter lut compute");
    skyViewShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyViewSpv), "atmosphere sky view lut compute");
    skyReprojectShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyReprojectSpv), "atmosphere sky reproject compute");
    aerialPerspectiveShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(aerialPerspectiveSpv), "atmosphere aerial perspective lut compute");
    skyCdfShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyCdfSpv), "atmosphere sky CDF compute");
    selectionOutlineShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(selectionSpv), "selection outline compute");
    luminanceHistogramShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(histogramSpv), "luminance histogram compute");
    exposureReduceShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(exposureSpv), "exposure reduce compute");
    toneMapShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(toneMapSpv), "tone map compute");
    fullscreenVertexShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(vertSpv), "fullscreen vertex");
    fullscreenFragmentShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fragSpv), "fullscreen fragment");
    const auto raygenSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rgen", shaderOutputDirectory);
    const auto missSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rmiss", shaderOutputDirectory);
    const auto shadowMissSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace_shadow.rmiss", shaderOutputDirectory);
    const auto closestHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rchit", shaderOutputDirectory);
    const auto primaryAnyHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rahit", shaderOutputDirectory);
    const auto shadowAnyHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace_shadow.rahit", shaderOutputDirectory);
    raygenShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(raygenSpv), "path trace raygen");
    primaryMissShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(missSpv), "path trace primary miss");
    shadowMissShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(shadowMissSpv), "path trace shadow miss");
    closestHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(closestHitSpv), "path trace closest hit");
    primaryAnyHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(primaryAnyHitSpv), "path trace primary any-hit");
    shadowAnyHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(shadowAnyHitSpv), "path trace shadow any-hit");

    shaderSources_ = {
        shaderDirectory / "denoiser.comp",
        shaderDirectory / "taa.comp",
        shaderDirectory / "restir_spatial.comp",
        shaderDirectory / "fog_integrate.comp",
        shaderDirectory / "transmittance_lut.comp",
        shaderDirectory / "multi_scatter_lut.comp",
        shaderDirectory / "sky_view_lut.comp",
        shaderDirectory / "sky_reproject.comp",
        shaderDirectory / "aerial_perspective_lut.comp",
        shaderDirectory / "sky_cdf.comp",
        shaderDirectory / "selection_outline.comp",
        shaderDirectory / "luminance_histogram.comp",
        shaderDirectory / "exposure_reduce.comp",
        shaderDirectory / "tone_map.comp",
        shaderDirectory / "fullscreen.vert",
        shaderDirectory / "fullscreen.frag",
        shaderDirectory / "pathtrace.rgen",
        shaderDirectory / "pathtrace.rmiss",
        shaderDirectory / "pathtrace_shadow.rmiss",
        shaderDirectory / "pathtrace.rchit",
        shaderDirectory / "pathtrace.rahit",
        shaderDirectory / "pathtrace_shadow.rahit",
    };

    layoutCache_ = std::make_unique<DescriptorLayoutCache>(context_.device());
    pipelineCache_ = std::make_unique<PipelineCache>(context_.device(), shaderOutputDirectory / "pipeline_cache.bin");
    atmosphereLutSystem_ = std::make_unique<AtmosphereLutSystem>(
        context_.device(),
        allocator_,
        *layoutCache_,
        *pipelineCache_,
        *transmittanceShader_,
        *multiScatterShader_,
        *skyViewShader_,
        *skyReprojectShader_,
        *aerialPerspectiveShader_,
        *skyCdfShader_);
    auto atmosphereBindings = ShaderReflection::bindingsForSet({raygenShader_->reflection()}, 1);
    for (VkDescriptorSetLayoutBinding& binding : atmosphereBindings) {
        binding.stageFlags = VK_SHADER_STAGE_ALL;
    }
    atmosphereSetLayout_ = layoutCache_->createLayout(std::move(atmosphereBindings));
    denoiserSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({denoiserShader_->reflection()}, 0));
    taaSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({taaShader_->reflection()}, 0));
    restirSpatialSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({restirSpatialShader_->reflection()}, 0));
    fogSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({fogShader_->reflection()}, 0));
    selectionOutlineSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({selectionOutlineShader_->reflection()}, 0));
    luminanceHistogramSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({luminanceHistogramShader_->reflection()}, 0));
    exposureReduceSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({exposureReduceShader_->reflection()}, 0));
    toneMapSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({toneMapShader_->reflection()}, 0));
    graphicsSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({
        fullscreenVertexShader_->reflection(),
        fullscreenFragmentShader_->reflection(),
    }, 0));
    std::vector<VkDescriptorBindingFlags> rtBindingFlags(rayTracingBindings().size(), 0);
    const auto rtBindings = rayTracingBindings();
    const BindlessCapabilities& rtBindless = context_.bindlessCapabilities();
    for (size_t i = 0; i < rtBindings.size(); ++i) {
        if (rtBindings[i].binding == 41) {
            if (rtBindless.partiallyBound) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            if (rtBindless.updateAfterBind) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            }
        }
    }
    const VkDescriptorSetLayoutCreateFlags rtLayoutFlags =
        rtBindless.updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    rayTracingSetLayout_ = layoutCache_->createLayout(rayTracingBindings(), rtLayoutFlags, std::move(rtBindingFlags));

    denoiserPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *denoiserShader_,
        std::vector<VkDescriptorSetLayout>{denoiserSetLayout_},
        ShaderReflection::mergePushConstants({denoiserShader_->reflection()}),
        *pipelineCache_);
    taaPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *taaShader_,
        std::vector<VkDescriptorSetLayout>{taaSetLayout_},
        ShaderReflection::mergePushConstants({taaShader_->reflection()}),
        *pipelineCache_);
    restirSpatialPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *restirSpatialShader_,
        std::vector<VkDescriptorSetLayout>{restirSpatialSetLayout_},
        ShaderReflection::mergePushConstants({restirSpatialShader_->reflection()}),
        *pipelineCache_);
    fogPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *fogShader_,
        std::vector<VkDescriptorSetLayout>{fogSetLayout_, atmosphereSetLayout_},
        ShaderReflection::mergePushConstants({fogShader_->reflection()}),
        *pipelineCache_);
    selectionOutlinePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *selectionOutlineShader_,
        std::vector<VkDescriptorSetLayout>{selectionOutlineSetLayout_},
        ShaderReflection::mergePushConstants({selectionOutlineShader_->reflection()}),
        *pipelineCache_);
    luminanceHistogramPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *luminanceHistogramShader_,
        std::vector<VkDescriptorSetLayout>{luminanceHistogramSetLayout_},
        ShaderReflection::mergePushConstants({luminanceHistogramShader_->reflection()}),
        *pipelineCache_);
    exposureReducePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *exposureReduceShader_,
        std::vector<VkDescriptorSetLayout>{exposureReduceSetLayout_},
        ShaderReflection::mergePushConstants({exposureReduceShader_->reflection()}),
        *pipelineCache_);
    toneMapPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *toneMapShader_,
        std::vector<VkDescriptorSetLayout>{toneMapSetLayout_},
        ShaderReflection::mergePushConstants({toneMapShader_->reflection()}),
        *pipelineCache_);
    graphicsPipeline_ = std::make_unique<GraphicsPipeline>(
        context_.device(),
        swapchainFormat,
        *fullscreenVertexShader_,
        *fullscreenFragmentShader_,
        std::vector<VkDescriptorSetLayout>{graphicsSetLayout_},
        ShaderReflection::mergePushConstants({
            fullscreenVertexShader_->reflection(),
            fullscreenFragmentShader_->reflection(),
        }),
        *pipelineCache_);
    if (true) {
        rayTracingScene_ = std::make_unique<RayTracingScene>(context_, allocator_, uploader_, scene_);
        rayTracingPipeline_ = std::make_unique<RayTracingPipeline>(
            context_.device(),
            context_.rayTracingInfo().rayTracingPipelineProperties,
            *raygenShader_,
            *primaryMissShader_,
            *shadowMissShader_,
            *closestHitShader_,
            *primaryAnyHitShader_,
            *shadowAnyHitShader_,
            std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_, atmosphereSetLayout_},
            *pipelineCache_,
            allocator_,
            uploader_);
        std::cout << "RT pipeline: SBT=" << rayTracingPipeline_->sbtBytes() << " bytes\n";
    }

    frames_.reserve(kRendererFramesInFlight);
    profilers_.reserve(kRendererFramesInFlight);
    for (uint32_t i = 0; i < kRendererFramesInFlight; ++i) {
        frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));
        profilers_.emplace_back(context_.device(), context_.physicalDevice());
    }
    for (auto& p : profilers_) {
        p.createPipelineStatsQuery(context_.device(), context_.supportsHardwareRayTracing());
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &fullscreenSampler_), "vkCreateSampler(path tracer fullscreen)");

    camera_.pos = {0.0f, 0.0f, 3.9f, 0.0f};
    camera_.forward = {0.0f, 0.0f, -1.0f, 0.0f};
    camera_.right = {1.0f, 0.0f, 0.0f, 0.0f};
    camera_.up = {0.0f, 1.0f, 0.0f, 0.0f};
    previousCameraPos_ = camera_.pos;
    settings_.materialTextureAnisotropy = scene_.materialTextureAnisotropy();
    settings_.debugView = debugView;
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.scale = settings_.debugScale;
    if (debugView != RendererDebugView::Beauty) {
        std::cout << "Renderer debug view: " << rendererDebugViewName(debugView) << '\n';
    }
}

PathTracerRenderer::~PathTracerRenderer() {
    if (pipelineCache_ && !shaderOutputDirectory_.empty()) {
        pipelineCache_->saveToFile(shaderOutputDirectory_ / "pipeline_cache.bin");
    }
    if (fullscreenSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), fullscreenSampler_, nullptr);
    }
}

bool PathTracerRenderer::shadersNeedReload() {
    if (shaderCompiler_ == nullptr) {
        return false;
    }
    for (const auto& source : shaderSources_) {
        if (shaderCompiler_->needsCompile(source, shaderOutputDirectory_ / source.filename().replace_extension(".spv"))) {
            return true;
        }
    }
    return false;
}

void PathTracerRenderer::beginFrame(uint32_t frameIndex, VkExtent2D renderExtent, VkExtent2D displayExtent) {
    currentFrame_ = frames_.at(frameIndex % frames_.size()).get();
    currentProfiler_ = &profilers_.at(frameIndex % profilers_.size());
    currentProfiler_->collectCompletedFrame();
    scene_.releaseRetiredMaterialSamplers(temporalFrameIndex_);
    validationLog_.beginFrame(temporalFrameIndex_);
    updateAdaptiveQuality(currentProfiler_->timings());
    if (temporalFrameIndex_ > 0 && temporalFrameIndex_ % 120u == 0u) {
        const GpuFrameTimings& timings = currentProfiler_->timings();
        std::cout << "GPU timings: path=" << timings.pathTraceMs
                  << " ms, denoise=" << timings.denoiserMs
                  << " ms, fullscreen=" << timings.fullscreenMs << " ms\n";
    }
    currentFrame_->beginFrame();
    if (renderExtent.width != renderExtent_.width ||
        renderExtent.height != renderExtent_.height ||
        displayExtent.width != displayExtent_.width ||
        displayExtent.height != displayExtent_.height ||
        rawImage_.handle() == VK_NULL_HANDLE) {
        if (rawImage_.handle() != VK_NULL_HANDLE) {
            checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(resize path tracer)");
        }
        createResolutionResources(renderExtent, displayExtent);
        resetAccumulation(AccumulationResetReason::Resize);
    }
    ++frameCount_;
    ++temporalFrameIndex_;
    if (temporalSystem_) {
        temporalSystem_->beginFrame(temporalFrameIndex_);
    }
    updateCamera();
}

void PathTracerRenderer::updateAdaptiveQuality(const GpuFrameTimings& timings) {
    adaptiveEffectiveMaxBounces_ = settings_.maxBounces;
    adaptiveEffectiveEnvironmentSamples_ = settings_.environmentDirectSamples;
    adaptiveEffectiveAtrousIterations_ = settings_.atrousIterations;
    adaptiveSkipRestirSpatial_ = false;
    adaptiveSkipDenoiser_ = false;

    if (settings_.adaptiveQualityMode == AdaptiveQualityMode::Off || !settings_.pathTracingEnabled) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
        adaptiveSmoothedGpuMs_ = 0.0f;
        return;
    }

    const float gpuMs =
        timings.pathTraceMs +
        timings.restirSpatialMs +
        timings.fogIntegrateMs +
        timings.atmosphereMs +
        timings.denoiserMs +
        timings.historyCopyMs +
        timings.taaMs +
        timings.autoExposureMs +
        timings.toneMapMs +
        timings.selectionOutlineMs +
        timings.fullscreenMs;
    if (gpuMs > 0.0f) {
        adaptiveSmoothedGpuMs_ = adaptiveSmoothedGpuMs_ <= 0.0f
            ? gpuMs
            : adaptiveSmoothedGpuMs_ * 0.85f + gpuMs * 0.15f;
    }

    const uint32_t mode = static_cast<uint32_t>(settings_.adaptiveQualityMode);
    const uint32_t maxTier = std::clamp(mode, 1u, 3u);
    const bool moving = cameraChangedThisFrame_;
    const float targetMs = std::clamp(settings_.adaptiveGpuFrameTargetMs, 4.0f, 100.0f);
    const bool overBudget = adaptiveSmoothedGpuMs_ > targetMs * 1.15f;
    const bool underBudget = adaptiveSmoothedGpuMs_ <= 0.0f || adaptiveSmoothedGpuMs_ < targetMs * 0.88f;
    const uint32_t stableFramesForFullQuality = mode == 1u ? 6u : (mode == 2u ? 10u : 14u);

    if (moving) {
        adaptiveQualityTier_ = std::max(adaptiveQualityTier_, std::min(maxTier, mode));
        adaptiveOverBudgetFrames_ = 0;
    } else if (overBudget) {
        ++adaptiveOverBudgetFrames_;
        if (adaptiveOverBudgetFrames_ >= 6u) {
            adaptiveQualityTier_ = std::min(maxTier, adaptiveQualityTier_ + 1u);
            adaptiveOverBudgetFrames_ = 0;
        }
    } else if (stillFrameCount_ >= stableFramesForFullQuality && underBudget) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
    }

    if (adaptiveQualityTier_ == 0) {
        return;
    }

    if (adaptiveQualityTier_ == 1u) {
        adaptiveEffectiveMaxBounces_ = std::max(2u, settings_.maxBounces > 1u ? settings_.maxBounces - 1u : settings_.maxBounces);
        adaptiveEffectiveEnvironmentSamples_ = std::max(1u, settings_.environmentDirectSamples);
        adaptiveEffectiveAtrousIterations_ = std::max(1u, settings_.atrousIterations > 1u ? settings_.atrousIterations - 1u : settings_.atrousIterations);
    } else if (adaptiveQualityTier_ == 2u) {
        adaptiveEffectiveMaxBounces_ = std::max(2u, std::min(settings_.maxBounces, settings_.maxBounces / 2u + 1u));
        adaptiveEffectiveEnvironmentSamples_ = 1u;
        adaptiveEffectiveAtrousIterations_ = std::max(1u, settings_.atrousIterations > 2u ? settings_.atrousIterations - 2u : 1u);
        adaptiveSkipRestirSpatial_ = moving;
    } else {
        adaptiveEffectiveMaxBounces_ = std::min(settings_.maxBounces, 2u);
        adaptiveEffectiveEnvironmentSamples_ = 1u;
        adaptiveEffectiveAtrousIterations_ = 1u;
        adaptiveSkipRestirSpatial_ = moving;
        adaptiveSkipDenoiser_ = moving && debugParams_.view == static_cast<uint32_t>(RendererDebugView::Beauty);
    }

    if (temporalFrameIndex_ == 1u || temporalFrameIndex_ % 120u == 0u) {
        validationLog_.recordPass(
            "adaptive quality tier=" + std::to_string(adaptiveQualityTier_) +
            " gpuMs=" + std::to_string(adaptiveSmoothedGpuMs_) +
            " bounces=" + std::to_string(adaptiveEffectiveMaxBounces_) +
            " envSamples=" + std::to_string(adaptiveEffectiveEnvironmentSamples_) +
            " atrous=" + std::to_string(adaptiveEffectiveAtrousIterations_) +
            " skipRestir=" + std::to_string(adaptiveSkipRestirSpatial_ ? 1u : 0u) +
            " skipDenoiser=" + std::to_string(adaptiveSkipDenoiser_ ? 1u : 0u));
    }
}

bool PathTracerRenderer::applySettings(const RendererSettings& settings) {
    RendererSettings next = settings;
    next.maxBounces = std::clamp(next.maxBounces, 1u, 16u);
    next.atrousIterations = std::clamp(next.atrousIterations, 1u, 5u);
    next.environmentDirectSamples = std::clamp(next.environmentDirectSamples, 1u, 8u);
    next.denoiserStrength = std::max(0.05f, next.denoiserStrength);
    next.taaSharpeningStrength = std::clamp(next.taaSharpeningStrength, 0.0f, 1.0f);
    next.sunIntensity = std::max(0.0f, next.sunIntensity);
    next.sunIlluminanceLux = std::max(0.0f, next.sunIlluminanceLux);
    next.sunColorTemperatureKelvin = std::clamp(next.sunColorTemperatureKelvin, 1000.0f, 40000.0f);
    next.sunColor = glm::max(next.sunColor, glm::vec3(0.0f));
    if (glm::dot(next.sunDirection, next.sunDirection) > 1.0e-6f) {
        next.sunDirection = glm::normalize(next.sunDirection);
    } else {
        next.sunDirection = glm::vec3(0.0f, 0.8240f, 0.5661f);
    }
    next.skyIntensity = std::max(0.0f, next.skyIntensity);
    next.sunElevation = std::clamp(next.sunElevation, -0.20f, 1.45f);
    constexpr float twoPi = 6.28318530717958647692f;
    if (std::isfinite(next.sunAzimuth)) {
        next.sunAzimuth = std::remainder(next.sunAzimuth, twoPi);
    } else {
        next.sunAzimuth = 0.0f;
    }
    next.exposure = std::max(0.05f, next.exposure);
    if (static_cast<uint32_t>(next.toneMapper) > static_cast<uint32_t>(ToneMapper::AgX)) {
        next.toneMapper = ToneMapper::ACES;
    }
    next.gamma = std::max(0.1f, next.gamma);
    next.contrast = std::max(0.0f, next.contrast);
    next.saturation = std::max(0.0f, next.saturation);
    next.whitePoint = std::max(0.001f, next.whitePoint);
    next.targetLuminance = std::max(0.001f, next.targetLuminance);
    next.minExposure = std::max(0.001f, next.minExposure);
    next.maxExposure = std::max(next.minExposure, next.maxExposure);
    next.adaptationSpeed = std::max(0.0f, next.adaptationSpeed);
    next.histogramMaxLogLuminance = std::max(next.histogramMinLogLuminance + 0.001f, next.histogramMaxLogLuminance);
    next.histogramLowPercentile = std::clamp(next.histogramLowPercentile, 0.0f, 1.0f);
    next.histogramHighPercentile = std::clamp(next.histogramHighPercentile, 0.0f, 1.0f);
    next.histogramTargetPercentile = std::clamp(next.histogramTargetPercentile, 0.0f, 1.0f);
    next.sunAngularRadius = std::isfinite(next.sunAngularRadius)
        ? std::clamp(next.sunAngularRadius, 0.0f, 0.08f)
        : 0.00465f;
    next.rayleighScaleHeight = std::clamp(next.rayleighScaleHeight, 1000.0f, 20000.0f);
    next.mieScaleHeight = std::clamp(next.mieScaleHeight, 200.0f, 5000.0f);
    next.mieAnisotropy = std::clamp(next.mieAnisotropy, 0.0f, 0.99f);
    next.groundAlbedo = std::clamp(next.groundAlbedo, 0.0f, 1.0f);
    next.physicalAperture = std::max(0.1f, next.physicalAperture);
    next.physicalShutterSeconds = std::max(1.0e-6f, next.physicalShutterSeconds);
    next.physicalIso = std::max(1.0f, next.physicalIso);
    next.physicalExposureCompensation = std::clamp(next.physicalExposureCompensation, -10.0f, 10.0f);
    next.indirectStrength = std::max(0.0f, next.indirectStrength);
    next.environmentIntensity = std::max(0.0f, next.environmentIntensity);
    next.environmentBackgroundIntensity = std::max(0.0f, next.environmentBackgroundIntensity);
    next.renderResolutionScale = std::clamp(next.renderResolutionScale, 0.25f, 1.0f);
    const float maxMaterialAnisotropy = allocator_.supportsSamplerAnisotropy() ? allocator_.maxSamplerAnisotropy() : 1.0f;
    next.materialTextureAnisotropy = std::clamp(
        std::isfinite(next.materialTextureAnisotropy) ? next.materialTextureAnisotropy : 1.0f,
        1.0f,
        maxMaterialAnisotropy);
    next.debugScale = std::max(0.05f, next.debugScale);
    next.shadowRayBias = std::clamp(next.shadowRayBias, 0.00001f, 0.05f);
    next.shadowDistanceBias = std::clamp(next.shadowDistanceBias, 0.0f, 0.1f);
    next.fireflyClamp = std::clamp(next.fireflyClamp, 1.0f, 512.0f);
    next.maxFrameDeltaSeconds = std::clamp(
        std::isfinite(next.maxFrameDeltaSeconds) ? next.maxFrameDeltaSeconds : (1.0f / 30.0f),
        1.0f / 240.0f,
        1.0f / 5.0f);
    next.russianRouletteMinSurvival = std::clamp(
        std::isfinite(next.russianRouletteMinSurvival) ? next.russianRouletteMinSurvival : 0.10f,
        0.02f,
        0.50f);
    if (static_cast<uint32_t>(next.adaptiveQualityMode) > static_cast<uint32_t>(AdaptiveQualityMode::Aggressive)) {
        next.adaptiveQualityMode = AdaptiveQualityMode::Off;
    }
    next.adaptiveGpuFrameTargetMs = std::clamp(
        std::isfinite(next.adaptiveGpuFrameTargetMs) ? next.adaptiveGpuFrameTargetMs : 16.6f,
        4.0f,
        100.0f);
    if (static_cast<uint32_t>(next.restirMode) > static_cast<uint32_t>(RestirMode::HybridCompare)) {
        next.restirMode = RestirMode::ClassicNee;
    }

    const bool changed =
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.cameraJitterEnabled != settings_.cameraJitterEnabled ||
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.taaEnabled != settings_.taaEnabled ||
        next.sunlightEnabled != settings_.sunlightEnabled ||
        next.directLightingEnabled != settings_.directLightingEnabled ||
        next.environmentEnabled != settings_.environmentEnabled ||
        next.maxBounces != settings_.maxBounces ||
        next.atrousIterations != settings_.atrousIterations ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        next.restirMode != settings_.restirMode ||
        next.debugView != settings_.debugView ||
        next.toneMapper != settings_.toneMapper ||
        next.autoExposureEnabled != settings_.autoExposureEnabled ||
        next.usePhysicalCamera != settings_.usePhysicalCamera ||
        std::abs(next.taaFeedback - settings_.taaFeedback) > 0.0001f ||
        std::abs(next.taaSharpeningStrength - settings_.taaSharpeningStrength) > 0.0001f ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f ||
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.sunIlluminanceLux - settings_.sunIlluminanceLux) > 0.5f ||
        glm::length(next.sunColor - settings_.sunColor) > 0.0001f ||
        glm::length(next.sunDirection - settings_.sunDirection) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.sunElevation - settings_.sunElevation) > 0.0001f ||
        std::abs(next.sunAzimuth - settings_.sunAzimuth) > 0.0001f ||
        std::abs(next.exposure - settings_.exposure) > 0.0001f ||
        std::abs(next.gamma - settings_.gamma) > 0.0001f ||
        std::abs(next.contrast - settings_.contrast) > 0.0001f ||
        std::abs(next.saturation - settings_.saturation) > 0.0001f ||
        std::abs(next.brightness - settings_.brightness) > 0.0001f ||
        std::abs(next.whitePoint - settings_.whitePoint) > 0.0001f ||
        std::abs(next.targetLuminance - settings_.targetLuminance) > 0.0001f ||
        std::abs(next.minExposure - settings_.minExposure) > 0.0001f ||
        std::abs(next.maxExposure - settings_.maxExposure) > 0.0001f ||
        std::abs(next.adaptationSpeed - settings_.adaptationSpeed) > 0.0001f ||
        std::abs(next.histogramMinLogLuminance - settings_.histogramMinLogLuminance) > 0.0001f ||
        std::abs(next.histogramMaxLogLuminance - settings_.histogramMaxLogLuminance) > 0.0001f ||
        std::abs(next.histogramLowPercentile - settings_.histogramLowPercentile) > 0.0001f ||
        std::abs(next.histogramHighPercentile - settings_.histogramHighPercentile) > 0.0001f ||
        std::abs(next.histogramTargetPercentile - settings_.histogramTargetPercentile) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f ||
        std::abs(next.rayleighScaleHeight - settings_.rayleighScaleHeight) > 0.5f ||
        std::abs(next.mieScaleHeight - settings_.mieScaleHeight) > 0.5f ||
        std::abs(next.mieAnisotropy - settings_.mieAnisotropy) > 0.0001f ||
        std::abs(next.groundAlbedo - settings_.groundAlbedo) > 0.0001f ||
        std::abs(next.physicalAperture - settings_.physicalAperture) > 0.0001f ||
        std::abs(next.physicalShutterSeconds - settings_.physicalShutterSeconds) > 0.000001f ||
        std::abs(next.physicalIso - settings_.physicalIso) > 0.0001f ||
        std::abs(next.physicalExposureCompensation - settings_.physicalExposureCompensation) > 0.0001f ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f ||
        std::abs(next.environmentIntensity - settings_.environmentIntensity) > 0.0001f ||
        std::abs(next.environmentRotation - settings_.environmentRotation) > 0.0001f ||
        std::abs(next.environmentBackgroundIntensity - settings_.environmentBackgroundIntensity) > 0.0001f ||
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f ||
        std::abs(next.materialTextureAnisotropy - settings_.materialTextureAnisotropy) > 0.0001f ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f ||
        std::abs(next.shadowRayBias - settings_.shadowRayBias) > 0.000001f ||
        std::abs(next.shadowDistanceBias - settings_.shadowDistanceBias) > 0.000001f ||
        std::abs(next.fireflyClamp - settings_.fireflyClamp) > 0.0001f ||
        std::abs(next.maxFrameDeltaSeconds - settings_.maxFrameDeltaSeconds) > 0.000001f ||
        std::abs(next.russianRouletteMinSurvival - settings_.russianRouletteMinSurvival) > 0.0001f ||
        next.adaptiveQualityMode != settings_.adaptiveQualityMode ||
        std::abs(next.adaptiveGpuFrameTargetMs - settings_.adaptiveGpuFrameTargetMs) > 0.0001f;
    if (!changed) {
        return false;
    }

    const bool environmentChanged =
        next.environmentEnabled != settings_.environmentEnabled ||
        std::abs(next.environmentIntensity - settings_.environmentIntensity) > 0.0001f ||
        std::abs(next.environmentRotation - settings_.environmentRotation) > 0.0001f ||
        std::abs(next.environmentBackgroundIntensity - settings_.environmentBackgroundIntensity) > 0.0001f;
    const bool lightingChanged =
        next.sunlightEnabled != settings_.sunlightEnabled ||
        next.directLightingEnabled != settings_.directLightingEnabled ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        next.restirMode != settings_.restirMode ||
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.sunIlluminanceLux - settings_.sunIlluminanceLux) > 0.5f ||
        glm::length(next.sunColor - settings_.sunColor) > 0.0001f ||
        glm::length(next.sunDirection - settings_.sunDirection) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.sunElevation - settings_.sunElevation) > 0.0001f ||
        std::abs(next.sunAzimuth - settings_.sunAzimuth) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f ||
        std::abs(next.rayleighScaleHeight - settings_.rayleighScaleHeight) > 0.5f ||
        std::abs(next.mieScaleHeight - settings_.mieScaleHeight) > 0.5f ||
        std::abs(next.mieAnisotropy - settings_.mieAnisotropy) > 0.0001f ||
        std::abs(next.groundAlbedo - settings_.groundAlbedo) > 0.0001f;
    const bool denoiserChanged =
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.atrousIterations != settings_.atrousIterations ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f;
    const bool taaChanged =
        next.taaEnabled != settings_.taaEnabled ||
        std::abs(next.taaFeedback - settings_.taaFeedback) > 0.0001f ||
        std::abs(next.taaSharpeningStrength - settings_.taaSharpeningStrength) > 0.0001f;
    const bool debugChanged =
        next.debugView != settings_.debugView ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f;
    const bool renderChanged =
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.cameraJitterEnabled != settings_.cameraJitterEnabled ||
        next.adaptiveQualityMode != settings_.adaptiveQualityMode ||
        next.maxBounces != settings_.maxBounces ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f ||
        std::abs(next.shadowRayBias - settings_.shadowRayBias) > 0.000001f ||
        std::abs(next.shadowDistanceBias - settings_.shadowDistanceBias) > 0.000001f ||
        std::abs(next.fireflyClamp - settings_.fireflyClamp) > 0.0001f ||
        std::abs(next.adaptiveGpuFrameTargetMs - settings_.adaptiveGpuFrameTargetMs) > 0.0001f ||
        std::abs(next.russianRouletteMinSurvival - settings_.russianRouletteMinSurvival) > 0.0001f;
    const bool renderResolutionChanged =
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f;
    const bool materialTextureFilteringChanged =
        std::abs(next.materialTextureAnisotropy - settings_.materialTextureAnisotropy) > 0.0001f;

    settings_ = next;
    if (renderChanged || denoiserChanged) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
    }
    if (materialTextureFilteringChanged) {
        const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint64_t>(frames_.size()) + 1ull;
        scene_.setMaterialTextureAnisotropy(settings_.materialTextureAnisotropy, retireFrame);
    }
    if (taaChanged || renderResolutionChanged) {
        taaHistoryValid_ = false;
    }

    physicalCamera_.setSettings({settings_.physicalAperture, settings_.physicalShutterSeconds, settings_.physicalIso, settings_.physicalExposureCompensation});
    const bool environmentUploaded = scene_.setEnvironmentControls(
        settings_.environmentEnabled,
        settings_.environmentIntensity,
        settings_.environmentRotation,
        settings_.environmentBackgroundIntensity);
    if (renderResolutionChanged) {
        resetAccumulation(AccumulationResetReason::Resize);
    } else if (materialTextureFilteringChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (environmentChanged || environmentUploaded) {
        resetAccumulation(AccumulationResetReason::EnvironmentChanged);
    } else if (lightingChanged) {
        resetAccumulation(AccumulationResetReason::LightingChanged);
    } else if (renderChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (denoiserChanged) {
        resetAccumulation(AccumulationResetReason::DenoiserSettingsChanged);
    } else if (taaChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (debugChanged) {
        resetAccumulation(AccumulationResetReason::DebugViewChanged);
    }
    return true;
}

void PathTracerRenderer::setCameraPose(glm::vec3 position, glm::vec3 forward) {
    if (glm::dot(forward, forward) <= 0.0f) {
        return;
    }
    forward = glm::normalize(forward);
    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    glm::vec3 right = glm::cross(forward, worldUp);
    if (glm::dot(right, right) <= 0.0001f) {
        right = {1.0f, 0.0f, 0.0f};
    } else {
        right = glm::normalize(right);
    }
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));

    const glm::vec3 oldPos = glm::vec3(camera_.pos);
    const glm::vec3 oldForward = glm::vec3(camera_.forward);
    if (glm::length(position - oldPos) <= 0.00001f && glm::length(forward - oldForward) <= 0.00001f) {
        return;
    }

    camera_.pos = glm::vec4(position, 0.0f);
    camera_.forward = glm::vec4(forward, 0.0f);
    camera_.right = glm::vec4(right, 0.0f);
    camera_.up = glm::vec4(up, 0.0f);
    cameraChangedThisFrame_ = true;
    resetAccumulation(AccumulationResetReason::CameraMoved);
}

void PathTracerRenderer::setCameraFovY(float fovY) {
    const float clampedFov = std::clamp(fovY, 0.1f, 3.0f);
    if (std::abs(camera_.fovY - clampedFov) <= 0.0001f) {
        return;
    }
    camera_.fovY = clampedFov;
    resetAccumulation(AccumulationResetReason::CameraMoved);
}

void PathTracerRenderer::resetAccumulation(AccumulationResetReason reason) {
    lastResetReason_ = reason;
    validationLog_.recordAccumulationInvalidation(accumulationResetReasonName(reason), temporalFrameIndex_);
    if (temporalSystem_) {
        temporalSystem_->setCameraCut(reason != AccumulationResetReason::CameraMoved, reason);
    }
    if (reason != AccumulationResetReason::CameraMoved) {
        taaHistoryValid_ = false;
    }
    frameCount_ = 0;
    if (reason != AccumulationResetReason::CameraMoved) {
        previousJitter_ = glm::vec2(0.0f);
        stillFrameCount_ = 0;
        if (reason == AccumulationResetReason::Resize) {
            temporalFrameIndex_ = 0;
        }
    }
}

void PathTracerRenderer::loadEnvironment(const std::filesystem::path& path) {
    checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(load environment)");
    scene_.loadEnvironment(uploader_, path);
    scene_.setEnvironmentControls(
        settings_.environmentEnabled,
        settings_.environmentIntensity,
        settings_.environmentRotation,
        settings_.environmentBackgroundIntensity);
    resetAccumulation(AccumulationResetReason::EnvironmentChanged);
}

bool PathTracerRenderer::updateMaterials(const SceneAsset& scene, const AssetManager& assets) {
    const bool updated = scene_.updateImportedMaterials(uploader_, scene, assets);
    if (updated) {
        resetAccumulation(AccumulationResetReason::MaterialChanged);
    }
    return updated;
}

bool PathTracerRenderer::updateSceneLights(const SceneAsset& scene) {
    const bool updated = scene_.updateSceneLights(uploader_, scene);
    if (updated) {
        resetAccumulation(AccumulationResetReason::LightingChanged);
    }
    return updated;
}

bool PathTracerRenderer::updateSceneTransforms(const SceneAsset& scene, const AssetManager& assets) {
    const bool updated = scene_.updateInstanceTransforms(uploader_, scene, assets);
    if (!updated) {
        return false;
    }
    if (rayTracingScene_ == nullptr ||
        !rayTracingScene_->refitTransforms(context_, allocator_, uploader_, scene_)) {
        rayTracingScene_ = std::make_unique<RayTracingScene>(context_, allocator_, uploader_, scene_);
    }
    resetAccumulation(AccumulationResetReason::SceneChanged);
    return true;
}

bool PathTracerRenderer::updateSceneVisibility(const SceneAsset& scene, const AssetManager& assets) {
    const bool updated = scene_.updateInstanceTransforms(uploader_, scene, assets);
    if (!updated) {
        return false;
    }
    if (rayTracingScene_ == nullptr ||
        !rayTracingScene_->refitTransforms(context_, allocator_, uploader_, scene_)) {
        return false;
    }
    resetAccumulation(AccumulationResetReason::SceneChanged);
    return true;
}

void PathTracerRenderer::setSelectedInstanceId(std::optional<uint32_t> instanceId) {
    selectedInstanceId_ = instanceId.value_or(UINT32_MAX);
}

std::optional<uint32_t> PathTracerRenderer::pickInstanceId(glm::vec2 viewportUv) {
    if (entityIdBuffer_.handle() == VK_NULL_HANDLE || entityIdBuffer_.mappedData() == nullptr || renderExtent_.width == 0 || renderExtent_.height == 0) {
        return std::nullopt;
    }

    const uint32_t x = std::min(renderExtent_.width - 1u, static_cast<uint32_t>(std::clamp(viewportUv.x, 0.0f, 1.0f) * static_cast<float>(renderExtent_.width)));
    const uint32_t y = std::min(renderExtent_.height - 1u, static_cast<uint32_t>(std::clamp(viewportUv.y, 0.0f, 1.0f) * static_cast<float>(renderExtent_.height)));
    const VkDeviceSize offset = (static_cast<VkDeviceSize>(y) * renderExtent_.width + x) * sizeof(uint32_t);

    checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(read entity pick buffer)");
    entityIdBuffer_.invalidate(sizeof(uint32_t), offset);
    const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const std::byte*>(entityIdBuffer_.mappedData()) + offset);
    if (id == UINT32_MAX) {
        return std::nullopt;
    }
    return id;
}

const GpuFrameTimings& PathTracerRenderer::timings() const {
    static const GpuFrameTimings empty{};
    if (currentProfiler_ != nullptr) {
        return currentProfiler_->timings();
    }
    return profilers_.empty() ? empty : profilers_.front().timings();
}

GpuPipelineStatistics PathTracerRenderer::pipelineStats() const {
    static const GpuPipelineStatistics empty{};
    if (currentProfiler_ != nullptr) {
        return currentProfiler_->pipelineStats();
    }
    return profilers_.empty() ? empty : profilers_.front().pipelineStats();
}

bool PathTracerRenderer::hardwareRayTracingAvailable() const {
    return context_.supportsHardwareRayTracing();
}

RayTracingRendererStats PathTracerRenderer::rayTracingStats() const {
    RayTracingRendererStats stats{};
    stats.active = rayTracingScene_ != nullptr && rayTracingPipeline_ != nullptr;
    if (!stats.active) {
        return stats;
    }
    stats.blasCount = rayTracingScene_->blasCount();
    stats.instanceCount = rayTracingScene_->instanceCount();
    stats.accelerationStructureBytes = rayTracingScene_->accelerationStructureBytes();
    stats.lastTlasRefitMs = rayTracingScene_->lastTlasRefitMs();
    stats.sbtBytes = rayTracingPipeline_->sbtBytes();
    return stats;
}

AtmosphereLutStats PathTracerRenderer::atmosphereLutStats() const {
    return atmosphereLutSystem_ != nullptr ? atmosphereLutSystem_->stats() : AtmosphereLutStats{};
}

VkDescriptorImageInfo PathTracerRenderer::viewportImageDescriptor() const {
    if (presentationImage_.handle() == VK_NULL_HANDLE) {
        return {};
    }
    return presentationImage_.sampledDescriptor(VK_NULL_HANDLE);
}

void PathTracerRenderer::createResolutionResources(VkExtent2D renderExtent, VkExtent2D displayExtent) {
    renderExtent_ = renderExtent;
    displayExtent_ = displayExtent;
    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(renderExtent.width) * renderExtent.height;
    rawImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer raw hdr",
    });
    denoisedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoised hdr",
    });
    historyImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoiser history",
    });
    taaImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer taa hdr",
    });
    taaHistoryImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer taa history",
    });
    presentationImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "path tracer presentation ldr",
    });
    cameraBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(CameraUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "camera uniform",
    });
    denoiserParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(DenoiserParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "denoiser params",
    });
    prevCameraBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(PrevCameraUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "denoiser previous camera",
    });
    debugParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(RendererDebugParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "renderer debug params",
    });
    accumulationBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(float) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path accumulation",
    });
    varianceBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path variance packed",
    });
    depthNormalBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path depth normal roughness packed",
    });
    worldPositionBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path world position packed",
    });
    previousWorldPositionBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "previous world position packed",
    });
    velocityBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "screen velocity packed",
    });
    entityIdBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "path entity id pick buffer",
    });
    pathDataBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(PathDataGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path data channels",
    });
    restirReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir current reservoir",
    });
    previousRestirReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir previous reservoir",
    });
    restirSpatialReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir spatial reservoir",
    });
    selectionParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(SelectionParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "selection outline params",
    });
    histogramBuffer_.create(allocator_, BufferDesc{
        .size = kHistogramBinCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "auto exposure histogram",
    });
    exposureBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(float) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "auto exposure state",
    });
    const float exposureState[4] = {settings_.exposure, settings_.exposure, settings_.targetLuminance, 0.0f};
    exposureBuffer_.write(exposureState, sizeof(exposureState));
    exposureBuffer_.flush(sizeof(exposureState));
    if (temporalSystem_) {
        temporalSystem_->createHistorySlot(
            "denoiser_history",
            historyImage_.format(),
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            1.0f);
        temporalSystem_->createHistorySlot(
            "previous_world_position",
            VK_FORMAT_R32G32_UINT,
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.8f);
        temporalSystem_->createHistorySlot(
            "taa_history",
            taaHistoryImage_.format(),
            VkExtent2D{displayExtent.width, displayExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            1.0f);
        temporalSystem_->createHistorySlot(
            "restir_reservoir",
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.75f);
        temporalSystem_->setCameraCut(true, AccumulationResetReason::Resize);
    }
}

void PathTracerRenderer::updateCamera() {
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.selectedInstance = selectedInstanceId_;
    debugParams_.scale = settings_.debugScale;

    camera_.sunIntensity = settings_.sunIntensity;
    camera_.skyIntensity = settings_.skyIntensity;
    camera_.exposure = settings_.usePhysicalCamera
        ? 2.0f * std::exp2(14.0f - physicalCamera_.ev100())
        : settings_.exposure;
    camera_.pathTracingEnabled = settings_.pathTracingEnabled ? 1u : 0u;
    camera_.maxBounces = adaptiveEffectiveMaxBounces_;
    camera_.sunlightEnabled = settings_.sunlightEnabled ? 1u : 0u;
    camera_.directLightingEnabled = settings_.directLightingEnabled ? 1u : 0u;
    camera_.sunAngularRadius = settings_.sunAngularRadius;
    camera_.indirectStrength = settings_.indirectStrength;
    camera_.environmentDirectSamples = adaptiveEffectiveEnvironmentSamples_;
    camera_.renderControls = glm::vec4(
        settings_.shadowRayBias,
        settings_.shadowDistanceBias,
        settings_.fireflyClamp,
        settings_.russianRouletteMinSurvival);
    // Manual exposure in this renderer is still calibrated around the legacy sun scalar.
    // Keep raw lux only for physical-camera mode where exposure is EV-based.
    camera_.sunDirectionIlluminance = glm::vec4(
        settings_.sunDirection,
        settings_.usePhysicalCamera ? settings_.sunIlluminanceLux : settings_.sunIntensity);
    camera_.sunColorAngularRadius = glm::vec4(settings_.sunColor, settings_.sunAngularRadius);
    const bool temporalCameraCut = temporalSystem_ ? temporalSystem_->isCameraCut() : temporalFrameIndex_ <= 1u;
    const bool temporalHistoryAvailable = !temporalCameraCut;
    camera_.atmosphere = glm::vec4(
        settings_.sunElevation,
        static_cast<float>(settings_.restirMode),
        temporalHistoryAvailable ? 1.0f : 0.0f,
        settings_.sunAzimuth);
    camera_.frameCount = frameCount_;
    camera_.temporalFrameIndex = temporalFrameIndex_;

    if (cameraChangedThisFrame_) {
        stillFrameCount_ = 0;
    } else {
        stillFrameCount_ = std::min(stillFrameCount_ + 1u, 60u);
    }
    const bool restoreFullJitter = stillFrameCount_ >= 2u;
    const float effectiveJitterScale = cameraChangedThisFrame_ ? 0.0f : (restoreFullJitter ? 1.0f : 0.0f);
    camera_.effectiveJitterScale = effectiveJitterScale;
    camera_.cameraMoving = cameraChangedThisFrame_ ? 1u : 0u;

    const float aspect = renderExtent_.height > 0 ? static_cast<float>(renderExtent_.width) / static_cast<float>(renderExtent_.height) : 1.0f;
    const glm::vec3 eye = glm::vec3(camera_.pos);
    const glm::vec3 center = eye + glm::normalize(glm::vec3(camera_.forward));
    const glm::mat4 view = glm::lookAtRH(eye, center, glm::normalize(glm::vec3(camera_.up)));
    glm::mat4 projection = glm::perspectiveRH_ZO(camera_.fovY, aspect, 0.01f, 1000.0f);
    projection[1][1] *= -1.0f;
    const bool jitterEnabled = settings_.pathTracingEnabled && settings_.taaEnabled && settings_.cameraJitterEnabled && effectiveJitterScale > 0.0f && renderExtent_.width > 0 && renderExtent_.height > 0;
    const uint32_t jitterIndex = temporalFrameIndex_ + 1u;
    const glm::vec2 currentJitter = jitterEnabled
        ? glm::vec2(halton(jitterIndex, 2u) - 0.5f, halton(jitterIndex, 3u) - 0.5f) * effectiveJitterScale
        : glm::vec2(0.0f);
    projection[2][0] -= currentJitter.x * 2.0f / static_cast<float>(std::max(renderExtent_.width, 1u));
    projection[2][1] -= currentJitter.y * 2.0f / static_cast<float>(std::max(renderExtent_.height, 1u));
    const glm::mat4 viewProj = projection * view;
    camera_.jitter = glm::vec4(currentJitter, previousJitter_);
    Buffer& frameUniforms = currentFrame_->uniformRing();
    frameUniforms.write(&camera_, sizeof(camera_), kFrameCameraUniformOffset);
    frameUniforms.flush(sizeof(camera_), kFrameCameraUniformOffset);

    prevCamera_.viewProj = viewProj;
    prevCamera_.invViewProj = glm::inverse(viewProj);
    prevCamera_.prevViewProj = previousViewProj_;
    prevCamera_.currentPos = camera_.pos;
    prevCamera_.prevPos = previousCameraPos_;
    prevCamera_.jitter = glm::vec4(currentJitter, previousJitter_);
    frameUniforms.write(&prevCamera_, sizeof(prevCamera_), kFramePrevCameraUniformOffset);
    frameUniforms.flush(sizeof(prevCamera_), kFramePrevCameraUniformOffset);

    const bool denoiserDebugView =
        debugParams_.view <= 4u ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::MotionVectors) ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight) ||
        (debugParams_.view >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
         debugParams_.view <= static_cast<uint32_t>(RendererDebugView::DenoiserKernelRadius));
    const bool allowDenoiserForDebugView = denoiserDebugView;
    const bool stablePreview = shouldRunTaa();
    const bool allowDenoiserWhileMoving = settings_.denoiseWhileMoving || stablePreview || !cameraChangedThisFrame_;
    denoiserParams_.enabled = settings_.pathTracingEnabled && settings_.denoiserEnabled && allowDenoiserForDebugView && allowDenoiserWhileMoving && !adaptiveSkipDenoiser_ ? 1u : 0u;
    denoiserParams_.strength = settings_.denoiserStrength;
    denoiserParams_.frameCount = temporalFrameIndex_;
    denoiserParams_.width = renderExtent_.width;
    denoiserParams_.height = renderExtent_.height;
    denoiserParams_.atrousIterations = adaptiveEffectiveAtrousIterations_;
    denoiserParams_.debugView = denoiserDebugView ? debugParams_.view : 0u;
    denoiserParams_.resetHistory = temporalCameraCut ? 1u : 0u;
    frameUniforms.write(&denoiserParams_, sizeof(denoiserParams_), kFrameDenoiserParamsOffset);
    frameUniforms.flush(sizeof(denoiserParams_), kFrameDenoiserParamsOffset);

    taaParams_.enabled = shouldRunTaa() ? 1u : 0u;
    taaParams_.frameCount = temporalFrameIndex_;
    taaParams_.width = displayExtent_.width;
    taaParams_.height = displayExtent_.height;
    const float taaFeedback = cameraChangedThisFrame_
        ? std::min(settings_.taaFeedback, 0.05f)
        : settings_.taaFeedback;
    taaParams_.feedback = std::clamp(taaFeedback, 0.01f, 0.5f);
    taaParams_.velocityScale = 64.0f;
    taaParams_.resetHistory = temporalCameraCut ? 1u : 0u;
    taaParams_.sharpeningStrength = settings_.taaSharpeningStrength;
    taaParams_.historyValid = taaHistoryValid_ ? 1u : 0u;
    taaParams_.cameraMoving = cameraChangedThisFrame_ ? 1u : 0u;
    taaParams_.renderWidth = renderExtent_.width;
    taaParams_.renderHeight = renderExtent_.height;
    frameUniforms.write(&taaParams_, sizeof(taaParams_), kFrameTaaParamsOffset);
    frameUniforms.flush(sizeof(taaParams_), kFrameTaaParamsOffset);
    if (temporalFrameIndex_ == 1u || temporalFrameIndex_ % 120u == 0u) {
        validationLog_.recordPass(
            "temporal state temporal=" + std::to_string(temporalFrameIndex_) +
            " accumulation=" + std::to_string(frameCount_) +
            " jitterScale=" + std::to_string(effectiveJitterScale) +
            " moving=" + std::to_string(cameraChangedThisFrame_ ? 1u : 0u) +
            " taaHistory=" + std::to_string(taaHistoryValid_ ? 1u : 0u) +
            " taaFeedback=" + std::to_string(taaParams_.feedback));
    }

    restirSpatialParams_.width = renderExtent_.width;
    restirSpatialParams_.height = renderExtent_.height;
    restirSpatialParams_.frameCount = temporalFrameIndex_;
    restirSpatialParams_.enabled = shouldRunRestirSpatial() ? 1u : 0u;
    frameUniforms.write(&restirSpatialParams_, sizeof(restirSpatialParams_), kFrameRestirSpatialParamsOffset);
    frameUniforms.flush(sizeof(restirSpatialParams_), kFrameRestirSpatialParamsOffset);

    fogParams_.width = renderExtent_.width;
    fogParams_.height = renderExtent_.height;
    fogParams_.debugView = debugParams_.view;
    fogParams_.enabled = settings_.pathTracingEnabled ? 1u : 0u;
    frameUniforms.write(&fogParams_, sizeof(fogParams_), kFrameFogParamsOffset);
    frameUniforms.flush(sizeof(fogParams_), kFrameFogParamsOffset);

    frameUniforms.write(&debugParams_, sizeof(debugParams_), kFrameDebugParamsOffset);
    frameUniforms.flush(sizeof(debugParams_), kFrameDebugParamsOffset);

    previousViewProj_ = viewProj;
    previousCameraPos_ = camera_.pos;
    previousJitter_ = currentJitter;
}

void PathTracerRenderer::recordPathTrace(VkCommandBuffer commandBuffer) {
    const VkPipelineStageFlags2 traceStage = pathTraceShaderStage();
    recordRenderGraphPlan();
    currentProfiler_->resetForFrame(commandBuffer);
    if (atmosphereLutSystem_ != nullptr) {
        validationLog_.recordPass("atmosphere lut update");
        currentProfiler_->write(commandBuffer, GpuProfiler::AtmosphereStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        atmosphereLutSystem_->setSkyDirection(settings_.sunDirection, settings_.skyIntensity);
        atmosphereLutSystem_->setAtmosphereParams(settings_.rayleighScaleHeight, settings_.mieScaleHeight, settings_.mieAnisotropy, settings_.groundAlbedo);
        atmosphereLutSystem_->setCameraPosition(glm::vec3(camera_.pos));
        atmosphereLutSystem_->record(commandBuffer, currentFrame_->descriptors());
        if (const AtmosphereSamplingSystem* sampling = atmosphereLutSystem_->samplingSystem()) {
            scene_.setSkyCdfDimensions(sampling->skyViewWidth(), sampling->skyViewHeight());
        }
        currentProfiler_->write(commandBuffer, GpuProfiler::AtmosphereEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceStart, traceStage);
    recordPathTraceGraph(commandBuffer);
    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceEnd, traceStage);
    currentProfiler_->markStatsSubmitted();
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    recordRestirSpatial(commandBuffer);
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    currentProfiler_->write(commandBuffer, GpuProfiler::FogIntegrateStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    recordHeightFog(commandBuffer);
    currentProfiler_->write(commandBuffer, GpuProfiler::FogIntegrateEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    if (shouldRunDenoiser()) {
        recordDenoiser(commandBuffer);
        copyHistoryResources(commandBuffer);
    } else {
        currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        skipDenoiserPass(commandBuffer);
    }
    if (shouldRunTaa()) {
        recordTaa(commandBuffer);
    } else {
        currentProfiler_->write(commandBuffer, GpuProfiler::TaaStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        currentProfiler_->write(commandBuffer, GpuProfiler::TaaEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    if (settings_.autoExposureEnabled) {
        recordAutoExposure(commandBuffer);
    } else {
        currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    recordToneMap(commandBuffer);
    recordSelectionOutline(commandBuffer);
    cameraChangedThisFrame_ = false;
    if (temporalSystem_) {
        temporalSystem_->endFrame();
    }
    currentProfiler_->markSubmitted();
}

void PathTracerRenderer::recordPathTraceGraph(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    graph.resources()[raw.index].hasInitialAccess = true;
    graph.resources()[raw.index].initialAccess = ResourceAccess{
        .stage = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : pathTraceShaderStage(),
        .access = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = rawImage_.layout(),
    };
    const RenderGraphResourceId accumulation = graph.createBuffer(bufferResource(accumulationBuffer_, "accumulation"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId entityIds = graph.createBuffer(bufferResource(entityIdBuffer_, "entity ids"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir"));
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(bufferResource(previousRestirReservoirBuffer_, "previous restir reservoir"));
    const PipelineDomain traceDomain = PipelineDomain::RayTracing;
    graph.addPass("path_trace_rt")
        .addStorageWrite(raw, traceDomain)
        .addStorageWrite(entityIds, traceDomain)
        .addStorageReadWrite(accumulation, traceDomain)
        .addStorageWrite(variance, traceDomain)
        .addStorageWrite(depthNormal, traceDomain)
        .addStorageWrite(worldPosition, traceDomain)
        .addStorageWrite(velocity, traceDomain)
        .addStorageWrite(pathData, traceDomain)
        .addStorageRead(previousRestirReservoir, traceDomain)
        .addStorageWrite(restirReservoir, traceDomain)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordPathTracePass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordPathTracePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("path tracing rt");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    currentProfiler_->beginPipelineStats(commandBuffer);
    recordHardwarePathTrace(commandBuffer);
    currentProfiler_->endPipelineStats(commandBuffer);
}

void PathTracerRenderer::recordRestirSpatial(VkCommandBuffer commandBuffer) {
    if (!shouldRunRestirSpatial()) {
        if (adaptiveSkipRestirSpatial_) {
            validationLog_.recordPass("adaptive skip restir spatial");
        }
        return;
    }

    RenderGraph graph(&allocator_);
    auto bufferResource = [](const Buffer& buffer, const char* name, std::optional<ResourceAccess> initial = std::nullopt) {
        RenderGraphResource resource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .external = true,
            .debugName = name,
        };
        if (initial.has_value()) {
            resource.hasInitialAccess = true;
            resource.initialAccess = *initial;
        }
        return resource;
    };

    const ResourceAccess pathWrite{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir", pathWrite));
    const RenderGraphResourceId restirSpatialReservoir = graph.createBuffer(bufferResource(restirSpatialReservoirBuffer_, "restir spatial reservoir"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal", pathWrite));

    graph.addPass("restir_spatial")
        .addStorageRead(restirReservoir, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageWrite(restirSpatialReservoir, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordRestirSpatialPass(cmd);
        });
    graph.addPass("restir_spatial_copy")
        .addStorageRead(restirSpatialReservoir, PipelineDomain::Transfer)
        .addStorageWrite(restirReservoir, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordRestirSpatialCopyPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordRestirSpatialPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("restir spatial reuse");
    DescriptorSet set = currentFrame_->descriptors().allocate(restirSpatialSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirReservoirBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirSpatialReservoirBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameRestirSpatialParamsOffset, sizeof(RestirSpatialParams)))
        .update(context_.device(), set);

    restirSpatialPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, restirSpatialPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    restirSpatialPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
}

void PathTracerRenderer::recordRestirSpatialCopyPass(VkCommandBuffer commandBuffer) {
    VkBufferCopy copy{};
    copy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirSpatialReservoirBuffer_.handle(), restirReservoirBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordHeightFog(VkCommandBuffer commandBuffer) {
    if (fogPipeline_ == nullptr || fogSetLayout_ == VK_NULL_HANDLE || rawImage_.handle() == VK_NULL_HANDLE || depthNormalBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    RenderGraph graph(&allocator_);
    auto imageResource = [](const Image& image, const char* name, ResourceAccess initial) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = initial,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name, ResourceAccess initial) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = initial,
            .debugName = name,
        };
    };

    const ResourceAccess pathWrite{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr", pathWrite));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal", pathWrite));
    graph.addPass("fog_integrate")
        .addStorageReadWrite(raw, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordHeightFogPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordHeightFogPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("height fog integrate");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(fogSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameCameraUniformOffset, sizeof(CameraUniform)))
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameFogParamsOffset, sizeof(FogParams)))
        .update(context_.device(), set);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    fogPipeline_->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, fogPipeline_->layout(), 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
    fogPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
}

void PathTracerRenderer::recordRenderGraphPlan() {
    if (rawImage_.handle() == VK_NULL_HANDLE || presentationImage_.handle() == VK_NULL_HANDLE) {
        return;
    }

    RenderGraph graph(&allocator_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    const RenderGraphResourceId denoised = graph.createTexture(imageResource(denoisedImage_, "denoised hdr"));
    const RenderGraphResourceId history = graph.createTexture(imageResource(historyImage_, "history hdr"));
    const RenderGraphResourceId taa = graph.createTexture(imageResource(taaImage_, "taa hdr"));
    const RenderGraphResourceId taaHistory = graph.createTexture(imageResource(taaHistoryImage_, "taa history hdr"));
    const RenderGraphResourceId presentation = graph.createTexture(imageResource(presentationImage_, "presentation ldr"));
    const RenderGraphResourceId entityIds = graph.createBuffer(bufferResource(entityIdBuffer_, "entity ids"));
    const RenderGraphResourceId accumulation = graph.createBuffer(bufferResource(accumulationBuffer_, "accumulation"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(bufferResource(previousWorldPositionBuffer_, "previous world position"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir"));
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(bufferResource(previousRestirReservoirBuffer_, "previous restir reservoir"));
    const RenderGraphResourceId restirSpatialReservoir = graph.createBuffer(bufferResource(restirSpatialReservoirBuffer_, "restir spatial reservoir"));

    const PipelineDomain traceDomain = PipelineDomain::RayTracing;
    graph.addPass("path_trace_rt")
        .addStorageWrite(raw, traceDomain)
        .addStorageWrite(entityIds, traceDomain)
        .addStorageReadWrite(accumulation, traceDomain)
        .addStorageWrite(variance, traceDomain)
        .addStorageWrite(depthNormal, traceDomain)
        .addStorageWrite(worldPosition, traceDomain)
        .addStorageWrite(velocity, traceDomain)
        .addStorageWrite(pathData, traceDomain)
        .addStorageRead(previousRestirReservoir, traceDomain)
        .addStorageWrite(restirReservoir, traceDomain);

    if (shouldRunRestirSpatial()) {
        graph.addPass("restir_spatial")
            .addStorageRead(restirReservoir, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(restirSpatialReservoir, PipelineDomain::Compute);
        graph.addPass("restir_spatial_copy")
            .addStorageRead(restirSpatialReservoir, PipelineDomain::Transfer)
            .addStorageWrite(restirReservoir, PipelineDomain::Transfer);
    }
    graph.addPass("fog_integrate")
        .addStorageReadWrite(raw, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute);

    RenderGraphResourceId toneInput = raw;
    if (shouldRunDenoiser()) {
        graph.addPass("temporal_denoiser")
            .addStorageReadWrite(raw, PipelineDomain::Compute)
            .addStorageReadWrite(history, PipelineDomain::Compute)
            .addStorageRead(variance, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(worldPosition, PipelineDomain::Compute)
            .addStorageRead(previousWorldPosition, PipelineDomain::Compute)
            .addStorageRead(velocity, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageWrite(denoised, PipelineDomain::Compute);
        graph.addPass("history_copy")
            .addStorageRead(denoised, PipelineDomain::Transfer)
            .addStorageWrite(history, PipelineDomain::Transfer)
            .addStorageRead(worldPosition, PipelineDomain::Transfer)
            .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
            .addStorageRead(restirReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
        toneInput = denoised;
    } else {
        graph.addPass("skip_denoiser_copy")
            .addStorageRead(raw, PipelineDomain::Transfer)
            .addStorageWrite(denoised, PipelineDomain::Transfer)
            .addStorageRead(worldPosition, PipelineDomain::Transfer)
            .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
            .addStorageRead(restirReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
    }

    if (shouldRunTaa()) {
        graph.addPass("taa_resolve")
            .addStorageRead(toneInput, PipelineDomain::Compute)
            .addStorageReadWrite(taaHistory, PipelineDomain::Compute)
            .addStorageRead(velocity, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(taa, PipelineDomain::Compute);
        graph.addPass("taa_history_copy")
            .addStorageRead(taa, PipelineDomain::Transfer)
            .addStorageWrite(taaHistory, PipelineDomain::Transfer);
        toneInput = taa;
    }

    if (settings_.autoExposureEnabled) {
        const RenderGraphResourceId histogram = graph.createBuffer(bufferResource(histogramBuffer_, "luminance histogram"));
        const RenderGraphResourceId exposure = graph.createBuffer(bufferResource(exposureBuffer_, "exposure"));
        graph.addPass("auto_exposure_histogram")
            .addStorageRead(toneInput, PipelineDomain::Compute)
            .addStorageWrite(histogram, PipelineDomain::Compute);
        graph.addPass("auto_exposure_reduce")
            .addStorageRead(histogram, PipelineDomain::Compute)
            .addStorageWrite(exposure, PipelineDomain::Compute);
    }

    graph.addPass("tone_map")
        .addStorageRead(toneInput, PipelineDomain::Compute)
        .addStorageWrite(presentation, PipelineDomain::Compute);

    if (selectedInstanceId_ != UINT32_MAX) {
        graph.addPass("selection_outline")
            .addStorageRead(entityIds, PipelineDomain::Compute)
            .addStorageReadWrite(presentation, PipelineDomain::Compute);
    }

    graph.compile();
    validationLog_.recordPass("render graph compiled pass count=" + std::to_string(graph.compiledPassOrder().size()));
    for (uint32_t passIndex : graph.compiledPassOrder()) {
        validationLog_.recordPass("render graph pass: " + graph.passes()[passIndex].name());
    }
    for (const RenderGraphBarrier& barrier : graph.compiledBarriers()) {
        const RenderGraphResource& resource = graph.resources()[barrier.resource.index];
        const std::string resourceName = resource.debugName != nullptr ? resource.debugName : "<unnamed>";
        const std::string beforePass = barrier.beforePass < graph.passes().size() ? graph.passes()[barrier.beforePass].name() : "<external>";
        const std::string afterPass = barrier.afterPass < graph.passes().size() ? graph.passes()[barrier.afterPass].name() : "<external>";
        validationLog_.recordBarrier(
            "render graph barrier " + resourceName + " " + beforePass + " -> " + afterPass,
            barrier.before.stage,
            barrier.before.access,
            barrier.after.stage,
            barrier.after.access);
        validationLog_.recordResourceState(ResourceStateEvent{
            .resource = resourceName,
            .beforePass = beforePass,
            .afterPass = afterPass,
            .beforeLayout = barrier.before.layout,
            .afterLayout = barrier.after.layout,
            .beforeStage = barrier.before.stage,
            .afterStage = barrier.after.stage,
            .beforeAccess = barrier.before.access,
            .afterAccess = barrier.after.access,
        });
    }
}

void PathTracerRenderer::recordHardwarePathTrace(VkCommandBuffer commandBuffer) {
    if (rayTracingPipeline_ == nullptr || rayTracingScene_ == nullptr) {
        throw std::runtime_error("Hardware ray tracing backend is active but RT pipeline/scene is not initialized");
    }

    DescriptorSet set = currentFrame_->descriptors().allocate(rayTracingSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, accumulationBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameCameraUniformOffset, sizeof(CameraUniform)))
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.materials().descriptorInfo())
        .writeBuffer(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.meshParamsBuffer().descriptorInfo())
        .writeImage(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, scene_.environmentImage().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(13, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = scene_.environmentSampler()})
        .writeBuffer(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envRows().descriptorInfo())
        .writeBuffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envCols().descriptorInfo())
        .writeBuffer(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.envParamsBuffer().descriptorInfo())
        .writeBuffer(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.spheres().descriptorInfo())
        .writeBuffer(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameDebugParamsOffset, sizeof(RendererDebugParams)))
        .writeBuffer(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.primitiveRecords().descriptorInfo())
        .writeBuffer(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.instanceRecords().descriptorInfo())
        .writeBuffer(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightRecords().descriptorInfo())
        .writeImageArray(41, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scene_.materialCombinedDescriptors())
        .writeBuffer(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.meshRecords().descriptorInfo())
        .writeBuffer(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localVertices().descriptorInfo())
        .writeBuffer(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localIndices().descriptorInfo())
        .writeBuffer(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localTriangles().descriptorInfo())
        .writeAccelerationStructure(33, rayTracingScene_->tlas())
        .writeBuffer(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.rtTriangleMaterialIds().descriptorInfo())
        .writeBuffer(35, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, entityIdBuffer_.descriptorInfo())
        .writeBuffer(36, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(37, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFramePrevCameraUniformOffset, sizeof(PrevCameraUniform)))
        .writeBuffer(38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirReservoirBuffer_.descriptorInfo())
        .writeBuffer(39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousRestirReservoirBuffer_.descriptorInfo())
        .writeBuffer(40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightBvhNodes().descriptorInfo())
        .writeBuffer(42, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .update(context_.device(), set);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    rayTracingPipeline_->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline_->layout(), 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
    rayTracingPipeline_->traceRays(commandBuffer, renderExtent_.width, renderExtent_.height);
}

void PathTracerRenderer::recordSelectionOutline(VkCommandBuffer commandBuffer) {
    if (selectedInstanceId_ == UINT32_MAX || selectionOutlinePipeline_ == nullptr || selectionOutlineSetLayout_ == VK_NULL_HANDLE) {
        currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        return;
    }
    RenderGraph graph(&allocator_);
    const RenderGraphResourceId presentation = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = presentationImage_.format(),
        .extent = presentationImage_.extent(),
        .image = presentationImage_.handle(),
        .imageRange = presentationImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = presentationImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "presentation ldr",
    });
    const RenderGraphResourceId entityIds = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = entityIdBuffer_.size(),
        .buffer = entityIdBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "entity ids",
    });
    graph.addPass("selection_outline")
        .addStorageRead(entityIds, PipelineDomain::Compute)
        .addStorageReadWrite(presentation, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordSelectionOutlinePass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordSelectionOutlinePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("selection outline");
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    const SelectionParams params{
        .selectedInstance = selectedInstanceId_,
        .width = displayExtent_.width,
        .height = displayExtent_.height,
        .enabled = 1u,
        .renderWidth = renderExtent_.width,
        .renderHeight = renderExtent_.height,
    };
    selectionParamsBuffer_.write(&params, sizeof(params));
    selectionParamsBuffer_.flush(sizeof(params));

    DescriptorSet set = currentFrame_->descriptors().allocate(selectionOutlineSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, presentationImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, entityIdBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, selectionParamsBuffer_.descriptorInfo())
        .update(context_.device(), set);

    selectionOutlinePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, selectionOutlinePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    selectionOutlinePipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height, 8, 8);

}

VkPipelineStageFlags2 PathTracerRenderer::pathTraceShaderStage() const {
    return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
}

void PathTracerRenderer::recordDenoiser(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    const RenderGraphResourceId history = graph.createTexture(imageResource(historyImage_, "history hdr"));
    const RenderGraphResourceId denoised = graph.createTexture(imageResource(denoisedImage_, "denoised hdr"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(bufferResource(previousWorldPositionBuffer_, "previous world position"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    graph.resources()[raw.index].hasInitialAccess = true;
    graph.resources()[raw.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    graph.resources()[history.index].hasInitialAccess = true;
    graph.resources()[history.index].initialAccess = ResourceAccess{
        .stage = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
        .access = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .layout = historyImage_.layout(),
    };
    graph.resources()[denoised.index].hasInitialAccess = true;
    graph.resources()[denoised.index].initialAccess = ResourceAccess{
        .stage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .access = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .layout = denoisedImage_.layout(),
    };
    graph.resources()[variance.index].hasInitialAccess = true;
    graph.resources()[variance.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[depthNormal.index].hasInitialAccess = true;
    graph.resources()[depthNormal.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[worldPosition.index].hasInitialAccess = true;
    graph.resources()[worldPosition.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[velocity.index].hasInitialAccess = true;
    graph.resources()[velocity.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[pathData.index].hasInitialAccess = true;
    graph.resources()[pathData.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };

    graph.addPass("temporal_denoiser")
        .addStorageReadWrite(raw, PipelineDomain::Compute)
        .addStorageReadWrite(history, PipelineDomain::Compute)
        .addStorageRead(variance, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageRead(worldPosition, PipelineDomain::Compute)
        .addStorageRead(previousWorldPosition, PipelineDomain::Compute)
        .addStorageRead(velocity, PipelineDomain::Compute)
        .addStorageRead(pathData, PipelineDomain::Compute)
        .addStorageWrite(denoised, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordDenoiserPass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordDenoiserPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("temporal denoiser compute");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(denoiserSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyImage_.storageDescriptor())
        .writeImage(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, denoisedImage_.storageDescriptor())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameDenoiserParamsOffset, sizeof(DenoiserParams)))
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousWorldPositionBuffer_.descriptorInfo())
        .writeBuffer(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFramePrevCameraUniformOffset, sizeof(PrevCameraUniform)))
        .writeBuffer(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .update(context_.device(), set);

    denoiserPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, denoiserPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    denoiserPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
}

void PathTracerRenderer::copyHistoryResources(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = denoisedImage_.format(),
        .extent = denoisedImage_.extent(),
        .image = denoisedImage_.handle(),
        .imageRange = denoisedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "denoised hdr",
    });
    const RenderGraphResourceId history = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = historyImage_.format(),
        .extent = historyImage_.extent(),
        .image = historyImage_.handle(),
        .imageRange = historyImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "history hdr",
    });
    const RenderGraphResourceId worldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = worldPositionBuffer_.size(),
        .buffer = worldPositionBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "world position",
    });
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousWorldPositionBuffer_.size(),
        .buffer = previousWorldPositionBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous world position",
    });
    const RenderGraphResourceId restirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = restirReservoirBuffer_.size(),
        .buffer = restirReservoirBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "restir reservoir",
    });
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousRestirReservoirBuffer_.size(),
        .buffer = previousRestirReservoirBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous restir reservoir",
    });
    graph.addPass("history_copy")
        .addStorageRead(denoised, PipelineDomain::Transfer)
        .addStorageWrite(history, PipelineDomain::Transfer)
        .addStorageRead(worldPosition, PipelineDomain::Transfer)
        .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
        .addStorageRead(restirReservoir, PipelineDomain::Transfer)
        .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            copyHistoryResourcesPass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

void PathTracerRenderer::copyHistoryResourcesPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("history copy");
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy imageCopy{};
    imageCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopy.srcSubresource.layerCount = 1;
    imageCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopy.dstSubresource.layerCount = 1;
    imageCopy.extent = denoisedImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        denoisedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        historyImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &imageCopy);

    VkBufferCopy copy{};
    copy.size = worldPositionBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, worldPositionBuffer_.handle(), previousWorldPositionBuffer_.handle(), 1, &copy);
    VkBufferCopy restirCopy{};
    restirCopy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirReservoirBuffer_.handle(), previousRestirReservoirBuffer_.handle(), 1, &restirCopy);
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("denoiser_history");
        temporalSystem_->markSlotWritten("previous_world_position");
        temporalSystem_->markSlotWritten("restir_reservoir");
    }
}

bool PathTracerRenderer::shouldRunDenoiser() const {
    if (denoiserParams_.enabled != 0u) {
        return true;
    }
    if (denoiserParams_.debugView >= 1u && denoiserParams_.debugView <= 4u) {
        return true;
    }
    if (denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::MotionVectors)) {
        return true;
    }
    if (denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
        denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight)) {
        return true;
    }
    if (denoiserParams_.debugView >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
        denoiserParams_.debugView <= static_cast<uint32_t>(RendererDebugView::DenoiserKernelRadius)) {
        return true;
    }
    return false;
}

bool PathTracerRenderer::shouldRunTaa() const {
    return settings_.pathTracingEnabled &&
        settings_.taaEnabled &&
        taaPipeline_ != nullptr &&
        taaSetLayout_ != VK_NULL_HANDLE &&
        taaImage_.handle() != VK_NULL_HANDLE &&
        taaHistoryImage_.handle() != VK_NULL_HANDLE &&
        velocityBuffer_.handle() != VK_NULL_HANDLE;
}

bool PathTracerRenderer::shouldRunRestirSpatial() const {
    return !adaptiveSkipRestirSpatial_ &&
        settings_.restirMode != RestirMode::ClassicNee &&
        restirSpatialPipeline_ != nullptr &&
        restirSpatialSetLayout_ != VK_NULL_HANDLE &&
        restirReservoirBuffer_.handle() != VK_NULL_HANDLE &&
        restirSpatialReservoirBuffer_.handle() != VK_NULL_HANDLE &&
        depthNormalBuffer_.handle() != VK_NULL_HANDLE;
}

const Image& PathTracerRenderer::postDenoiseImage() const {
    return denoisedImage_;
}

const Image& PathTracerRenderer::hdrPostProcessImage() const {
    return shouldRunTaa() ? taaImage_ : postDenoiseImage();
}

void PathTracerRenderer::recordTaa(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    const Image& inputImage = postDenoiseImage();
    const RenderGraphResourceId input = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = inputImage.format(),
        .extent = inputImage.extent(),
        .image = inputImage.handle(),
        .imageRange = inputImage.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = inputImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            .access = inputImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            .layout = inputImage.layout(),
        },
        .debugName = "taa input hdr",
    });
    const RenderGraphResourceId output = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaImage_.format(),
        .extent = taaImage_.extent(),
        .image = taaImage_.handle(),
        .imageRange = taaImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = taaImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "taa output hdr",
    });
    const RenderGraphResourceId history = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaHistoryImage_.format(),
        .extent = taaHistoryImage_.extent(),
        .image = taaHistoryImage_.handle(),
        .imageRange = taaHistoryImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
            .access = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .layout = taaHistoryImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "taa history hdr",
    });
    const RenderGraphResourceId velocity = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = velocityBuffer_.size(),
        .buffer = velocityBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        },
        .debugName = "screen velocity",
    });
    const RenderGraphResourceId depthNormal = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = depthNormalBuffer_.size(),
        .buffer = depthNormalBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "depth normal",
    });
    graph.addPass("taa_resolve")
        .addStorageRead(input, PipelineDomain::Compute)
        .addStorageReadWrite(history, PipelineDomain::Compute)
        .addStorageRead(velocity, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageWrite(output, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordTaaPass(cmd);
        });
    graph.addPass("taa_history_copy")
        .addStorageRead(output, PipelineDomain::Transfer)
        .addStorageWrite(history, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordTaaHistoryCopyPass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordTaaPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("taa resolve");
    const Image& inputImage = postDenoiseImage();
    inputImage.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(taaSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, inputImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, taaHistoryImage_.storageDescriptor())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, taaImage_.storageDescriptor())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameTaaParamsOffset, sizeof(TaaParams)))
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .update(context_.device(), set);

    taaPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, taaPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    taaPipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height);
}

void PathTracerRenderer::recordTaaHistoryCopyPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("taa history copy");
    taaImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = taaImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        taaImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        taaHistoryImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("taa_history");
    }
    taaHistoryValid_ = true;
}

void PathTracerRenderer::recordAutoExposure(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    const Image& sourceImage = hdrPostProcessImage();
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = sourceImage.format(),
        .extent = sourceImage.extent(),
        .image = sourceImage.handle(),
        .imageRange = sourceImage.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = sourceImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            .access = sourceImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = sourceImage.layout(),
        },
        .debugName = "post temporal hdr",
    });
    const RenderGraphResourceId histogram = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = histogramBuffer_.size(),
        .buffer = histogramBuffer_.handle(),
        .external = true,
        .debugName = "luminance histogram",
    });
    const RenderGraphResourceId exposure = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = exposureBuffer_.size(),
        .buffer = exposureBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "exposure",
    });
    graph.addPass("auto_exposure_histogram_clear")
        .addStorageWrite(histogram, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            validationLog_.recordPass("auto exposure histogram clear");
            vkCmdFillBuffer(cmd, histogramBuffer_.handle(), 0, histogramBuffer_.size(), 0);
        });
    graph.addPass("auto_exposure_histogram")
        .addStorageRead(denoised, PipelineDomain::Compute)
        .addStorageReadWrite(histogram, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordAutoExposureHistogramPass(cmd);
        });
    graph.addPass("auto_exposure_reduce")
        .addStorageRead(histogram, PipelineDomain::Compute)
        .addStorageWrite(exposure, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordAutoExposureReducePass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordAutoExposureHistogramPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("auto exposure histogram");
    DescriptorSet histogramSet = currentFrame_->descriptors().allocate(luminanceHistogramSetLayout_);
    const Image& sourceImage = hdrPostProcessImage();
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, histogramBuffer_.descriptorInfo())
        .update(context_.device(), histogramSet);

    luminanceHistogramPipeline_->bind(commandBuffer);
    VkDescriptorSet descriptorSet = histogramSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, luminanceHistogramPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const HistogramParams histogramParams{
        .width = sourceImage.extent().width,
        .height = sourceImage.extent().height,
        .minLogLuminance = settings_.histogramMinLogLuminance,
        .maxLogLuminance = settings_.histogramMaxLogLuminance,
    };
    vkCmdPushConstants(
        commandBuffer,
        luminanceHistogramPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(histogramParams),
        &histogramParams);
    luminanceHistogramPipeline_->dispatch(commandBuffer, sourceImage.extent().width, sourceImage.extent().height);
}

void PathTracerRenderer::recordAutoExposureReducePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("auto exposure reduce");
    DescriptorSet exposureSet = currentFrame_->descriptors().allocate(exposureReduceSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, histogramBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, exposureBuffer_.descriptorInfo())
        .update(context_.device(), exposureSet);

    exposureReducePipeline_->bind(commandBuffer);
    VkDescriptorSet descriptorSet = exposureSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposureReducePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const ExposureReduceParams exposureParams{
        .pixelCount = hdrPostProcessImage().extent().width * hdrPostProcessImage().extent().height,
        .targetLuminance = settings_.targetLuminance,
        .minExposure = settings_.minExposure,
        .maxExposure = settings_.maxExposure,
        .adaptationSpeed = settings_.adaptationSpeed,
        .lowPercentile = settings_.histogramLowPercentile,
        .highPercentile = settings_.histogramHighPercentile,
        .targetPercentile = settings_.histogramTargetPercentile,
        .deltaSeconds = frameDeltaSeconds_,
        .minLogLuminance = settings_.histogramMinLogLuminance,
        .maxLogLuminance = settings_.histogramMaxLogLuminance,
    };
    vkCmdPushConstants(
        commandBuffer,
        exposureReducePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(exposureParams),
        &exposureParams);
    exposureReducePipeline_->dispatch(commandBuffer, 1, 1, 1, 1);
}

void PathTracerRenderer::recordToneMap(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    const Image& sourceImage = hdrPostProcessImage();
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = sourceImage.format(),
        .extent = sourceImage.extent(),
        .image = sourceImage.handle(),
        .imageRange = sourceImage.fullRange(),
        .external = true,
        .debugName = "post temporal hdr",
    });
    const RenderGraphResourceId presentation = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = presentationImage_.format(),
        .extent = presentationImage_.extent(),
        .image = presentationImage_.handle(),
        .imageRange = presentationImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = presentationImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "presentation ldr",
    });
    const RenderGraphResourceId exposure = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = exposureBuffer_.size(),
        .buffer = exposureBuffer_.handle(),
        .external = true,
        .debugName = "exposure",
    });
    graph.addPass("tone_map")
        .addStorageRead(denoised, PipelineDomain::Compute)
        .addStorageRead(exposure, PipelineDomain::Compute)
        .addStorageWrite(presentation, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordToneMapPass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::ToneMapStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    currentProfiler_->write(commandBuffer, GpuProfiler::ToneMapEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordToneMapPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("tone map compute");
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet toneMapSet = currentFrame_->descriptors().allocate(toneMapSetLayout_);
    const Image& sourceImage = hdrPostProcessImage();
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, presentationImage_.storageDescriptor())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, exposureBuffer_.descriptorInfo())
        .update(context_.device(), toneMapSet);

    toneMapPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = toneMapSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, toneMapPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const float effectiveExposure = settings_.usePhysicalCamera
        ? 2.0f * std::exp2(14.0f - physicalCamera_.ev100())
        : settings_.exposure;
    const ToneMapParams toneMapParams{
        .toneMapper = static_cast<uint32_t>(settings_.toneMapper),
        .debugView = static_cast<uint32_t>(settings_.debugView),
        .autoExposureEnabled = settings_.autoExposureEnabled ? 1u : 0u,
        .exposure = effectiveExposure,
        .gamma = settings_.gamma,
        .contrast = settings_.contrast,
        .saturation = settings_.saturation,
        .brightness = settings_.brightness,
        .whitePoint = settings_.whitePoint,
    };
    vkCmdPushConstants(
        commandBuffer,
        toneMapPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(toneMapParams),
        &toneMapParams);
    toneMapPipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height);
}

void PathTracerRenderer::skipDenoiserPass(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_);
    const RenderGraphResourceId raw = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = rawImage_.format(),
        .extent = rawImage_.extent(),
        .image = rawImage_.handle(),
        .imageRange = rawImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "raw hdr",
    });
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = denoisedImage_.format(),
        .extent = denoisedImage_.extent(),
        .image = denoisedImage_.handle(),
        .imageRange = denoisedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .access = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = denoisedImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "denoised hdr",
    });
    const RenderGraphResourceId worldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = worldPositionBuffer_.size(),
        .buffer = worldPositionBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "world position",
    });
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousWorldPositionBuffer_.size(),
        .buffer = previousWorldPositionBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous world position",
    });
    const RenderGraphResourceId restirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = restirReservoirBuffer_.size(),
        .buffer = restirReservoirBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "restir reservoir",
    });
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousRestirReservoirBuffer_.size(),
        .buffer = previousRestirReservoirBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous restir reservoir",
    });
    graph.addPass("skip_denoiser_copy")
        .addStorageRead(raw, PipelineDomain::Transfer)
        .addStorageWrite(denoised, PipelineDomain::Transfer)
        .addStorageRead(worldPosition, PipelineDomain::Transfer)
        .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
        .addStorageRead(restirReservoir, PipelineDomain::Transfer)
        .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            skipDenoiserCopyPass(cmd);
        });
    graph.compile();
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    graph.execute(commandBuffer, temporalFrameIndex_);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

void PathTracerRenderer::skipDenoiserCopyPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass(adaptiveSkipDenoiser_ ? "adaptive skip denoiser copy" : "skip denoiser copy");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = rawImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        rawImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        denoisedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);

    VkBufferCopy worldCopy{};
    worldCopy.size = worldPositionBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, worldPositionBuffer_.handle(), previousWorldPositionBuffer_.handle(), 1, &worldCopy);
    VkBufferCopy restirCopy{};
    restirCopy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirReservoirBuffer_.handle(), previousRestirReservoirBuffer_.handle(), 1, &restirCopy);
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("previous_world_position");
        temporalSystem_->markSlotWritten("restir_reservoir");
    }
}

void PathTracerRenderer::recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent) {
    validationLog_.recordPass("fullscreen presentation");
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenStart, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    DescriptorSet set = currentFrame_->descriptors().allocate(graphicsSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, presentationImage_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .update(context_.device(), set);

    graphicsPipeline_->bind(commandBuffer, swapchainExtent);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenEnd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

void PathTracerRenderer::recordEditorPresentationStart(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("editor viewport presentation");
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenStart, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
}

void PathTracerRenderer::recordEditorPresentationEnd(VkCommandBuffer commandBuffer) {
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenEnd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

} // namespace rtv
