#include "rtv/EditorDockspace.h"

#include "rtv/FileDialog.h"
#include "rtv/KeyBindings.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace rtv {

void EditorDockspace::begin(EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawMainMenu(visibility, requests);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockspaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    if (layoutResetRequested_) {
        buildDefaultLayout();
        layoutResetRequested_ = false;
        requests.resetLayout = false;
    }
    drawHelpWindows();
}

void EditorDockspace::end() {
    ImGui::End();
}

void EditorDockspace::requestResetLayout() {
    layoutResetRequested_ = true;
}

void EditorDockspace::setProfilePath(const std::filesystem::path& scenePath) {
    std::filesystem::path next = scenePath;
    if (next.empty()) {
        return;
    }
    next.replace_extension(".layout.ini");
    if (next == profilePath_) {
        return;
    }
    profilePath_ = std::move(next);
    loadLayout();
}

void EditorDockspace::saveLayout() const {
    if (!profilePath_.empty()) {
        ImGui::SaveIniSettingsToDisk(profilePath_.string().c_str());
    }
}

void EditorDockspace::loadLayout() {
    if (!profilePath_.empty() && std::filesystem::exists(profilePath_)) {
        ImGui::LoadIniSettingsFromDisk(profilePath_.string().c_str());
        layoutResetRequested_ = false;
    }
}

void EditorDockspace::buildDefaultLayout() {
    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID rightBottom = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, &bottom, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &rightBottom, &right);

    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector / Properties", right);
    ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
    ImGui::DockBuilderDockWindow("Asset Browser", bottom);
    ImGui::DockBuilderDockWindow("Render Settings", bottom);
    ImGui::DockBuilderDockWindow("Debug / Profiler", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockspace::drawMainMenu(EditorPanelVisibility& visibility, EditorRequests& requests) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open glTF\tCtrl+O")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.loadGltf = *path;
            }
        }
        if (ImGui::MenuItem("Open HDR\tCtrl+H")) {
            visibility.assetBrowser = true;
            if (auto path = openHdrFileDialog()) {
                requests.loadHdr = *path;
            }
        }
        if (ImGui::MenuItem("Open Level")) {
            visibility.assetBrowser = true;
            if (auto path = openSceneJsonFileDialog()) {
                requests.loadSceneJson = *path;
            }
        }
        if (ImGui::MenuItem("Save Level\tCtrl+S")) {
            visibility.assetBrowser = true;
            if (auto path = saveSceneJsonFileDialog()) {
                requests.saveSceneJson = *path;
                setProfilePath(*path);
                saveLayout();
            }
        }
        if (ImGui::MenuItem("Save Layout")) {
            requests.saveLayout = true;
            saveLayout();
        }
        if (ImGui::MenuItem("Reload Shaders\tCtrl+R")) {
            requests.reloadShaders = true;
            requests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (ImGui::MenuItem("Reset Layout")) {
            requests.resetLayout = true;
            requestResetLayout();
        }
        if (ImGui::MenuItem("Exit\tAlt+F4")) {
            requests.exit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo\tCtrl+Z")) {
            requests.undo = true;
        }
        if (ImGui::MenuItem("Redo\tCtrl+Y")) {
            requests.redo = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &visibility.viewport);
        ImGui::MenuItem("Scene Hierarchy", nullptr, &visibility.sceneHierarchy);
        ImGui::MenuItem("Inspector / Properties", nullptr, &visibility.inspector);
        ImGui::MenuItem("Asset Browser", nullptr, &visibility.assetBrowser);
        ImGui::MenuItem("Material Editor", nullptr, &visibility.materialEditor);
        ImGui::MenuItem("Render Settings", nullptr, &visibility.renderSettings);
        ImGui::MenuItem("Debug / Profiler", nullptr, &visibility.debugProfiler);
        ImGui::MenuItem("Scene Stats", nullptr, &visibility.sceneStats);
        ImGui::MenuItem("GPU Diagnostics", nullptr, &visibility.gpuDiagnostics);
        if (ImGui::MenuItem("Reset Dock Layout")) {
            requests.resetLayout = true;
            requestResetLayout();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Reset Accumulation\tR")) {
            requests.resetAccumulation = AccumulationResetReason::Manual;
        }
        if (ImGui::MenuItem("Toggle Denoiser\tF2")) {
            requests.toggleDenoiser = true;
        }
        if (ImGui::MenuItem("Cycle Debug View\tF1")) {
            requests.toggleDebugView = true;
        }
        if (ImGui::MenuItem("Cycle Intermediate Views\tF7")) {
            requests.cycleIntermediateView = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void EditorDockspace::drawHelpWindows() {
    if (showControls_) {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &showControls_)) {
            std::string currentCategory;
            bool categoryOpen = false;
            for (const KeyBinding& binding : allKeyBindings()) {
                if (binding.category != currentCategory) {
                    currentCategory = binding.category;
                    categoryOpen = ImGui::CollapsingHeader(currentCategory.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (categoryOpen) {
                    ImGui::BulletText("%s: %s", binding.key.c_str(), binding.description.c_str());
                }
            }
        }
        ImGui::End();
    }
    if (showRendererInfo_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 160.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Info", &showRendererInfo_)) {
            ImGui::TextUnformatted("Hardware RT path tracing, temporal denoising, debug views, glTF loading, HDR environments, and GPU profiling are owned by the existing renderer.");
            ImGui::TextUnformatted("The editor layer submits requests and displays renderer state without replacing the render pipeline.");
        }
        ImGui::End();
    }
}

} // namespace rtv
