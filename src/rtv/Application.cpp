#include "rtv/Application.h"

#include "rtv/CommandSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/FileDialog.h"
#include "rtv/GltfLoader.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/PipelineDemo.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ResourceDemo.h"
#include "rtv/SceneOperations.h"
#include "rtv/SceneUpdateRouter.h"
#include "rtv/Swapchain.h"
#include "rtv/UiOverlay.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <Volk/volk.h>
#include <GLFW/glfw3.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace rtv {

namespace {
constexpr int initialWidth = 1280;
constexpr int initialHeight = 720;
constexpr uint64_t largeSceneTriangleThreshold = 1'000'000ull;

constexpr RendererDebugView intermediateViews[] = {
    RendererDebugView::Beauty,
    RendererDebugView::DirectLighting,
    RendererDebugView::IndirectLighting,
    RendererDebugView::Variance,
    RendererDebugView::Normals,
    RendererDebugView::Depth,
    RendererDebugView::MotionVectors,
};

RendererDebugView nextDebugView(RendererDebugView view) {
    const uint32_t raw = static_cast<uint32_t>(view);
    const uint32_t next = raw >= static_cast<uint32_t>(RendererDebugView::RestirReservoirM) ? 0u : raw + 1u;
    return static_cast<RendererDebugView>(next);
}

uint64_t countSceneTriangles(const SceneAsset& scene, const AssetManager& assets) {
    uint64_t triangles = 0;
    for (MeshAssetHandle handle : scene.meshes) {
        if (const MeshAsset* mesh = assets.mesh(handle)) {
            triangles += mesh->indices.size() / 3u;
        }
    }
    return triangles;
}

RendererSettings interactiveSettingsForScene(RendererSettings settings, const SceneAsset& scene, const AssetManager& assets, bool& changed) {
    changed = false;
    const uint64_t triangleCount = countSceneTriangles(scene, assets);
    if (triangleCount < largeSceneTriangleThreshold) {
        return settings;
    }

    const uint32_t cappedBounces = std::min(settings.maxBounces, 4u);
    const float cappedScale = std::min(settings.renderResolutionScale, 0.5f);
    changed = cappedBounces != settings.maxBounces ||
        std::abs(cappedScale - settings.renderResolutionScale) > 0.0001f ||
        !settings.denoiserEnabled;
    settings.maxBounces = cappedBounces;
    settings.renderResolutionScale = cappedScale;
    settings.denoiserEnabled = true;
    if (changed) {
        std::cout << "Large glTF scene detected (" << triangleCount
                  << " triangles); using interactive defaults: bounces="
                  << settings.maxBounces << " resolution_scale="
                  << settings.renderResolutionScale << ". Raise these in Render Settings for final-quality renders.\n";
    }
    return settings;
}

void syncDocumentRenderSettings(SceneDocument& document, const RendererSettings& settings) {
    RenderSettings render = document.renderSettings();
    render.pathTracingEnabled = settings.pathTracingEnabled;
    render.cameraJitterEnabled = settings.cameraJitterEnabled;
    render.directLightingEnabled = settings.directLightingEnabled;
    render.maxBounces = settings.maxBounces;
    render.environmentDirectSamples = settings.environmentDirectSamples;
    render.toneMapper = settings.toneMapper;
    render.exposure = settings.exposure;
    render.gamma = settings.gamma;
    render.contrast = settings.contrast;
    render.saturation = settings.saturation;
    render.brightness = settings.brightness;
    render.whitePoint = settings.whitePoint;
    render.autoExposureEnabled = settings.autoExposureEnabled;
    render.targetLuminance = settings.targetLuminance;
    render.minExposure = settings.minExposure;
    render.maxExposure = settings.maxExposure;
    render.adaptationSpeed = settings.adaptationSpeed;
    render.histogramMinLogLuminance = settings.histogramMinLogLuminance;
    render.histogramMaxLogLuminance = settings.histogramMaxLogLuminance;
    render.histogramLowPercentile = settings.histogramLowPercentile;
    render.histogramHighPercentile = settings.histogramHighPercentile;
    render.histogramTargetPercentile = settings.histogramTargetPercentile;
    render.sunlightEnabled = settings.sunlightEnabled;
    render.sunIntensity = settings.sunIntensity;
    render.skyIntensity = settings.skyIntensity;
    render.sunElevation = settings.sunElevation;
    render.sunAngularRadius = settings.sunAngularRadius;
    render.indirectStrength = settings.indirectStrength;
    render.restirMode = settings.restirMode;
    render.denoiserEnabled = settings.denoiserEnabled;
    render.atrousIterations = settings.atrousIterations;
    render.denoiserStrength = settings.denoiserStrength;
    render.taaEnabled = settings.taaEnabled;
    render.taaFeedback = settings.taaFeedback;
    render.debugView = settings.debugView;
    render.resolutionScale = settings.renderResolutionScale;
    document.setRenderSettings(render);
    Environment environment = document.environment();
    environment.backgroundIntensity = settings.environmentBackgroundIntensity;
    document.setEnvironment(std::move(environment));
}

RendererSettings rendererSettingsFromDocument(const SceneDocument& document, RendererSettings settings) {
    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    settings.pathTracingEnabled = render.pathTracingEnabled;
    settings.cameraJitterEnabled = render.cameraJitterEnabled;
    settings.directLightingEnabled = render.directLightingEnabled;
    settings.maxBounces = render.maxBounces;
    settings.environmentDirectSamples = render.environmentDirectSamples;
    settings.toneMapper = render.toneMapper;
    settings.exposure = render.exposure;
    settings.gamma = render.gamma;
    settings.contrast = render.contrast;
    settings.saturation = render.saturation;
    settings.brightness = render.brightness;
    settings.whitePoint = render.whitePoint;
    settings.autoExposureEnabled = render.autoExposureEnabled;
    settings.targetLuminance = render.targetLuminance;
    settings.minExposure = render.minExposure;
    settings.maxExposure = render.maxExposure;
    settings.adaptationSpeed = render.adaptationSpeed;
    settings.histogramMinLogLuminance = render.histogramMinLogLuminance;
    settings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
    settings.histogramLowPercentile = render.histogramLowPercentile;
    settings.histogramHighPercentile = render.histogramHighPercentile;
    settings.histogramTargetPercentile = render.histogramTargetPercentile;
    settings.sunlightEnabled = render.sunlightEnabled;
    settings.sunIntensity = render.sunIntensity;
    settings.skyIntensity = render.skyIntensity;
    settings.sunElevation = render.sunElevation;
    settings.sunAngularRadius = render.sunAngularRadius;
    settings.indirectStrength = render.indirectStrength;
    settings.restirMode = render.restirMode;
    settings.denoiserEnabled = render.denoiserEnabled;
    settings.atrousIterations = render.atrousIterations;
    settings.denoiserStrength = render.denoiserStrength;
    settings.taaEnabled = render.taaEnabled;
    settings.taaFeedback = render.taaFeedback;
    settings.debugView = render.debugView;
    settings.renderResolutionScale = render.resolutionScale;
    settings.environmentEnabled = environment.enabled;
    settings.environmentIntensity = environment.intensity;
    settings.environmentRotation = environment.rotation;
    settings.environmentBackgroundIntensity = environment.backgroundIntensity;
    return settings;
}

void applyDocumentMaterialAssignments(const SceneDocument& document, AssetManager& assets) {
    for (const Entity* entity : document.registry().entities()) {
        if (!entity->meshRenderer.has_value()) {
            continue;
        }
        MeshAsset* mesh = assets.mesh(entity->meshRenderer->mesh);
        if (mesh == nullptr) {
            continue;
        }
        const std::vector<MaterialSlot>& slots = entity->meshRenderer->materialSlots;
        for (size_t i = 0; i < slots.size() && i < mesh->primitives.size(); ++i) {
            const MaterialAssetHandle material = slots[i].resolvedMaterial();
            if (material.valid()) {
                mesh->primitives[i].material = material;
            }
        }
    }
}

std::filesystem::path resolveProjectRoot() {
    std::filesystem::path candidate = std::filesystem::current_path();
    while (!candidate.empty()) {
        auto shadersDir = candidate / "native" / "vulkan" / "shaders";
        if (std::filesystem::exists(shadersDir / "pathtrace.rgen")) {
            return candidate;
        }
        candidate = candidate.parent_path();
    }
    return std::filesystem::current_path();
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* current = &entity;
    glm::mat4 result(1.0f);
    constexpr int maxDepth = 512;
    for (int depth = 0; depth < maxDepth && current != nullptr; ++depth) {
        result = current->transform.localMatrix() * result;
        if (!current->parent.valid()) {
            break;
        }
        const Entity* parent = registry.entity(current->parent);
        if (parent == nullptr) {
            break;
        }
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return result;
}

EntityId duplicateEntityRecursive(SceneRegistry& registry, Entity source, EntityId parent) {
    const EntityId copyId = registry.createEntity(source.name.empty() ? "Entity Copy" : source.name + " Copy");
    Entity* copy = registry.entity(copyId);
    if (copy == nullptr) {
        return {};
    }

    copy->transform = source.transform;
    copy->transform.dirty = true;
    copy->meshRenderer = source.meshRenderer;
    copy->light = source.light;
    copy->camera = source.camera;
    if (copy->camera.has_value()) {
        copy->camera->active = false;
    }
    copy->parent = parent;
    copy->children.clear();
    if (Entity* parentEntity = registry.entity(parent)) {
        parentEntity->children.push_back(copyId);
    }

    const std::vector<EntityId> children = source.children;
    for (EntityId childId : children) {
        if (const Entity* child = registry.entity(childId)) {
            (void)duplicateEntityRecursive(registry, *child, copyId);
        }
    }
    return copyId;
}

void windowFocusCallback(GLFWwindow* window, int focused) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onWindowFocusChanged(focused == GLFW_TRUE);
    }
}

void fileDropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onFilesDropped(count, paths);
    }
}
}

Application::Application(
    RendererDebugView debugView,
    std::optional<std::filesystem::path> gltfPath,
    std::optional<std::filesystem::path> hdrPath,
    std::optional<std::filesystem::path> scenePath,
    std::optional<bool> denoiserOverride,
    std::optional<RestirMode> restirModeOverride,
    bool debugViewOverride,
    bool validationCameraMotion)
    : debugView_(debugView),
      gltfPath_(std::move(gltfPath)),
      hdrPath_(std::move(hdrPath)),
      scenePath_(std::move(scenePath)),
      denoiserOverride_(denoiserOverride),
      restirModeOverride_(restirModeOverride),
      debugViewOverride_(debugViewOverride),
      validationCameraMotion_(validationCameraMotion) {
    initWindow();
    initVulkan();
}

Application::~Application() {
    if (pendingSceneLoad_.valid()) {
        pendingSceneLoad_.wait();
    }
    if (commandSystem_) {
        commandSystem_->waitIdle();
    }

    if (uiOverlay_) {
        uiOverlay_->editor().editorPrefs().save(EditorPreferences::defaultPath());
    }
    commandSystem_.reset();
    uiOverlay_.reset();
    pathTracer_.reset();
    pipelineDemo_.reset();
    resourceDemo_.reset();
    swapchain_.reset();
    uploader_.reset();
    uploadContext_.reset();
    allocator_.reset();
    context_.reset();

    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void Application::run(uint32_t maxFrames) {
    mainLoop(maxFrames);
}

void Application::initWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(initialWidth, initialHeight, "Ray Tracing Engine - Vulkan", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("glfwCreateWindow failed");
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowFocusCallback(window_, windowFocusCallback);
    glfwSetDropCallback(window_, fileDropCallback);
    glfwGetWindowPos(window_, &windowedX_, &windowedY_);
    glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

    std::cout << "Controls: hold right mouse in the viewport to look/move, WASD move, QE/Space/Ctrl vertical, Shift fast, F11 borderless fullscreen.\n"
              << "Settings: F1 debug view, F2 denoiser, F3 denoise while moving, F4 sun, F5 env, F6 direct light, R reset.\n"
              << "Adjust: +/- exposure, 1-4 tone mapper, 5 auto exposure, </> env intensity, [/ ] env rotation, PageUp/PageDown bounces, Home/End a-trous.\n"
              << "Files: drop .hdr for environment maps or .gltf/.glb for scene reload.\n";
}

void Application::initVulkan() {
    context_ = std::make_unique<VulkanContext>(window_);
    allocator_ = std::make_unique<ResourceAllocator>(*context_);
    uploadContext_ = std::make_unique<UploadContext>(context_->device(), context_->graphicsQueue(), context_->queueFamilies().graphics.value());
    uploader_ = std::make_unique<BufferUploader>(*allocator_, *uploadContext_);
    swapchain_ = std::make_unique<Swapchain>(*context_, window_);
    commandSystem_ = std::make_unique<CommandSystem>(*context_, *swapchain_);
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    bool loadedSceneDocument = false;
    if (scenePath_.has_value()) {
        if (!sceneDocument_.loadJson(*scenePath_)) {
            throw std::runtime_error("Scene JSON load failed: " + scenePath_->string());
        }
        loadedSceneDocument = true;
        gltfPath_ = sceneDocument_.sourceGltfPath();
        if (!hdrPath_.has_value()) {
            hdrPath_ = sceneDocument_.sourceHdrPath();
        }
        if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
            GltfLoader loader(assets_);
            importedScene_ = loader.loadWithCache(*gltfPath_);
        }
        undoStack_.clear();
        std::cout << "Loaded scene JSON: " << scenePath_->string() << '\n';
    } else if (gltfPath_.has_value()) {
        GltfLoader loader(assets_);
        importedScene_ = loader.loadWithCache(*gltfPath_);
        sceneDocument_.importSceneAsset(*importedScene_);
        sceneDocument_.setSourceGltfPath(gltfPath_);
        undoStack_.clear();
        std::cout << "Loaded glTF: " << gltfPath_->string()
                  << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size() << '\n';
    } else if (!loadedSceneDocument) {
        initializeFallbackSceneDocument();
    }
    sceneDocument_.setSourceHdrPath(hdrPath_);
    rebuildGpuSceneAsset();
    RendererSettings startupSettings{};
    startupSettings.debugView = debugView_;
    if (loadedSceneDocument) {
        startupSettings = rendererSettingsFromDocument(sceneDocument_, startupSettings);
        if (debugViewOverride_) {
            startupSettings.debugView = debugView_;
        } else {
            debugView_ = startupSettings.debugView;
        }
    }
    bool largeSceneSettingsChanged = false;
    if (importedScene_.has_value()) {
        startupSettings = interactiveSettingsForScene(startupSettings, *importedScene_, assets_, largeSceneSettingsChanged);
        if (largeSceneSettingsChanged) {
            syncDocumentRenderSettings(sceneDocument_, startupSettings);
        }
    }
    if (denoiserOverride_.has_value()) {
        startupSettings.denoiserEnabled = *denoiserOverride_;
    }
    if (restirModeOverride_.has_value()) {
        startupSettings.restirMode = *restirModeOverride_;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    createPathTracer((loadedSceneDocument || importedScene_.has_value() || denoiserOverride_.has_value() || restirModeOverride_.has_value()) ? &startupSettings : nullptr);
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    uiOverlay_ = std::make_unique<UiOverlay>(window_, *context_, *swapchain_);
    if (loadedSceneDocument) {
        uiOverlay_->editor().cameraBookmarks().deserialize(sceneDocument_);
    }
    commandSystem_->setPathTracer(pathTracer_.get());
    commandSystem_->setUiOverlay(uiOverlay_.get());
    uiOverlay_->editor().editorPrefs().load(EditorPreferences::defaultPath());
}

void Application::mainLoop(uint32_t maxFrames) {
    const auto start = std::chrono::steady_clock::now();
    uint32_t frameCount = 0;
    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - start).count();
        const float deltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
        lastFrameSeconds_ = seconds;

        if (uiOverlay_) {
            uiOverlay_->beginFrame();
        }
        processRuntimeControls(deltaSeconds);
        applyValidationCameraMotion(frameCount);
        notifications_.update(deltaSeconds);
        EditorRequests editorRequests;
        if (pendingOpenLevel_) {
            pendingOpenLevel_ = false;
            if (auto path = openSceneJsonFileDialog()) {
                editorRequests.loadSceneJson = *path;
                editorRequests.resetAccumulation = AccumulationResetReason::SceneChanged;
            }
        }
        if (pendingSaveLevel_) {
            pendingSaveLevel_ = false;
            if (auto path = saveSceneJsonFileDialog()) {
                editorRequests.saveSceneJson = *path;
            }
        }
        if (pendingReloadShaders_) {
            pendingReloadShaders_ = false;
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (pathTracer_ && pathTracer_->shadersNeedReload()) {
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (uiOverlay_ && pathTracer_) {
            editorRequests = uiOverlay_->build(
                *pathTracer_,
                swapchain_->extent(),
                importedScene_ ? &*importedScene_ : nullptr,
                &sceneDocument_,
                importedScene_ ? &assets_ : nullptr,
                gltfPath_,
                hdrPath_,
                &gpuInstanceEntities_,
                sceneLoadingStatus_,
                &cameraController_,
                deltaSeconds * 1000.0f,
                &notifications_);
        }
        applyEditorRequests(editorRequests, false);
        commandSystem_->drawFrame(seconds, deltaSeconds);
        applyEditorRequests(editorRequests, true);
        pollAsyncSceneLoad();
        updateWindowTitle(seconds);

        ++frameCount;
        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }
}

void Application::onWindowFocusChanged(bool focused) {
    if (!focused && window_ != nullptr) {
        cameraController_.releaseMouse(window_);
    }
}

void Application::onFilesDropped(int count, const char** paths) {
    if (pathTracer_ == nullptr || paths == nullptr || count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (paths[i] == nullptr) {
            continue;
        }
        std::filesystem::path path{paths[i]};
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension != ".hdr") {
            if (extension == ".gltf" || extension == ".glb") {
                requestGltfSceneLoad(path);
            } else {
                std::cout << "Dropped file ignored: " << path.string() << " (supported: .hdr, .gltf, .glb)\n";
            }
            continue;
        }

        try {
            pathTracer_->loadEnvironment(path);
            hdrPath_ = path;
            sceneDocument_.setSourceHdrPath(hdrPath_);
            sceneDocument_.markDirty(SceneUpdateKind::EnvironmentOnly);
            RendererSettings settings = pathTracer_->settings();
            settings.environmentEnabled = true;
            pathTracer_->applySettings(settings);
            notifications_.notify("HDR environment loaded", NotificationType::Success);
            std::cout << "Loaded dropped HDR environment: " << path.string() << '\n';
        } catch (const std::exception& error) {
            notifications_.notify("HDR environment load failed", NotificationType::Error);
            std::cerr << "Dropped HDR load failed: " << error.what() << '\n';
        }
    }
}

