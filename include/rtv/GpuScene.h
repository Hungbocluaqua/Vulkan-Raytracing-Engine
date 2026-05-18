#pragma once

#include "rtv/Buffer.h"
#include "rtv/BindlessResources.h"
#include "rtv/Image.h"
#include "rtv/SceneCache.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rtv {

class BufferUploader;
class ResourceAllocator;
class AssetManager;
struct SceneAsset;

struct CameraUniform {
    glm::vec4 pos{};
    glm::vec4 forward{};
    glm::vec4 right{};
    glm::vec4 up{};
    uint32_t frameCount = 0;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float exposure = 1.0f;
    uint32_t pathTracingEnabled = 1;
    uint32_t maxBounces = 8;
    uint32_t sunlightEnabled = 1;
    uint32_t directLightingEnabled = 1;
    float fovY = 60.0f * 0.017453292519943295f;
    float sunAngularRadius = 0.0093f;
    float indirectStrength = 1.0f;
    uint32_t environmentDirectSamples = 1;
};

struct MeshParamsUniform {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t bvhNodeCount = 0;
    uint32_t materialCount = 0;
    uint32_t enabled = 0;
    uint32_t sphereCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t instanceCount = 0;
    uint32_t lightCount = 0;
    float emissiveTotalArea = 0.0f;
    uint32_t meshCount = 0;
    uint32_t localVertexCount = 0;
    uint32_t localIndexCount = 0;
    uint32_t localBvhNodeCount = 0;
    uint32_t localTriangleCount = 0;
    uint32_t tlasNodeCount = 0;
    uint32_t tlasInstanceIndexCount = 0;
    uint32_t padding2 = 0;
    uint32_t padding3 = 0;
    uint32_t padding4 = 0;
};

struct GpuMeshRecord {
    glm::uvec4 vertexIndexData{};
    glm::uvec4 primitiveData{};
    glm::uvec4 bvhData{};
    glm::uvec4 flags{};
};

struct GpuPrimitiveRecord {
    glm::uvec4 indexData{};
    glm::uvec4 metadata{};
};

struct GpuInstanceRecord {
    glm::mat4 transform{1.0f};
    glm::mat4 inverseTransform{1.0f};
    glm::uvec4 metadata{};
};

struct GpuLocalVertex {
    glm::vec4 positionUvX{};
    glm::vec4 normalUvY{};
    glm::vec4 tangent{};
};

struct GpuInstanceBoundsRecord {
    glm::vec4 bmin{};
    glm::vec4 bmax{};
};

struct GpuLightRecord {
    glm::uvec4 metadata{};
    glm::vec4 data{};
};

struct EnvParamsUniform {
    uint32_t enabled = 1;
    float intensity = 1.0f;
    float rotation = 0.0f;
    uint32_t width = 1;
    uint32_t height = 1;
    float backgroundIntensity = 0.35f;
    uint32_t procedural = 1;
    float pad2 = 0.0f;
    float invTotalLum = 1.0f;
    float pad3 = 0.0f;
    float pad4 = 0.0f;
    float pad5 = 0.0f;
};

struct RayTracingMeshBuildInput {
    uint32_t meshIndex = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t primitiveOffset = 0;
    uint32_t primitiveCount = 0;
    bool containsAlphaTestedGeometry = false;
    bool opaqueTraversalSafe = false;
};

struct RayTracingInstanceBuildInput {
    uint32_t instanceIndex = 0;
    uint32_t meshIndex = 0;
    glm::mat4 transform{1.0f};
    uint32_t flags = 0;
    bool visible = true;
};

class GpuScene {
public:
    GpuScene(
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const SceneAsset* importedScene = nullptr,
        const AssetManager* assets = nullptr,
        std::optional<std::filesystem::path> environmentPath = std::nullopt,
        std::optional<std::filesystem::path> sceneCachePath = std::nullopt);
    ~GpuScene();

