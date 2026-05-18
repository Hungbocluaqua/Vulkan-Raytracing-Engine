#include "rtv/PathTracerRenderer.h"

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
#include "rtv/ShaderCompiler.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"
#include "rtv/VulkanContext.h"

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace rtv {

namespace {

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
        descriptorBinding(19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, allRt, 128),
        descriptorBinding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(23, VK_DESCRIPTOR_TYPE_SAMPLER, allRt, 128),
        descriptorBinding(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(33, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
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
    case AccumulationResetReason::BackendChanged: return "BackendChanged";
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
    std::optional<std::filesystem::path> sceneCachePath,
    RendererBackend requestedBackend)
    : context_(context),
      allocator_(allocator),
      uploader_(uploader),
      scene_(allocator, uploader, importedScene, assets, std::move(environmentPath), std::move(sceneCachePath)) {
    requestedBackend_ = requestedBackend;
    settings_.requestedBackend = requestedBackend_;
    if (requestedBackend_ == RendererBackend::Compute) {
        activeBackend_ = RendererBackend::Compute;
    } else if (requestedBackend_ == RendererBackend::HardwareRayTracing) {
        if (!context_.supportsHardwareRayTracing()) {
            throw std::runtime_error("Hardware ray tracing backend was requested but this Vulkan device does not support the required KHR ray tracing features/extensions");
        }
        activeBackend_ = RendererBackend::HardwareRayTracing;
    } else {
        activeBackend_ = context_.supportsHardwareRayTracing() ? RendererBackend::HardwareRayTracing : RendererBackend::Compute;
        if (activeBackend_ == RendererBackend::Compute) {
            std::cout << "Renderer backend requested: auto; hardware ray tracing unavailable, using compute backend.\n";
        }
    }
    std::cout << "Renderer backend requested: " << rendererBackendName(requestedBackend_)
              << "\nRenderer backend active: " << rendererBackendName(activeBackend_) << '\n';

    ShaderCompiler compiler(glslangPath());
    const auto pathSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.comp", shaderOutputDirectory);
    const auto denoiserSpv = compiler.compileIfNeeded(shaderDirectory / "denoiser.comp", shaderOutputDirectory);
    const auto vertSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.vert", shaderOutputDirectory);
    const auto fragSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.frag", shaderOutputDirectory);

    pathTraceShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(pathSpv), "path trace compute");
    denoiserShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(denoiserSpv), "temporal denoiser compute");
    fullscreenVertexShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(vertSpv), "fullscreen vertex");
    fullscreenFragmentShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fragSpv), "fullscreen fragment");
    if (activeBackend_ == RendererBackend::HardwareRayTracing) {
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
    }

    layoutCache_ = std::make_unique<DescriptorLayoutCache>(context_.device());
    pipelineCache_ = std::make_unique<PipelineCache>(context_.device());
    auto pathTraceBindings = ShaderReflection::bindingsForSet({pathTraceShader_->reflection()}, 0);
    std::vector<VkDescriptorBindingFlags> pathTraceBindingFlags(pathTraceBindings.size(), 0);
    const BindlessCapabilities& bindless = context_.bindlessCapabilities();
    if (bindless.partiallyBound || bindless.updateAfterBind) {
        for (size_t i = 0; i < pathTraceBindings.size(); ++i) {
            if (pathTraceBindings[i].binding == 19 && pathTraceBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                if (bindless.partiallyBound) {
                    pathTraceBindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
                }
                if (bindless.updateAfterBind) {
                    pathTraceBindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                }
            }
        }
    }
    const VkDescriptorSetLayoutCreateFlags pathTraceLayoutFlags =
        bindless.updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    pathTraceSetLayout_ = layoutCache_->createLayout(
        std::move(pathTraceBindings),
        pathTraceLayoutFlags,
        std::move(pathTraceBindingFlags));
    denoiserSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({denoiserShader_->reflection()}, 0));
    graphicsSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({
        fullscreenVertexShader_->reflection(),
        fullscreenFragmentShader_->reflection(),
    }, 0));
    if (activeBackend_ == RendererBackend::HardwareRayTracing) {
        std::vector<VkDescriptorBindingFlags> rtBindingFlags(rayTracingBindings().size(), 0);
        const auto rtBindings = rayTracingBindings();
        const BindlessCapabilities& rtBindless = context_.bindlessCapabilities();
        for (size_t i = 0; i < rtBindings.size(); ++i) {
            if ((rtBindings[i].binding == 19 || rtBindings[i].binding == 23) && rtBindless.partiallyBound) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            if (rtBindings[i].binding == 19 && rtBindless.updateAfterBind) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            }
        }
        const VkDescriptorSetLayoutCreateFlags rtLayoutFlags =
            rtBindless.updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        rayTracingSetLayout_ = layoutCache_->createLayout(rayTracingBindings(), rtLayoutFlags, std::move(rtBindingFlags));
    }

    pathTracePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *pathTraceShader_,
        std::vector<VkDescriptorSetLayout>{pathTraceSetLayout_},
        ShaderReflection::mergePushConstants({pathTraceShader_->reflection()}),
        *pipelineCache_);
    denoiserPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *denoiserShader_,
        std::vector<VkDescriptorSetLayout>{denoiserSetLayout_},
        ShaderReflection::mergePushConstants({denoiserShader_->reflection()}),
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
    if (activeBackend_ == RendererBackend::HardwareRayTracing) {
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
            std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_},
            *pipelineCache_,
            allocator_,
            uploader_);
        std::cout << "RT pipeline: SBT=" << rayTracingPipeline_->sbtBytes() << " bytes\n";
    }

    frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));
    frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));
    profilers_.emplace_back(context_.device(), context_.physicalDevice());
    profilers_.emplace_back(context_.device(), context_.physicalDevice());

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
    settings_.debugView = debugView;
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.scale = settings_.debugScale;
    if (debugView != RendererDebugView::Beauty) {
        std::cout << "Renderer debug view: " << rendererDebugViewName(debugView) << '\n';
    }
}

