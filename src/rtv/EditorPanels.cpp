#include "rtv/EditorPanels.h"

#include "rtv/RendererDebug.h"

#include <imgui.h>

namespace rtv {

const std::array<RendererDebugView, 28>& editorDebugViews() {
    static constexpr std::array<RendererDebugView, 28> views = {
        RendererDebugView::Beauty,
        RendererDebugView::Variance,
        RendererDebugView::Normals,
        RendererDebugView::ReprojectionConfidence,
        RendererDebugView::DenoiserRejection,
        RendererDebugView::Depth,
        RendererDebugView::Roughness,
        RendererDebugView::DirectLighting,
        RendererDebugView::IndirectLighting,
        RendererDebugView::EmissiveContribution,
        RendererDebugView::EnvironmentContribution,
        RendererDebugView::TraversalSteps,
        RendererDebugView::BvhDepth,
        RendererDebugView::InstanceId,
        RendererDebugView::MeshId,
        RendererDebugView::TlasSteps,
        RendererDebugView::TraversalMismatch,
        RendererDebugView::LightPdf,
        RendererDebugView::BsdfPdf,
        RendererDebugView::MisWeight,
        RendererDebugView::DirectSampleType,
        RendererDebugView::Albedo,
        RendererDebugView::ClayMaterial,
        RendererDebugView::FirstBounceThroughput,
        RendererDebugView::SecondaryEnvironmentMiss,
        RendererDebugView::BounceCount,
        RendererDebugView::SecondaryEnvironmentRadiance,
        RendererDebugView::WhiteEnvironmentTransport,
    };
    return views;
}

int editorDebugViewIndex(RendererDebugView view) {
    const auto& views = editorDebugViews();
    for (int i = 0; i < static_cast<int>(views.size()); ++i) {
        if (views[static_cast<size_t>(i)] == view) {
            return i;
        }
    }
    return 0;
}

void editorDebugViewCombo(const char* label, RendererSettings& settings, bool& changed) {
    int selectedDebug = editorDebugViewIndex(settings.debugView);
    if (ImGui::BeginCombo(label, rendererDebugViewName(settings.debugView))) {
        const auto& views = editorDebugViews();
        for (int i = 0; i < static_cast<int>(views.size()); ++i) {
            const bool selected = i == selectedDebug;
            if (ImGui::Selectable(rendererDebugViewName(views[static_cast<size_t>(i)]), selected)) {
                selectedDebug = i;
                settings.debugView = views[static_cast<size_t>(i)];
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void requestSettings(EditorRequests& requests, const RendererSettings& settings) {
    requests.settings = settings;
}

} // namespace rtv
