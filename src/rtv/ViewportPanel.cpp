#include "rtv/ViewportPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/RendererDebug.h"
#include "rtv/SceneOperations.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace rtv {

namespace {

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    if (!entity.parent.valid()) {
        return entity.transform.localMatrix();
    }
    const Entity* parent = registry.entity(entity.parent);
    if (parent == nullptr) {
        return entity.transform.localMatrix();
    }
    return entityWorldMatrix(registry, *parent) * entity.transform.localMatrix();
}

glm::mat4 parentWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* parent = registry.entity(entity.parent);
    return parent != nullptr ? entityWorldMatrix(registry, *parent) : glm::mat4{1.0f};
}

void writeLocalTransformFromMatrix(Entity& entity, const glm::mat4& matrix) {
    glm::vec3 skew{};
    glm::vec4 perspective{};
    glm::quat orientation{};
    glm::vec3 translation{};
    glm::vec3 scale{1.0f};
    if (!glm::decompose(matrix, scale, orientation, translation, skew, perspective)) {
        return;
    }
    entity.transform.position = translation;
    entity.transform.rotationEuler = glm::eulerAngles(glm::normalize(orientation));
    entity.transform.scale = scale;
    entity.transform.dirty = true;
}

float activeCameraFov(const SceneDocument& document) {
    const EntityId active = document.activeCamera();
    if (const Entity* cameraEntity = document.registry().entity(active)) {
        if (cameraEntity->camera.has_value()) {
            return cameraEntity->camera->verticalFovRadians;
        }
    }
    return 60.0f * 0.017453292519943295f;
}

bool cameraBasis(const CameraController& camera, glm::vec3& forward, glm::vec3& right, glm::vec3& up) {
    forward = glm::normalize(camera.direction());
    right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    if (glm::dot(right, right) <= 0.0001f) {
        right = {1.0f, 0.0f, 0.0f};
    } else {
        right = glm::normalize(right);
    }
    up = glm::normalize(glm::cross(right, forward));
    return glm::dot(forward, forward) > 0.0f && glm::dot(up, up) > 0.0f;
}

glm::mat4 editorViewMatrix(const CameraController& camera) {
    glm::vec3 forward{};
    glm::vec3 right{};
    glm::vec3 up{};
    cameraBasis(camera, forward, right, up);
    return glm::lookAtRH(camera.position(), camera.position() + forward, up);
}

glm::mat4 editorProjectionMatrix(float fovY, float aspect) {
    return glm::perspectiveRH_NO(fovY, aspect, 0.01f, 1000.0f);
}

float viewportAspect(const EditorRuntimeState& state) {
    const float w = static_cast<float>(state.viewport.imageSize.x);
    const float h = static_cast<float>(state.viewport.imageSize.y > 0u ? state.viewport.imageSize.y : 1u);
    return w / h;
}

SceneUpdateKind transformUpdateKind(const SceneDocument& document, const Entity& entity) {
    const bool hasMesh = entity.meshRenderer.has_value();
    const bool hasLight = entity.light.has_value();
    const bool hasSun = entity.sun.has_value();
    const bool hasActiveCamera = entity.camera.has_value() && document.activeCamera() == entity.id;
    if (((hasLight || hasSun) && hasMesh) || (hasActiveCamera && hasMesh) || (hasActiveCamera && (hasLight || hasSun))) {
        return SceneUpdateKind::TopologyChanged;
    }
    if (hasActiveCamera) {
        return SceneUpdateKind::CameraOnly;
    }
    if (hasLight || hasSun) {
        return SceneUpdateKind::LightOnly;
    }
    return SceneUpdateKind::TransformOnly;
}