    [[nodiscard]] Buffer& vertices() { return *vertices_; }
    [[nodiscard]] Buffer& indices() { return *indices_; }
    [[nodiscard]] Buffer& bvhNodes() { return *bvhNodes_; }
    [[nodiscard]] Buffer& triangles() { return *triangles_; }
    [[nodiscard]] Buffer& materials() { return *materials_; }
    [[nodiscard]] Buffer& spheres() { return *spheres_; }
    [[nodiscard]] Buffer& meshRecords() { return *meshRecords_; }
    [[nodiscard]] Buffer& primitiveRecords() { return *primitiveRecords_; }
    [[nodiscard]] Buffer& instanceRecords() { return *instanceRecords_; }
    [[nodiscard]] Buffer& rtTriangleMaterialIds() { return *rtTriangleMaterialIds_; }
    [[nodiscard]] Buffer& lightRecords() { return *lightRecords_; }
    [[nodiscard]] Buffer& localVertices() { return *localVertices_; }
    [[nodiscard]] Buffer& localIndices() { return *localIndices_; }
    [[nodiscard]] Buffer& instanceBounds() { return *instanceBounds_; }
    [[nodiscard]] Buffer& localBvhNodes() { return *localBvhNodes_; }
    [[nodiscard]] Buffer& localTriangles() { return *localTriangles_; }
    [[nodiscard]] Buffer& tlasNodes() { return *tlasNodes_; }
    [[nodiscard]] Buffer& tlasInstanceIndices() { return *tlasInstanceIndices_; }
    [[nodiscard]] Buffer& envRows() { return *envRows_; }
    [[nodiscard]] Buffer& envCols() { return *envCols_; }
    [[nodiscard]] Buffer& meshParamsBuffer() { return *meshParamsBuffer_; }
    [[nodiscard]] Buffer& envParamsBuffer() { return *envParamsBuffer_; }
    [[nodiscard]] Image& environmentImage() { return *environmentImage_; }
    [[nodiscard]] const Buffer& meshRecords() const { return *meshRecords_; }
    [[nodiscard]] const Buffer& primitiveRecords() const { return *primitiveRecords_; }
    [[nodiscard]] const Buffer& instanceRecords() const { return *instanceRecords_; }
    [[nodiscard]] const Buffer& rtTriangleMaterialIds() const { return *rtTriangleMaterialIds_; }
    [[nodiscard]] const Buffer& localVertices() const { return *localVertices_; }
    [[nodiscard]] const Buffer& localIndices() const { return *localIndices_; }
    [[nodiscard]] const Buffer& instanceBounds() const { return *instanceBounds_; }
    [[nodiscard]] const Buffer& localBvhNodes() const { return *localBvhNodes_; }
    [[nodiscard]] const Buffer& localTriangles() const { return *localTriangles_; }
    [[nodiscard]] const Buffer& tlasNodes() const { return *tlasNodes_; }
    [[nodiscard]] const Buffer& tlasInstanceIndices() const { return *tlasInstanceIndices_; }
    [[nodiscard]] VkSampler environmentSampler() const { return environmentSampler_; }
    [[nodiscard]] const std::vector<VkDescriptorImageInfo>& materialTextureDescriptors() const { return materialTextureTable_.descriptors(); }
    [[nodiscard]] const std::vector<VkDescriptorImageInfo>& materialSamplerDescriptors() const { return materialSamplerDescriptors_; }
    [[nodiscard]] VkSampler materialSampler() const { return materialSampler_; }

    [[nodiscard]] const MeshParamsUniform& meshParams() const { return meshParams_; }
    [[nodiscard]] const EnvParamsUniform& envParams() const { return envParams_; }
    [[nodiscard]] const std::vector<RayTracingMeshBuildInput>& rayTracingMeshes() const { return rayTracingMeshes_; }
    [[nodiscard]] const std::vector<RayTracingInstanceBuildInput>& rayTracingInstances() const { return rayTracingInstances_; }

    bool setEnvironmentControls(bool enabled, float intensity, float rotation, float backgroundIntensity);
    void loadEnvironment(BufferUploader& uploader, const std::filesystem::path& path);
    bool updateImportedMaterials(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);

private:
    void createCornellBox(BufferUploader& uploader);
    void createImportedScene(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);
    void createImportedSceneFromCache(BufferUploader& uploader, const CachedScene& cached);
    void createDefaultMaterialTexture(BufferUploader& uploader);
    void createImportedMaterialTextures(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);
    void createCachedMaterialTextures(BufferUploader& uploader, const CachedScene& cached);
    void createEnvironment(BufferUploader& uploader);
    void uploadEnvironmentParams();
    void destroyMaterialTextureSamplers();
    void rebuildMaterialSamplerDescriptors(uint32_t slotCount);

    ResourceAllocator& allocator_;
    std::optional<std::filesystem::path> environmentPath_;
    std::optional<std::filesystem::path> sceneCachePath_;
    std::unique_ptr<Buffer> vertices_;
    std::unique_ptr<Buffer> indices_;
    std::unique_ptr<Buffer> bvhNodes_;
    std::unique_ptr<Buffer> triangles_;
    std::unique_ptr<Buffer> materials_;
    std::unique_ptr<Buffer> spheres_;
    std::unique_ptr<Buffer> meshRecords_;
    std::unique_ptr<Buffer> primitiveRecords_;
    std::unique_ptr<Buffer> instanceRecords_;
    std::unique_ptr<Buffer> rtTriangleMaterialIds_;
    std::unique_ptr<Buffer> lightRecords_;
    std::unique_ptr<Buffer> localVertices_;
    std::unique_ptr<Buffer> localIndices_;
    std::unique_ptr<Buffer> instanceBounds_;
    std::unique_ptr<Buffer> localBvhNodes_;
    std::unique_ptr<Buffer> localTriangles_;
    std::unique_ptr<Buffer> tlasNodes_;
    std::unique_ptr<Buffer> tlasInstanceIndices_;
    std::unique_ptr<Buffer> envRows_;
    std::unique_ptr<Buffer> envCols_;
    std::unique_ptr<Buffer> meshParamsBuffer_;
    std::unique_ptr<Buffer> envParamsBuffer_;
    std::unique_ptr<Image> environmentImage_;
    BindlessTextureTable materialTextureTable_;
    VkSampler environmentSampler_ = VK_NULL_HANDLE;
    VkSampler materialSampler_ = VK_NULL_HANDLE;
    std::vector<VkSampler> materialTextureSamplers_;
    std::vector<VkDescriptorImageInfo> materialSamplerDescriptors_;
    MeshParamsUniform meshParams_{};
    EnvParamsUniform envParams_{};
    std::vector<RayTracingMeshBuildInput> rayTracingMeshes_;
    std::vector<RayTracingInstanceBuildInput> rayTracingInstances_;
};

} // namespace rtv
