#include "rtv/GpuScene.h"

#include "rtv/BatchUploader.h"
#include "rtv/BvhBuilder.h"
#include "rtv/ParallelBvhBuilder.h"
#include "rtv/AssetManager.h"
#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/EnvironmentImportanceSampler.h"
#include "rtv/LightBvh.h"
#include "rtv/HdrLoader.h"
#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <glm/gtc/matrix_inverse.hpp>

namespace rtv {

namespace {

constexpr uint32_t instanceFlagVisible = 1u << 0u;
constexpr uint32_t instanceFlagVisibleToCamera = 1u << 1u;
constexpr uint32_t instanceFlagCastShadow = 1u << 2u;

uint32_t nodeInstanceFlags(const SceneNodeAsset& node) {
    uint32_t flags = 0u;
    if (node.visible) {
        flags |= instanceFlagVisible;
    }
    if (node.visibleToCamera) {
        flags |= instanceFlagVisibleToCamera;
    }
    if (node.castShadow) {
        flags |= instanceFlagCastShadow;
    }
    return flags;
}

constexpr uint32_t maxMaterialTextures = 1024;
constexpr uint64_t fastImportedBvhTriangleThreshold = 1'000'000ull;
constexpr uint32_t materialFlagManualBaseColorSrgb = 1u << 0u;
constexpr uint32_t materialFlagManualEmissiveSrgb = 1u << 1u;
constexpr uint32_t materialVec4Stride = 5u;

static_assert(sizeof(GpuMeshRecord) == 64);
static_assert(sizeof(GpuPrimitiveRecord) == 32);
static_assert(sizeof(GpuInstanceRecord) == 208);
static_assert(sizeof(GpuLocalVertex) == 48);
static_assert(sizeof(GpuInstanceBoundsRecord) == 32);
static_assert(sizeof(GpuLightRecord) == 80);
static_assert(sizeof(MeshParamsUniform) == 80);

bool hasValidGpuCache(const CachedScene& cached, const SceneAsset& scene) {
    if (cached.meshGpuRecords.empty() || cached.meshParams.meshCount == 0) {
        return false;
    }
    if (cached.textures.size() != scene.textures.size() ||
        cached.materials.size() != scene.materials.size()) {
        return false;
    }
    if (cached.meshGpuRecords.size() != cached.meshParams.meshCount) {
        return false;
    }
    if (cached.primitiveRecords.empty() || cached.instanceRecords.empty()) {
        return false;
    }
    if (cached.tlasNodes.empty() || cached.tlasInstanceIndices.empty()) {
        return false;
    }
    if (cached.meshParams.localBvhNodeCount == 0 ||
        cached.meshParams.localTriangleCount == 0) {
        return false;
    }
    for (const auto& rec : cached.meshGpuRecords) {
        if (rec.localBvh.packedNodes.empty() || rec.localBvh.triangleData.empty()) {
            return false;
        }
    }
    return true;
}

CachedMeshParams toCachedMeshParams(const MeshParamsUniform& params) {
    return CachedMeshParams{
        .vertexCount = params.vertexCount,
        .triangleCount = params.triangleCount,
        .bvhNodeCount = params.bvhNodeCount,
        .materialCount = params.materialCount,
        .enabled = params.enabled,
        .sphereCount = params.sphereCount,
        .primitiveCount = params.primitiveCount,
        .instanceCount = params.instanceCount,
        .lightCount = params.lightCount,
        .emissiveTotalArea = params.emissiveTotalArea,
        .meshCount = params.meshCount,
        .localVertexCount = params.localVertexCount,
        .localIndexCount = params.localIndexCount,
        .localBvhNodeCount = params.localBvhNodeCount,
        .localTriangleCount = params.localTriangleCount,
        .tlasNodeCount = params.tlasNodeCount,
        .tlasInstanceIndexCount = params.tlasInstanceIndexCount,
    };
}

MeshParamsUniform fromCachedMeshParams(const CachedMeshParams& params) {
    return MeshParamsUniform{
        .vertexCount = params.vertexCount,
        .triangleCount = params.triangleCount,
        .bvhNodeCount = params.bvhNodeCount,
        .materialCount = params.materialCount,
        .enabled = params.enabled,
        .sphereCount = params.sphereCount,
        .primitiveCount = params.primitiveCount,
        .instanceCount = params.instanceCount,
        .lightCount = params.lightCount,
        .emissiveTotalArea = params.emissiveTotalArea,
        .meshCount = params.meshCount,
        .localVertexCount = params.localVertexCount,
        .localIndexCount = params.localIndexCount,
        .localBvhNodeCount = params.localBvhNodeCount,
        .localTriangleCount = params.localTriangleCount,
        .tlasNodeCount = params.tlasNodeCount,
        .tlasInstanceIndexCount = params.tlasInstanceIndexCount,
    };
}

uint32_t cachedMaterialTextureFlag(const CachedScene& cached, int32_t textureIndex, uint32_t flag) {
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= cached.textures.size()) {
        return 0;
    }
    return cached.textures[static_cast<size_t>(textureIndex)].srgb ? 0u : flag;
}

std::vector<glm::vec4> buildCachedMaterialData(const CachedScene& cached) {
    std::vector<glm::vec4> materialData;
    materialData.reserve(std::max<size_t>(1, cached.materials.size()) * materialVec4Stride);
    for (const CachedMaterialData& material : cached.materials) {
        const uint32_t flags =
            cachedMaterialTextureFlag(cached, material.baseColorTextureIndex, materialFlagManualBaseColorSrgb) |
            cachedMaterialTextureFlag(cached, material.emissiveTextureIndex, materialFlagManualEmissiveSrgb);
        const float type = 3.0f;
        materialData.push_back({glm::vec3(material.baseColorFactor), material.roughnessFactor});
        materialData.push_back({1.5f, type, material.metallicFactor, static_cast<float>(flags)});
        materialData.push_back({material.emissiveFactor, material.baseColorFactor.a});
        materialData.push_back({
            material.baseColorTextureIndex >= 0 ? static_cast<float>(material.baseColorTextureIndex) : -1.0f,
            material.normalTextureIndex >= 0 ? static_cast<float>(material.normalTextureIndex) : -1.0f,
            material.metallicRoughnessTextureIndex >= 0 ? static_cast<float>(material.metallicRoughnessTextureIndex) : -1.0f,
            material.emissiveTextureIndex >= 0 ? static_cast<float>(material.emissiveTextureIndex) : -1.0f});
        materialData.push_back({
            material.alphaCutoff,
            static_cast<float>(material.alphaMode),
            static_cast<float>(material.doubleSided),
            0.0f});
    }
    if (materialData.empty()) {
        materialData.push_back({0.8f, 0.8f, 0.8f, 1.0f});
        materialData.push_back({1.5f, 0.0f, 0.0f, 0.0f});
        materialData.push_back({0.0f, 0.0f, 0.0f, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 0.0f, 0.0f});
    }
    return materialData;
}

void addFileDependency(CachedScene& cached, const std::filesystem::path& path, std::unordered_set<std::string>& seen) {
    if (path.empty() || !std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
        return;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::string key = normalized.string();
    if (!seen.insert(key).second) {
        return;
    }
    cached.dependencies.push_back(FileDependency{
        .path = key,
        .size = static_cast<uint64_t>(std::filesystem::file_size(normalized)),
        .mtime = SceneCache::fileMtime(normalized),
    });
}

struct TextureColorUsage {
    bool baseColor = false;
    bool emissive = false;
    bool metallicRoughness = false;
    bool normal = false;

    [[nodiscard]] bool color() const { return baseColor || emissive; }
    [[nodiscard]] bool data() const { return metallicRoughness || normal; }
};

struct MaterialCpu {
    glm::vec3 color{};
    float roughness = 1.0f;
    float ior = 1.0f;
    uint32_t type = 0;
    float metallic = 0.0f;
    glm::vec3 emissive{};
};

struct TriBuild {
    glm::vec3 v0{};
    glm::vec3 v1{};
    glm::vec3 v2{};
    glm::vec3 normal{};
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    glm::vec3 center{};
    uint32_t material = 0;
};

struct PackedNode {
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    bool leaf = false;
    uint32_t ropePlusOne = 0;
    uint32_t child0 = 0;
    uint32_t child1 = 0;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
    uint32_t childCount = 0;
};

struct BuildNode {
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    int left = -1;
    int right = -1;
    uint32_t triOffset = 0;
    uint32_t triCount = 0;
};

struct CpuBounds {
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{-std::numeric_limits<float>::max()};
};

struct CpuTlasNode {
    CpuBounds bounds{};
    bool leaf = false;
    int rope = -1;
    uint32_t child0 = 0;
    uint32_t child1 = 0;
    uint32_t instanceOffset = 0;
    uint32_t instanceCount = 0;
};

glm::vec3 minVec(glm::vec3 a, glm::vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

glm::vec3 maxVec(glm::vec3 a, glm::vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void includePoint(CpuBounds& bounds, glm::vec3 point) {
    bounds.bmin = minVec(bounds.bmin, point);
    bounds.bmax = maxVec(bounds.bmax, point);
}

void includeBounds(CpuBounds& bounds, const CpuBounds& other) {
    includePoint(bounds, other.bmin);
    includePoint(bounds, other.bmax);
}

glm::vec3 boundsCenter(const CpuBounds& bounds) {
    return (bounds.bmin + bounds.bmax) * 0.5f;
}

CpuBounds boundsFromPositions(const std::vector<glm::vec3>& positions) {
    CpuBounds bounds;
    for (glm::vec3 point : positions) {
        includePoint(bounds, point);
    }
    return bounds;
}

CpuBounds transformBounds(const CpuBounds& bounds, const glm::mat4& transform) {
    CpuBounds result;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const glm::vec3 p{
            (corner & 1u) != 0u ? bounds.bmax.x : bounds.bmin.x,
            (corner & 2u) != 0u ? bounds.bmax.y : bounds.bmin.y,
            (corner & 4u) != 0u ? bounds.bmax.z : bounds.bmin.z,
        };
        includePoint(result, glm::vec3(transform * glm::vec4(p, 1.0f)));
    }
    return result;
}

GpuInstanceBoundsRecord makeInstanceBoundsRecord(const CpuBounds& bounds, uint32_t instanceIndex, uint32_t meshIndex) {
    return GpuInstanceBoundsRecord{
        .bmin = {bounds.bmin, static_cast<float>(instanceIndex)},
        .bmax = {bounds.bmax, static_cast<float>(meshIndex)},
    };
}

GpuLocalVertex makeLocalVertex(glm::vec3 position, glm::vec3 normal, glm::vec2 uv = glm::vec2{0.0f}, glm::vec4 tangent = glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}) {
    return GpuLocalVertex{
        .positionUvX = {position, uv.x},
        .normalUvY = {normal, uv.y},
        .tangent = tangent,
    };
}

void threadTlasRopes(std::vector<CpuTlasNode>& nodes, uint32_t nodeIndex, int rope) {
    if (nodeIndex >= nodes.size()) {
        return;
    }
    CpuTlasNode& node = nodes[nodeIndex];
    node.rope = rope;
    if (!node.leaf) {
        threadTlasRopes(nodes, node.child0, static_cast<int>(node.child1));
        threadTlasRopes(nodes, node.child1, rope);
    }
}

uint32_t buildTlasRecursive(
    const std::vector<GpuInstanceBoundsRecord>& instanceBounds,
    std::vector<uint32_t>& refs,
    uint32_t begin,
    uint32_t end,
    std::vector<CpuTlasNode>& nodes,
    std::vector<uint32_t>& orderedInstances) {
    CpuBounds nodeBounds;
    CpuBounds centroidBounds;
    for (uint32_t i = begin; i < end; ++i) {
        const GpuInstanceBoundsRecord& record = instanceBounds[refs[i]];
        CpuBounds bounds{glm::vec3(record.bmin), glm::vec3(record.bmax)};
        includeBounds(nodeBounds, bounds);
        includePoint(centroidBounds, boundsCenter(bounds));
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.push_back(CpuTlasNode{.bounds = nodeBounds});
    const uint32_t count = end - begin;
    if (count <= 4u) {
        CpuTlasNode& node = nodes[nodeIndex];
        node.leaf = true;
        node.instanceOffset = static_cast<uint32_t>(orderedInstances.size());
        node.instanceCount = count;
        orderedInstances.insert(orderedInstances.end(), refs.begin() + begin, refs.begin() + end);
        return nodeIndex;
    }

    const glm::vec3 extent = centroidBounds.bmax - centroidBounds.bmin;
    uint32_t axis = 0;
    if (extent.y > extent.x && extent.y >= extent.z) {
        axis = 1;
    } else if (extent.z > extent.x && extent.z > extent.y) {
        axis = 2;
    }
    const uint32_t mid = begin + count / 2u;
    std::nth_element(refs.begin() + begin, refs.begin() + mid, refs.begin() + end, [&](uint32_t a, uint32_t b) {
        const CpuBounds boundsA{glm::vec3(instanceBounds[a].bmin), glm::vec3(instanceBounds[a].bmax)};
        const CpuBounds boundsB{glm::vec3(instanceBounds[b].bmin), glm::vec3(instanceBounds[b].bmax)};
        return boundsCenter(boundsA)[axis] < boundsCenter(boundsB)[axis];
    });

    const uint32_t child0 = buildTlasRecursive(instanceBounds, refs, begin, mid, nodes, orderedInstances);
    const uint32_t child1 = buildTlasRecursive(instanceBounds, refs, mid, end, nodes, orderedInstances);
    nodes[nodeIndex].child0 = child0;
    nodes[nodeIndex].child1 = child1;
    return nodeIndex;
}

void buildTlas(
    const std::vector<GpuInstanceBoundsRecord>& instanceBounds,
    std::vector<glm::vec4>& packedNodes,
    std::vector<uint32_t>& orderedInstances) {
    packedNodes.clear();
    orderedInstances.clear();
    if (instanceBounds.empty()) {
        return;
    }

    std::vector<uint32_t> refs(instanceBounds.size());
    std::iota(refs.begin(), refs.end(), 0u);
    std::vector<CpuTlasNode> nodes;
    nodes.reserve(instanceBounds.size() * 2u);
    const uint32_t root = buildTlasRecursive(instanceBounds, refs, 0u, static_cast<uint32_t>(refs.size()), nodes, orderedInstances);
    threadTlasRopes(nodes, root, -1);

    packedNodes.reserve(nodes.size() * 4u);
    for (const CpuTlasNode& node : nodes) {
        packedNodes.push_back({node.bounds.bmin, node.leaf ? 1.0f : 0.0f});
        packedNodes.push_back({node.bounds.bmax, node.rope >= 0 ? static_cast<float>(node.rope + 1) : 0.0f});
        if (node.leaf) {
            packedNodes.push_back({static_cast<float>(node.instanceOffset), static_cast<float>(node.instanceCount), 0.0f, 0.0f});
        } else {
            packedNodes.push_back({static_cast<float>(node.child0), static_cast<float>(node.child1), 0.0f, 0.0f});
        }
        packedNodes.push_back({node.leaf ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f});
    }
}

std::vector<TextureColorUsage> classifyTextureUsage(const SceneAsset& scene, const AssetManager& assets) {
    std::vector<TextureColorUsage> usage(scene.textures.size());
    auto slotFor = [&](TextureAssetHandle texture) -> uint32_t {
        if (!texture.valid()) {
            return UINT32_MAX;
        }
        for (uint32_t slot = 0; slot < scene.textures.size(); ++slot) {
            if (scene.textures[slot].index == texture.index) {
                return slot;
            }
        }
        return UINT32_MAX;
    };

    for (MaterialAssetHandle handle : scene.materials) {
        const MaterialAsset* material = assets.material(handle);
        if (material == nullptr) {
            continue;
        }
        uint32_t slot = slotFor(material->baseColorTexture);
        if (slot < usage.size()) {
            usage[slot].baseColor = true;
        }
        slot = slotFor(material->emissiveTexture);
        if (slot < usage.size()) {
            usage[slot].emissive = true;
        }
        slot = slotFor(material->metallicRoughnessTexture);
        if (slot < usage.size()) {
            usage[slot].metallicRoughness = true;
        }
        slot = slotFor(material->normalTexture);
        if (slot < usage.size()) {
            usage[slot].normal = true;
        }
    }
    return usage;
}

bool uploadTextureAsSrgb(const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    return slot < usage.size() && usage[slot].color() && !usage[slot].data();
}

std::vector<uint8_t> fallbackTexturePixels(const std::vector<TextureColorUsage>& usage, uint32_t slot) {
    if (slot < usage.size() && usage[slot].normal) {
        return {128, 128, 255, 255};
    }
    if (slot < usage.size() && usage[slot].metallicRoughness) {
        return {255, 255, 0, 255};
    }
    return {255, 255, 255, 255};
}

void uploadBuffer(ResourceAllocator& allocator, BufferUploader& uploader, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const void* data, VkDeviceSize bytes, const char* name) {
    if (allocator.supportsDeviceAddress() && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    buffer = std::make_unique<Buffer>(allocator, BufferDesc{
        .size = std::max<VkDeviceSize>(bytes, 4),
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = name,
    });
    if (bytes > 0) {
        uploader.uploadToBuffer(*buffer, data, bytes);
    }
}

void uploadBufferBatched(BatchUploader& batch, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const void* data, VkDeviceSize bytes, const char* name) {
    if (batch.allocator().supportsDeviceAddress() && (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    buffer = std::make_unique<Buffer>(batch.allocator(), BufferDesc{
        .size = std::max<VkDeviceSize>(bytes, 4),
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = name,
    });
    if (bytes > 0) {
        batch.enqueueBufferUpload(*buffer, data, bytes);
    }
}

template <typename T>
void uploadVectorBatched(BatchUploader& batch, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const std::vector<T>& data, const char* name) {
    uploadBufferBatched(batch, buffer, usage, data.data(), sizeof(T) * data.size(), name);
}

template <typename T>
void uploadVector(ResourceAllocator& allocator, BufferUploader& uploader, std::unique_ptr<Buffer>& buffer, VkBufferUsageFlags usage, const std::vector<T>& data, const char* name) {
    uploadBuffer(allocator, uploader, buffer, usage, data.data(), sizeof(T) * data.size(), name);
}

VkFilter toVkFilter(TextureFilter filter) {
    return filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode toVkAddressMode(TextureWrap wrap) {
    switch (wrap) {
    case TextureWrap::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::MirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TextureWrap::Repeat:
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

bool sameSampler(const TextureSamplerDesc& a, const TextureSamplerDesc& b) {
    return a.minFilter == b.minFilter &&
        a.magFilter == b.magFilter &&
        a.wrapS == b.wrapS &&
        a.wrapT == b.wrapT;
}

void createMaterialSampler(VkDevice device, VkSampler& sampler, const TextureSamplerDesc& desc) {
    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = toVkFilter(desc.magFilter);
    samplerInfo.minFilter = toVkFilter(desc.minFilter);
    samplerInfo.addressModeU = toVkAddressMode(desc.wrapS);
    samplerInfo.addressModeV = toVkAddressMode(desc.wrapT);
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipmapMode = desc.minFilter == TextureFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler(material textures)");
}

VkSampler createOwnedMaterialSampler(VkDevice device, const TextureSamplerDesc& desc) {
    VkSampler sampler = VK_NULL_HANDLE;
    createMaterialSampler(device, sampler, desc);
    return sampler;
}

TextureSamplerDesc selectMaterialSampler(const SceneAsset& scene, const AssetManager& assets, bool& mixedSamplers) {
    TextureSamplerDesc selected{};
    bool hasSelected = false;
    mixedSamplers = false;
    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets.texture(handle);
        if (texture == nullptr) {
            continue;
        }
        if (!hasSelected) {
            selected = texture->sampler;
            hasSelected = true;
            continue;
        }
        if (!sameSampler(selected, texture->sampler)) {
            mixedSamplers = true;
        }
    }
    return selected;
}

GpuPrimitiveRecord makePrimitiveRecord(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex, uint32_t materialIndex, uint32_t firstTriangle, uint32_t triangleCount) {
    return GpuPrimitiveRecord{
        .indexData = {firstIndex, indexCount, firstVertex, materialIndex},
        .metadata = {firstTriangle, triangleCount, 0u, 0u},
    };
}

GpuMeshRecord makeMeshRecord(
    uint32_t firstVertex,
    uint32_t vertexCount,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    uint32_t bvhNodeOffset = 0,
    uint32_t bvhNodeCount = 0,
    uint32_t triangleOffset = 0,
    uint32_t triangleCount = 0) {
    return GpuMeshRecord{
        .vertexIndexData = {firstVertex, vertexCount, firstIndex, indexCount},
        .primitiveData = {primitiveOffset, primitiveCount, 0u, 0u},
        .bvhData = {bvhNodeOffset, bvhNodeCount, triangleOffset, triangleCount},
        .flags = {0u, 0u, 0u, 0u},
    };
}

GpuInstanceRecord makeInstanceRecord(
    const glm::mat4& transform,
    uint32_t meshIndex,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    uint32_t flags = instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow,
    const glm::mat4* prevTransform = nullptr) {
    return GpuInstanceRecord{
        .transform = transform,
        .inverseTransform = glm::inverse(transform),
        .prevTransform = prevTransform != nullptr ? *prevTransform : transform,
        .metadata = {meshIndex, primitiveOffset, primitiveCount, flags},
    };
}

std::vector<uint32_t> buildRtTriangleMaterialIds(const std::vector<GpuPrimitiveRecord>& primitiveRecords, uint32_t rawTriangleCount) {
    std::vector<uint32_t> materialIds(std::max(rawTriangleCount, 1u), 0u);
    for (const GpuPrimitiveRecord& primitive : primitiveRecords) {
        const uint32_t firstTriangle = primitive.indexData.x / 3u;
        const uint32_t triangleCount = primitive.indexData.y / 3u;
        if (firstTriangle >= materialIds.size()) {
            continue;
        }
        const uint32_t end = std::min(firstTriangle + triangleCount, static_cast<uint32_t>(materialIds.size()));
        std::fill(materialIds.begin() + firstTriangle, materialIds.begin() + end, primitive.indexData.w);
    }
    return materialIds;
}

bool primitivesAreOpaqueTraversalSafe(
    const std::vector<GpuPrimitiveRecord>& primitiveRecords,
    uint32_t primitiveOffset,
    uint32_t primitiveCount,
    const std::vector<bool>& materialOpaqueTraversalSafe) {
    if (primitiveCount == 0) {
        return false;
    }
    for (uint32_t i = 0; i < primitiveCount; ++i) {
        const uint32_t primitiveIndex = primitiveOffset + i;
        if (primitiveIndex >= primitiveRecords.size()) {
            return false;
        }
        const uint32_t materialIndex = primitiveRecords[primitiveIndex].indexData.w;
        if (materialIndex >= materialOpaqueTraversalSafe.size() || !materialOpaqueTraversalSafe[materialIndex]) {
            return false;
        }
    }
    return true;
}

float triangleArea(glm::vec3 v0, glm::vec3 v1, glm::vec3 v2) {
    return 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
}

float luminance(glm::vec3 value) {
    return glm::dot(value, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

std::vector<GpuLightRecord> buildLightRecords(
    const std::vector<GpuMeshRecord>& meshRecords,
    const std::vector<GpuInstanceRecord>& instanceRecords,
    const std::vector<glm::vec4>& localTriangleData,
    const std::vector<glm::vec3>& materialEmissive,
    const std::vector<glm::vec4>& sphereData,
    float& totalArea) {
    totalArea = 0.0f;
    std::vector<GpuLightRecord> lights;
    for (uint32_t instanceIndex = 0; instanceIndex < instanceRecords.size(); ++instanceIndex) {
        const GpuInstanceRecord& instance = instanceRecords[instanceIndex];
        if ((instance.metadata.w & instanceFlagVisible) == 0u) {
            continue;
        }
        const uint32_t meshIndex = instance.metadata.x;
        if (meshIndex >= meshRecords.size()) {
            continue;
        }
        const GpuMeshRecord& mesh = meshRecords[meshIndex];
        const uint32_t firstTriangle = mesh.bvhData.z;
        const uint32_t triangleCount = mesh.bvhData.w;
        for (uint32_t localTriangle = 0; localTriangle < triangleCount; ++localTriangle) {
            const uint32_t packedIndex = firstTriangle + localTriangle;
            const uint32_t triBase = packedIndex * 12u;
            if (triBase + 3u >= localTriangleData.size()) {
                continue;
            }
            const uint32_t material = static_cast<uint32_t>(std::max(localTriangleData[triBase + 3u].w, 0.0f) + 0.5f);
            if (material >= materialEmissive.size() || glm::length(materialEmissive[material]) <= 0.0f) {
                continue;
            }
            const glm::vec3 v0 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 0u]), 1.0f));
            const glm::vec3 v1 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 1u]), 1.0f));
            const glm::vec3 v2 = glm::vec3(instance.transform * glm::vec4(glm::vec3(localTriangleData[triBase + 2u]), 1.0f));
            const glm::vec3 emissive = materialEmissive[material];
            const float area = triangleArea(v0, v1, v2);
            if (area <= 0.0f) {
                continue;
            }
            const float weight = area * std::max(luminance(emissive), 0.0001f);
            totalArea += weight;
            lights.push_back(GpuLightRecord{
                .metadata = {0u, packedIndex, material, instanceIndex},
                .data0 = {weight, totalArea, 0.0f, area},
            });
        }
    }

    const uint32_t sphereCount = static_cast<uint32_t>(sphereData.size() / 4u);
    for (uint32_t sphereIndex = 0; sphereIndex < sphereCount; ++sphereIndex) {
        const glm::vec4 sphere = sphereData[sphereIndex * 4u];
        const glm::vec3 emissive = glm::vec3(sphereData[sphereIndex * 4u + 3u]);
        if (glm::length(emissive) <= 0.0f || sphere.w <= 0.0f) {
            continue;
        }
        const float area = 4.0f * 3.14159265358979323846f * sphere.w * sphere.w;
        const float weight = area * std::max(luminance(emissive), 0.0001f);
        totalArea += weight;
        lights.push_back(GpuLightRecord{
            .metadata = {1u, sphereIndex, 0u, static_cast<uint32_t>(lights.size())},
            .data0 = {weight, totalArea, sphere.w, area},
        });
    }
    return lights;
}

glm::vec3 safeNormalize(glm::vec3 value, glm::vec3 fallback) {
    const float len2 = glm::dot(value, value);
    return len2 > 0.000001f ? value * glm::inversesqrt(len2) : fallback;
}

std::vector<GpuLightRecord> buildAuthoredLightRecords(const std::vector<SceneLightAsset>& lights, float startWeight, float& totalWeight) {
    totalWeight = startWeight;
    std::vector<GpuLightRecord> records;
    records.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const SceneLightAsset& light = lights[i];
        if (!light.enabled || light.intensity <= 0.0f || luminance(light.color) <= 0.0f) {
            continue;
        }

        const glm::vec3 radiance = light.color * light.intensity;
        const float size = std::max(light.sizeOrRadius, 0.0001f);
        float weight = std::max(luminance(radiance), 0.0001f);
        const uint32_t type = 2u + std::min(light.type, 2u);
        const float area = type == 4u ? size * size : 0.0f;
        if (type == 4u) {
            weight *= area;
        }
        totalWeight += weight;

        const glm::vec3 position = glm::vec3(light.transform[3]);
        const glm::vec3 forward = safeNormalize(glm::mat3(light.transform) * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
        const glm::vec3 toLightDirection = -forward;
        const glm::vec3 normal = forward;

        GpuLightRecord record{};
        record.metadata = {type, i, 0u, 0u};
        record.data0 = {weight, totalWeight, size, area};
        record.data1 = type == 2u ? glm::vec4(toLightDirection, normal.x) : glm::vec4(position, normal.x);
        record.data2 = {radiance, normal.y};
        record.data3 = {normal.z, 0.0f, 0.0f, 0.0f};
        records.push_back(record);
    }
    return records;
}

std::vector<GpuLightRecord> combineLightRecords(const std::vector<GpuLightRecord>& emissiveRecords, const std::vector<SceneLightAsset>& authoredLights, float emissiveWeight, float& totalWeight) {
    std::vector<GpuLightRecord> records = emissiveRecords;
    float runningWeight = emissiveWeight;
    std::vector<GpuLightRecord> authored = buildAuthoredLightRecords(authoredLights, runningWeight, totalWeight);
    records.insert(records.end(), authored.begin(), authored.end());
    if (authored.empty()) {
        totalWeight = runningWeight;
    }
    return records;
}

} // namespace

GpuScene::GpuScene(
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const SceneAsset* importedScene,
    const AssetManager* assets,
    std::optional<std::filesystem::path> environmentPath,
    std::optional<std::filesystem::path> sceneCachePath)
    : allocator_(allocator),
      environmentPath_(std::move(environmentPath)),
      sceneCachePath_(std::move(sceneCachePath)) {
    bool usedGpuCache = false;
    if (importedScene != nullptr && assets != nullptr && !importedScene->meshes.empty()) {
        if (sceneCachePath_.has_value() && importedScene->lights.empty()) {
            auto cached = SceneCache::load(*sceneCachePath_);
            if (cached.has_value() && hasValidGpuCache(*cached, *importedScene)) {
                createImportedSceneFromCache(uploader, *cached);
                usedGpuCache = true;
            }
        }
        if (!usedGpuCache) {
            createImportedScene(uploader, *importedScene, *assets);
        }
    } else {
        createCornellBox(uploader);
    }
    createEnvironment(uploader);
}

GpuScene::~GpuScene() {
    destroyMaterialTextureSamplers();
    if (environmentSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(allocator_.device(), environmentSampler_, nullptr);
    }
    if (materialSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(allocator_.device(), materialSampler_, nullptr);
    }
}

void GpuScene::destroyMaterialTextureSamplers() {
    for (VkSampler sampler : materialTextureSamplers_) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(allocator_.device(), sampler, nullptr);
        }
    }
    materialTextureSamplers_.clear();
}

