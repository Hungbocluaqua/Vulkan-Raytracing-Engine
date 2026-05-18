#pragma once

#include "rtv/FrameResources.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/GpuScene.h"
#include "rtv/Image.h"
#include "rtv/RendererBackend.h"
#include "rtv/RendererDebug.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
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
class ShaderModule;
class VulkanContext;
class AssetManager;
struct SceneAsset;

struct RendererSettings {
    bool pathTracingEnabled = true;
    bool denoiserEnabled = true;
    bool denoiseWhileMoving = false;
    bool sunlightEnabled = true;
    bool directLightingEnabled = true;
    bool environmentEnabled = true;
    uint32_t maxBounces = 8;
    uint32_t atrousIterations = 4;
    uint32_t environmentDirectSamples = 1;
    float denoiserStrength = 1.0f;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float exposure = 0.75f;
    float sunAngularRadius = 0.0093f;
    float indirectStrength = 1.0f;
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;
    float environmentBackgroundIntensity = 0.35f;
    float renderResolutionScale = 1.0f;
    RendererBackend requestedBackend = RendererBackend::Auto;
    RendererDebugView debugView = RendererDebugView::Beauty;
    float debugScale = 1.0f;
};

struct RayTracingRendererStats {
    bool active = false;
    uint32_t blasCount = 0;
    uint32_t instanceCount = 0;
    VkDeviceSize accelerationStructureBytes = 0;
    VkDeviceSize sbtBytes = 0;
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
    BackendChanged,
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
        std::optional<std::filesystem::path> sceneCachePath = std::nullopt,
        RendererBackend requestedBackend = RendererBackend::Auto);
    ~PathTracerRenderer();

    void beginFrame(uint32_t frameIndex, VkExtent2D extent);
    void recordPathTrace(VkCommandBuffer commandBuffer);
    void recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent);
    void recordEditorPresentationStart(VkCommandBuffer commandBuffer);
    void recordEditorPresentationEnd(VkCommandBuffer commandBuffer);

    bool applySettings(const RendererSettings& settings);
    void setCameraPose(glm::vec3 position, glm::vec3 forward);
    void setCameraFovY(float fovY);
    void resetAccumulation(AccumulationResetReason reason = AccumulationResetReason::Manual);
    void loadEnvironment(const std::filesystem::path& path);
    bool updateMaterials(const SceneAsset& scene, const AssetManager& assets);

    [[nodiscard]] const RendererSettings& settings() const { return settings_; }
    [[nodiscard]] RendererBackend requestedBackend() const { return requestedBackend_; }
    [[nodiscard]] RendererBackend activeBackend() const { return activeBackend_; }
    [[nodiscard]] bool hardwareRayTracingAvailable() const;
    [[nodiscard]] RayTracingRendererStats rayTracingStats() const;
    [[nodiscard]] uint32_t sampleCount() const { return frameCount_; }
    [[nodiscard]] const GpuFrameTimings& timings() const;
    [[nodiscard]] AccumulationResetReason lastAccumulationResetReason() const { return lastResetReason_; }
    [[nodiscard]] const RendererValidationLog& validationLog() const { return validationLog_; }
    [[nodiscard]] const GpuScene& scene() const { return scene_; }
    [[nodiscard]] VkDescriptorImageInfo viewportImageDescriptor() const;
    [[nodiscard]] VkExtent2D renderExtent() const { return extent_; }

private:
    struct DenoiserParams {
        uint32_t enabled = 1;
        float strength = 1.0f;
        uint32_t frameCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t atrousIterations = 4;
        uint32_t debugView = 0;
        uint32_t padding1 = 0;
    };

    struct PrevCameraUniform {
        glm::mat4 viewProj{1.0f};
        glm::mat4 invViewProj{1.0f};
        glm::mat4 prevViewProj{1.0f};
        glm::vec4 currentPos{};
        glm::vec4 prevPos{};
    };

    struct FullscreenParams {
        float exposure = 0.75f;
        uint32_t debugView = 0;
        float padding0 = 0.0f;
        float padding1 = 0.0f;
    };

    void createResolutionResources(VkExtent2D extent);
    void updateCamera();
    void recordDenoiser(VkCommandBuffer commandBuffer);
    void copyHistoryResources(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool shouldRunDenoiser() const;
    void skipDenoiserPass(VkCommandBuffer commandBuffer);
    void recordHardwarePathTrace(VkCommandBuffer commandBuffer);
    void recordComputePathTrace(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkPipelineStageFlags2 pathTraceShaderStage() const;

    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    BufferUploader& uploader_;
    GpuScene scene_;

    VkExtent2D extent_{};
    uint32_t frameCount_ = 0;
    bool cameraChangedThisFrame_ = false;
    AccumulationResetReason lastResetReason_ = AccumulationResetReason::Startup;
    CameraUniform camera_{};
    RendererSettings settings_{};
    DenoiserParams denoiserParams_{};
    PrevCameraUniform prevCamera_{};
    RendererDebugParams debugParams_{};
    RendererBackend requestedBackend_ = RendererBackend::Auto;
    RendererBackend activeBackend_ = RendererBackend::Compute;
    glm::mat4 previousViewProj_{1.0f};
    glm::vec4 previousCameraPos_{};

    Image rawImage_;
    Image denoisedImage_;
    Image historyImage_;
    Buffer cameraBuffer_;
    Buffer denoiserParamsBuffer_;
    Buffer prevCameraBuffer_;
    Buffer debugParamsBuffer_;
    Buffer accumulationBuffer_;
    Buffer varianceBuffer_;
    Buffer depthNormalBuffer_;
    Buffer worldPositionBuffer_;
    Buffer previousWorldPositionBuffer_;

    VkSampler fullscreenSampler_ = VK_NULL_HANDLE;
    std::unique_ptr<DescriptorLayoutCache> layoutCache_;
    std::unique_ptr<PipelineCache> pipelineCache_;
    std::unique_ptr<ShaderModule> pathTraceShader_;
    std::unique_ptr<ShaderModule> denoiserShader_;
    std::unique_ptr<ShaderModule> fullscreenVertexShader_;
    std::unique_ptr<ShaderModule> fullscreenFragmentShader_;
    std::unique_ptr<ShaderModule> raygenShader_;
    std::unique_ptr<ShaderModule> primaryMissShader_;
    std::unique_ptr<ShaderModule> shadowMissShader_;
    std::unique_ptr<ShaderModule> closestHitShader_;
    std::unique_ptr<ShaderModule> primaryAnyHitShader_;
    std::unique_ptr<ShaderModule> shadowAnyHitShader_;
    std::unique_ptr<ComputePipeline> pathTracePipeline_;
    std::unique_ptr<ComputePipeline> denoiserPipeline_;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline_;
    std::unique_ptr<RayTracingPipeline> rayTracingPipeline_;
    std::unique_ptr<RayTracingScene> rayTracingScene_;
    VkDescriptorSetLayout pathTraceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rayTracingSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout denoiserSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout graphicsSetLayout_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<FrameResources>> frames_;
    std::vector<GpuProfiler> profilers_;
    FrameResources* currentFrame_ = nullptr;
    GpuProfiler* currentProfiler_ = nullptr;
    RendererValidationLog validationLog_;
};

} // namespace rtv