void Application::reloadGltfScene(const std::filesystem::path& path) {
    PendingSceneLoadResult result;
    result.path = path;
    try {
        GltfLoader loader(result.assets);
        result.scene = loader.loadWithCache(path);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    commitLoadedGltfScene(std::move(result));
}

void Application::requestGltfSceneLoad(const std::filesystem::path& path) {
    if (pendingSceneLoad_.valid() &&
        pendingSceneLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        sceneLoadingStatus_ = "Scene load already running: " + pendingSceneLoadPath_->string();
        notifications_.notify("Scene load already running", NotificationType::Warning);
        return;
    }

    if (pendingSceneLoad_.valid()) {
        (void)pendingSceneLoad_.get();
    }

    pendingSceneLoadPath_ = path;
    sceneLoadingStatus_ = "Loading glTF in background: " + path.string();
    notifications_.notify("Loading glTF scene", NotificationType::Info);
    std::cout << sceneLoadingStatus_ << '\n';
    pendingSceneLoad_ = std::async(std::launch::async, [path]() {
        PendingSceneLoadResult result;
        result.path = path;
        try {
            GltfLoader loader(result.assets);
            result.scene = loader.loadWithCache(path);
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    });
}

void Application::pollAsyncSceneLoad() {
    if (!pendingSceneLoad_.valid()) {
        return;
    }
    if (pendingSceneLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    PendingSceneLoadResult result = pendingSceneLoad_.get();
    pendingSceneLoadPath_.reset();
    if (!result.error.empty()) {
        sceneLoadingStatus_ = "glTF load failed: " + result.error;
        notifications_.notify("glTF load failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    sceneLoadingStatus_ = "Finalizing GPU scene: " + result.path.string();
    commitLoadedGltfScene(std::move(result));
}

void Application::commitLoadedGltfScene(PendingSceneLoadResult&& result) {
    if (!context_ || !allocator_ || !uploader_ || !swapchain_ || !commandSystem_) {
        return;
    }
    if (!result.error.empty()) {
        sceneLoadingStatus_ = "glTF load failed: " + result.error;
        notifications_.notify("glTF load failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    const RendererSettings previousSettings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    bool largeSceneSettingsChanged = false;
    RendererSettings reloadSettings = interactiveSettingsForScene(previousSettings, result.scene, result.assets, largeSceneSettingsChanged);

    try {
        SceneDocument nextDocument;
        nextDocument.importSceneAsset(result.scene);
        nextDocument.setSourceGltfPath(result.path);
        nextDocument.setSourceHdrPath(hdrPath_);
        syncDocumentRenderSettings(nextDocument, reloadSettings);
        applyDocumentMaterialAssignments(nextDocument, result.assets);

        const SceneGpuBuildResult build = sceneBuilder_.build(nextDocument, &result.assets, reloadSettings);
        const std::optional<std::filesystem::path> cachePath = SceneCache::cachePathFor(result.path);

        commandSystem_->waitIdle();
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            build.sceneAsset.meshes.empty() ? nullptr : &build.sceneAsset,
            build.sceneAsset.meshes.empty() ? nullptr : &result.assets,
            cachePath,
            &reloadSettings);

        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        cameraController_.releaseMouse(window_);

        assets_ = std::move(result.assets);
        importedScene_ = std::move(result.scene);
        gltfPath_ = result.path;
        sceneDocument_ = std::move(nextDocument);
        sceneDocument_.clearDirty();
        undoStack_.clear();
        gpuSceneAsset_ = std::move(build.sceneAsset);
        gpuInstanceEntities_ = std::move(build.instanceEntities);
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        commandSystem_->setPathTracer(pathTracer_.get());
    } catch (const std::exception& error) {
        sceneLoadingStatus_ = "glTF GPU finalization failed: " + std::string(error.what());
        notifications_.notify("glTF GPU finalization failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    sceneLoadingStatus_ = "Loaded glTF: " + result.path.string();
    if (uiOverlay_) {
        uiOverlay_->editor().editorPrefs().addRecentFile(result.path);
    }
    notifications_.notify("Scene loaded", NotificationType::Success);
    std::cout << "Reloaded glTF: " << result.path.string()
              << " meshes=" << importedScene_->meshes.size()
              << " materials=" << importedScene_->materials.size()
              << " textures=" << importedScene_->textures.size()
              << " nodes=" << importedScene_->nodes.size() << '\n';
}

void Application::applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild) {
    if (!pathTracer_) {
        return;
    }

    if (!allowResourceRebuild) {
        if (requests.undo) {
            if (undoStack_.undo()) {
                notifications_.notify("Undo", NotificationType::Info);
            }
        }
        if (requests.redo) {
            if (undoStack_.redo()) {
                notifications_.notify("Redo", NotificationType::Info);
            }
        }
        if (requests.settings.has_value()) {
            applyRendererSettingsSafely(*requests.settings, false);
        }
        (void)applyPendingSceneUpdate(false);
        if (requests.toggleDenoiser) {
            RendererSettings settings = pathTracer_->settings();
            settings.denoiserEnabled = !settings.denoiserEnabled;
            pathTracer_->applySettings(settings);
        }
        if (requests.cycleIntermediateView) {
            RendererSettings settings = pathTracer_->settings();
            constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
            int idx = 0;
            for (int i = 0; i < count; ++i) {
                if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
            }
            settings.debugView = intermediateViews[idx];
            pathTracer_->applySettings(settings);
        }
        if (requests.resetAccumulation.has_value()) {
            pathTracer_->resetAccumulation(*requests.resetAccumulation);
        }
        if (requests.cameraMoveSpeed.has_value()) {
            cameraController_.setMoveSpeed(*requests.cameraMoveSpeed);
        }
        if (requests.resetCamera) {
            cameraController_.reset(*pathTracer_);
        }
        return;
    }

    if (pendingPostFrameSettings_.has_value()) {
        RendererSettings pending = *pendingPostFrameSettings_;
        pendingPostFrameSettings_.reset();
        applyRendererSettingsSafely(pending, true);
    }

    if (requests.loadHdr.has_value()) {
        try {
            pathTracer_->loadEnvironment(*requests.loadHdr);
            hdrPath_ = *requests.loadHdr;
            uiOverlay_->editor().editorPrefs().addRecentFile(*requests.loadHdr);
            sceneDocument_.setSourceHdrPath(hdrPath_);
            RendererSettings settings = pathTracer_->settings();
            settings.environmentEnabled = true;
            pathTracer_->applySettings(settings);
            std::cout << "Loaded HDR from editor: " << requests.loadHdr->string() << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Editor HDR load failed: " << error.what() << '\n';
        }
    }

    if (requests.saveSceneJson.has_value()) {
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().cameraBookmarks().serialize(sceneDocument_);
        }
        if (sceneDocument_.saveJson(*requests.saveSceneJson)) {
            std::cout << "Saved scene JSON: " << requests.saveSceneJson->string() << '\n';
        } else {
            std::cerr << "Scene JSON save failed: " << requests.saveSceneJson->string() << '\n';
        }
    }

    if (requests.loadSceneJson.has_value()) {
        if (sceneDocument_.loadJson(*requests.loadSceneJson)) {
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().cameraBookmarks().deserialize(sceneDocument_);
            }
            gltfPath_ = sceneDocument_.sourceGltfPath();
            hdrPath_ = sceneDocument_.sourceHdrPath();
            if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
                try {
                    assets_.clear();
                    GltfLoader loader(assets_);
                    importedScene_ = loader.loadWithCache(*gltfPath_);
                } catch (const std::exception& error) {
                    std::cerr << "Referenced glTF load failed for level: " << error.what() << '\n';
                }
            }
            undoStack_.clear();
            std::cout << "Loaded scene JSON: " << requests.loadSceneJson->string() << '\n';
        } else {
            std::cerr << "Scene JSON load failed: " << requests.loadSceneJson->string() << '\n';
        }
    }

    if (requests.materialUpdate.has_value()) {
        MaterialAsset* material = assets_.material(MaterialAssetHandle{requests.materialUpdate->materialId});
        if (material != nullptr) {
            *material = requests.materialUpdate->material;
            bool gpuUpdated = false;
            if (gpuSceneAsset_.has_value()) {
                gpuUpdated = pathTracer_->updateMaterials(*gpuSceneAsset_, assets_);
            }
            if (!gpuUpdated) {
                pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
            }
            sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
            sceneDocument_.clearDirty();
        }
    }

    if (requests.materialAssignment.has_value()) {
        MeshAsset* mesh = assets_.mesh(requests.materialAssignment->mesh);
        if (mesh != nullptr && requests.materialAssignment->primitiveIndex < mesh->primitives.size()) {
            mesh->primitives[requests.materialAssignment->primitiveIndex].material = requests.materialAssignment->material;
            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        }
    }

    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (requests.duplicateEntity.has_value()) {
        (void)sceneOps.duplicateEntity(*requests.duplicateEntity);
    }

    if (requests.deleteEntity.has_value()) {
        (void)sceneOps.deleteEntity(*requests.deleteEntity);
    }

    if (requests.reparentEntity.has_value()) {
        const auto [child, newParent] = *requests.reparentEntity;
        (void)sceneOps.reparentEntity(child, newParent);
    }

    if (requests.setEntityVisibility.has_value()) {
        (void)sceneOps.setVisibility(requests.setEntityVisibility->entity, requests.setEntityVisibility->value);
    }

    if (requests.setEntityLocked.has_value()) {
        (void)sceneOps.setLocked(requests.setEntityLocked->entity, requests.setEntityLocked->value);
    }

    if (requests.setEntityTransform.has_value()) {
        sceneOps.setTransformGizmoDrag(
            requests.setEntityTransform->entity,
            requests.setEntityTransform->oldTransform,
            requests.setEntityTransform->newTransform);
    }

    if (requests.focusOnEntity.has_value()) {
        const Entity* entity = sceneDocument_.registry().entity(*requests.focusOnEntity);
        if (entity != nullptr) {
            const glm::vec3 target = glm::vec3(entityWorldMatrix(sceneDocument_.registry(), *entity)[3]);
            const glm::vec3 position = cameraController_.position();
            glm::vec3 direction = target - position;
            if (glm::dot(direction, direction) > 0.0001f) {
                cameraController_.setPose(position, glm::normalize(direction), *pathTracer_);
            }
        }
    }

    if (requests.saveCameraBookmark.has_value()) {
        uiOverlay_->editor().cameraBookmarks().saveBookmark(
            cameraController_, *requests.saveCameraBookmark, &pathTracer_->settings());
    }
    if (requests.loadCameraBookmarkIndex.has_value()) {
        const auto& bookmarks = uiOverlay_->editor().cameraBookmarks().bookmarks();
        const size_t index = *requests.loadCameraBookmarkIndex;
        if (index < bookmarks.size()) {
            RendererSettings settings = pathTracer_->settings();
            uiOverlay_->editor().cameraBookmarks().loadBookmark(
                bookmarks[index], cameraController_, *pathTracer_, &settings);
            pathTracer_->applySettings(settings);
        }
    }
    if (requests.deleteCameraBookmarkIndex.has_value()) {
        uiOverlay_->editor().cameraBookmarks().deleteBookmark(*requests.deleteCameraBookmarkIndex);
    }
    if (requests.removeFavorite.has_value()) {
        uiOverlay_->editor().editorPrefs().removeFavorite(*requests.removeFavorite);
    }

    (void)applyPendingSceneUpdate(allowResourceRebuild);

    if (requests.reloadShaders) {
        reloadShadersFromEditor();
    }

    if (requests.loadGltf.has_value()) {
        requestGltfSceneLoad(*requests.loadGltf);
    }

    if (requests.exit && window_ != nullptr) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void Application::applyValidationCameraMotion(uint32_t frameIndex) {
    if (!validationCameraMotion_ || pathTracer_ == nullptr) {
        return;
    }
    const float angle = static_cast<float>(frameIndex) * 0.035f;
    const float radius = 4.2f;
    const glm::vec3 target{0.0f, 0.55f, 0.0f};
    const glm::vec3 position{
        std::sin(angle) * radius,
        0.75f + std::sin(angle * 0.37f) * 0.25f,
        std::cos(angle) * radius};
    cameraController_.setPose(position, glm::normalize(target - position), *pathTracer_);
}

bool Application::applyPendingSceneUpdate(bool allowResourceRebuild) {
    if (!pathTracer_ || !sceneDocument_.dirty()) {
        return false;
    }

    const SceneUpdateKind pendingKind = sceneDocument_.pendingUpdate();
    SceneUpdateRoute route = SceneUpdateRouter::route(pendingKind);
    pathTracer_->validationLog().recordSceneUpdateRoute(
        sceneUpdateKindName(route.kind),
        sceneUpdateGpuActionName(route.action));
    if (route.action == SceneUpdateGpuAction::None) {
        sceneDocument_.clearDirty();
        return true;
    }
    if (!allowResourceRebuild &&
        (route.requiresRendererRebuild || route.action == SceneUpdateGpuAction::UpdateTransforms)) {
        return false;
    }

    std::optional<SceneGpuBuildResult> build;
    auto ensureBuild = [&]() -> SceneGpuBuildResult& {
        if (!build.has_value()) {
            build = sceneBuilder_.build(sceneDocument_, &assets_, pathTracer_->settings());
        }
        return *build;
    };
    if (route.requiresGpuSceneBuild) {
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
    }

    auto rebuildRenderer = [&]() {
        SceneGpuBuildResult& sceneBuild = ensureBuild();
        const RendererSettings previousSettings = pathTracer_->settings();
        commandSystem_->waitIdle();
        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        gpuSceneAsset_ = sceneBuild.sceneAsset;
        gpuInstanceEntities_ = sceneBuild.instanceEntities;
        rebuildGpuSceneAsset();
        pathTracer_.reset();
        createPathTracer(&previousSettings);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(route.resetReason);
        commandSystem_->setPathTracer(pathTracer_.get());
    };

    switch (route.action) {
    case SceneUpdateGpuAction::UpdateCamera:
        applyActiveSceneCamera();
        break;
    case SceneUpdateGpuAction::UpdateLights:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateSceneLights(*gpuSceneAsset_)) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateMaterials:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateMaterials(*gpuSceneAsset_, assets_)) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateEnvironment:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::ApplyRendererSettings:
        applyRendererSettingsSafely(rendererSettingsFromDocument(sceneDocument_, pathTracer_->settings()), allowResourceRebuild);
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateVisibility:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateSceneVisibility(*gpuSceneAsset_, assets_) && allowResourceRebuild) {
            rebuildRenderer();
        }
        break;
    case SceneUpdateGpuAction::UpdateTransforms:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateSceneTransforms(*gpuSceneAsset_, assets_)) {
            if (!allowResourceRebuild) {
                return false;
            }
            rebuildRenderer();
        }
        break;
    case SceneUpdateGpuAction::RebuildTopology:
        if (!allowResourceRebuild) {
            return false;
        }
        rebuildRenderer();
        break;
    case SceneUpdateGpuAction::None:
        break;
    }

    sceneDocument_.clearDirty();
    if (route.action == SceneUpdateGpuAction::RebuildTopology) {
        notifications_.notify("Scene topology rebuilt", NotificationType::Info);
    }
    return true;
}

void Application::applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return;
    }

    const RendererSettings current = pathTracer_->settings();
    const bool renderResolutionChanged =
        std::abs(settings.renderResolutionScale - current.renderResolutionScale) > 0.0001f;
    if (!renderResolutionChanged || allowRenderResolutionChange) {
        pathTracer_->applySettings(settings);
        return;
    }

    RendererSettings immediate = settings;
    immediate.renderResolutionScale = current.renderResolutionScale;
    pathTracer_->applySettings(immediate);
    pendingPostFrameSettings_ = settings;
}