std::optional<uint32_t> instanceForEntity(const EditorRuntimeState& state, EntityId entityId) {
    if (state.instanceEntities == nullptr || !entityId.valid()) {
        return std::nullopt;
    }
    for (uint32_t i = 0; i < state.instanceEntities->size(); ++i) {
        if ((*state.instanceEntities)[i] == entityId) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<EntityId> entityForInstance(const EditorRuntimeState& state, uint32_t instanceId) {
    if (state.instanceEntities == nullptr || instanceId >= state.instanceEntities->size()) {
        return std::nullopt;
    }
    return (*state.instanceEntities)[instanceId];
}

std::optional<ImVec2> projectViewToScreen(
    const EditorRuntimeState& state,
    const glm::mat4& projection,
    glm::vec3 viewPoint,
    float nearPlane) {
    if (viewPoint.z > -nearPlane) {
        return std::nullopt;
    }
    const glm::vec4 clip = projection * glm::vec4(viewPoint, 1.0f);
    if (clip.w <= 0.0f) {
        return std::nullopt;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return ImVec2{
        state.viewport.imageOrigin.x + (ndc.x * 0.5f + 0.5f) * state.viewport.imageSize.x,
        state.viewport.imageOrigin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * state.viewport.imageSize.y,
    };
}

void drawSelectedLightOverlay(
    const EditorRuntimeState& state,
    const glm::mat4& view,
    const glm::mat4& projection,
    const Entity& entity) {
    if (!entity.light.has_value()) {
        return;
    }

    constexpr float nearPlane = 0.01f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), entity);
    glm::vec3 center = glm::vec3(world[3]);
    const glm::vec3 viewCenter = glm::vec3(view * glm::vec4(center, 1.0f));
    const std::optional<ImVec2> screenCenter = projectViewToScreen(state, projection, viewCenter, nearPlane);
    if (!screenCenter.has_value()) {
        return;
    }

    drawList->AddCircle(*screenCenter, 8.0f, IM_COL32(255, 214, 80, 255), 24, 2.0f);
    drawList->AddLine(ImVec2(screenCenter->x - 10.0f, screenCenter->y), ImVec2(screenCenter->x + 10.0f, screenCenter->y), IM_COL32(255, 214, 80, 220), 2.0f);
    drawList->AddLine(ImVec2(screenCenter->x, screenCenter->y - 10.0f), ImVec2(screenCenter->x, screenCenter->y + 10.0f), IM_COL32(255, 214, 80, 220), 2.0f);

    const Light& light = *entity.light;
    if (light.type == LightType::Point) {
        glm::vec3 forward{};
        glm::vec3 right{};
        glm::vec3 up{};
        cameraBasis(*state.camera, forward, right, up);
        const glm::vec3 radiusPoint = center + right * std::max(light.sizeOrRadius, 0.05f);
        const std::optional<ImVec2> screenRadius = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(radiusPoint, 1.0f)), nearPlane);
        if (screenRadius.has_value()) {
            const float radius = std::max(8.0f, std::abs(screenRadius->x - screenCenter->x));
            drawList->AddCircle(*screenCenter, radius, IM_COL32(255, 214, 80, 120), 32, 1.5f);
        }
    } else if (light.type == LightType::Area) {
        const glm::vec3 axisX = glm::normalize(glm::mat3(world) * glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 axisY = glm::normalize(glm::mat3(world) * glm::vec3(0.0f, 1.0f, 0.0f));
        const float halfSize = std::max(light.sizeOrRadius * 0.5f, 0.05f);
        std::array<glm::vec3, 4> corners{
            center + axisX * halfSize + axisY * halfSize,
            center - axisX * halfSize + axisY * halfSize,
            center - axisX * halfSize - axisY * halfSize,
            center + axisX * halfSize - axisY * halfSize,
        };
        std::array<ImVec2, 4> screenCorners{};
        bool valid = true;
        for (size_t i = 0; i < corners.size(); ++i) {
            const std::optional<ImVec2> screen = projectViewToScreen(state, projection, glm::vec3(view * glm::vec4(corners[i], 1.0f)), nearPlane);
            if (!screen.has_value()) {
                valid = false;
                break;
            }
            screenCorners[i] = *screen;
        }
        if (valid) {
            drawList->AddPolyline(screenCorners.data(), static_cast<int>(screenCorners.size()), IM_COL32(255, 214, 80, 180), ImDrawFlags_Closed, 2.0f);
        }
    }
}