std::vector<VkDescriptorImageInfo> GpuScene::materialCombinedDescriptors() const {
    const auto& texDescs = materialTextureTable_.descriptors();
    std::vector<VkDescriptorImageInfo> result;
    result.reserve(texDescs.size());
    for (const auto& tex : texDescs) {
        VkDescriptorImageInfo combined{};
        combined.imageView = tex.imageView;
        combined.imageLayout = tex.imageLayout;
        combined.sampler = materialSampler_;
        result.push_back(combined);
    }
    return result;
}

bool GpuScene::setEnvironmentControls(bool enabled, float intensity, float rotation, float backgroundIntensity) {
    const uint32_t enabledValue = enabled ? 1u : 0u;
    const bool changed =
        envParams_.enabled != enabledValue ||
        std::abs(envParams_.intensity - intensity) > 0.0001f ||
        std::abs(envParams_.rotation - rotation) > 0.0001f ||
        std::abs(envParams_.backgroundIntensity - backgroundIntensity) > 0.0001f;
    if (!changed) {
        return false;
    }

    envParams_.enabled = enabledValue;
    envParams_.intensity = std::max(0.0f, intensity);
    envParams_.rotation = rotation;
    envParams_.backgroundIntensity = std::max(0.0f, backgroundIntensity);
    uploadEnvironmentParams();
    return true;
}

