#pragma once

#include "rtv/TextureAsset.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct MeshAssetHandle {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

struct MaterialAssetHandle {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

struct MeshVertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 texcoord{};
};

struct MaterialAsset {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    uint32_t alphaMode = 0;
    uint32_t doubleSided = 0;
    TextureAssetHandle baseColorTexture{};
    TextureAssetHandle normalTexture{};
    TextureAssetHandle metallicRoughnessTexture{};
    TextureAssetHandle emissiveTexture{};
    uint32_t shaderCompatibilityMask = 1u;
};

constexpr uint32_t kMaterialClosureFlagDiffuse      = 1u << 0u;
constexpr uint32_t kMaterialClosureFlagSpecular     = 1u << 1u;
constexpr uint32_t kMaterialClosureFlagSss          = 1u << 2u;
constexpr uint32_t kMaterialClosureFlagTransmission = 1u << 3u;
constexpr uint32_t kMaterialClosureFlagClearcoat    = 1u << 4u;
constexpr uint32_t kMaterialClosureFlagSheen        = 1u << 5u;
constexpr uint32_t kMaterialClosureFlagThinFilm     = 1u << 6u;
constexpr uint32_t kMaterialClosureFlagMetal        = 1u << 7u;

struct MeshPrimitiveAsset {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    MaterialAssetHandle material{};
};

struct MeshAsset {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshPrimitiveAsset> primitives;
};

struct SceneNodeAsset {
    std::string name;
    glm::mat4 transform{1.0f};
    MeshAssetHandle mesh{};
    bool visible = true;
    bool castShadow = true;
    bool visibleToCamera = true;
    bool hasCamera = false;
    float cameraYfov = 60.0f * 0.017453292519943295f;
    float cameraNear = 0.01f;
    float cameraFar = 1000.0f;
    int32_t parent = -1;
    std::vector<uint32_t> children;
};

struct SceneLightAsset {
    uint32_t type = 1;
    glm::mat4 transform{1.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float sizeOrRadius = 1.0f;
    bool enabled = true;
    int32_t nodeIndex = -1;
};

struct SceneAsset {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<TextureAssetHandle> textures;
    std::vector<MaterialAssetHandle> materials;
    std::vector<MeshAssetHandle> meshes;
    std::vector<SceneNodeAsset> nodes;
    std::vector<SceneLightAsset> lights;
    std::vector<uint32_t> rootNodes;
};

} // namespace rtv
