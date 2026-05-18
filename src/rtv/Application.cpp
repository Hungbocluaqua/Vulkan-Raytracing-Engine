#include "rtv/Application.h"

#include "rtv/CommandSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/GltfLoader.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/PipelineDemo.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ResourceDemo.h"
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

namespace rtv {

namespace {
constexpr int initialWidth = 1280;
constexpr int initialHeight = 720;
constexpr uint64_t largeSceneTriangleThreshold = 1'000'000ull;

RendererDebugView nextDebugView(RendererDebugView view) {
    const uint32_t raw = static_cast<uint32_t>(view);
    const uint32_t next = raw >= static_cast<uint32_t>(RendererDebugView::ClayMaterial) ? 0u : raw + 1u;
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
    render.directLightingEnabled = settings.directLightingEnabled;
    render.maxBounces = settings.maxBounces;
    render.environmentDirectSamples = settings.environmentDirectSamples;
    render.exposure = settings.exposure;
    render.sunlightEnabled = settings.sunlightEnabled;
    render.sunIntensity = settings.sunIntensity;
    render.skyIntensity = settings.skyIntensity;
    render.sunAngularRadius = settings.sunAngularRadius;
    render.indirectStrength = settings.indirectStrength;
    render.denoiserEnabled = settings.denoiserEnabled;
    render.atrousIterations = settings.atrousIterations;
    render.denoiserStrength = settings.denoiserStrength;
    render.debugView = settings.debugView;
    render.resolutionScale = settings.renderResolutionScale;
    render.requestedBackend = settings.requestedBackend;
    document.setRenderSettings(render);
    Environment environment = document.environment();
    environment.backgroundIntensity = settings.environmentBackgroundIntensity;
    document.setEnvironment(std::move(environment));
}

std::filesystem::path resolveProjectRoot() {
    std::filesystem::path candidate = std::filesystem::current_path();
    while (!candidate.empty()) {
        auto shadersDir = candidate / "native" / "vulkan" / "shaders";
        if (std::filesystem::exists(shadersDir / "pathtrace.comp")) {
            return candidate;
        }
        candidate = candidate.parent_path();
    }
    return std::filesystem::current_path();
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
    RendererBackend requestedBackend)
    : debugView_(debugView),
      requestedBackend_(requestedBackend),
      gltfPath_(std::move(gltfPath)),
      hdrPath_(std::move(hdrPath)) {
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
              << "Adjust: +/- exposure, </> env intensity, [/ ] env rotation, PageUp/PageDown bounces, Home/End a-trous.\n"
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
    if (gltfPath_.has_value()) {
        GltfLoader loader(assets_);
        importedScene_ = loader.loadWithCache(*gltfPath_);
        sceneDocument_.importSceneAsset(*importedScene_);
        sceneDocument_.setSourceGltfPath(gltfPath_);
        std::cout << "Loaded glTF: " << gltfPath_->string()
                  << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size() << '\n';
    } else {
        initializeFallbackSceneDocument();
    }
    sceneDocument_.setSourceHdrPath(hdrPath_);
    rebuildGpuSceneAsset();
    (void)shaderDir;
    RendererSettings startupSettings{};
    startupSettings.debugView = debugView_;
    startupSettings.requestedBackend = requestedBackend_;
    bool largeSceneSettingsChanged = false;
    if (importedScene_.has_value()) {
        startupSettings = interactiveSettingsForScene(startupSettings, *importedScene_, assets_, largeSceneSettingsChanged);
        if (largeSceneSettingsChanged) {
            syncDocumentRenderSettings(sceneDocument_, startupSettings);
        }
    }
    createPathTracer(importedScene_.has_value() ? &startupSettings : nullptr);
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    uiOverlay_ = std::make_unique<UiOverlay>(window_, *context_, *swapchain_);
    commandSystem_->setPathTracer(pathTracer_.get());
    commandSystem_->setUiOverlay(uiOverlay_.get());
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
        EditorRequests editorRequests;
        if (uiOverlay_ && pathTracer_) {
            editorRequests = uiOverlay_->build(
                *pathTracer_,
                swapchain_->extent(),
                importedScene_ ? &*importedScene_ : nullptr,
                &sceneDocument_,
                importedScene_ ? &assets_ : nullptr,
                gltfPath_,
                hdrPath_,
                sceneLoadingStatus_,
                &cameraController_,
                deltaSeconds * 1000.0f);
        }
        applyEditorRequests(editorRequests, false);
        commandSystem_->drawFrame(seconds);
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
            std::cout << "Loaded dropped HDR environment: " << path.string() << '\n';
        } catch (const std::exception& error) {
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
        return;
    }