void Application::reloadShadersFromEditor() {
    if (!pathTracer_ || !commandSystem_) {
        return;
    }
    const RendererSettings previousSettings = pathTracer_->settings();
    commandSystem_->waitIdle();
    if (uiOverlay_) {
        uiOverlay_->invalidateViewportTexture();
        uiOverlay_->editor().invalidateAssetThumbnails();
    }
    pathTracer_.reset();
    createPathTracer(&previousSettings);
    applyActiveSceneCamera();
    pathTracer_->resetAccumulation(AccumulationResetReason::ShaderReloaded);
    commandSystem_->setPathTracer(pathTracer_.get());
    notifications_.notify("Shaders reloaded", NotificationType::Success);
    std::cout << "Reloaded shaders from editor.\n";
}

std::unique_ptr<PathTracerRenderer> Application::makePathTracer(
    const SceneAsset* sceneAsset,
    const AssetManager* assets,
    std::optional<std::filesystem::path> sceneCachePath,
    const RendererSettings* settingsToRestore) {
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    const auto shaderOutDir = projectRoot / "native" / "vulkan" / "build" / "shaders";
    auto renderer = std::make_unique<PathTracerRenderer>(
        *context_,
        *allocator_,
        *uploader_,
        swapchain_->format(),
        shaderDir,
        shaderOutDir,
        debugView_,
        sceneAsset,
        sceneAsset != nullptr ? assets : nullptr,
        hdrPath_,
        std::move(sceneCachePath));
    if (settingsToRestore != nullptr) {
        renderer->applySettings(*settingsToRestore);
    }
    return renderer;
}