void GpuScene::createDefaultMaterialTexture(BufferUploader& uploader) {
    if (materialSampler_ == VK_NULL_HANDLE) {
        createMaterialSampler(allocator_.device(), materialSampler_, TextureSamplerDesc{});
    }

    if (materialTextureTable_.residentCount() > 0) {
        return;
    }
    destroyMaterialTextureSamplers();

    const std::array<uint8_t, 4> white = {255, 255, 255, 255};
    std::vector<std::unique_ptr<Image>> images;
    auto image = std::make_unique<Image>(allocator_, ImageDesc{
        .width = 1,
        .height = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "default material texture",
    });
    uploader.uploadToImage2D(*image, white.data(), white.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    images.push_back(std::move(image));
    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
}

void GpuScene::createImportedMaterialTextures(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    materialTextureTable_.clear();
    destroyMaterialTextureSamplers();
    bool mixedSamplers = false;
    const TextureSamplerDesc materialSamplerDesc = selectMaterialSampler(importedScene, assets, mixedSamplers);
    createMaterialSampler(allocator_.device(), materialSampler_, materialSamplerDesc);
    if (mixedSamplers) {
        std::cout << "glTF scene uses mixed texture samplers; using the first sampler until per-texture bindless samplers are enabled.\n";
    }

    const std::vector<TextureColorUsage> usage = classifyTextureUsage(importedScene, assets);
    const uint32_t textureCount = std::min<uint32_t>(static_cast<uint32_t>(importedScene.textures.size()), maxMaterialTextures);

    struct PendingTexture {
        std::unique_ptr<Image> image;
        std::vector<uint8_t> pixels;
        TextureSamplerDesc sampler;
    };
    std::vector<PendingTexture> pendingTextures;
    pendingTextures.reserve(std::max(1u, textureCount));

    for (uint32_t slot = 0; slot < std::max(1u, textureCount); ++slot) {
        const TextureAsset* texture = slot < textureCount ? assets.texture(importedScene.textures[slot]) : nullptr;
        std::vector<uint8_t> pixels;
        uint32_t width = 1;
        uint32_t height = 1;
        const char* name = "imported material texture";
        if (texture != nullptr && !texture->rgba8.empty() && texture->width > 0 && texture->height > 0) {
            pixels = texture->fallback ? fallbackTexturePixels(usage, slot) : texture->rgba8;
            width = texture->width;
            height = texture->height;
            name = texture->fallback ? "fallback material texture" : name;
        } else {
            pixels = fallbackTexturePixels(usage, slot);
            name = "default material texture";
        }

        const uint32_t textureMipLevels = texture != nullptr && texture->isCompressed
            ? 1u
            : std::max(1u, static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1u));
        const VkFormat textureFormat = texture != nullptr && texture->isCompressed
            ? texture->compressedFormat
            : (uploadTextureAsSrgb(usage, slot) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);

        auto image = std::make_unique<Image>(allocator_, ImageDesc{
            .width = width,
            .height = height,
            .mipLevels = textureMipLevels,
            .format = textureFormat,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .debugName = name,
        });

        const TextureSamplerDesc samplerDesc = texture != nullptr ? texture->sampler : materialSamplerDesc;
        pendingTextures.push_back({
            std::move(image),
            std::move(pixels),
            samplerDesc,
        });
    }

    BatchUploader batch(uploader);
    batch.begin();
    for (auto& pending : pendingTextures) {
        batch.enqueueImageUpload(*pending.image, pending.pixels.data(), pending.pixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    batch.submit();

    std::vector<std::unique_ptr<Image>> images;
    images.reserve(pendingTextures.size());
    for (auto& pending : pendingTextures) {
        materialTextureSamplers_.push_back(createOwnedMaterialSampler(allocator_.device(), pending.sampler));
        images.push_back(std::move(pending.image));
    }

    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
    std::cout << "Material textures resident: " << materialTextureTable_.residentCount() << " / " << materialTextureTable_.slotCount() << " slots\n";
}

void GpuScene::createCachedMaterialTextures(BufferUploader& uploader, const CachedScene& cached) {
    materialTextureTable_.clear();
    destroyMaterialTextureSamplers();

    TextureSamplerDesc materialSamplerDesc{};
    if (!cached.textures.empty()) {
        const CachedTextureData& first = cached.textures.front();
        materialSamplerDesc.minFilter = static_cast<TextureFilter>(first.minFilter);
        materialSamplerDesc.magFilter = static_cast<TextureFilter>(first.magFilter);
        materialSamplerDesc.wrapS = static_cast<TextureWrap>(first.wrapS);
        materialSamplerDesc.wrapT = static_cast<TextureWrap>(first.wrapT);
    }
    createMaterialSampler(allocator_.device(), materialSampler_, materialSamplerDesc);

    const uint32_t textureCount = std::min<uint32_t>(static_cast<uint32_t>(cached.textures.size()), maxMaterialTextures);

    struct PendingTexture {
        std::unique_ptr<Image> image;
        std::vector<uint8_t> pixels;
        TextureSamplerDesc sampler;
    };
    std::vector<PendingTexture> pendingTextures;
    pendingTextures.reserve(std::max(1u, textureCount));

    for (uint32_t slot = 0; slot < std::max(1u, textureCount); ++slot) {
        const CachedTextureData* texture = slot < textureCount ? &cached.textures[slot] : nullptr;
        std::vector<uint8_t> pixels;
        uint32_t width = 1;
        uint32_t height = 1;
        bool srgb = false;
        const char* name = "cached material texture";
        TextureSamplerDesc samplerDesc = materialSamplerDesc;
        if (texture != nullptr && !texture->rgba8.empty() && texture->width > 0 && texture->height > 0) {
            pixels = texture->rgba8;
            width = texture->width;
            height = texture->height;
            srgb = texture->srgb;
            samplerDesc.minFilter = static_cast<TextureFilter>(texture->minFilter);
            samplerDesc.magFilter = static_cast<TextureFilter>(texture->magFilter);
            samplerDesc.wrapS = static_cast<TextureWrap>(texture->wrapS);
            samplerDesc.wrapT = static_cast<TextureWrap>(texture->wrapT);
        } else {
            pixels = {255, 255, 255, 255};
            name = "default cached material texture";
        }

        const bool textureCompressed = texture != nullptr && texture->isCompressed;
        const uint32_t textureMipLevels = textureCompressed
            ? 1u
            : std::max(1u, static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))) + 1u));
        const VkFormat textureFormat = textureCompressed
            ? static_cast<VkFormat>(texture->compressedFormat)
            : (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        auto image = std::make_unique<Image>(allocator_, ImageDesc{
            .width = width,
            .height = height,
            .mipLevels = textureMipLevels,
            .format = textureFormat,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .debugName = name,
        });
        pendingTextures.push_back({
            std::move(image),
            std::move(pixels),
            samplerDesc,
        });
    }

    BatchUploader batch(uploader);
    batch.begin();
    for (auto& pending : pendingTextures) {
        batch.enqueueImageUpload(*pending.image, pending.pixels.data(), pending.pixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    batch.submit();

    std::vector<std::unique_ptr<Image>> images;
    images.reserve(pendingTextures.size());
    for (auto& pending : pendingTextures) {
        materialTextureSamplers_.push_back(createOwnedMaterialSampler(allocator_.device(), pending.sampler));
        images.push_back(std::move(pending.image));
    }

    materialTextureTable_.setImages(std::move(images), maxMaterialTextures);
    std::cout << "Cached material textures resident: " << materialTextureTable_.residentCount() << " / " << materialTextureTable_.slotCount() << " slots\n";
}

void GpuScene::loadEnvironment(BufferUploader& uploader, const std::filesystem::path& path) {
    environmentPath_ = path;
    createEnvironment(uploader);
}

bool GpuScene::updateImportedMaterials(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    if (materials_ == nullptr || importedScene.materials.empty()) {
        return false;
    }

    const std::vector<TextureColorUsage> textureUsage = classifyTextureUsage(importedScene, assets);
    auto textureSlotFor = [&](TextureAssetHandle texture) {
        const uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, texture, maxMaterialTextures);
        return slot == UINT32_MAX ? -1.0f : static_cast<float>(slot);
    };

    std::vector<glm::vec4> materialData;
    materialData.reserve(importedScene.materials.size() * materialVec4Stride);
    for (MaterialAssetHandle handle : importedScene.materials) {
        const MaterialAsset* material = assets.material(handle);
        const glm::vec4 base = material != nullptr ? material->baseColorFactor : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        const glm::vec3 emissive = material != nullptr ? material->emissiveFactor : glm::vec3(0.0f);
        const float roughness = material != nullptr ? material->roughnessFactor : 1.0f;
        const float metallic = material != nullptr ? material->metallicFactor : 0.0f;
        const uint32_t type = 3u;
        uint32_t flags = 0;
        if (material != nullptr) {
            uint32_t slot = textureSlotIndexFor(material->baseColorTexture);
            if (slot != UINT32_MAX && !uploadTextureAsSrgb(textureUsage, slot)) {
                flags |= materialFlagManualBaseColorSrgb;
            }
            slot = textureSlotIndexFor(material->emissiveTexture);
            if (slot != UINT32_MAX && !uploadTextureAsSrgb(textureUsage, slot)) {
                flags |= materialFlagManualEmissiveSrgb;
            }
        }

        materialData.push_back({glm::vec3(base), roughness});
        materialData.push_back({1.5f, static_cast<float>(type), metallic, static_cast<float>(flags)});
        materialData.push_back({emissive, base.a});
        materialData.push_back({
            material != nullptr ? textureSlotFor(material->baseColorTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->normalTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->metallicRoughnessTexture) : -1.0f,
            material != nullptr ? textureSlotFor(material->emissiveTexture) : -1.0f});
        materialData.push_back({
            material != nullptr ? material->alphaCutoff : 0.5f,
            material != nullptr ? static_cast<float>(material->alphaMode) : 0.0f,
            material != nullptr ? static_cast<float>(material->doubleSided) : 0.0f,
            0.0f});
    }

    const VkDeviceSize byteSize = sizeof(glm::vec4) * materialData.size();
    if (byteSize == 0 || byteSize > materials_->size()) {
        return false;
    }
    uploader.uploadToBuffer(*materials_, materialData.data(), byteSize);
    return true;
}