    if (pendingSceneLoad_.valid()) {
        (void)pendingSceneLoad_.get();
    }

    pendingSceneLoadPath_ = path;
    sceneLoadingStatus_ = "Loading glTF in background: " + path.string();
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
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    const RendererSettings previousSettings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    commandSystem_->waitIdle();
    if (uiOverlay_) {
        uiOverlay_->invalidateViewportTexture();
    }
    cameraController_.releaseMouse(window_);

    pathTracer_.reset();
    assets_ = std::move(result.assets);
    importedScene_ = std::move(result.scene);
    gltfPath_ = result.path;
    sceneDocument_.importSceneAsset(*importedScene_);
    sceneDocument_.setSourceGltfPath(gltfPath_);
    sceneDocument_.setSourceHdrPath(hdrPath_);
    rebuildGpuSceneAsset();
    bool largeSceneSettingsChanged = false;
    RendererSettings reloadSettings = interactiveSettingsForScene(previousSettings, *importedScene_, assets_, largeSceneSettingsChanged);
    if (largeSceneSettingsChanged) {
        syncDocumentRenderSettings(sceneDocument_, reloadSettings);
    }
    createPathTracer(&reloadSettings);
    applyActiveSceneCamera();
    commandSystem_->setPathTracer(pathTracer_.get());

    sceneLoadingStatus_ = "Loaded glTF: " + result.path.string();
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
        if (requests.settings.has_value()) {
            applyRendererSettingsSafely(*requests.settings, false);
        }
        if (sceneDocument_.dirty() &&
            sceneDocument_.pendingUpdate() != SceneUpdateKind::FullSceneRebuild &&
            sceneDocument_.pendingUpdate() != SceneUpdateKind::TransformOnly) {
            const SceneGpuBuildResult build = sceneBuilder_.build(sceneDocument_, &assets_, pathTracer_->settings());
            applyRendererSettingsSafely(build.rendererSettings, false);
            if (build.updateKind != SceneUpdateKind::None) {
                pathTracer_->resetAccumulation(build.accumulationReason);
                sceneDocument_.clearDirty();
            }
        }
        if (requests.toggleDenoiser) {
            RendererSettings settings = pathTracer_->settings();
            settings.denoiserEnabled = !settings.denoiserEnabled;
            pathTracer_->applySettings(settings);
        }
        if (requests.toggleDebugView) {
            RendererSettings settings = pathTracer_->settings();
            settings.debugView = nextDebugView(settings.debugView);
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
        if (sceneDocument_.saveJson(*requests.saveSceneJson)) {
            std::cout << "Saved scene JSON: " << requests.saveSceneJson->string() << '\n';
        } else {
            std::cerr << "Scene JSON save failed: " << requests.saveSceneJson->string() << '\n';
        }
    }