void drawSelectionOverlay(const EditorRuntimeState& state, const EditorSelection& selection) {
    if (state.sceneDocument == nullptr || state.camera == nullptr || !selection.entityId().valid()) {
        return;
    }
    const Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
    if (entity == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(*state.camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));
    if (entity->light.has_value()) {
        drawSelectedLightOverlay(state, view, projection, *entity);
    }
}

void drawGridOverlay(const EditorRuntimeState& state, const CameraController& camera) {
    if (state.sceneDocument == nullptr) {
        return;
    }
    const glm::mat4 view = editorViewMatrix(camera);
    const glm::mat4 projection = editorProjectionMatrix(activeCameraFov(*state.sceneDocument), viewportAspect(state));

    const glm::vec3 camPos = camera.position();
    const float ox = state.viewport.imageOrigin.x;
    const float oy = state.viewport.imageOrigin.y;
    const float iw = static_cast<float>(state.viewport.imageSize.x);
    const float ih = static_cast<float>(state.viewport.imageSize.y);

    auto clip = [&](glm::vec3 wp) -> glm::vec4 {
        return projection * view * glm::vec4(wp, 1.0f);
    };
    auto screen = [&](glm::vec4 c) -> ImVec2 {
        glm::vec3 ndc = glm::vec3(c) / c.w;
        return ImVec2(ox + (ndc.x * 0.5f + 0.5f) * iw, oy + (1.0f - (ndc.y * 0.5f + 0.5f)) * ih);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto drawClippedLine = [&](glm::vec3 a, glm::vec3 b, ImU32 color, float thk) {
        glm::vec4 ca = clip(a);
        glm::vec4 cb = clip(b);

        auto visibleNear = [](const glm::vec4& c) {
            return c.w > 0.0001f && c.z >= -c.w;
        };

        bool va = visibleNear(ca);
        bool vb = visibleNear(cb);

        if (!va && !vb) {
            return;
        }

        if (va && vb) {
            dl->AddLine(screen(ca), screen(cb), color, thk);
            return;
        }

        float da = ca.z + ca.w;
        float db = cb.z + cb.w;
        float t = da / (da - db);
        t = std::clamp(t, 0.0f, 1.0f);

        glm::vec4 ci = ca + (cb - ca) * t;

        if (!va) ca = ci;
        else     cb = ci;

        dl->AddLine(screen(ca), screen(cb), color, thk);
    };

    constexpr float halfExtent = 20.0f;
    constexpr float stepSz     = 1.0f;
    constexpr int   halfSteps  = static_cast<int>(halfExtent / stepSz);

    const float cx = std::floor(camPos.x / stepSz) * stepSz;
    const float cz = std::floor(camPos.z / stepSz) * stepSz;

    for (int i = -halfSteps; i <= halfSteps; ++i) {
        const float pos  = static_cast<float>(i) * stepSz;
        const bool major = (i % 5) == 0;
        const ImU32 color = major ? IM_COL32(140, 140, 140, 160) : IM_COL32(80, 80, 80, 70);
        const float thk   = major ? 1.5f : 0.7f;

        drawClippedLine(glm::vec3(cx + pos, 0.0f, cz - halfExtent),
                        glm::vec3(cx + pos, 0.0f, cz + halfExtent), color, thk);
        drawClippedLine(glm::vec3(cx - halfExtent, 0.0f, cz + pos),
                        glm::vec3(cx + halfExtent, 0.0f, cz + pos), color, thk);
    }
}

void drawAxesIndicator(const EditorRuntimeState& state, const CameraController& camera) {
    const float size = 48.0f, margin = 14.0f;
    const float ox = state.viewport.imageOrigin.x;
    const float oy = state.viewport.imageOrigin.y;
    const float iw = static_cast<float>(state.viewport.imageSize.x);
    const ImVec2 origin(ox + iw - margin - size, oy + margin + size);

    const glm::mat3 rot = glm::mat3(editorViewMatrix(camera));
    const glm::vec3 xDir = rot * glm::vec3(1,0,0);
    const glm::vec3 yDir = rot * glm::vec3(0,1,0);
    const glm::vec3 zDir = rot * glm::vec3(0,0,1);

    struct AxisItem { const char* label; ImU32 color; float depth; ImVec2 tip; };
    auto makeItem = [&](glm::vec3 d, const char* lbl, ImU32 clr) -> AxisItem {
        ImVec2 t(origin.x + d.x * size * 0.7f, origin.y - d.y * size * 0.7f);
        return {lbl, clr, d.z, t};
    };

    AxisItem axes[3] = {
        makeItem(xDir, "X", IM_COL32(255,80,80,255)),
        makeItem(yDir, "Y", IM_COL32(80,255,80,255)),
        makeItem(zDir, "Z", IM_COL32(80,80,255,255)),
    };
    std::sort(std::begin(axes), std::end(axes),
              [](const AxisItem& a, const AxisItem& b) { return a.depth < b.depth; });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(origin, 2.5f, IM_COL32(200,200,200,255));
    for (const auto& ax : axes) {
        dl->AddLine(origin, ax.tip, ax.color, 2.0f);
        dl->AddText(nullptr, 13.0f, {ax.tip.x+3,ax.tip.y-7}, ax.color, ax.label);
    }
}

} // namespace

