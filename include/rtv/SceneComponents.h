#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/RendererBackend.h"
#include "rtv/RendererDebug.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotationEuler{0.0f};
    glm::vec3 scale{1.0f};
    bool dirty = true;

    [[nodiscard]] glm::quat rotation() const {
        return glm::quat(rotationEuler);
    }

    [[nodiscard]] glm::mat4 localMatrix() const {
        const glm::mat4 translation = glm::translate(glm::mat4{1.0f}, position);
        const glm::mat4 rotationMatrix = glm::mat4_cast(rotation());
        const glm::mat4 scaleMatrix = glm::scale(glm::mat4{1.0f}, scale);
        return translation * rotationMatrix * scaleMatrix;
    }

    [[nodiscard]] glm::mat4 worldMatrix(const glm::mat4& parent = glm::mat4{1.0f}) const {
        return parent * localMatrix();
    }

    void markClean() {
        dirty = false;
    }
};

struct MaterialSlot {
    std::string name;
    MaterialAssetHandle material{};
    std::optional<MaterialAssetHandle> overrideMaterial;
    TextureAssetHandle textureReference{};

    [[nodiscard]] MaterialAssetHandle resolvedMaterial() const {
        return overrideMaterial.value_or(material);
    }
};

struct MeshRenderer {
    MeshAssetHandle mesh{};
    std::vector<MaterialSlot> materialSlots;
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    uint32_t rendererInstanceIndex = UINT32_MAX;
};

enum class LightType : uint32_t {
    Directional,
    Point,
    Area,
};

struct Light {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    bool enabled = true;
};

struct Camera {
    float verticalFovRadians = 60.0f * 0.017453292519943295f;
    float nearPlane = 0.01f;
    float farPlane = 1000.0f;
    bool active = false;
    bool useRenderSettingsExposure = true;
};

struct Environment {
    std::filesystem::path hdrPath;
    float intensity = 1.0f;
    float rotation = 0.0f;
    float backgroundIntensity = 0.35f;
    bool enabled = true;
};

struct RenderSettings {
    bool pathTracingEnabled = true;
    bool directLightingEnabled = true;
    uint32_t maxBounces = 8;
    uint32_t environmentDirectSamples = 1;
    float exposure = 0.75f;
    bool sunlightEnabled = true;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float sunAngularRadius = 0.0093f;
    float indirectStrength = 1.0f;
    bool denoiserEnabled = true;
    uint32_t atrousIterations = 4;
    float denoiserStrength = 1.0f;
    RendererDebugView debugView = RendererDebugView::Beauty;
    bool accumulate = true;
    uint32_t accumulationLimit = 0;
    float resolutionScale = 1.0f;
    RendererBackend requestedBackend = RendererBackend::Auto;
};

enum class SceneUpdateKind : uint32_t {
    None,
    MaterialOnly,
    TransformOnly,
    LightOnly,
    EnvironmentOnly,
    CameraOnly,
    FullSceneRebuild,
};

[[nodiscard]] const char* sceneUpdateKindName(SceneUpdateKind kind);

} // namespace rtv
