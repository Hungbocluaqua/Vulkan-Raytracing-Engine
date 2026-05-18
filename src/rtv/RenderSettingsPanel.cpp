#include "rtv/RenderSettingsPanel.h"

#include <imgui.h>

#include <cmath>

namespace rtv {

void RenderSettingsPanel::draw(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Render Settings")) {
        ImGui::End();
        return;
    }

    RendererSettings settings = state.renderer.settings();
    if (state.sceneDocument != nullptr) {
        const RenderSettings& render = state.sceneDocument->renderSettings();
        const Environment& environment = state.sceneDocument->environment();
        settings.pathTracingEnabled = render.pathTracingEnabled;
        settings.directLightingEnabled = render.directLightingEnabled;
        settings.maxBounces = render.maxBounces;
        settings.environmentDirectSamples = render.environmentDirectSamples;
        settings.exposure = render.exposure;
        settings.sunlightEnabled = render.sunlightEnabled;
        settings.sunIntensity = render.sunIntensity;
        settings.skyIntensity = render.skyIntensity;
        settings.sunAngularRadius = render.sunAngularRadius;
        settings.indirectStrength = render.indirectStrength;
        settings.denoiserEnabled = render.denoiserEnabled;
        settings.atrousIterations = render.atrousIterations;
        settings.denoiserStrength = render.denoiserStrength;
        settings.debugView = render.debugView;
        settings.renderResolutionScale = render.resolutionScale;
        settings.requestedBackend = render.requestedBackend;
        settings.environmentEnabled = environment.enabled;
        settings.environmentIntensity = environment.intensity;
        settings.environmentRotation = environment.rotation;
        settings.environmentBackgroundIntensity = environment.backgroundIntensity;
    }
    bool changed = false;
    uint32_t minBounces = 1;
    uint32_t maxBounces = 16;
    uint32_t minEnvSamples = 1;
    uint32_t maxEnvSamples = 8;
    uint32_t minAtrous = 1;
    uint32_t maxAtrous = 5;

    ImGui::SeparatorText("Rendering");
    const char* backendItems[] = {"Auto", "Compute", "Hardware Ray Tracing"};
    int backendIndex = settings.requestedBackend == RendererBackend::Compute
        ? 1
        : (settings.requestedBackend == RendererBackend::HardwareRayTracing ? 2 : 0);
    if (ImGui::Combo("Backend", &backendIndex, backendItems, 3)) {
        if (backendIndex == 2 && !state.renderer.hardwareRayTracingAvailable()) {
            settings.requestedBackend = RendererBackend::Auto;
        } else {
            settings.requestedBackend = backendIndex == 1
                ? RendererBackend::Compute
                : (backendIndex == 2 ? RendererBackend::HardwareRayTracing : RendererBackend::Auto);
        }
        changed = true;
    }
    ImGui::Text("Active Backend: %s", rendererBackendDisplayName(state.renderer.activeBackend()));
    ImGui::Text("Hardware RT: %s", state.renderer.hardwareRayTracingAvailable() ? "Available" : "Unavailable");
    editorDebugViewCombo("Debug View", settings, changed);
    changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 4.0f, "%.2f");
    changed |= ImGui::SliderScalar("Max Bounces", ImGuiDataType_U32, &settings.maxBounces, &minBounces, &maxBounces);
    changed |= ImGui::SliderScalar("Environment Samples", ImGuiDataType_U32, &settings.environmentDirectSamples, &minEnvSamples, &maxEnvSamples);
    changed |= ImGui::Checkbox("Path Tracing", &settings.pathTracingEnabled);
    changed |= ImGui::Checkbox("Direct Lighting", &settings.directLightingEnabled);
    changed |= ImGui::SliderFloat("Indirect Strength", &settings.indirectStrength, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Render Resolution Scale", &settings.renderResolutionScale, 0.25f, 1.0f, "%.2f");

    ImGui::SeparatorText("Sun");
    changed |= ImGui::Checkbox("Sunlight", &settings.sunlightEnabled);
    changed |= ImGui::SliderFloat("Sun Intensity", &settings.sunIntensity, 0.0f, 10.0f, "%.2f");
    changed |= ImGui::SliderFloat("Sun Size", &settings.sunAngularRadius, 0.0f, 0.08f, "%.4f");
    changed |= ImGui::SliderFloat("Sky Intensity", &settings.skyIntensity, 0.0f, 3.0f, "%.2f");

    ImGui::SeparatorText("Environment");
    changed |= ImGui::Checkbox("Environment", &settings.environmentEnabled);
    changed |= ImGui::SliderFloat("Environment Intensity", &settings.environmentIntensity, 0.0f, 8.0f, "%.2f");
    changed |= ImGui::SliderFloat("Background Intensity", &settings.environmentBackgroundIntensity, 0.0f, 2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Environment Rotation", &settings.environmentRotation, -6.28318f, 6.28318f, "%.2f");

    ImGui::SeparatorText("Denoiser");
    changed |= ImGui::Checkbox("Denoiser", &settings.denoiserEnabled);
    changed |= ImGui::Checkbox("Denoise While Moving", &settings.denoiseWhileMoving);
    changed |= ImGui::SliderScalar("A-trous Iterations", ImGuiDataType_U32, &settings.atrousIterations, &minAtrous, &maxAtrous);
    changed |= ImGui::SliderFloat("Denoiser Strength", &settings.denoiserStrength, 0.05f, 4.0f, "%.2f");

    if (ImGui::Button("Reset Accumulation")) {
        requests.resetAccumulation = AccumulationResetReason::Manual;
    }

    if (changed) {
        if (state.sceneDocument != nullptr) {
            RenderSettings& render = state.sceneDocument->renderSettings();
            Environment& environment = state.sceneDocument->environment();
            const bool environmentChanged =
                environment.enabled != settings.environmentEnabled ||
                std::abs(environment.intensity - settings.environmentIntensity) > 0.0001f ||
                std::abs(environment.rotation - settings.environmentRotation) > 0.0001f ||
                std::abs(environment.backgroundIntensity - settings.environmentBackgroundIntensity) > 0.0001f;
            const bool lightingChanged =
                render.sunlightEnabled != settings.sunlightEnabled ||
                render.directLightingEnabled != settings.directLightingEnabled ||
                render.environmentDirectSamples != settings.environmentDirectSamples ||
                std::abs(render.sunIntensity - settings.sunIntensity) > 0.0001f ||
                std::abs(render.skyIntensity - settings.skyIntensity) > 0.0001f ||
                std::abs(render.sunAngularRadius - settings.sunAngularRadius) > 0.0001f;
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
            environment.enabled = settings.environmentEnabled;
            environment.intensity = settings.environmentIntensity;
            environment.rotation = settings.environmentRotation;
            environment.backgroundIntensity = settings.environmentBackgroundIntensity;
            const SceneUpdateKind kind = environmentChanged
                ? SceneUpdateKind::EnvironmentOnly
                : (lightingChanged ? SceneUpdateKind::LightOnly : SceneUpdateKind::CameraOnly);
            state.sceneDocument->markDirty(kind);
            requests.sceneUpdate = kind;
        }
        requestSettings(requests, settings);
    }

    ImGui::End();
}

} // namespace rtv
