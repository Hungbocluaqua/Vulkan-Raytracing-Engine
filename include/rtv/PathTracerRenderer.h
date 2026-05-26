#pragma once

#include "rtv/FrameResources.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/GpuScene.h"
#include "rtv/Image.h"
#include "rtv/PhysicalCamera.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtv {

class BufferUploader;
class ComputePipeline;
class DescriptorLayoutCache;
class GraphicsPipeline;
class PipelineCache;
class RayTracingPipeline;
class RayTracingScene;
class ResourceAllocator;
class ShaderCompiler;
class ShaderModule;
class TemporalSystem;
class VulkanContext;
class AssetManager;
class AtmosphereLutSystem;
class PhysicalCamera;
struct AtmosphereLutStats;
struct SceneAsset;

struct RayTracingRendererStats {
    bool active = false;
    uint32_t blasCount = 0;
    uint32_t instanceCount = 0;
    VkDeviceSize accelerationStructureBytes = 0;
    VkDeviceSize sbtBytes = 0;
    float lastTlasRefitMs = 0.0f;
};

enum class AccumulationResetReason : uint32_t {
    Startup,
    Resize,
    CameraMoved,
    Manual,
    RenderSettingsChanged,
    LightingChanged,
    EnvironmentChanged,
    DenoiserSettingsChanged,
    DebugViewChanged,
    SceneChanged,
    MaterialChanged,
    ShaderReloaded,
};

[[nodiscard]] const char* accumulationResetReasonName(AccumulationResetReason reason);

class PathTracerRenderer {
public:
    PathTracerRenderer(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        VkFormat swapchainFormat,
        const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& shaderOutputDirectory,
        RendererDebugView debugView = RendererDebugView::Beauty,
        const SceneAsset* importedScene = nullptr,
        const AssetManager* assets = nullptr,
        std::optional<std::filesystem::path> environmentPath = std::nullopt,
        std::optional<std::filesystem::path> sceneCachePath = std::nullopt);
    ~PathTracerRenderer();

