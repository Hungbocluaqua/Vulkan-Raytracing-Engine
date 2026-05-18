#include "rtv/SceneToGpuSceneBuilder.h"

#include "rtv/AssetManager.h"

namespace rtv {

SceneGpuBuildResult SceneToGpuSceneBuilder::build(
    const SceneDocument& document,
    const AssetManager*,
    const RendererSettings& currentSettings) const {
    SceneGpuBuildResult result;
    result.updateKind = document.pendingUpdate();
    result.sceneAsset = document.toSceneAsset();
    result.rendererSettings = currentSettings;

    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    result.rendererSettings.pathTracingEnabled = render.pathTracingEnabled;
    result.rendererSettings.directLightingEnabled = render.directLightingEnabled;
    result.rendererSettings.maxBounces = render.maxBounces;
    result.rendererSettings.environmentDirectSamples = render.environmentDirectSamples;
    result.rendererSettings.exposure = render.exposure;
    result.rendererSettings.sunlightEnabled = render.sunlightEnabled;
    result.rendererSettings.sunIntensity = render.sunIntensity;
    result.rendererSettings.skyIntensity = render.skyIntensity;
    result.rendererSettings.sunAngularRadius = render.sunAngularRadius;
    result.rendererSettings.indirectStrength = render.indirectStrength;
    result.rendererSettings.denoiserEnabled = render.denoiserEnabled;
    result.rendererSettings.atrousIterations = render.atrousIterations;
    result.rendererSettings.denoiserStrength = render.denoiserStrength;
    result.rendererSettings.debugView = render.debugView;
    result.rendererSettings.environmentEnabled = environment.enabled;
    result.rendererSettings.environmentIntensity = environment.intensity;
    result.rendererSettings.environmentRotation = environment.rotation;
    result.rendererSettings.environmentBackgroundIntensity = environment.backgroundIntensity;
    result.rendererSettings.renderResolutionScale = render.resolutionScale;

    for (const Entity* entity : document.registry().entities()) {
        if (entity->meshRenderer.has_value() && entity->meshRenderer->visible && entity->meshRenderer->visibleToCamera) {
            result.instanceEntities.push_back(entity->id);
        }
    }

    result.accumulationReason = accumulationReasonFor(result.updateKind);
    result.requiresRendererRebuild = result.updateKind == SceneUpdateKind::FullSceneRebuild;
    return result;
}

AccumulationResetReason SceneToGpuSceneBuilder::accumulationReasonFor(SceneUpdateKind kind) {
    switch (kind) {
    case SceneUpdateKind::None: return AccumulationResetReason::Manual;
    case SceneUpdateKind::MaterialOnly: return AccumulationResetReason::MaterialChanged;
    case SceneUpdateKind::TransformOnly: return AccumulationResetReason::SceneChanged;
    case SceneUpdateKind::LightOnly: return AccumulationResetReason::LightingChanged;
    case SceneUpdateKind::EnvironmentOnly: return AccumulationResetReason::EnvironmentChanged;
    case SceneUpdateKind::CameraOnly: return AccumulationResetReason::CameraMoved;
    case SceneUpdateKind::FullSceneRebuild: return AccumulationResetReason::SceneChanged;
    }
    return AccumulationResetReason::Manual;
}

} // namespace rtv