    if (requests.loadSceneJson.has_value()) {
        if (sceneDocument_.loadJson(*requests.loadSceneJson)) {
            gltfPath_ = sceneDocument_.sourceGltfPath();
            hdrPath_ = sceneDocument_.sourceHdrPath();
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

    if (sceneDocument_.dirty() &&
        (sceneDocument_.pendingUpdate() == SceneUpdateKind::FullSceneRebuild ||
            sceneDocument_.pendingUpdate() == SceneUpdateKind::TransformOnly)) {
        const RendererSettings previousSettings = pathTracer_->settings();
        commandSystem_->waitIdle();
        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        rebuildGpuSceneAsset();
        pathTracer_.reset();
        createPathTracer(&previousSettings);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(AccumulationResetReason::SceneChanged);
        commandSystem_->setPathTracer(pathTracer_.get());
        sceneDocument_.clearDirty();
    }

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

void Application::applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return;
    }

    const RendererSettings current = pathTracer_->settings();
    const bool renderResolutionChanged =
        std::abs(settings.renderResolutionScale - current.renderResolutionScale) > 0.0001f;
    const bool backendChanged = settings.requestedBackend != current.requestedBackend;
    if (backendChanged && allowRenderResolutionChange) {
        commandSystem_->waitIdle();
        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        pathTracer_.reset();
        createPathTracer(&settings);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(AccumulationResetReason::BackendChanged);
        commandSystem_->setPathTracer(pathTracer_.get());
        return;
    }
    if (backendChanged) {
        pendingPostFrameSettings_ = settings;
        return;
    }
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
    }
        pathTracer_.reset();
        createPathTracer(&previousSettings);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(AccumulationResetReason::ShaderReloaded);
    commandSystem_->setPathTracer(pathTracer_.get());
    std::cout << "Reloaded shaders from editor.\n";
}

void Application::createPathTracer(const RendererSettings* settingsToRestore) {
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    const auto shaderOutDir = projectRoot / "native" / "vulkan" / "build" / "shaders";
    std::optional<std::filesystem::path> cachePath;
    if (gltfPath_.has_value()) {
        cachePath = SceneCache::cachePathFor(*gltfPath_);
    }
    pathTracer_ = std::make_unique<PathTracerRenderer>(
        *context_,
        *allocator_,
        *uploader_,
        swapchain_->format(),
        shaderDir,
        shaderOutDir,
        debugView_,
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
        hdrPath_,
        cachePath,
        settingsToRestore != nullptr ? settingsToRestore->requestedBackend : requestedBackend_);
    if (settingsToRestore != nullptr) {
        pathTracer_->applySettings(*settingsToRestore);
    }
}

void Application::applyActiveSceneCamera() {
    if (pathTracer_ == nullptr || !sceneDocument_.activeCamera().valid()) {
        return;
    }

    const Entity* cameraEntity = sceneDocument_.registry().entity(sceneDocument_.activeCamera());
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    const glm::mat4 transform = cameraEntity->transform.localMatrix();
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
    SceneGpuBuildResult build = sceneBuilder_.build(sceneDocument_, &assets_, settings);
    gpuSceneAsset_ = std::move(build.sceneAsset);
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
    sceneDocument_.markDirty(SceneUpdateKind::FullSceneRebuild);
}

void Application::processRuntimeControls(float deltaSeconds) {
    if (!pathTracer_) {
        return;
    }

    const bool uiWantsKeyboard = uiOverlay_ != nullptr && uiOverlay_->wantsKeyboard();
    const bool uiWantsTextInput = uiOverlay_ != nullptr && uiOverlay_->wantsTextInput();
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
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F1)) {
        settings.debugView = nextDebugView(settings.debugView);
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_0)) {
        settings.debugView = RendererDebugView::Beauty;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F11)) {
        toggleBorderlessFullscreen();
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F2)) {
        settings.denoiserEnabled = !settings.denoiserEnabled;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F3)) {
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F4)) {
        settings.sunlightEnabled = !settings.sunlightEnabled;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F5)) {
        settings.environmentEnabled = !settings.environmentEnabled;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_F6)) {
        settings.directLightingEnabled = !settings.directLightingEnabled;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_R)) {
        pathTracer_->resetAccumulation();
    }

    const float exposureRate = 0.9f * deltaSeconds;
    if (!uiWantsKeyboard && (glfwGetKey(window_, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_ADD) == GLFW_PRESS)) {
        settings.exposure += exposureRate;
        changed = true;
    }
    if (!uiWantsKeyboard && (glfwGetKey(window_, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS)) {
        settings.exposure = std::max(0.05f, settings.exposure - exposureRate);
        changed = true;
    }

    const float envRate = 1.2f * deltaSeconds;
    if (!uiWantsKeyboard && glfwGetKey(window_, GLFW_KEY_PERIOD) == GLFW_PRESS) {
        settings.environmentIntensity += envRate;
        changed = true;
    }
    if (!uiWantsKeyboard && glfwGetKey(window_, GLFW_KEY_COMMA) == GLFW_PRESS) {
        settings.environmentIntensity = std::max(0.0f, settings.environmentIntensity - envRate);
        changed = true;
    }
    const float rotationRate = 1.4f * deltaSeconds;
    if (!uiWantsKeyboard && glfwGetKey(window_, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation += rotationRate;
        changed = true;
    }
    if (!uiWantsKeyboard && glfwGetKey(window_, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation -= rotationRate;
        changed = true;
    }

    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_PAGE_UP)) {
        ++settings.maxBounces;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_PAGE_DOWN) && settings.maxBounces > 1) {
        --settings.maxBounces;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_HOME)) {
        ++settings.atrousIterations;
        changed = true;
    }
    if (!uiWantsKeyboard && pressedOnce(GLFW_KEY_END) && settings.atrousIterations > 1) {
        --settings.atrousIterations;
        changed = true;
    }

    if (changed && pathTracer_->applySettings(settings)) {
        std::cout << "Settings changed: debug=" << rendererDebugViewName(settings.debugView)
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
    title << "Ray Tracing Engine - Vulkan"
          << " | samples " << pathTracer_->sampleCount()
          << " | " << rendererDebugViewName(settings.debugView)
          << " | denoise " << (settings.denoiserEnabled ? "on" : "off")
          << " | sun " << (settings.sunlightEnabled ? "on" : "off")
          << " | env " << (settings.environmentEnabled ? "on" : "off")
          << " | GPU "
          << std::fixed << std::setprecision(2)
          << timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs << " ms";
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