bool GpuScene::updateSceneLights(BufferUploader& uploader, const SceneAsset& scene) {
    if (lightRecords_ == nullptr || meshParamsBuffer_ == nullptr) {
        return false;
    }

    float totalWeight = emissiveLightRecords_.empty() ? 0.0f : emissiveLightRecords_.back().data0.y;
    std::vector<GpuLightRecord> records = combineLightRecords(emissiveLightRecords_, scene.lights, totalWeight, totalWeight);
    uploadLightRecords(uploader, std::move(records), totalWeight);
    return true;
}

bool GpuScene::updateInstanceTransforms(BufferUploader& uploader, const SceneAsset& scene, const AssetManager& assets) {
    if (instanceRecords_ == nullptr || instanceBounds_ == nullptr || tlasNodes_ == nullptr || tlasInstanceIndices_ == nullptr || meshParams_.meshCount == 0) {
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> meshRecordIndexForAsset;
    std::vector<CpuBounds> localMeshBounds;
    std::vector<uint32_t> primitiveOffsets;
    std::vector<uint32_t> primitiveCounts;
    localMeshBounds.reserve(scene.meshes.size());
    primitiveOffsets.reserve(scene.meshes.size());
    primitiveCounts.reserve(scene.meshes.size());

    uint32_t primitiveOffset = 0;
    for (MeshAssetHandle handle : scene.meshes) {
        const MeshAsset* mesh = assets.mesh(handle);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty()) {
            continue;
        }
        CpuBounds bounds;
        for (const MeshVertex& vertex : mesh->vertices) {
            includePoint(bounds, vertex.position);
        }
        const uint32_t meshRecordIndex = static_cast<uint32_t>(localMeshBounds.size());
        meshRecordIndexForAsset.emplace(handle.index, meshRecordIndex);
        localMeshBounds.push_back(bounds);
        primitiveOffsets.push_back(primitiveOffset);
        primitiveCounts.push_back(static_cast<uint32_t>(mesh->primitives.size()));
        primitiveOffset += static_cast<uint32_t>(mesh->primitives.size());
    }

    std::vector<GpuInstanceRecord> instanceRecords;
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    std::vector<RayTracingInstanceBuildInput> rayTracingInstances;
    const std::vector<GpuInstanceRecord> previousInstanceRecords = instanceRecordCpu_;

    auto appendInstance = [&](const glm::mat4& transform, MeshAssetHandle meshHandle, uint32_t flags) {
        const auto recordIt = meshRecordIndexForAsset.find(meshHandle.index);
        if (recordIt == meshRecordIndexForAsset.end()) {
            return;
        }
        const uint32_t meshRecordIndex = recordIt->second;
        const uint32_t instanceIndex = static_cast<uint32_t>(instanceRecords.size());
        const glm::mat4 prevTransform =
            instanceIndex < previousInstanceRecords.size()
                ? previousInstanceRecords[instanceIndex].transform
                : transform;
        instanceRecords.push_back(makeInstanceRecord(
            transform,
            meshRecordIndex,
            primitiveOffsets[meshRecordIndex],
            primitiveCounts[meshRecordIndex],
            flags,
            &prevTransform));
        instanceBounds.push_back(makeInstanceBoundsRecord(transformBounds(localMeshBounds[meshRecordIndex], transform), instanceIndex, meshRecordIndex));
        rayTracingInstances.push_back(RayTracingInstanceBuildInput{
            .instanceIndex = instanceIndex,
            .meshIndex = meshRecordIndex,
            .transform = transform,
            .flags = flags,
            .visible = (flags & instanceFlagVisible) != 0u,
        });
    };

    auto visitNode = [&](auto&& self, uint32_t nodeIndex, glm::mat4 parent) -> void {
        if (nodeIndex >= scene.nodes.size()) {
            return;
        }
        const SceneNodeAsset& node = scene.nodes[nodeIndex];
        const glm::mat4 world = parent * node.transform;
        if (node.mesh.valid()) {
            appendInstance(world, node.mesh, nodeInstanceFlags(node));
        }
        for (uint32_t child : node.children) {
            self(self, child, world);
        }
    };

    if (!scene.rootNodes.empty()) {
        for (uint32_t root : scene.rootNodes) {
            visitNode(visitNode, root, glm::mat4{1.0f});
        }
    } else {
        for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
            if (scene.nodes[i].parent < 0) {
                visitNode(visitNode, i, glm::mat4{1.0f});
            }
        }
    }

    if (instanceRecords.empty() || instanceRecords.size() != meshParams_.instanceCount) {
        return false;
    }

    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);
    if (tlasData.empty() || tlasInstanceIndices.empty()) {
        return false;
    }

    uploadVector(allocator_, uploader, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "updated instance records");
    uploadVector(allocator_, uploader, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "updated instance bounds");
    uploadVector(allocator_, uploader, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "updated tlas nodes");
    uploadVector(allocator_, uploader, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "updated tlas instance indices");

    instanceRecordCpu_ = instanceRecords;
    rayTracingInstances_ = std::move(rayTracingInstances);
    meshParams_.tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u);
    meshParams_.tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size());
    if (meshParamsBuffer_) {
        meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
        meshParamsBuffer_->flush(sizeof(meshParams_));
    }
    return true;
}