PathTracerRenderer::~PathTracerRenderer() {
    if (fullscreenSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), fullscreenSampler_, nullptr);
    }
}

void PathTracerRenderer::beginFrame(uint32_t frameIndex, VkExtent2D extent) {
    currentFrame_ = frames_.at(frameIndex % frames_.size()).get();
    currentProfiler_ = &profilers_.at(frameIndex % profilers_.size());
    currentProfiler_->collectCompletedFrame();
    validationLog_.beginFrame(frameCount_);
    if (frameCount_ > 0 && frameCount_ % 120u == 0u) {
        const GpuFrameTimings& timings = currentProfiler_->timings();
        std::cout << "GPU timings: path=" << timings.pathTraceMs
                  << " ms, denoise=" << timings.denoiserMs
                  << " ms, fullscreen=" << timings.fullscreenMs << " ms\n";
    }
    currentFrame_->beginFrame();
    if (extent.width != extent_.width || extent.height != extent_.height || rawImage_.handle() == VK_NULL_HANDLE) {
        if (rawImage_.handle() != VK_NULL_HANDLE) {
            checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(resize path tracer)");
        }
        createResolutionResources(extent);
        resetAccumulation(AccumulationResetReason::Resize);
    }
    ++frameCount_;
    updateCamera();
}