    void beginFrame(uint32_t frameIndex, VkExtent2D renderExtent, VkExtent2D displayExtent);
    void setFrameDeltaSeconds(float deltaSeconds) { frameDeltaSeconds_ = deltaSeconds; }
    void recordPathTrace(VkCommandBuffer commandBuffer);
    void recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent);
    void recordEditorPresentationStart(VkCommandBuffer commandBuffer);
    void recordEditorPresentationEnd(VkCommandBuffer commandBuffer);

    bool applySettings(const RendererSettings& settings);
    void setCameraPose(glm::vec3 position, glm::vec3 forward);
    void setCameraFovY(float fovY);
    void resetAccumulation(AccumulationResetReason reason = AccumulationResetReason::Manual);
    void loadEnvironment(const std::filesystem::path& path);
    [[nodiscard]] bool shadersNeedReload();
    bool updateMaterials(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneLights(const SceneAsset& scene);
    bool updateSceneTransforms(const SceneAsset& scene, const AssetManager& assets);
    bool updateSceneVisibility(const SceneAsset& scene, const AssetManager& assets);
    void setSelectedInstanceId(std::optional<uint32_t> instanceId);
    [[nodiscard]] std::optional<uint32_t> pickInstanceId(glm::vec2 viewportUv);

    [[nodiscard]] const RendererSettings& settings() const { return settings_; }
    [[nodiscard]] bool hardwareRayTracingAvailable() const;
    [[nodiscard]] RayTracingRendererStats rayTracingStats() const;
    [[nodiscard]] uint32_t sampleCount() const { return frameCount_; }
    [[nodiscard]] const GpuFrameTimings& timings() const;
    [[nodiscard]] GpuPipelineStatistics pipelineStats() const;
    [[nodiscard]] AccumulationResetReason lastAccumulationResetReason() const { return lastResetReason_; }
    [[nodiscard]] const RendererValidationLog& validationLog() const { return validationLog_; }
    [[nodiscard]] RendererValidationLog& validationLog() { return validationLog_; }
    [[nodiscard]] const TemporalSystem* temporalSystem() const { return temporalSystem_.get(); }
    [[nodiscard]] AtmosphereLutStats atmosphereLutStats() const;
    [[nodiscard]] const GpuScene& scene() const { return scene_; }
    [[nodiscard]] VkDescriptorImageInfo viewportImageDescriptor() const;
    [[nodiscard]] VkExtent2D renderExtent() const { return renderExtent_; }
    [[nodiscard]] VkExtent2D displayExtent() const { return displayExtent_; }

private:
    struct DenoiserParams {
        uint32_t enabled = 1;
        float strength = 1.0f;
        uint32_t frameCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t atrousIterations = 4;
        uint32_t debugView = 0;
        uint32_t resetHistory = 1;
    };

    struct PrevCameraUniform {
        glm::mat4 viewProj{1.0f};
        glm::mat4 invViewProj{1.0f};
        glm::mat4 prevViewProj{1.0f};
        glm::vec4 currentPos{};
        glm::vec4 prevPos{};
        glm::vec4 jitter{}; // xy = current subpixel jitter, zw = previous subpixel jitter
    };

    struct ToneMapParams {
        uint32_t toneMapper = static_cast<uint32_t>(ToneMapper::ACES);
        uint32_t debugView = 0;
        uint32_t autoExposureEnabled = 0;
        float exposure = 2.0f;
        float gamma = 2.2f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float brightness = 0.0f;
        float whitePoint = 4.0f;
    };

    struct HistogramParams {
        uint32_t width = 0;
        uint32_t height = 0;
        float minLogLuminance = -10.0f;
        float maxLogLuminance = 10.0f;
    };

    struct ExposureReduceParams {
        uint32_t pixelCount = 0;
        float targetLuminance = 0.18f;
        float minExposure = 0.25f;
        float maxExposure = 8.0f;
        float adaptationSpeed = 2.0f;
        float lowPercentile = 0.05f;
        float highPercentile = 0.95f;
        float targetPercentile = 0.60f;
        float deltaSeconds = 0.0f;
        float minLogLuminance = -10.0f;
        float maxLogLuminance = 10.0f;
    };

    struct SelectionParams {
        uint32_t selectedInstance = UINT32_MAX;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t enabled = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
    };

    struct TaaParams {
        uint32_t enabled = 1;
        uint32_t frameCount = 0;
        uint32_t width = 0; // display width
        uint32_t height = 0; // display height
        float feedback = 0.08f;
        float velocityScale = 64.0f;
        uint32_t resetHistory = 1;
        float sharpeningStrength = 0.08f;
        uint32_t historyValid = 0;
        uint32_t cameraMoving = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
    };

    struct RestirSpatialParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameCount = 0;
        uint32_t enabled = 0;
    };

    struct FogParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t debugView = 0;
        uint32_t enabled = 1;
        float density = 0.000035f;
        float heightFalloff = 1200.0f;
        float maxDistance = 10000.0f;
        float padding = 0.0f;
    };

    struct RestirReservoirGpu {
        glm::uvec4 metadata{};
        glm::vec4 sampleValueConfidence{};
        glm::vec4 targetPdfWeightSumM{};
    };

    struct PathDataGpu {
        glm::vec4 directDiffuse{};
        glm::vec4 directSpecular{};
        glm::vec4 indirectDiffuse{};
        glm::vec4 indirectSpecular{};
        glm::vec4 albedoRoughnessHitConfidence{};
    };

    void createResolutionResources(VkExtent2D renderExtent, VkExtent2D displayExtent);
    void updateCamera();
    void recordPathTraceGraph(VkCommandBuffer commandBuffer);
    void recordPathTracePass(VkCommandBuffer commandBuffer);
    void recordRestirSpatial(VkCommandBuffer commandBuffer);
    void recordRestirSpatialPass(VkCommandBuffer commandBuffer);
    void recordRestirSpatialCopyPass(VkCommandBuffer commandBuffer);
    void recordHeightFog(VkCommandBuffer commandBuffer);
    void recordHeightFogPass(VkCommandBuffer commandBuffer);
    void recordDenoiser(VkCommandBuffer commandBuffer);
    void recordDenoiserPass(VkCommandBuffer commandBuffer);
    void recordTaa(VkCommandBuffer commandBuffer);
    void recordTaaPass(VkCommandBuffer commandBuffer);
    void recordTaaHistoryCopyPass(VkCommandBuffer commandBuffer);
    void recordAutoExposure(VkCommandBuffer commandBuffer);
    void recordAutoExposureHistogramPass(VkCommandBuffer commandBuffer);
    void recordAutoExposureReducePass(VkCommandBuffer commandBuffer);
    void recordToneMap(VkCommandBuffer commandBuffer);
    void recordToneMapPass(VkCommandBuffer commandBuffer);
    void recordSelectionOutline(VkCommandBuffer commandBuffer);
    void recordSelectionOutlinePass(VkCommandBuffer commandBuffer);
    void recordRenderGraphPlan();
    void updateAdaptiveQuality(const GpuFrameTimings& timings);
    void copyHistoryResources(VkCommandBuffer commandBuffer);
    void copyHistoryResourcesPass(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool shouldRunDenoiser() const;
    [[nodiscard]] bool shouldRunTaa() const;
    [[nodiscard]] bool shouldRunRestirSpatial() const;
    [[nodiscard]] const Image& postDenoiseImage() const;
    [[nodiscard]] const Image& hdrPostProcessImage() const;
    void skipDenoiserPass(VkCommandBuffer commandBuffer);
    void skipDenoiserCopyPass(VkCommandBuffer commandBuffer);
    void recordHardwarePathTrace(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkPipelineStageFlags2 pathTraceShaderStage() const;

    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    BufferUploader& uploader_;
    GpuScene scene_;

    VkExtent2D renderExtent_{};
    VkExtent2D displayExtent_{};
    uint32_t frameCount_ = 0;
    uint32_t temporalFrameIndex_ = 0;
    uint32_t stillFrameCount_ = 0;
    float frameDeltaSeconds_ = 0.0f;
    bool cameraChangedThisFrame_ = false;
    float adaptiveSmoothedGpuMs_ = 0.0f;
    uint32_t adaptiveQualityTier_ = 0;
    uint32_t adaptiveOverBudgetFrames_ = 0;
    uint32_t adaptiveEffectiveMaxBounces_ = 8;
    uint32_t adaptiveEffectiveEnvironmentSamples_ = 1;
    uint32_t adaptiveEffectiveAtrousIterations_ = 4;
    bool adaptiveSkipRestirSpatial_ = false;
    bool adaptiveSkipDenoiser_ = false;
    AccumulationResetReason lastResetReason_ = AccumulationResetReason::Startup;
    CameraUniform camera_{};
    RendererSettings settings_{};
    DenoiserParams denoiserParams_{};
    TaaParams taaParams_{};
    RestirSpatialParams restirSpatialParams_{};
    FogParams fogParams_{};
    PrevCameraUniform prevCamera_{};
    RendererDebugParams debugParams_{};
    glm::mat4 previousViewProj_{1.0f};
    glm::vec4 previousCameraPos_{};
    glm::vec2 previousJitter_{0.0f};
    bool taaHistoryValid_ = false;

    Image rawImage_;
    Image denoisedImage_;
    Image historyImage_;
    Image taaImage_;
    Image taaHistoryImage_;
    Image presentationImage_;
    Buffer cameraBuffer_;
    Buffer denoiserParamsBuffer_;
    Buffer prevCameraBuffer_;
    Buffer debugParamsBuffer_;
    Buffer accumulationBuffer_;
    Buffer varianceBuffer_;
    Buffer depthNormalBuffer_;
    Buffer worldPositionBuffer_;
    Buffer previousWorldPositionBuffer_;
    Buffer velocityBuffer_;
    Buffer entityIdBuffer_;
    Buffer pathDataBuffer_;
    Buffer restirReservoirBuffer_;
    Buffer previousRestirReservoirBuffer_;
    Buffer restirSpatialReservoirBuffer_;
    Buffer selectionParamsBuffer_;
    Buffer histogramBuffer_;
    Buffer exposureBuffer_;

    VkSampler fullscreenSampler_ = VK_NULL_HANDLE;
    std::unique_ptr<DescriptorLayoutCache> layoutCache_;
    std::unique_ptr<PipelineCache> pipelineCache_;
    std::unique_ptr<AtmosphereLutSystem> atmosphereLutSystem_;
    std::unique_ptr<ShaderModule> denoiserShader_;
    std::unique_ptr<ShaderModule> taaShader_;
    std::unique_ptr<ShaderModule> restirSpatialShader_;
    std::unique_ptr<ShaderModule> fogShader_;
    std::unique_ptr<ShaderModule> transmittanceShader_;
    std::unique_ptr<ShaderModule> multiScatterShader_;
    std::unique_ptr<ShaderModule> skyViewShader_;
    std::unique_ptr<ShaderModule> skyReprojectShader_;
    std::unique_ptr<ShaderModule> aerialPerspectiveShader_;
    std::unique_ptr<ShaderModule> skyCdfShader_;
    std::unique_ptr<ShaderModule> selectionOutlineShader_;
    std::unique_ptr<ShaderModule> luminanceHistogramShader_;
    std::unique_ptr<ShaderModule> exposureReduceShader_;
    std::unique_ptr<ShaderModule> toneMapShader_;
    std::unique_ptr<ShaderModule> fullscreenVertexShader_;
    std::unique_ptr<ShaderModule> fullscreenFragmentShader_;
    std::unique_ptr<ShaderModule> raygenShader_;
    std::unique_ptr<ShaderModule> primaryMissShader_;
    std::unique_ptr<ShaderModule> shadowMissShader_;
    std::unique_ptr<ShaderModule> closestHitShader_;
    std::unique_ptr<ShaderModule> primaryAnyHitShader_;
    std::unique_ptr<ShaderModule> shadowAnyHitShader_;
    std::unique_ptr<ComputePipeline> denoiserPipeline_;
    std::unique_ptr<ComputePipeline> taaPipeline_;
    std::unique_ptr<ComputePipeline> restirSpatialPipeline_;
    std::unique_ptr<ComputePipeline> fogPipeline_;
    std::unique_ptr<ComputePipeline> selectionOutlinePipeline_;
    std::unique_ptr<ComputePipeline> luminanceHistogramPipeline_;
    std::unique_ptr<ComputePipeline> exposureReducePipeline_;
    std::unique_ptr<ComputePipeline> toneMapPipeline_;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline_;
    std::unique_ptr<RayTracingPipeline> rayTracingPipeline_;
    std::unique_ptr<RayTracingScene> rayTracingScene_;
    std::unique_ptr<TemporalSystem> temporalSystem_;
    PhysicalCamera physicalCamera_;
    VkDescriptorSetLayout atmosphereSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rayTracingSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout denoiserSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout taaSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout restirSpatialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout fogSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout selectionOutlineSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout luminanceHistogramSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout exposureReduceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout toneMapSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::vector<GpuProfiler> profilers_;
    FrameResources* currentFrame_ = nullptr;
    GpuProfiler* currentProfiler_ = nullptr;
    RendererValidationLog validationLog_;
    std::unique_ptr<ShaderCompiler> shaderCompiler_;
    std::vector<std::filesystem::path> shaderSources_;
    std::filesystem::path shaderOutputDirectory_;
    uint32_t selectedInstanceId_ = UINT32_MAX;
};

} // namespace rtv