void GpuScene::createCornellBox(BufferUploader& uploader) {
    createDefaultMaterialTexture(uploader);

    const float s = 1.5f;
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> faceMaterials;
    std::vector<GpuLocalVertex> localVertexData;

    auto pushQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, uint32_t material) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 tangent = glm::normalize(b - a);
        vertices.insert(vertices.end(), {a, b, c, d});
        localVertexData.insert(localVertexData.end(), {
            makeLocalVertex(a, normal, {0.0f, 0.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(b, normal, {1.0f, 0.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(c, normal, {1.0f, 1.0f}, glm::vec4{tangent, 1.0f}),
            makeLocalVertex(d, normal, {0.0f, 1.0f}, glm::vec4{tangent, 1.0f}),
        });
        indices.insert(indices.end(), {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
        faceMaterials.insert(faceMaterials.end(), {material, material});
    };

    // Five inward-facing Cornell-box walls. The front is intentionally open for the camera.
    pushQuad({-s, -s, -s}, { s, -s, -s}, { s,  s, -s}, {-s,  s, -s}, 0); // back
    pushQuad({-s, -s,  s}, {-s, -s, -s}, {-s,  s, -s}, {-s,  s,  s}, 1); // left
    pushQuad({ s, -s, -s}, { s, -s,  s}, { s,  s,  s}, { s,  s, -s}, 2); // right
    pushQuad({-s, -s,  s}, { s, -s,  s}, { s, -s, -s}, {-s, -s, -s}, 0); // floor
    pushQuad({-s,  s, -s}, { s,  s, -s}, { s,  s,  s}, {-s,  s,  s}, 0); // ceiling

    const float lightSize = 0.6f;
    const float lightY = s - 0.01f;
    pushQuad(
        {-lightSize, lightY, -lightSize},
        { lightSize, lightY, -lightSize},
        { lightSize, lightY,  lightSize},
        {-lightSize, lightY,  lightSize},
        3);

    const std::vector<MaterialCpu> mats = {
        {{0.73f, 0.73f, 0.73f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{0.63f, 0.06f, 0.05f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{0.14f, 0.45f, 0.09f}, 1.0f, 1.0f, 0, 0.0f, {0.0f, 0.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, 0, 0.0f, {15.0f, 13.0f, 10.0f}},
        {{0.86f, 0.95f, 1.0f}, 0.0f, 1.33f, 2, 0.0f, {}},
        {{0.95f, 0.96f, 0.97f}, 0.08f, 1.5f, 3, 1.0f, {}},
        {{0.95f, 0.93f, 0.88f}, 0.12f, 1.5f, 4, 0.0f, {}},
    };
    const std::vector<bool> materialOpaqueTraversalSafe(mats.size(), true);

    auto pushSphereMesh = [&](glm::vec3 center, float radius, uint32_t material) {
        constexpr uint32_t longitude = 160;
        constexpr uint32_t latitude = 80;
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (uint32_t y = 0; y <= latitude; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(latitude);
            const float phi = v * 3.14159265358979323846f;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);
            for (uint32_t x = 0; x <= longitude; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(longitude);
                const float theta = u * 2.0f * 3.14159265358979323846f;
                const glm::vec3 normal{
                    sinPhi * std::cos(theta),
                    cosPhi,
                    sinPhi * std::sin(theta),
                };
                const glm::vec3 tangent = glm::normalize(glm::vec3{-std::sin(theta), 0.0f, std::cos(theta)});
                const glm::vec3 position = center + normal * radius;
                vertices.push_back(position);
                localVertexData.push_back(makeLocalVertex(position, normal, {u, v}, glm::vec4{tangent, 1.0f}));
            }
        }

        const uint32_t rowStride = longitude + 1u;
        for (uint32_t y = 0; y < latitude; ++y) {
            for (uint32_t x = 0; x < longitude; ++x) {
                const uint32_t i0 = base + y * rowStride + x;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + rowStride;
                const uint32_t i3 = i2 + 1u;
                indices.insert(indices.end(), {i0, i1, i2, i1, i3, i2});
                faceMaterials.insert(faceMaterials.end(), {material, material});
            }
        }
    };

    pushSphereMesh({-0.6f, -1.2f, -0.5f}, 0.3f, 4u);
    pushSphereMesh({ 0.0f, -1.2f, -0.8f}, 0.3f, 5u);
    pushSphereMesh({ 0.6f, -1.2f, -0.5f}, 0.3f, 6u);

    std::vector<glm::vec4> vertexData;
    vertexData.reserve(vertices.size());
    for (glm::vec3 v : vertices) {
        vertexData.push_back({v, 0.0f});
    }
    const std::vector<uint32_t> localIndices = indices;

    const BvhBuildResult bvh = buildBvh(vertices, indices, faceMaterials);
    const std::vector<glm::vec4> bvhData = packBvhNodesForGpu(bvh.packedNodes);
    const std::vector<glm::vec4> triangleData = packTrianglesForGpu(bvh);
    const std::vector<glm::vec4> localBvhData = bvhData;
    const std::vector<glm::vec4> localTriangleData = triangleData;
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(faceMaterials.size());
    for (uint32_t triangle = 0; triangle < static_cast<uint32_t>(faceMaterials.size()); ++triangle) {
        primitiveRecords.push_back(makePrimitiveRecord(
            triangle * 3u,
            3u,
            0u,
            faceMaterials[triangle],
            triangle,
            1u));
    }
    const std::vector<GpuMeshRecord> meshRecords = {
        makeMeshRecord(
            0u,
            static_cast<uint32_t>(vertices.size()),
            0u,
            static_cast<uint32_t>(indices.size()),
            0u,
            static_cast<uint32_t>(primitiveRecords.size()),
            0u,
            static_cast<uint32_t>(bvh.packedNodes.size()),
            0u,
            static_cast<uint32_t>(bvh.leafTriangleIndices.size())),
    };
    const std::vector<GpuInstanceRecord> instanceRecords = {
        makeInstanceRecord(glm::mat4{1.0f}, 0u, 0u, static_cast<uint32_t>(primitiveRecords.size())),
    };
    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    rayTracingMeshes_.clear();
    rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
        .meshIndex = 0u,
        .firstVertex = 0u,
        .vertexCount = static_cast<uint32_t>(localVertexData.size()),
        .firstIndex = 0u,
        .indexCount = static_cast<uint32_t>(localIndices.size()),
        .primitiveOffset = 0u,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
            primitiveRecords,
            0u,
            static_cast<uint32_t>(primitiveRecords.size()),
            materialOpaqueTraversalSafe),
        .updateMode = AccelUpdateMode::Static,
    });
    rayTracingInstances_.clear();
    rayTracingInstances_.push_back(RayTracingInstanceBuildInput{
        .instanceIndex = 0u,
        .meshIndex = 0u,
        .transform = glm::mat4{1.0f},
    });
    const CpuBounds sceneBounds = boundsFromPositions(vertices);
    const std::vector<GpuInstanceBoundsRecord> instanceBounds = {
        makeInstanceBoundsRecord(sceneBounds, 0u, 0u),
    };
    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);

    std::vector<glm::vec4> materialData;
    std::vector<glm::vec3> materialEmissive;
    materialEmissive.reserve(mats.size());
    for (const auto& m : mats) {
        materialData.push_back({m.color, m.roughness});
        materialData.push_back({m.ior, static_cast<float>(m.type), m.metallic, 0.0f});
        materialData.push_back({m.emissive, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 1.0f, 0.0f});
        materialEmissive.push_back(m.emissive);
    }

    std::vector<glm::vec4> sphereData;

    float emissiveTotalArea = 0.0f;
    emissiveLightRecords_ = buildLightRecords(meshRecords, instanceRecords, localTriangleData, materialEmissive, sphereData, emissiveTotalArea);
    const std::vector<GpuLightRecord> lightRecords = emissiveLightRecords_;

    meshParams_ = {
        .vertexCount = static_cast<uint32_t>(vertices.size()),
        .triangleCount = static_cast<uint32_t>(bvh.triangles.size()),
        .bvhNodeCount = static_cast<uint32_t>(bvh.packedNodes.size()),
        .materialCount = static_cast<uint32_t>(mats.size()),
        .enabled = 1,
        .sphereCount = 0,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .instanceCount = static_cast<uint32_t>(instanceRecords.size()),
        .lightCount = static_cast<uint32_t>(lightRecords.size()),
        .emissiveTotalArea = emissiveTotalArea,
        .meshCount = static_cast<uint32_t>(meshRecords.size()),
        .localVertexCount = static_cast<uint32_t>(localVertexData.size()),
        .localIndexCount = static_cast<uint32_t>(localIndices.size()),
        .localBvhNodeCount = static_cast<uint32_t>(bvh.packedNodes.size()),
        .localTriangleCount = static_cast<uint32_t>(bvh.leafTriangleIndices.size()),
        .tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u),
        .tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size()),
    };
    std::cout << "Cornell scene: vertices=" << meshParams_.vertexCount
              << " triangles=" << meshParams_.triangleCount
              << " bvh_nodes=" << meshParams_.bvhNodeCount
              << " materials=" << meshParams_.materialCount << '\n';

    {
        BatchUploader batch(uploader);
        batch.begin();
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertexData, "scene vertices");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indices, "scene indices");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bvhData, "scene bvh nodes");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangleData, "scene triangles");
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "scene materials");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sphereData, "scene spheres");
        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "scene mesh records");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "scene primitive records");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "scene instance records");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "scene rt triangle material ids");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "scene emissive light records");
        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "scene local mesh vertices");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "scene local mesh indices");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "scene instance bounds");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "scene local bvh nodes");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "scene local bvh triangles");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "scene tlas nodes");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "scene tlas instance indices");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "mesh params",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::createImportedScene(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets) {
    createImportedMaterialTextures(uploader, importedScene, assets);

    std::vector<glm::vec4> vertexData;
    std::vector<uint32_t> indices;
    std::vector<glm::vec4> bvhData;
    std::vector<glm::vec4> triangleData;
    std::vector<glm::vec4> materialData;
    std::vector<glm::vec3> materialEmissive;
    std::vector<bool> materialOpaqueTraversalSafe;
    std::vector<GpuMeshRecord> meshRecords;
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    std::vector<GpuInstanceRecord> instanceRecords;
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    std::vector<GpuLocalVertex> localVertexData;
    std::vector<uint32_t> localIndices;
    std::vector<glm::vec4> localBvhData;
    std::vector<glm::vec4> localTriangleData;
    std::vector<glm::vec4> tlasData;
    std::vector<uint32_t> tlasInstanceIndices;
    std::vector<CpuBounds> localMeshBounds;

    std::vector<MaterialAssetHandle> materialHandles = importedScene.materials;
    if (materialHandles.empty()) {
        materialHandles.push_back(MaterialAssetHandle{0});
    }
    const std::vector<TextureColorUsage> textureUsage = classifyTextureUsage(importedScene, assets);
    auto textureSlotFor = [&](TextureAssetHandle texture) {
        const uint32_t slot = GpuScene::textureSlotIndexFor(importedScene, texture, maxMaterialTextures);
        return slot == UINT32_MAX ? -1.0f : static_cast<float>(slot);
    };

    for (MaterialAssetHandle handle : materialHandles) {
        const MaterialAsset* material = assets.material(handle);
        const glm::vec4 base = material != nullptr ? material->baseColorFactor : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        const glm::vec3 emissive = material != nullptr ? material->emissiveFactor : glm::vec3(0.0f);
        const float roughness = material != nullptr ? material->roughnessFactor : 1.0f;
        const float metallic = material != nullptr ? material->metallicFactor : 0.0f;
        const uint32_t type = 3u;
        const float baseColorTexture = material != nullptr ? textureSlotFor(material->baseColorTexture) : -1.0f;
        const float normalTexture = material != nullptr ? textureSlotFor(material->normalTexture) : -1.0f;
        const float metallicRoughnessTexture = material != nullptr ? textureSlotFor(material->metallicRoughnessTexture) : -1.0f;
        const float emissiveTexture = material != nullptr ? textureSlotFor(material->emissiveTexture) : -1.0f;
        uint32_t flags = 0;
        if (material != nullptr) {
            uint32_t slot = textureSlotIndexFor(material->baseColorTexture);
            if (slot != UINT32_MAX && !uploadTextureAsSrgb(textureUsage, slot)) {
                flags |= materialFlagManualBaseColorSrgb;
            }
            slot = textureSlotIndexFor(material->emissiveTexture);
            if (slot != UINT32_MAX && !uploadTextureAsSrgb(textureUsage, slot)) {
                flags |= materialFlagManualEmissiveSrgb;
            }
        }
        materialData.push_back({glm::vec3(base), roughness});
        materialData.push_back({1.5f, static_cast<float>(type), metallic, static_cast<float>(flags)});
        materialData.push_back({emissive, base.a});
        materialData.push_back({baseColorTexture, normalTexture, metallicRoughnessTexture, emissiveTexture});
        materialData.push_back({
            material != nullptr ? material->alphaCutoff : 0.5f,
            material != nullptr ? static_cast<float>(material->alphaMode) : 0.0f,
            material != nullptr ? static_cast<float>(material->doubleSided) : 0.0f,
            0.0f});
        materialEmissive.push_back(emissive);
        materialOpaqueTraversalSafe.push_back(material != nullptr && material->alphaMode == 0u && material->doubleSided != 0u);
    }
    if (materialData.empty()) {
        materialData.push_back({0.8f, 0.8f, 0.8f, 1.0f});
        materialData.push_back({1.5f, 0.0f, 0.0f, 0.0f});
        materialData.push_back({0.0f, 0.0f, 0.0f, 1.0f});
        materialData.push_back({-1.0f, -1.0f, -1.0f, -1.0f});
        materialData.push_back({0.5f, 0.0f, 0.0f, 0.0f});
        materialEmissive.push_back(glm::vec3(0.0f));
        materialOpaqueTraversalSafe.push_back(false);
    }

    std::unordered_map<uint32_t, uint32_t> materialIndexForAsset;
    materialIndexForAsset.reserve(materialHandles.size());
    for (uint32_t i = 0; i < materialHandles.size(); ++i) {
        materialIndexForAsset.emplace(materialHandles[i].index, i);
    }

    std::unordered_map<uint32_t, uint32_t> meshRecordIndexForAsset;
    meshRecordIndexForAsset.reserve(importedScene.meshes.size());
    uint64_t importedTriangleCount = 0;
    for (MeshAssetHandle handle : importedScene.meshes) {
        if (const MeshAsset* mesh = assets.mesh(handle)) {
            importedTriangleCount += mesh->indices.size() / 3u;
        }
    }
    const BvhBuildQuality importedBvhQuality = importedTriangleCount >= fastImportedBvhTriangleThreshold
        ? BvhBuildQuality::MortonFast
        : BvhBuildQuality::BinnedSah;
    if (importedBvhQuality == BvhBuildQuality::MortonFast) {
        std::cout << "Large imported geometry detected; using fast Morton BVH build for " << importedTriangleCount << " triangles.\n";
    }

    struct MeshPrep {
        MeshAssetHandle handle;
        const MeshAsset* mesh = nullptr;
        uint32_t firstVertex = 0;
        uint32_t firstIndex = 0;
        uint32_t primitiveOffset = 0;
        uint32_t localTriangleCursor = 0;
        uint32_t meshRecordIndex = 0;
        CpuBounds localBounds;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> texcoords;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec4> tangents;
        std::vector<uint32_t> faceMaterials;
    };

    std::vector<MeshPrep> meshPrep;
    meshPrep.reserve(importedScene.meshes.size());
    uint32_t localTriangleCursor = 0;

    for (MeshAssetHandle handle : importedScene.meshes) {
        const MeshAsset* mesh = assets.mesh(handle);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty()) {
            continue;
        }

        MeshPrep prep;
        prep.handle = handle;
        prep.mesh = mesh;
        prep.firstVertex = static_cast<uint32_t>(localVertexData.size());
        prep.firstIndex = static_cast<uint32_t>(localIndices.size());
        prep.primitiveOffset = static_cast<uint32_t>(primitiveRecords.size());
        prep.localTriangleCursor = localTriangleCursor;
        prep.positions.reserve(mesh->vertices.size());
        prep.texcoords.reserve(mesh->vertices.size());
        prep.normals.reserve(mesh->vertices.size());
        prep.tangents.reserve(mesh->vertices.size());
        localVertexData.reserve(localVertexData.size() + mesh->vertices.size());

        for (const MeshVertex& vertex : mesh->vertices) {
            const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
            const glm::vec3 normal = normalLen2 > 1.0e-10f ? glm::normalize(vertex.normal) : glm::vec3{0.0f, 1.0f, 0.0f};
            glm::vec3 tangent = glm::vec3(vertex.tangent);
            const float tangentLen2 = glm::dot(tangent, tangent);
            tangent = tangentLen2 > 1.0e-10f ? glm::normalize(tangent) : glm::vec3{1.0f, 0.0f, 0.0f};
            const glm::vec4 packedTangent{tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f};
            localVertexData.push_back(makeLocalVertex(vertex.position, normal, vertex.texcoord, packedTangent));
            prep.positions.push_back(vertex.position);
            prep.texcoords.push_back(vertex.texcoord);
            prep.normals.push_back(normal);
            prep.tangents.push_back(packedTangent);
            includePoint(prep.localBounds, vertex.position);
        }
        localIndices.reserve(localIndices.size() + mesh->indices.size());
        for (uint32_t index : mesh->indices) {
            localIndices.push_back(prep.firstVertex + index);
        }

        for (const MeshPrimitiveAsset& primitive : mesh->primitives) {
            const uint32_t triangleCount = primitive.indexCount / 3u;
            const auto materialIt = materialIndexForAsset.find(primitive.material.index);
            const uint32_t materialIndex = materialIt != materialIndexForAsset.end() ? materialIt->second : 0u;
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                prep.faceMaterials.push_back(materialIndex);
            }
            primitiveRecords.push_back(makePrimitiveRecord(
                prep.firstIndex + primitive.firstIndex,
                primitive.indexCount,
                prep.firstVertex + primitive.firstVertex,
                materialIndex,
                prep.localTriangleCursor,
                triangleCount));
            prep.localTriangleCursor += triangleCount;
        }
        localTriangleCursor = prep.localTriangleCursor;
        meshPrep.push_back(std::move(prep));
    }

    std::vector<ParallelBvhBuildTask> bvhTasks;
    bvhTasks.reserve(meshPrep.size());
    for (const auto& prep : meshPrep) {
        ParallelBvhBuildTask task;
        task.vertices = &prep.positions;
        task.indices = &prep.mesh->indices;
        task.faceMaterials = &prep.faceMaterials;
        task.texcoords = &prep.texcoords;
        task.normals = &prep.normals;
        task.tangents = &prep.tangents;
        task.quality = importedBvhQuality;
        bvhTasks.push_back(task);
    }

    const auto bvhResults = ParallelBvhBuilder::buildAll(bvhTasks);

    for (size_t i = 0; i < meshPrep.size(); ++i) {
        const auto& prep = meshPrep[i];
        const BvhBuildResult& localBvh = bvhResults[i].bvh;

        const uint32_t localBvhNodeOffset = static_cast<uint32_t>(localBvhData.size() / 4u);
        const uint32_t localTriangleOffset = static_cast<uint32_t>(localTriangleData.size() / 12u);

        const std::vector<glm::vec4> packedLocalBvh = packBvhNodesForGpu(localBvh.packedNodes);
        const std::vector<glm::vec4> packedLocalTriangles = packTrianglesForGpu(localBvh);
        localBvhData.insert(localBvhData.end(), packedLocalBvh.begin(), packedLocalBvh.end());
        localTriangleData.insert(localTriangleData.end(), packedLocalTriangles.begin(), packedLocalTriangles.end());

        const uint32_t meshRecordIndex = static_cast<uint32_t>(meshRecords.size());
        meshRecords.push_back(makeMeshRecord(
            prep.firstVertex,
            static_cast<uint32_t>(prep.mesh->vertices.size()),
            prep.firstIndex,
            static_cast<uint32_t>(prep.mesh->indices.size()),
            prep.primitiveOffset,
            static_cast<uint32_t>(prep.mesh->primitives.size()),
            localBvhNodeOffset,
            static_cast<uint32_t>(localBvh.packedNodes.size()),
            localTriangleOffset,
            static_cast<uint32_t>(localBvh.leafTriangleIndices.size())));
        rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
            .meshIndex = meshRecordIndex,
            .firstVertex = prep.firstVertex,
            .vertexCount = static_cast<uint32_t>(prep.mesh->vertices.size()),
            .firstIndex = prep.firstIndex,
            .indexCount = static_cast<uint32_t>(prep.mesh->indices.size()),
            .primitiveOffset = prep.primitiveOffset,
            .primitiveCount = static_cast<uint32_t>(prep.mesh->primitives.size()),
            .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
                primitiveRecords,
                prep.primitiveOffset,
                static_cast<uint32_t>(prep.mesh->primitives.size()),
                materialOpaqueTraversalSafe),
            .updateMode = AccelUpdateMode::Static,
        });
        localMeshBounds.push_back(prep.localBounds);
        meshRecordIndexForAsset.emplace(prep.handle.index, meshRecordIndex);
    }

    auto appendInstance = [&](const glm::mat4& transform, uint32_t meshIndex, uint32_t flags) {
        auto recordIt = meshRecordIndexForAsset.find(meshIndex);
        if (recordIt == meshRecordIndexForAsset.end()) {
            return;
        }
        const uint32_t meshRecordIndex = recordIt->second;
        const GpuMeshRecord& meshRecord = meshRecords[meshRecordIndex];
        const uint32_t primitiveOffset = meshRecord.primitiveData.x;
        const uint32_t primitiveCount = meshRecord.primitiveData.y;
        const uint32_t instanceIndex = static_cast<uint32_t>(instanceRecords.size());
        instanceRecords.push_back(makeInstanceRecord(transform, meshRecordIndex, primitiveOffset, primitiveCount, flags));
        rayTracingInstances_.push_back(RayTracingInstanceBuildInput{
            .instanceIndex = instanceIndex,
            .meshIndex = meshRecordIndex,
            .transform = transform,
            .flags = flags,
            .visible = (flags & instanceFlagVisible) != 0u,
        });
        instanceBounds.push_back(makeInstanceBoundsRecord(transformBounds(localMeshBounds[meshRecordIndex], transform), instanceIndex, meshRecordIndex));
    };

    auto visitNode = [&](auto&& self, uint32_t nodeIndex, glm::mat4 parent) -> void {
        if (nodeIndex >= importedScene.nodes.size()) {
            return;
        }
        const SceneNodeAsset& node = importedScene.nodes[nodeIndex];
        const glm::mat4 world = parent * node.transform;
        if (node.mesh.valid()) {
            appendInstance(world, node.mesh.index, nodeInstanceFlags(node));
        }
        for (uint32_t child : node.children) {
            self(self, child, world);
        }
    };
    if (!importedScene.rootNodes.empty()) {
        for (uint32_t root : importedScene.rootNodes) {
            visitNode(visitNode, root, glm::mat4{1.0f});
        }
    } else {
        for (uint32_t i = 0; i < importedScene.nodes.size(); ++i) {
            if (importedScene.nodes[i].parent < 0) {
                visitNode(visitNode, i, glm::mat4{1.0f});
            }
        }
    }

    if (instanceRecords.empty() || localVertexData.empty() || localIndices.empty() || localBvhData.empty() || localTriangleData.empty()) {
        createCornellBox(uploader);
        return;
    }

    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    buildTlas(instanceBounds, tlasData, tlasInstanceIndices);

    std::vector<glm::vec4> sphereData;
    float emissiveTotalArea = 0.0f;
    emissiveLightRecords_ = buildLightRecords(meshRecords, instanceRecords, localTriangleData, materialEmissive, sphereData, emissiveTotalArea);
    float lightSelectionWeight = emissiveTotalArea;
    const std::vector<GpuLightRecord> lightRecords = combineLightRecords(emissiveLightRecords_, importedScene.lights, emissiveTotalArea, lightSelectionWeight);
    meshParams_ = {
        .vertexCount = static_cast<uint32_t>(localVertexData.size()),
        .triangleCount = localTriangleCursor,
        .bvhNodeCount = 0,
        .materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride),
        .enabled = 1,
        .sphereCount = 0,
        .primitiveCount = static_cast<uint32_t>(primitiveRecords.size()),
        .instanceCount = static_cast<uint32_t>(instanceRecords.size()),
        .lightCount = static_cast<uint32_t>(lightRecords.size()),
        .emissiveTotalArea = lightSelectionWeight,
        .meshCount = static_cast<uint32_t>(meshRecords.size()),
        .localVertexCount = static_cast<uint32_t>(localVertexData.size()),
        .localIndexCount = static_cast<uint32_t>(localIndices.size()),
        .localBvhNodeCount = static_cast<uint32_t>(localBvhData.size() / 4u),
        .localTriangleCount = static_cast<uint32_t>(localTriangleData.size() / 12u),
        .tlasNodeCount = static_cast<uint32_t>(tlasData.size() / 4u),
        .tlasInstanceIndexCount = static_cast<uint32_t>(tlasInstanceIndices.size()),
    };
    std::cout << "Imported scene GPU data: meshes=" << meshParams_.meshCount
              << " instances=" << meshParams_.instanceCount
              << " local_triangles=" << meshParams_.localTriangleCount
              << " local_bvh_nodes=" << meshParams_.localBvhNodeCount
              << " tlas_nodes=" << meshParams_.tlasNodeCount << '\n';

    {
        BatchUploader batch(uploader);
        batch.begin();
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vertexData, "imported scene vertices");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indices, "imported scene indices");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bvhData, "imported scene bvh nodes");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangleData, "imported scene triangles");
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "imported scene materials");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sphereData, "imported scene spheres");
        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "imported mesh records");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "imported primitive records");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "imported instance records");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "imported rt triangle material ids");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "imported emissive light records");
        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "imported local mesh vertices");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "imported local mesh indices");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "imported instance bounds");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "imported local bvh nodes");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "imported local bvh triangles");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasData, "imported tlas nodes");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasInstanceIndices, "imported tlas instance indices");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "imported mesh params",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);

    if (sceneCachePath_.has_value() && !importedScene.sourcePath.empty() && importedScene.lights.empty()) {
        CachedScene gpuCached;
        gpuCached.name = importedScene.name;
        gpuCached.sourceMtime = SceneCache::fileMtime(importedScene.sourcePath);
        std::filesystem::path binPath = importedScene.sourcePath.parent_path() / (importedScene.sourcePath.stem().string() + ".bin");
        gpuCached.sourceBinMtime = SceneCache::fileMtime(binPath);

        auto getTextureIndex = [&](TextureAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.textures.size()); ++i) {
                if (importedScene.textures[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };
        auto getMaterialIndex = [&](MaterialAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.materials.size()); ++i) {
                if (importedScene.materials[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };
        auto getMeshIndex = [&](MeshAssetHandle handle) -> int32_t {
            if (!handle.valid()) return -1;
            for (int32_t i = 0; i < static_cast<int32_t>(importedScene.meshes.size()); ++i) {
                if (importedScene.meshes[static_cast<size_t>(i)].index == handle.index) return i;
            }
            return -1;
        };

        for (TextureAssetHandle handle : importedScene.textures) {
            const TextureAsset* texture = assets.texture(handle);
            if (texture == nullptr) continue;
            CachedTextureData cachedTex;
            cachedTex.name = texture->name;
            cachedTex.sourcePath = texture->sourcePath.string();
            cachedTex.width = texture->width;
            cachedTex.height = texture->height;
            cachedTex.channels = texture->channels;
            cachedTex.mipLevels = texture->mipLevels;
            cachedTex.srgb = texture->srgb;
            cachedTex.fallback = texture->fallback;
            cachedTex.isCompressed = texture->isCompressed;
            cachedTex.compressedFormat = static_cast<uint32_t>(texture->compressedFormat);
            cachedTex.rgba8 = texture->rgba8;
            cachedTex.minFilter = static_cast<uint32_t>(texture->sampler.minFilter);
            cachedTex.magFilter = static_cast<uint32_t>(texture->sampler.magFilter);
            cachedTex.wrapS = static_cast<uint32_t>(texture->sampler.wrapS);
            cachedTex.wrapT = static_cast<uint32_t>(texture->sampler.wrapT);
            gpuCached.textures.push_back(std::move(cachedTex));
        }
        std::unordered_set<std::string> dependencyPaths;
        for (TextureAssetHandle handle : importedScene.textures) {
            const TextureAsset* texture = assets.texture(handle);
            if (texture == nullptr || texture->sourcePath.empty() || texture->sourcePath == importedScene.sourcePath) {
                continue;
            }
            addFileDependency(gpuCached, texture->sourcePath, dependencyPaths);
        }

        for (MaterialAssetHandle handle : importedScene.materials) {
            const MaterialAsset* material = assets.material(handle);
            if (material == nullptr) continue;
            CachedMaterialData cachedMat;
            cachedMat.name = material->name;
            cachedMat.baseColorFactor = material->baseColorFactor;
            cachedMat.emissiveFactor = material->emissiveFactor;
            cachedMat.metallicFactor = material->metallicFactor;
            cachedMat.roughnessFactor = material->roughnessFactor;
            cachedMat.alphaCutoff = material->alphaCutoff;
            cachedMat.alphaMode = material->alphaMode;
            cachedMat.doubleSided = material->doubleSided;
            cachedMat.baseColorTextureIndex = getTextureIndex(material->baseColorTexture);
            cachedMat.normalTextureIndex = getTextureIndex(material->normalTexture);
            cachedMat.metallicRoughnessTextureIndex = getTextureIndex(material->metallicRoughnessTexture);
            cachedMat.emissiveTextureIndex = getTextureIndex(material->emissiveTexture);
            gpuCached.materials.push_back(std::move(cachedMat));
        }

        uint32_t meshPrepIndex = 0;
        for (const auto& prep : meshPrep) {
            CachedMeshData cachedMesh;
            cachedMesh.name = prep.mesh->name;
            cachedMesh.vertices = prep.mesh->vertices;
            cachedMesh.indices = prep.mesh->indices;
            for (const MeshPrimitiveAsset& prim : prep.mesh->primitives) {
                CachedPrimitiveData cachedPrim;
                cachedPrim.firstVertex = prim.firstVertex;
                cachedPrim.vertexCount = prim.vertexCount;
                cachedPrim.firstIndex = prim.firstIndex;
                cachedPrim.indexCount = prim.indexCount;
                cachedPrim.materialIndex = getMaterialIndex(prim.material);
                cachedMesh.primitives.push_back(std::move(cachedPrim));
            }
            gpuCached.meshes.push_back(std::move(cachedMesh));

            const BvhBuildResult& localBvh = bvhResults[meshPrepIndex].bvh;
            CachedMeshGpuRecord gpuRec;
            const auto& rec = meshRecords[meshPrepIndex];
            gpuRec.vertexIndexData = rec.vertexIndexData;
            gpuRec.primitiveData = rec.primitiveData;
            gpuRec.bvhData = rec.bvhData;
            gpuRec.flags = rec.flags;
            gpuRec.localBvh.packedNodes = packBvhNodesForGpu(localBvh.packedNodes);
            gpuRec.localBvh.triangleData = packTrianglesForGpu(localBvh);
            gpuRec.localBvh.triangleCount = static_cast<uint32_t>(localBvh.triangles.size());
            gpuRec.localBvh.leafTriangleCount = static_cast<uint32_t>(localBvh.leafTriangleIndices.size());
            gpuCached.meshGpuRecords.push_back(std::move(gpuRec));
            ++meshPrepIndex;
        }

        for (size_t i = 0; i < importedScene.nodes.size(); ++i) {
            const SceneNodeAsset& node = importedScene.nodes[i];
            CachedNodeData cachedNode;
            cachedNode.name = node.name;
            cachedNode.transform = node.transform;
            cachedNode.meshIndex = getMeshIndex(node.mesh);
            cachedNode.parentIndex = node.parent;
            cachedNode.children = node.children;
            gpuCached.nodes.push_back(std::move(cachedNode));
        }
        gpuCached.rootNodes = importedScene.rootNodes;

        for (const auto& prim : primitiveRecords) {
            CachedPrimitiveRecord cachedPrim;
            cachedPrim.indexData = prim.indexData;
            cachedPrim.metadata = prim.metadata;
            gpuCached.primitiveRecords.push_back(cachedPrim);
        }
        for (const auto& inst : instanceRecords) {
            CachedInstanceRecord cachedInst;
            cachedInst.transform = inst.transform;
            cachedInst.inverseTransform = inst.inverseTransform;
            cachedInst.metadata = inst.metadata;
            gpuCached.instanceRecords.push_back(cachedInst);
        }
        for (const auto& bounds : instanceBounds) {
            CachedInstanceBoundsRecord cachedBounds;
            cachedBounds.bmin = bounds.bmin;
            cachedBounds.bmax = bounds.bmax;
            gpuCached.instanceBounds.push_back(cachedBounds);
        }
        gpuCached.tlasNodes = tlasData;
        gpuCached.tlasInstanceIndices = tlasInstanceIndices;
        for (const auto& light : lightRecords) {
            CachedLightRecord cachedLight;
            cachedLight.metadata = light.metadata;
            cachedLight.data0 = light.data0;
            cachedLight.data1 = light.data1;
            cachedLight.data2 = light.data2;
            cachedLight.data3 = light.data3;
            gpuCached.lightRecords.push_back(cachedLight);
        }
        gpuCached.meshParams = toCachedMeshParams(meshParams_);

        if (SceneCache::save(*sceneCachePath_, gpuCached)) {
            std::cout << "GPU scene cache saved: " << sceneCachePath_->string() << "\n";
        }
    }
}

void GpuScene::createImportedSceneFromCache(BufferUploader& uploader, const CachedScene& cached) {
    std::cout << "GPU cache hit: restoring cached BVH data for " << cached.meshGpuRecords.size() << " meshes.\n";
    createCachedMaterialTextures(uploader, cached);

    std::vector<GpuMeshRecord> meshRecords;
    meshRecords.reserve(cached.meshGpuRecords.size());
    std::vector<GpuPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(cached.primitiveRecords.size());
    std::vector<GpuInstanceRecord> instanceRecords;
    instanceRecords.reserve(cached.instanceRecords.size());
    std::vector<GpuInstanceBoundsRecord> instanceBounds;
    instanceBounds.reserve(cached.instanceBounds.size());
    std::vector<GpuLocalVertex> localVertexData;
    std::vector<uint32_t> localIndices;
    std::vector<glm::vec4> localBvhData;
    std::vector<glm::vec4> localTriangleData;
    std::vector<GpuLightRecord> lightRecords;
    lightRecords.reserve(cached.lightRecords.size());
    rayTracingMeshes_.clear();
    rayTracingInstances_.clear();
    std::vector<bool> materialOpaqueTraversalSafe;
    materialOpaqueTraversalSafe.reserve(cached.materials.size());
    for (const CachedMaterialData& material : cached.materials) {
        materialOpaqueTraversalSafe.push_back(material.alphaMode == 0u && material.doubleSided != 0u);
    }
    for (const auto& cachedPrim : cached.primitiveRecords) {
        GpuPrimitiveRecord rec{};
        rec.indexData = cachedPrim.indexData;
        rec.metadata = cachedPrim.metadata;
        primitiveRecords.push_back(rec);
    }

    for (const auto& cachedMesh : cached.meshGpuRecords) {
        GpuMeshRecord rec{};
        rec.vertexIndexData = cachedMesh.vertexIndexData;
        rec.primitiveData = cachedMesh.primitiveData;
        rec.flags = cachedMesh.flags;

        rec.bvhData.x = static_cast<uint32_t>(localBvhData.size() / 4u);
        rec.bvhData.y = static_cast<uint32_t>(cachedMesh.localBvh.packedNodes.size());
        rec.bvhData.z = static_cast<uint32_t>(localTriangleData.size() / 12u);
        rec.bvhData.w = cachedMesh.localBvh.leafTriangleCount;
        meshRecords.push_back(rec);
        rayTracingMeshes_.push_back(RayTracingMeshBuildInput{
            .meshIndex = static_cast<uint32_t>(meshRecords.size() - 1),
            .firstVertex = rec.vertexIndexData.x,
            .vertexCount = rec.vertexIndexData.y,
            .firstIndex = rec.vertexIndexData.z,
            .indexCount = rec.vertexIndexData.w,
            .primitiveOffset = rec.primitiveData.x,
            .primitiveCount = rec.primitiveData.y,
            .opaqueTraversalSafe = primitivesAreOpaqueTraversalSafe(
                primitiveRecords,
                rec.primitiveData.x,
                rec.primitiveData.y,
                materialOpaqueTraversalSafe),
            .updateMode = AccelUpdateMode::Static,
        });

        localBvhData.insert(localBvhData.end(), cachedMesh.localBvh.packedNodes.begin(), cachedMesh.localBvh.packedNodes.end());
        localTriangleData.insert(localTriangleData.end(), cachedMesh.localBvh.triangleData.begin(), cachedMesh.localBvh.triangleData.end());
    }

    for (const CachedMeshData& mesh : cached.meshes) {
        const uint32_t vertexBase = static_cast<uint32_t>(localVertexData.size());
        localVertexData.reserve(localVertexData.size() + mesh.vertices.size());
        for (const MeshVertex& vertex : mesh.vertices) {
            const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
            const glm::vec3 normal = normalLen2 > 1.0e-10f ? glm::normalize(vertex.normal) : glm::vec3{0.0f, 1.0f, 0.0f};
            glm::vec3 tangent = glm::vec3(vertex.tangent);
            const float tangentLen2 = glm::dot(tangent, tangent);
            tangent = tangentLen2 > 1.0e-10f ? glm::normalize(tangent) : glm::vec3{1.0f, 0.0f, 0.0f};
            localVertexData.push_back(makeLocalVertex(vertex.position, normal, vertex.texcoord, glm::vec4{tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f}));
        }
        localIndices.reserve(localIndices.size() + mesh.indices.size());
        for (uint32_t index : mesh.indices) {
            localIndices.push_back(vertexBase + index);
        }
    }

    for (const auto& cachedInst : cached.instanceRecords) {
        GpuInstanceRecord rec{};
        rec.transform = cachedInst.transform;
        rec.inverseTransform = cachedInst.inverseTransform;
        rec.prevTransform = cachedInst.transform;
        rec.metadata = cachedInst.metadata;
        instanceRecords.push_back(rec);
        rayTracingInstances_.push_back(RayTracingInstanceBuildInput{
            .instanceIndex = static_cast<uint32_t>(instanceRecords.size() - 1),
            .meshIndex = rec.metadata.x,
            .transform = rec.transform,
            .flags = rec.metadata.w,
        });
    }

    for (const auto& cachedBounds : cached.instanceBounds) {
        GpuInstanceBoundsRecord rec{};
        rec.bmin = cachedBounds.bmin;
        rec.bmax = cachedBounds.bmax;
        instanceBounds.push_back(rec);
    }

    for (const auto& cachedLight : cached.lightRecords) {
        GpuLightRecord rec{};
        rec.metadata = cachedLight.metadata;
        rec.data0 = cachedLight.data0;
        rec.data1 = cachedLight.data1;
        rec.data2 = cachedLight.data2;
        rec.data3 = cachedLight.data3;
        lightRecords.push_back(rec);
    }
    emissiveLightRecords_ = lightRecords;

    const std::vector<uint32_t> rtTriangleMaterialIds =
        buildRtTriangleMaterialIds(primitiveRecords, static_cast<uint32_t>(localIndices.size() / 3u));
    meshParams_ = fromCachedMeshParams(cached.meshParams);
    meshParams_.enabled = 1;
    meshParams_.localVertexCount = static_cast<uint32_t>(localVertexData.size());
    meshParams_.localIndexCount = static_cast<uint32_t>(localIndices.size());
    const std::vector<glm::vec4> materialData = buildCachedMaterialData(cached);
    meshParams_.materialCount = static_cast<uint32_t>(materialData.size() / materialVec4Stride);

    {
        BatchUploader batch(uploader);
        batch.begin();

        std::vector<glm::vec4> emptyVec4;
        std::vector<uint32_t> emptyU32;
        uploadVectorBatched(batch, vertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene vertices (cached)");
        uploadVectorBatched(batch, indices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyU32, "imported scene indices (cached)");
        uploadVectorBatched(batch, bvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene bvh nodes (cached)");
        uploadVectorBatched(batch, triangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptyVec4, "imported scene triangles (cached)");

        std::vector<glm::vec4> emptySpheres;
        uploadVectorBatched(batch, materials_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, materialData, "imported scene materials (cached)");
        uploadVectorBatched(batch, spheres_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, emptySpheres, "imported scene spheres (cached)");

        uploadVectorBatched(batch, meshRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshRecords, "imported mesh records (cached)");
        uploadVectorBatched(batch, primitiveRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, primitiveRecords, "imported primitive records (cached)");
        uploadVectorBatched(batch, instanceRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceRecords, "imported instance records (cached)");
        uploadVectorBatched(batch, rtTriangleMaterialIds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rtTriangleMaterialIds, "imported rt triangle material ids (cached)");
        uploadVectorBatched(batch, lightRecords_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRecords, "imported emissive light records (cached)");

        uploadVectorBatched(batch, localVertices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localVertexData, "imported local mesh vertices (cached)");
        uploadVectorBatched(batch, localIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, localIndices, "imported local mesh indices (cached)");
        uploadVectorBatched(batch, instanceBounds_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instanceBounds, "imported instance bounds (cached)");
        uploadVectorBatched(batch, localBvhNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localBvhData, "imported local bvh nodes (cached)");
        uploadVectorBatched(batch, localTriangles_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, localTriangleData, "imported local bvh triangles (cached)");
        uploadVectorBatched(batch, tlasNodes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached.tlasNodes, "imported tlas nodes (cached)");
        uploadVectorBatched(batch, tlasInstanceIndices_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached.tlasInstanceIndices, "imported tlas instance indices (cached)");
        batch.submit();
    }

    instanceRecordCpu_ = instanceRecords;
    meshParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(MeshParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "imported mesh params (cached)",
    });
    meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
    meshParamsBuffer_->flush(sizeof(meshParams_));
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::createEnvironment(BufferUploader& uploader) {
    constexpr uint32_t width = 512;
    constexpr uint32_t height = 256;
    const bool useExternalEnvironment = environmentPath_.has_value();
    const HdrImageData environment = useExternalEnvironment
        ? HdrLoader::loadRadiance(*environmentPath_)
        : HdrLoader::createProcedural(width, height);
    const EnvironmentImportanceData importance = EnvironmentImportanceSampler::build(environment.rgba.data(), environment.width, environment.height);
    const std::vector<uint16_t> halfPixels = rgba32fToRgba16f(environment.rgba);

    std::cout << (useExternalEnvironment ? "Loaded HDR environment: " : "Using procedural environment: ")
              << (useExternalEnvironment ? environmentPath_->string() : std::string("procedural"))
              << " (" << environment.width << "x" << environment.height << ")\n";

    environmentImage_ = std::make_unique<Image>(allocator_, ImageDesc{
        .width = environment.width,
        .height = environment.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = useExternalEnvironment ? "HDR environment" : "procedural environment",
    });

    {
        BatchUploader batch(uploader);
        batch.begin();
        batch.enqueueImageUpload(*environmentImage_, halfPixels.data(), sizeof(uint16_t) * halfPixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        uploadVectorBatched(batch, envRows_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, importance.rowAlias, "environment row alias");
        uploadVectorBatched(batch, envCols_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, importance.columnAlias, "environment col alias");
        batch.submit();
    }

    envParams_ = {
        .enabled = 1,
        .intensity = 1.0f,
        .rotation = 0.0f,
        .width = environment.width,
        .height = environment.height,
        .backgroundIntensity = 0.35f,
        .procedural = useExternalEnvironment ? 0u : 1u,
        .invTotalLum = importance.invTotalLuminance,
    };
    envParamsBuffer_ = std::make_unique<Buffer>(allocator_, BufferDesc{
        .size = sizeof(EnvParamsUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "environment params",
    });
    envParamsBuffer_->write(&envParams_, sizeof(envParams_));
    envParamsBuffer_->flush(sizeof(envParams_));

    if (environmentSampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(allocator_.device(), &samplerInfo, nullptr, &environmentSampler_), "vkCreateSampler(environment)");
    }
}

void GpuScene::uploadEnvironmentParams() {
    if (envParamsBuffer_) {
        envParamsBuffer_->write(&envParams_, sizeof(envParams_));
        envParamsBuffer_->flush(sizeof(envParams_));
    }
}

void GpuScene::uploadLightRecords(BufferUploader& uploader, std::vector<GpuLightRecord> lightRecords, float totalWeight) {
    if (lightRecords.empty()) {
        lightRecords.push_back(GpuLightRecord{});
        totalWeight = 0.0f;
    }

    uploadBuffer(
        allocator_,
        uploader,
        lightRecords_,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        lightRecords.data(),
        sizeof(GpuLightRecord) * lightRecords.size(),
        "scene light records");

    meshParams_.lightCount = static_cast<uint32_t>(lightRecords.size());
    meshParams_.emissiveTotalArea = totalWeight;
    if (meshParamsBuffer_) {
        meshParamsBuffer_->write(&meshParams_, sizeof(meshParams_));
        meshParamsBuffer_->flush(sizeof(meshParams_));
    }
    uploadLightBvh(uploader, lightRecords);
}

void GpuScene::uploadLightBvh(BufferUploader& uploader, const std::vector<GpuLightRecord>& lightRecords) {
    std::vector<float> lightPower;
    lightPower.reserve(lightRecords.size());
    for (const auto& rec : lightRecords) {
        lightPower.push_back(std::max(rec.data0.x, 0.0f));
    }
    std::vector<LightBvhNode> bvhNodes = buildLightBvh(lightPower, 1);
    std::vector<glm::vec4> packed = packLightBvhNodesForGpu(bvhNodes);
    if (packed.empty()) {
        packed.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
    }
    uploadBuffer(
        allocator_,
        uploader,
        lightBvhNodes_,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        packed.data(),
        sizeof(glm::vec4) * packed.size(),
        "scene light bvh nodes");
}

uint32_t GpuScene::textureSlotIndexFor(const SceneAsset& scene, TextureAssetHandle texture, uint32_t maxSlots) {
    if (!texture.valid()) {
        return UINT32_MAX;
    }
    for (uint32_t slot = 0; slot < scene.textures.size() && slot < maxSlots; ++slot) {
        if (scene.textures[slot].index == texture.index) {
            return slot;
        }
    }
    return UINT32_MAX;
}

} // namespace rtv