bool PathTracerRenderer::applySettings(const RendererSettings& settings) {
    RendererSettings next = settings;
    next.maxBounces = std::clamp(next.maxBounces, 1u, 16u);
    next.atrousIterations = std::clamp(next.atrousIterations, 1u, 5u);
    next.environmentDirectSamples = std::clamp(next.environmentDirectSamples, 1u, 8u);
    next.denoiserStrength = std::max(0.05f, next.denoiserStrength);
    next.sunIntensity = std::max(0.0f, next.sunIntensity);
    next.skyIntensity = std::max(0.0f, next.skyIntensity);
    next.exposure = std::max(0.05f, next.exposure);
    next.sunAngularRadius = std::max(0.0f, next.sunAngularRadius);
    next.indirectStrength = std::max(0.0f, next.indirectStrength);
    next.environmentIntensity = std::max(0.0f, next.environmentIntensity);
    next.environmentBackgroundIntensity = std::max(0.0f, next.environmentBackgroundIntensity);
    next.renderResolutionScale = std::clamp(next.renderResolutionScale, 0.25f, 1.0f);
    next.debugScale = std::max(0.05f, next.debugScale);

    const bool changed =
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.sunlightEnabled != settings_.sunlightEnabled ||
        next.directLightingEnabled != settings_.directLightingEnabled ||
        next.environmentEnabled != settings_.environmentEnabled ||
        next.maxBounces != settings_.maxBounces ||
        next.atrousIterations != settings_.atrousIterations ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        next.debugView != settings_.debugView ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f ||
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.exposure - settings_.exposure) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f ||
        std::abs(next.environmentIntensity - settings_.environmentIntensity) > 0.0001f ||
        std::abs(next.environmentRotation - settings_.environmentRotation) > 0.0001f ||
        std::abs(next.environmentBackgroundIntensity - settings_.environmentBackgroundIntensity) > 0.0001f ||
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f ||
        next.requestedBackend != settings_.requestedBackend ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f;
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
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f;
    const bool denoiserChanged =
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.atrousIterations != settings_.atrousIterations ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f;
    const bool debugChanged =
        next.debugView != settings_.debugView ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f;
    const bool renderChanged =
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.maxBounces != settings_.maxBounces ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        std::abs(next.exposure - settings_.exposure) > 0.0001f ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f;
    const bool renderResolutionChanged =
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f;

    settings_ = next;
    const bool environmentUploaded = scene_.setEnvironmentControls(
        settings_.environmentEnabled,
        settings_.environmentIntensity,
        settings_.environmentRotation,
        settings_.environmentBackgroundIntensity);
    if (renderResolutionChanged) {
        resetAccumulation(AccumulationResetReason::Resize);
    } else if (environmentChanged || environmentUploaded) {
        resetAccumulation(AccumulationResetReason::EnvironmentChanged);
    } else if (lightingChanged) {
        resetAccumulation(AccumulationResetReason::LightingChanged);
    } else if (renderChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (denoiserChanged) {
        resetAccumulation(AccumulationResetReason::DenoiserSettingsChanged);
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
    validationLog_.recordAccumulationInvalidation(accumulationResetReasonName(reason), frameCount_);
    frameCount_ = 0;
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

const GpuFrameTimings& PathTracerRenderer::timings() const {
    static const GpuFrameTimings empty{};
    if (currentProfiler_ != nullptr) {
        return currentProfiler_->timings();
    }
    return profilers_.empty() ? empty : profilers_.front().timings();
}

bool PathTracerRenderer::hardwareRayTracingAvailable() const {
    return context_.supportsHardwareRayTracing();
}

RayTracingRendererStats PathTracerRenderer::rayTracingStats() const {
    RayTracingRendererStats stats{};
    stats.active = activeBackend_ == RendererBackend::HardwareRayTracing && rayTracingScene_ != nullptr && rayTracingPipeline_ != nullptr;
    if (!stats.active) {
        return stats;
    }
    stats.blasCount = rayTracingScene_->blasCount();
    stats.instanceCount = rayTracingScene_->instanceCount();
    stats.accelerationStructureBytes = rayTracingScene_->accelerationStructureBytes();
    stats.sbtBytes = rayTracingPipeline_->sbtBytes();
    return stats;
}

VkDescriptorImageInfo PathTracerRenderer::viewportImageDescriptor() const {
    if (denoisedImage_.handle() == VK_NULL_HANDLE) {
        return {};
    }
    return denoisedImage_.sampledDescriptor(VK_NULL_HANDLE);
}

void PathTracerRenderer::createResolutionResources(VkExtent2D extent) {
    extent_ = extent;
    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(extent.width) * extent.height;
    rawImage_.create(allocator_, ImageDesc{
        .width = extent.width,
        .height = extent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer raw hdr",
    });
    denoisedImage_.create(allocator_, ImageDesc{
        .width = extent.width,
        .height = extent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoised hdr",
    });
    historyImage_.create(allocator_, ImageDesc{
        .width = extent.width,
        .height = extent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoiser history",
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
        .size = pixelCount * sizeof(uint32_t) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path depth normal packed",
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
}

void PathTracerRenderer::updateCamera() {
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.scale = settings_.debugScale;

    camera_.sunIntensity = settings_.sunIntensity;
    camera_.skyIntensity = settings_.skyIntensity;
    camera_.exposure = settings_.exposure;
    camera_.pathTracingEnabled = settings_.pathTracingEnabled ? 1u : 0u;
    camera_.maxBounces = settings_.maxBounces;
    camera_.sunlightEnabled = settings_.sunlightEnabled ? 1u : 0u;
    camera_.directLightingEnabled = settings_.directLightingEnabled ? 1u : 0u;
    camera_.sunAngularRadius = settings_.sunAngularRadius;
    camera_.indirectStrength = settings_.indirectStrength;
    camera_.environmentDirectSamples = settings_.environmentDirectSamples;
    camera_.frameCount = frameCount_;
    cameraBuffer_.write(&camera_, sizeof(camera_));
    cameraBuffer_.flush(sizeof(camera_));

    const float aspect = extent_.height > 0 ? static_cast<float>(extent_.width) / static_cast<float>(extent_.height) : 1.0f;
    const glm::vec3 eye = glm::vec3(camera_.pos);
    const glm::vec3 center = eye + glm::normalize(glm::vec3(camera_.forward));
    const glm::mat4 view = glm::lookAtRH(eye, center, glm::normalize(glm::vec3(camera_.up)));
    glm::mat4 projection = glm::perspectiveRH_ZO(camera_.fovY, aspect, 0.01f, 1000.0f);
    projection[1][1] *= -1.0f;
    const glm::mat4 viewProj = projection * view;

    prevCamera_.viewProj = viewProj;
    prevCamera_.invViewProj = glm::inverse(viewProj);
    prevCamera_.prevViewProj = previousViewProj_;
    prevCamera_.currentPos = camera_.pos;
    prevCamera_.prevPos = previousCameraPos_;
    prevCameraBuffer_.write(&prevCamera_, sizeof(prevCamera_));
    prevCameraBuffer_.flush(sizeof(prevCamera_));

    const bool allowDenoiserForDebugView = debugParams_.view <= 4u;
    const bool allowDenoiserWhileMoving = settings_.denoiseWhileMoving || !cameraChangedThisFrame_;
    denoiserParams_.enabled = settings_.pathTracingEnabled && settings_.denoiserEnabled && allowDenoiserForDebugView && allowDenoiserWhileMoving ? 1u : 0u;
    denoiserParams_.strength = settings_.denoiserStrength;
    denoiserParams_.frameCount = frameCount_;
    denoiserParams_.width = extent_.width;
    denoiserParams_.height = extent_.height;
    denoiserParams_.atrousIterations = settings_.atrousIterations;
    denoiserParams_.debugView = debugParams_.view <= 4u ? debugParams_.view : 0u;
    denoiserParamsBuffer_.write(&denoiserParams_, sizeof(denoiserParams_));
    denoiserParamsBuffer_.flush(sizeof(denoiserParams_));

    debugParamsBuffer_.write(&debugParams_, sizeof(debugParams_));
    debugParamsBuffer_.flush(sizeof(debugParams_));

    previousViewProj_ = viewProj;
    previousCameraPos_ = camera_.pos;
}

void PathTracerRenderer::recordPathTrace(VkCommandBuffer commandBuffer) {
    const VkPipelineStageFlags2 traceStage = pathTraceShaderStage();
    validationLog_.recordPass(activeBackend_ == RendererBackend::HardwareRayTracing ? "path tracing rt" : "path tracing compute");
    currentProfiler_->resetForFrame(commandBuffer);
    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceStart, traceStage);
    validationLog_.recordBarrier(
        "raw image -> path trace storage write",
        rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : traceStage,
        rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        traceStage,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawImage_.handle(),
        .oldLayout = rawImage_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = rawImage_.fullRange(),
        .srcStage = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : traceStage,
        .srcAccess = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .dstStage = traceStage,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    if (activeBackend_ == RendererBackend::HardwareRayTracing) {
        recordHardwarePathTrace(commandBuffer);
        currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceEnd, traceStage);
        if (shouldRunDenoiser()) {
            recordDenoiser(commandBuffer);
            copyHistoryResources(commandBuffer);
        } else {
            skipDenoiserPass(commandBuffer);
        }
        cameraChangedThisFrame_ = false;
        currentProfiler_->markSubmitted();
        return;
    }

    DescriptorSet set = currentFrame_->descriptors().allocate(pathTraceSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, accumulationBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cameraBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.vertices().descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.indices().descriptorInfo())
        .writeBuffer(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.bvhNodes().descriptorInfo())
        .writeBuffer(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.triangles().descriptorInfo())
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.materials().descriptorInfo())
        .writeBuffer(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.meshParamsBuffer().descriptorInfo())
        .writeImage(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, scene_.environmentImage().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(13, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = scene_.environmentSampler()})
        .writeBuffer(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envRows().descriptorInfo())
        .writeBuffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envCols().descriptorInfo())
        .writeBuffer(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.envParamsBuffer().descriptorInfo())
        .writeBuffer(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.spheres().descriptorInfo())
        .writeBuffer(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, debugParamsBuffer_.descriptorInfo())
        .writeImageArray(19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, scene_.materialTextureDescriptors())
        .writeBuffer(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.primitiveRecords().descriptorInfo())
        .writeBuffer(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.instanceRecords().descriptorInfo())
        .writeImageArray(23, VK_DESCRIPTOR_TYPE_SAMPLER, scene_.materialSamplerDescriptors())
        .writeBuffer(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightRecords().descriptorInfo())
        .writeBuffer(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.meshRecords().descriptorInfo())
        .writeBuffer(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localVertices().descriptorInfo())
        .writeBuffer(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localIndices().descriptorInfo())
        .writeBuffer(28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.instanceBounds().descriptorInfo())
        .writeBuffer(29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localBvhNodes().descriptorInfo())
        .writeBuffer(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localTriangles().descriptorInfo())
        .writeBuffer(31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.tlasNodes().descriptorInfo())
        .writeBuffer(32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.tlasInstanceIndices().descriptorInfo())
        .update(context_.device(), set);

    pathTracePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pathTracePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    pathTracePipeline_->dispatch(commandBuffer, extent_.width, extent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceEnd, traceStage);

    if (shouldRunDenoiser()) {
        recordDenoiser(commandBuffer);
        copyHistoryResources(commandBuffer);
    } else {
        skipDenoiserPass(commandBuffer);
    }
    cameraChangedThisFrame_ = false;
    currentProfiler_->markSubmitted();
}

void PathTracerRenderer::recordHardwarePathTrace(VkCommandBuffer commandBuffer) {
    if (rayTracingPipeline_ == nullptr || rayTracingScene_ == nullptr) {
        throw std::runtime_error("Hardware ray tracing backend is active but RT pipeline/scene is not initialized");
    }

    DescriptorSet set = currentFrame_->descriptors().allocate(rayTracingSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, accumulationBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cameraBuffer_.descriptorInfo())
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
        .writeBuffer(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, debugParamsBuffer_.descriptorInfo())
        .writeImageArray(19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, scene_.materialTextureDescriptors())
        .writeBuffer(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.primitiveRecords().descriptorInfo())
        .writeBuffer(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.instanceRecords().descriptorInfo())
        .writeImageArray(23, VK_DESCRIPTOR_TYPE_SAMPLER, scene_.materialSamplerDescriptors())
        .writeBuffer(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightRecords().descriptorInfo())
        .writeBuffer(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.meshRecords().descriptorInfo())
        .writeBuffer(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localVertices().descriptorInfo())
        .writeBuffer(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localIndices().descriptorInfo())
        .writeBuffer(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localTriangles().descriptorInfo())
        .writeAccelerationStructure(33, rayTracingScene_->tlas())
        .writeBuffer(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.rtTriangleMaterialIds().descriptorInfo())
        .update(context_.device(), set);

    rayTracingPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    rayTracingPipeline_->traceRays(commandBuffer, extent_.width, extent_.height);
}

void PathTracerRenderer::recordComputePathTrace(VkCommandBuffer) {
}

VkPipelineStageFlags2 PathTracerRenderer::pathTraceShaderStage() const {
    return activeBackend_ == RendererBackend::HardwareRayTracing
        ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
        : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
}

void PathTracerRenderer::recordDenoiser(VkCommandBuffer commandBuffer) {
    const VkPipelineStageFlags2 traceStage = pathTraceShaderStage();
    validationLog_.recordPass("temporal denoiser compute");
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    validationLog_.recordBarrier(
        "raw image path trace write -> denoiser read",
        traceStage,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = rawImage_.fullRange(),
        .srcStage = traceStage,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = denoisedImage_.handle(),
        .oldLayout = denoisedImage_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = denoisedImage_.fullRange(),
        .srcStage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccess = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = historyImage_.handle(),
        .oldLayout = historyImage_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = historyImage_.fullRange(),
        .srcStage = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccess = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    barrier::cmdBufferBarrier(commandBuffer, {
        .buffer = depthNormalBuffer_.handle(),
        .size = depthNormalBuffer_.size(),
        .srcStage = traceStage,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
    barrier::cmdBufferBarrier(commandBuffer, {
        .buffer = varianceBuffer_.handle(),
        .size = varianceBuffer_.size(),
        .srcStage = traceStage,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
    barrier::cmdBufferBarrier(commandBuffer, {
        .buffer = worldPositionBuffer_.handle(),
        .size = worldPositionBuffer_.size(),
        .srcStage = traceStage,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });

    DescriptorSet set = currentFrame_->descriptors().allocate(denoiserSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyImage_.storageDescriptor())
        .writeImage(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, denoisedImage_.storageDescriptor())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, denoiserParamsBuffer_.descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousWorldPositionBuffer_.descriptorInfo())
        .writeBuffer(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, prevCameraBuffer_.descriptorInfo())
        .update(context_.device(), set);

    denoiserPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, denoiserPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    denoiserPipeline_->dispatch(commandBuffer, extent_.width, extent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::copyHistoryResources(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("history copy");
    barrier::cmdTransitionImage(commandBuffer, {
        .image = denoisedImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .range = denoisedImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = historyImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .range = historyImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });
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

    barrier::cmdTransitionImage(commandBuffer, {
        .image = denoisedImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = denoisedImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = historyImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = historyImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    barrier::cmdBufferBarrier(commandBuffer, {
        .buffer = worldPositionBuffer_.handle(),
        .size = worldPositionBuffer_.size(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    VkBufferCopy copy{};
    copy.size = worldPositionBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, worldPositionBuffer_.handle(), previousWorldPositionBuffer_.handle(), 1, &copy);
    barrier::cmdBufferBarrier(commandBuffer, {
        .buffer = previousWorldPositionBuffer_.handle(),
        .size = previousWorldPositionBuffer_.size(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    });
}

bool PathTracerRenderer::shouldRunDenoiser() const {
    if (denoiserParams_.enabled != 0u) {
        return true;
    }
    if (denoiserParams_.debugView >= 1u && denoiserParams_.debugView <= 4u) {
        return true;
    }
    return false;
}

void PathTracerRenderer::skipDenoiserPass(VkCommandBuffer commandBuffer) {
    const VkPipelineStageFlags2 traceStage = pathTraceShaderStage();
    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .range = rawImage_.fullRange(),
        .srcStage = traceStage,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    rawImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = denoisedImage_.handle(),
        .oldLayout = denoisedImage_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .range = denoisedImage_.fullRange(),
        .srcStage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccess = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });
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

    barrier::cmdTransitionImage(commandBuffer, {
        .image = denoisedImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = denoisedImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawImage_.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = rawImage_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStage = traceStage,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void PathTracerRenderer::recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent) {
    validationLog_.recordPass("fullscreen presentation");
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenStart, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    DescriptorSet set = currentFrame_->descriptors().allocate(graphicsSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, denoisedImage_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .update(context_.device(), set);

    graphicsPipeline_->bind(commandBuffer, swapchainExtent);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const FullscreenParams fullscreenParams{
        .exposure = settings_.exposure,
        .debugView = static_cast<uint32_t>(settings_.debugView),
    };
    vkCmdPushConstants(
        commandBuffer,
        graphicsPipeline_->layout(),
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(fullscreenParams),
        &fullscreenParams);
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