void Application::createPathTracer(const RendererSettings* settingsToRestore) {
    std::optional<std::filesystem::path> cachePath;
    if (gltfPath_.has_value()) {
        cachePath = SceneCache::cachePathFor(*gltfPath_);
    }
    const SceneAsset* sceneAsset = gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr;
    pathTracer_ = makePathTracer(sceneAsset, sceneAsset != nullptr ? &assets_ : nullptr, std::move(cachePath), settingsToRestore);
}

void Application::applyActiveSceneCamera() {
    if (pathTracer_ == nullptr || !sceneDocument_.activeCamera().valid()) {
        return;
    }

    const Entity* cameraEntity = sceneDocument_.registry().entity(sceneDocument_.activeCamera());
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    const glm::mat4 transform = entityWorldMatrix(sceneDocument_.registry(), *cameraEntity);
    const glm::vec3 position = glm::vec3(transform[3]);
    glm::vec3 forward = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
    if (glm::dot(forward, forward) <= 0.0f) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    pathTracer_->setCameraFovY(cameraEntity->camera->verticalFovRadians);
    cameraController_.setPose(position, forward, *pathTracer_);
}

void Application::rebuildGpuSceneAsset() {
    const RendererSettings settings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    applyDocumentMaterialAssignments(sceneDocument_, assets_);
    SceneGpuBuildResult build = sceneBuilder_.build(sceneDocument_, &assets_, settings);
    gpuSceneAsset_ = std::move(build.sceneAsset);
    gpuInstanceEntities_ = std::move(build.instanceEntities);
}