void ViewportPanel::draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Viewport")) {
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        state.viewport.focused = focused_;
        state.viewport.hovered = hovered_;

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        lastContentExtent_.width = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.x)));
        lastContentExtent_.height = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.y)));
        state.viewport.imageOrigin = {imagePos.x, imagePos.y};
        state.viewport.imageSize = {avail.x, avail.y};
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        state.viewport.mousePosition = {mousePos.x, mousePos.y};
        state.viewport.mouseUv = {
            avail.x > 0.0f ? std::clamp((mousePos.x - imagePos.x) / avail.x, 0.0f, 1.0f) : 0.0f,
            avail.y > 0.0f ? std::clamp((mousePos.y - imagePos.y) / avail.y, 0.0f, 1.0f) : 0.0f,
        };
        state.viewport.leftClicked = hovered_ && !state.viewport.mouseCaptureActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        VkExtent2D expectedExtent = lastContentExtent_;
        const float scale = state.renderer.settings().renderResolutionScale;
        expectedExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.width) * scale));
        expectedExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.height) * scale));
        const bool imageMatchesPanel =
            expectedExtent.width == state.viewport.renderExtent.width &&
            expectedExtent.height == state.viewport.renderExtent.height;

        if (imageMatchesPanel && state.viewport.textureReady && state.viewport.texture != VK_NULL_HANDLE) {
            ImGui::Image(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state.viewport.texture)),
                avail,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        } else {
            ImGui::Dummy(avail);
            ImGui::GetWindowDrawList()->AddRectFilled(
                imagePos,
                ImVec2(imagePos.x + avail.x, imagePos.y + avail.y),
                IM_COL32(18, 20, 23, 255));
        }

        const RendererSettings& settings = state.renderer.settings();
        const GpuFrameTimings& timings = state.renderer.timings();
        const VkExtent2D extent = state.viewport.renderExtent;
        bool gizmoHoveredOrUsing = false;

        if (focused_ && !state.viewport.mouseCaptureActive && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_T)) {
                transformGizmoMode_ = 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                transformGizmoMode_ = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                transformGizmoMode_ = 2;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L)) {
                localGizmoMode_ = !localGizmoMode_;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_G)) {
                showGrid_ = !showGrid_;
            }
        }
        const float gpuTotal = timings.pathTraceMs + timings.denoiserMs + timings.historyCopyMs +
            timings.taaMs + timings.autoExposureMs + timings.toneMapMs + timings.selectionOutlineMs +
            timings.fullscreenMs;
        const ImU32 perfColor = gpuTotal < 16.0f ? IM_COL32(120, 220, 120, 255)
                              : gpuTotal < 33.0f ? IM_COL32(240, 220, 100, 255)
                              : IM_COL32(240, 100, 100, 255);

        const int numHudLines = 6;
        const float hudLineH = 22.0f;
        const float hudPad = 8.0f;
        const float hudW = 360.0f;
        const float hudH = hudPad * 2.0f + hudLineH * static_cast<float>(numHudLines);
        const float hudX = imagePos.x + 12.0f;
        const float hudY = imagePos.y + 12.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(hudX, hudY), ImVec2(hudX + hudW, hudY + hudH),
            IM_COL32(0, 0, 0, 160), 6.0f);

        auto hudText = [&](int line, const char* text, ImU32 color = IM_COL32(200, 200, 200, 255)) {
            dl->AddText(ImVec2(hudX + hudPad, hudY + hudPad + hudLineH * static_cast<float>(line)),
                color, text);
        };

        std::ostringstream perfFmt;
        perfFmt << std::fixed << std::setprecision(2);
        perfFmt << "GPU: " << gpuTotal << " ms  (trace " << timings.pathTraceMs
                << "  denoise " << timings.denoiserMs
                << "  TAA " << timings.taaMs
                << "  tone " << timings.toneMapMs << ")";
        hudText(0, perfFmt.str().c_str(), perfColor);

        hudText(1, ("Samples: " + std::to_string(state.renderer.sampleCount())).c_str());

        if (state.viewport.mouseCaptureActive) {
            hudText(2, "Moving - accumulation paused", IM_COL32(255, 200, 60, 255));
        } else {
            const char* debugViewName = rendererDebugViewName(settings.debugView);
            hudText(2, debugViewName,
                settings.debugView == RendererDebugView::Beauty ? IM_COL32(180, 200, 230, 255) : IM_COL32(240, 180, 100, 255));
        }

        std::ostringstream statusFmt;
        if (settings.denoiserEnabled) statusFmt << "Denoiser: ON  ";
        else statusFmt << "Denoiser: OFF  ";
        if (settings.taaEnabled) statusFmt << "TAA: ON";
        else statusFmt << "TAA: OFF";
        hudText(3, statusFmt.str().c_str(), IM_COL32(150, 200, 255, 200));

        {
            std::ostringstream resFmt;
            resFmt << extent.width << "x" << extent.height
                   << "  HW RT"
                   << "  reset:" << accumulationResetReasonName(state.renderer.lastAccumulationResetReason());
            hudText(4, resFmt.str().c_str(), IM_COL32(140, 140, 140, 220));
        }

        const uint32_t accumLimit = settings.accumulationLimit;
        if (accumLimit > 0u) {
            std::ostringstream accumFmt;
            const float accumProgress = static_cast<float>(state.renderer.sampleCount()) / static_cast<float>(accumLimit);
            accumFmt << "Accumulation: " << static_cast<int>(accumProgress * 100.0f) << "%";
            hudText(5, accumFmt.str().c_str(), IM_COL32(200, 220, 200, 220));
        } else {
            const uint32_t currentSamples = state.renderer.sampleCount();
            bool flashReset = lastSampleCount_ > 0u && currentSamples <= lastSampleCount_;
            lastSampleCount_ = currentSamples;
            if (flashReset) {
                const char* reason = accumulationResetReasonName(state.renderer.lastAccumulationResetReason());
                std::ostringstream resetFmt;
                resetFmt << "Reset: " << reason;
                hudText(5, resetFmt.str().c_str(), IM_COL32(255, 140, 80, 250));
            } else {
                hudText(5, "", IM_COL32(0, 0, 0, 0));
            }
        }

        drawSelectionOverlay(state, selection);

        if (state.camera != nullptr) {
            if (showAxes_) drawAxesIndicator(state, *state.camera);
            if (showGrid_) drawGridOverlay(state, *state.camera);
        }

        if (state.sceneDocument != nullptr && selection.entityId().valid()) {
            Entity* entity = state.sceneDocument->registry().entity(selection.entityId());
            if (entity != nullptr && !entity->locked && state.camera != nullptr) {
                ImGui::SetCursorScreenPos(ImVec2(imagePos.x + 370.0f, imagePos.y + 10.0f));
                ImGui::BeginGroup();
                if (ImGui::RadioButton("T", transformGizmoMode_ == 0)) {
                    transformGizmoMode_ = 0;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("R", transformGizmoMode_ == 1)) {
                    transformGizmoMode_ = 1;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("S", transformGizmoMode_ == 2)) {
                    transformGizmoMode_ = 2;
                }
                ImGui::SameLine();
                ImGui::Checkbox("Local", &localGizmoMode_);
                ImGui::SameLine();
                ImGui::Checkbox("Snap", &snap_.enabled);
                if (snap_.enabled) {
                    ImGui::SameLine();
                    if (transformGizmoMode_ == 0) {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapTranslate", &snap_.translation, 0.01f, 0.001f, 100.0f, "%.2f");
                    } else if (transformGizmoMode_ == 1) {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapRotate", &snap_.rotation, 1.0f, 0.1f, 180.0f, "%.0f");
                    } else {
                        ImGui::SetNextItemWidth(74.0f);
                        ImGui::DragFloat("##snapScale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.2f");
                    }
                }
                ImGui::EndGroup();

                const glm::mat4 view = editorViewMatrix(*state.camera);
                const glm::mat4 projection = editorProjectionMatrix(
                    activeCameraFov(*state.sceneDocument),
                    viewportAspect(state));

                glm::mat4 world = entityWorldMatrix(state.sceneDocument->registry(), *entity);
                const ImGuizmo::OPERATION operation = transformGizmoMode_ == 0
                    ? ImGuizmo::TRANSLATE
                    : (transformGizmoMode_ == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
                const glm::mat4 previousWorld = world;
                ImGuizmo::BeginFrame();
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist(dl);
                ImGuizmo::SetRect(state.viewport.imageOrigin.x, state.viewport.imageOrigin.y, state.viewport.imageSize.x, state.viewport.imageSize.y);
                float snapValues[3] = {};
                if (snap_.enabled) {
                    const float value = transformGizmoMode_ == 0
                        ? snap_.translation
                        : (transformGizmoMode_ == 1 ? snap_.rotation : snap_.scale);
                    snapValues[0] = value;
                    snapValues[1] = value;
                    snapValues[2] = value;
                }
                const bool manipulated = ImGuizmo::Manipulate(
                    glm::value_ptr(view),
                    glm::value_ptr(projection),
                    operation,
                    localGizmoMode_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                    glm::value_ptr(world),
                    nullptr,
                    snap_.enabled ? snapValues : nullptr);
                const bool isOver = ImGuizmo::IsOver();
                const bool isUsing = ImGuizmo::IsUsing();
                gizmoHoveredOrUsing = isOver || isUsing;

                if (isUsing && gizmoState_ == GizmoInteractionState::Idle) {
                    gizmoDragActive_ = true;
                    gizmoDragEntity_ = entity->id;
                    gizmoDragOriginal_ = entity->transform;
                }

                if (manipulated && world != previousWorld) {
                    const glm::mat4 local = glm::inverse(parentWorldMatrix(state.sceneDocument->registry(), *entity)) * world;
                    writeLocalTransformFromMatrix(*entity, local);
                    const SceneUpdateKind updateKind = transformUpdateKind(*state.sceneDocument, *entity);
                    state.sceneDocument->markDirty(updateKind);
                    requests.sceneUpdate = updateKind;
                    requests.previewEntityTransform = EditorEntityTransformPreview{
                        .entity = entity->id,
                        .transform = entity->transform,
                        .updateKind = updateKind,
                    };
                }

                updateGizmoState(isOver, isUsing, transformGizmoMode_);

                if (isUsing) {
                    const char* label = transformGizmoMode_ == 0
                        ? "Moving selection"
                        : (transformGizmoMode_ == 1 ? "Rotating selection" : "Scaling selection");
                    const ImVec2 textSize = ImGui::CalcTextSize(label);
                    const ImVec2 labelPos(
                        state.viewport.imageOrigin.x + state.viewport.imageSize.x * 0.5f - textSize.x * 0.5f - 10.0f,
                        state.viewport.imageOrigin.y + state.viewport.imageSize.y - 52.0f);
                    dl->AddRectFilled(
                        labelPos,
                        ImVec2(labelPos.x + textSize.x + 20.0f, labelPos.y + textSize.y + 12.0f),
                        IM_COL32(20, 24, 28, 210),
                        5.0f);
                    dl->AddText(
                        ImVec2(labelPos.x + 10.0f, labelPos.y + 6.0f),
                        IM_COL32(170, 215, 255, 255),
                        label);
                }

                if (!isUsing && gizmoDragActive_) {
                    commitGizmoDrag(requests, *state.sceneDocument);
                }
            }
        }

        if (state.viewport.leftClicked && !gizmoHoveredOrUsing) {
            if (const std::optional<uint32_t> pickedInstance = state.renderer.pickInstanceId(state.viewport.mouseUv)) {
                if (const std::optional<EntityId> pickedEntity = entityForInstance(state, *pickedInstance)) {
                    const Entity* entity = state.sceneDocument != nullptr ? state.sceneDocument->registry().entity(*pickedEntity) : nullptr;
                    if (entity == nullptr || !entity->locked) {
                        selection.selectEntity(*pickedEntity);
                    }
                }
            }
        }

        const bool gizmoDragging = gizmoState_ == GizmoInteractionState::DraggingTranslate
            || gizmoState_ == GizmoInteractionState::DraggingRotate
            || gizmoState_ == GizmoInteractionState::DraggingScale;
        state.viewport.mouseCaptureActive = state.viewport.mouseCaptureActive || gizmoDragging;

        state.renderer.setSelectedInstanceId(instanceForEntity(state, selection.entityId()));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

VkExtent2D ViewportPanel::desiredRenderExtent(VkExtent2D fallback) const {
    if (lastContentExtent_.width == 0 || lastContentExtent_.height == 0) {
        return fallback;
    }
    return lastContentExtent_;
}

void ViewportPanel::commitGizmoDrag(EditorRequests& requests, SceneDocument& document) {
    if (!gizmoDragActive_) {
        return;
    }
    Entity* entity = document.registry().entity(gizmoDragEntity_);
    if (entity != nullptr) {
        const Transform finalTransform = entity->transform;
        const SceneUpdateKind updateKind = transformUpdateKind(document, *entity);
        document.markDirty(updateKind);
        requests.sceneUpdate = updateKind;
        requests.setEntityTransform = EditorEntityTransformChange{
            .entity = gizmoDragEntity_,
            .oldTransform = gizmoDragOriginal_,
            .newTransform = finalTransform,
        };
    }
    gizmoDragActive_ = false;
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
}

void ViewportPanel::abortGizmoDrag() {
    if (!gizmoDragActive_) {
        return;
    }
    gizmoDragActive_ = false;
    gizmoDragEntity_ = {};
    gizmoDragOriginal_ = {};
}

void ViewportPanel::updateGizmoState(bool isOver, bool isUsing, int gizmoMode) {
    switch (gizmoState_) {
        case GizmoInteractionState::Idle:
            if (isUsing) {
                gizmoState_ = gizmoMode == 0 ? GizmoInteractionState::DraggingTranslate
                    : (gizmoMode == 1 ? GizmoInteractionState::DraggingRotate
                    : GizmoInteractionState::DraggingScale);
            } else if (isOver) {
                gizmoState_ = GizmoInteractionState::Hovered;
            }
            break;
        case GizmoInteractionState::Hovered:
            if (isUsing) {
                gizmoState_ = gizmoMode == 0 ? GizmoInteractionState::DraggingTranslate
                    : (gizmoMode == 1 ? GizmoInteractionState::DraggingRotate
                    : GizmoInteractionState::DraggingScale);
            } else if (!isOver) {
                gizmoState_ = GizmoInteractionState::Idle;
            }
            break;
        case GizmoInteractionState::DraggingTranslate:
        case GizmoInteractionState::DraggingRotate:
        case GizmoInteractionState::DraggingScale:
            if (!isUsing) {
                gizmoState_ = isOver ? GizmoInteractionState::Hovered : GizmoInteractionState::Idle;
            }
            break;
    }
}

} // namespace rtv
