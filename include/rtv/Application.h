#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorPanels.h"
#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/SceneToGpuSceneBuilder.h"
#include "rtv/NotificationManager.h"
#include "rtv/UndoStack.h"

#include <memory>
#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

struct GLFWwindow;

namespace rtv {

class CommandSystem;
class BufferUploader;
class ResourceAllocator;
class ResourceDemo;
class PipelineDemo;
class PathTracerRenderer;
class Swapchain;

class Application final : private NonCopyable {
public:
    explicit Application(
        RendererDebugView debugView = RendererDebugView::Beauty,
        std::optional<std::filesystem::path> gltfPath = std::nullopt,
        std::optional<std::filesystem::path> hdrPath = std::nullopt,
        std::optional<std::filesystem::path> scenePath = std::nullopt,
        std::optional<bool> denoiserOverride = std::nullopt,
        std::optional<RestirMode> restirModeOverride = std::nullopt,
        bool debugViewOverride = false,
        bool validationCameraMotion = false);
    ~Application();

    void run(uint32_t maxFrames = 0);
    void onWindowFocusChanged(bool focused);
    void onFilesDropped(int count, const char** paths);

private:
    struct PendingSceneLoadResult {
        std::filesystem::path path;
        AssetManager assets;
        SceneAsset scene;
        std::string error;
    };

    void initWindow();
    void initVulkan();
    void mainLoop(uint32_t maxFrames);
    void applyValidationCameraMotion(uint32_t frameIndex);
    void processRuntimeControls(float deltaSeconds);
    void updateWindowTitle(float seconds);
    void toggleBorderlessFullscreen();
    void reloadGltfScene(const std::filesystem::path& path);
    void requestGltfSceneLoad(const std::filesystem::path& path);
    void pollAsyncSceneLoad();
    void commitLoadedGltfScene(PendingSceneLoadResult&& result);
    void applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild);
    bool applyPendingSceneUpdate(bool allowResourceRebuild);
    void applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange);
    void reloadShadersFromEditor();
    [[nodiscard]] std::unique_ptr<PathTracerRenderer> makePathTracer(
        const SceneAsset* sceneAsset,
        const AssetManager* assets,
        std::optional<std::filesystem::path> sceneCachePath,
        const RendererSettings* settingsToRestore);
    void createPathTracer(const RendererSettings* settingsToRestore = nullptr);
    void applyActiveSceneCamera();
    void rebuildGpuSceneAsset();
    void initializeFallbackSceneDocument();
    [[nodiscard]] bool pressedOnce(int key);

    GLFWwindow* window_ = nullptr;
    RendererDebugView debugView_ = RendererDebugView::Beauty;
    std::optional<std::filesystem::path> gltfPath_;
    std::optional<std::filesystem::path> hdrPath_;
    std::optional<std::filesystem::path> scenePath_;
    std::optional<bool> denoiserOverride_;
    std::optional<RestirMode> restirModeOverride_;
    bool debugViewOverride_ = false;
    bool validationCameraMotion_ = false;
    bool pendingOpenLevel_ = false;
    bool pendingSaveLevel_ = false;
    bool pendingReloadShaders_ = false;
    AssetManager assets_;
    CameraController cameraController_;
    std::array<unsigned char, 512> keyState_{};
    float lastFrameSeconds_ = 0.0f;
    float lastTitleUpdateSeconds_ = -1.0f;
    bool borderlessFullscreen_ = false;
    int windowedX_ = 100;
    int windowedY_ = 100;
    int windowedWidth_ = 1280;
    int windowedHeight_ = 720;
    std::optional<SceneAsset> importedScene_;
    SceneDocument sceneDocument_;
    SceneEventBus sceneEventBus_;
    NotificationManager notifications_;
    UndoStack undoStack_;
    SceneToGpuSceneBuilder sceneBuilder_;
    std::optional<SceneAsset> gpuSceneAsset_;
    std::vector<EntityId> gpuInstanceEntities_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<ResourceAllocator> allocator_;
    std::unique_ptr<UploadContext> uploadContext_;
    std::unique_ptr<BufferUploader> uploader_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<CommandSystem> commandSystem_;
    std::unique_ptr<UiOverlay> uiOverlay_;
    std::unique_ptr<ResourceDemo> resourceDemo_;
    std::unique_ptr<PipelineDemo> pipelineDemo_;
    std::unique_ptr<PathTracerRenderer> pathTracer_;
    std::optional<RendererSettings> pendingPostFrameSettings_;
    std::future<PendingSceneLoadResult> pendingSceneLoad_;
    std::optional<std::filesystem::path> pendingSceneLoadPath_;
    std::string sceneLoadingStatus_;
};

} // namespace rtv