void Application::initializeFallbackSceneDocument() {
    sceneDocument_ = SceneDocument{};
    EntityId camera = sceneDocument_.registry().createEntity("Camera");
    Camera cameraComponent;
    cameraComponent.active = true;
    sceneDocument_.registry().addCamera(camera, cameraComponent);
    sceneDocument_.setActiveCamera(camera);
    (void)sceneDocument_.registry().createEntity("Cornell Fallback");
    sceneDocument_.clearDirty();
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
}

void Application::processRuntimeControls(float deltaSeconds) {
    if (!pathTracer_) {
        return;
    }

    const bool uiWantsTextInput = uiOverlay_ != nullptr && uiOverlay_->wantsTextInput();
    const bool shortcutsBlocked = uiWantsTextInput;
    const bool viewportInteraction = uiOverlay_ != nullptr && uiOverlay_->viewportInteractionActive();
    const bool viewportHovered = uiOverlay_ != nullptr && uiOverlay_->viewportHovered();
    const bool cameraCaptured = cameraController_.mouseCaptured();
    cameraController_.update(
        window_,
        deltaSeconds,
        *pathTracer_,
        viewportHovered || cameraCaptured,
        (viewportInteraction || cameraCaptured) && !uiWantsTextInput);

    RendererSettings settings = pathTracer_->settings();
    bool changed = false;
    const bool ctrlDown =
        glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_Z)) {
        if (undoStack_.undo()) {
            notifications_.notify("Undo", NotificationType::Info);
            (void)applyPendingSceneUpdate(false);
        }
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_Y)) {
        if (undoStack_.redo()) {
            notifications_.notify("Redo", NotificationType::Info);
            (void)applyPendingSceneUpdate(false);
        }
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F1)) {
        settings.debugView = nextDebugView(settings.debugView);
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_0)) {
        settings.debugView = RendererDebugView::Beauty;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_1)) {
        settings.toneMapper = ToneMapper::Linear;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_2)) {
        settings.toneMapper = ToneMapper::Reinhard;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_3)) {
        settings.toneMapper = ToneMapper::ACES;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_4)) {
        settings.toneMapper = ToneMapper::PBRNeutral;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_5)) {
        settings.autoExposureEnabled = !settings.autoExposureEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F11)) {
        toggleBorderlessFullscreen();
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F2)) {
        settings.denoiserEnabled = !settings.denoiserEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F3)) {
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F4)) {
        settings.sunlightEnabled = !settings.sunlightEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F5)) {
        settings.environmentEnabled = !settings.environmentEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F6)) {
        settings.directLightingEnabled = !settings.directLightingEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F7)) {
        constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
        int idx = 0;
        for (int i = 0; i < count; ++i) {
            if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
        }
        settings.debugView = intermediateViews[idx];
        changed = true;
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_R)) {
        pendingReloadShaders_ = true;
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_L)) {
        pendingOpenLevel_ = true;
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_S)) {
        pendingSaveLevel_ = true;
    }
    const bool resetAccumulationPressed = !shortcutsBlocked && !ctrlDown && pressedOnce(GLFW_KEY_R);
    if (resetAccumulationPressed && !viewportInteraction) {
        pathTracer_->resetAccumulation();
    }

    const float exposureRate = 0.9f * deltaSeconds;
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_ADD) == GLFW_PRESS)) {
        settings.exposure += exposureRate;
        changed = true;
    }
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS)) {
        settings.exposure = std::max(0.05f, settings.exposure - exposureRate);
        changed = true;
    }

    const float envRate = 1.2f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_PERIOD) == GLFW_PRESS) {
        settings.environmentIntensity += envRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_COMMA) == GLFW_PRESS) {
        settings.environmentIntensity = std::max(0.0f, settings.environmentIntensity - envRate);
        changed = true;
    }
    const float rotationRate = 1.4f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation += rotationRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation -= rotationRate;
        changed = true;
    }

    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_UP)) {
        ++settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_DOWN) && settings.maxBounces > 1) {
        --settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_HOME)) {
        ++settings.atrousIterations;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_END) && settings.atrousIterations > 1) {
        --settings.atrousIterations;
        changed = true;
    }

    if (changed && pathTracer_->applySettings(settings)) {
        std::cout << "Settings changed: debug=" << rendererDebugViewName(settings.debugView)
                  << " tone=" << toneMapperName(settings.toneMapper)
                  << " autoExposure=" << (settings.autoExposureEnabled ? "on" : "off")
                  << " denoiser=" << (settings.denoiserEnabled ? "on" : "off")
                  << " sun=" << (settings.sunlightEnabled ? "on" : "off")
                  << " env=" << (settings.environmentEnabled ? "on" : "off")
                  << " bounces=" << settings.maxBounces << '\n';
    }
}

void Application::updateWindowTitle(float seconds) {
    if (!pathTracer_ || seconds - lastTitleUpdateSeconds_ < 0.25f) {
        return;
    }
    lastTitleUpdateSeconds_ = seconds;

    const RendererSettings& settings = pathTracer_->settings();
    const GpuFrameTimings& timings = pathTracer_->timings();
    std::ostringstream title;
    if (sceneDocument_.dirty()) {
        title << "[Modified] ";
    }
    title << (gltfPath_.has_value() ? gltfPath_->stem().string() : "Untitled")
          << " - Ray Tracing Engine"
          << " | samples " << pathTracer_->sampleCount()
          << " | " << rendererDebugViewName(settings.debugView)
          << " | denoise " << (settings.denoiserEnabled ? "on" : "off")
          << " | sun " << (settings.sunlightEnabled ? "on" : "off")
          << " | env " << (settings.environmentEnabled ? "on" : "off")
          << " | GPU "
          << std::fixed << std::setprecision(2)
          << timings.atmosphereMs + timings.pathTraceMs + timings.restirSpatialMs + timings.fogIntegrateMs + timings.denoiserMs + timings.fullscreenMs << " ms";
    glfwSetWindowTitle(window_, title.str().c_str());
}

void Application::toggleBorderlessFullscreen() {
    if (window_ == nullptr) {
        return;
    }

    cameraController_.releaseMouse(window_);
    if (!borderlessFullscreen_) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        if (monitor == nullptr || mode == nullptr) {
            return;
        }

        borderlessFullscreen_ = true;
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        borderlessFullscreen_ = false;
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedWidth_, windowedHeight_, 0);
    }
}

bool Application::pressedOnce(int key) {
    if (key < 0 || static_cast<size_t>(key) >= keyState_.size()) {
        return false;
    }
    const bool down = glfwGetKey(window_, key) == GLFW_PRESS;
    const bool wasDown = keyState_[static_cast<size_t>(key)] != 0;
    keyState_[static_cast<size_t>(key)] = down ? 1u : 0u;
    return down && !wasDown;
}

} // namespace rtv
