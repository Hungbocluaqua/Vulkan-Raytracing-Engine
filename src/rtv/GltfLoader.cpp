#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "rtv/GltfLoader.h"

#include "rtv/AssetManager.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureLoader.h"

#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace rtv {

namespace {

[[nodiscard]] const uint8_t* accessorBytes(const tinygltf::Model& model, const tinygltf::Accessor& accessor) {
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    const tinygltf::Buffer& buffer = model.buffers[static_cast<size_t>(view.buffer)];
    return buffer.data.data() + view.byteOffset + accessor.byteOffset;
}

template <typename T>
[[nodiscard]] const T& accessorElement(const tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t index) {
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    const size_t stride = static_cast<size_t>(accessor.ByteStride(view));
    const size_t byteStride = stride > 0 ? stride : sizeof(T);
    return *reinterpret_cast<const T*>(accessorBytes(model, accessor) + index * byteStride);
}

[[nodiscard]] uint32_t indexElement(const tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t index) {
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    const size_t stride = static_cast<size_t>(accessor.ByteStride(view));
    const uint8_t* data = accessorBytes(model, accessor);
    const size_t byteStride = stride > 0 ? stride : tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
    const uint8_t* src = data + index * byteStride;
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        return *reinterpret_cast<const uint16_t*>(src);
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        return *reinterpret_cast<const uint32_t*>(src);
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        return *src;
    }
    return 0u;
}

[[nodiscard]] glm::mat4 nodeTransform(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 result{1.0f};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                result[col][row] = static_cast<float>(node.matrix[static_cast<size_t>(col * 4 + row)]);
            }
        }
        return result;
    }

    glm::vec3 translation{0.0f};
    glm::vec3 scale{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    if (node.translation.size() == 3) {
        translation = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2])};
    }
    if (node.scale.size() == 3) {
        scale = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2])};
    }
    if (node.rotation.size() == 4) {
        rotation = {static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])};
    }
    return glm::translate(glm::mat4{1.0f}, translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0f}, scale);
}

[[nodiscard]] bool isDataUri(const std::string& uri) {
    return uri.rfind("data:", 0) == 0;
}

[[nodiscard]] std::filesystem::path textureSourcePath(const tinygltf::Image& image, const std::filesystem::path& gltfPath) {
    if (!image.uri.empty() && !isDataUri(image.uri)) {
        return (gltfPath.parent_path() / image.uri).lexically_normal();
    }
    return gltfPath;
}

void addDependency(CachedScene& cached, const std::filesystem::path& path, std::unordered_set<std::string>& seen) {
    if (path.empty()) {
        return;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::string key = normalized.string();
    if (!seen.insert(key).second) {
        return;
    }
    if (!std::filesystem::exists(normalized) || !std::filesystem::is_regular_file(normalized)) {
        cached.dependencies.push_back(FileDependency{
            .path = key,
            .size = std::numeric_limits<uint64_t>::max(),
            .mtime = 0,
        });
        return;
    }
    cached.dependencies.push_back(FileDependency{
        .path = key,
        .size = static_cast<uint64_t>(std::filesystem::file_size(normalized)),
        .mtime = SceneCache::fileMtime(normalized),
    });
}

[[nodiscard]] bool dependenciesValid(const CachedScene& cached) {
    for (const FileDependency& dep : cached.dependencies) {
        const std::filesystem::path path = dep.path;
        if (dep.size == std::numeric_limits<uint64_t>::max()) {
            if (std::filesystem::exists(path)) {
                return false;
            }
            continue;
        }
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
            return false;
        }
        if (static_cast<uint64_t>(std::filesystem::file_size(path)) != dep.size) {
            return false;
        }
        if (SceneCache::fileMtime(path) != dep.mtime) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isKtx2Data(const std::vector<uint8_t>& data) {
    constexpr uint8_t ktx2Magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    return data.size() >= sizeof(ktx2Magic) && std::memcmp(data.data(), ktx2Magic, sizeof(ktx2Magic)) == 0;
}

[[nodiscard]] bool isKtx2Path(const std::string& uri) {
    if (uri.empty()) return false;
    const std::string lower = [&] {
        std::string s = uri;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }();
    return lower.ends_with(".ktx2");
}

[[nodiscard]] TextureAsset textureFromImage(const tinygltf::Image& image, const std::filesystem::path& gltfPath) {
    TextureAsset texture;
    texture.name = image.name;
    texture.sourcePath = textureSourcePath(image, gltfPath);
    texture.width = static_cast<uint32_t>(std::max(image.width, 1));
    texture.height = static_cast<uint32_t>(std::max(image.height, 1));
    texture.channels = 4;

    if (isKtx2Path(image.uri) || isKtx2Data(image.image)) {
        std::filesystem::path ktxPath;
        if (isKtx2Path(image.uri)) {
            ktxPath = texture.sourcePath;
        }
        try {
            TextureData td = ktxPath.empty()
                ? TextureLoader::loadKtx2(texture.sourcePath.string())
                : TextureLoader::loadKtx2(ktxPath.string());
            texture.width = static_cast<uint32_t>(td.width);
            texture.height = static_cast<uint32_t>(td.height);
            texture.channels = 4;
            texture.mipLevels = td.mipLevels;
            texture.isCompressed = td.isCompressed;
            texture.compressedFormat = td.compressedFormat;
            texture.rgba8 = std::move(td.pixels);
            texture.fallback = false;
            return texture;
        } catch (const std::runtime_error& e) {
            std::cerr << "KTX2 texture load failed: " << e.what() << ", falling back\n";
        }
    }

    const size_t pixelCount = static_cast<size_t>(texture.width) * texture.height;
    const size_t expectedRgbaBytes = pixelCount * 4u;
    const bool canDecode8Bit =
        image.bits == 8 &&
        image.component > 0 &&
        image.component <= 4 &&
        image.image.size() >= pixelCount * static_cast<size_t>(image.component);

    if (!canDecode8Bit) {
        texture.width = 1;
        texture.height = 1;
        texture.fallback = true;
        texture.rgba8 = {255, 255, 255, 255};
        std::cerr << "glTF texture fallback: "
                  << (texture.sourcePath.empty() ? std::string("<embedded>") : texture.sourcePath.string())
                  << " could not be decoded as 8-bit RGBA data; using white.\n";
        return texture;
    }

    texture.rgba8.resize(expectedRgbaBytes);
    if (image.component == 4 && image.image.size() == expectedRgbaBytes) {
        texture.rgba8 = image.image;
    } else {
        for (size_t i = 0; i < pixelCount; ++i) {
            const size_t src = i * static_cast<size_t>(image.component);
            const size_t dst = i * 4u;
            texture.rgba8[dst + 0] = image.image[src + 0];
            texture.rgba8[dst + 1] = image.component > 1 ? image.image[src + 1] : image.image[src + 0];
            texture.rgba8[dst + 2] = image.component > 2 ? image.image[src + 2] : image.image[src + 0];
            texture.rgba8[dst + 3] = image.component > 3 ? image.image[src + 3] : 255;
        }
    }
    return texture;
}

[[nodiscard]] TextureFilter textureFilterFromGltf(int filter) {
    switch (filter) {
    case TINYGLTF_TEXTURE_FILTER_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
        return TextureFilter::Nearest;
    case TINYGLTF_TEXTURE_FILTER_LINEAR:
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
    default:
        return TextureFilter::Linear;
    }
}

[[nodiscard]] TextureWrap textureWrapFromGltf(int wrap) {
    switch (wrap) {
    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return TextureWrap::ClampToEdge;
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
        return TextureWrap::MirroredRepeat;
    case TINYGLTF_TEXTURE_WRAP_REPEAT:
    default:
        return TextureWrap::Repeat;
    }
}

[[nodiscard]] TextureSamplerDesc samplerFromGltf(const tinygltf::Model& model, const tinygltf::Texture& texture) {
    TextureSamplerDesc desc{};
    if (texture.sampler < 0 || static_cast<size_t>(texture.sampler) >= model.samplers.size()) {
        return desc;
    }

    const tinygltf::Sampler& sampler = model.samplers[static_cast<size_t>(texture.sampler)];
    desc.minFilter = textureFilterFromGltf(sampler.minFilter);
    desc.magFilter = textureFilterFromGltf(sampler.magFilter);
    desc.wrapS = textureWrapFromGltf(sampler.wrapS);
    desc.wrapT = textureWrapFromGltf(sampler.wrapT);
    return desc;
}

[[nodiscard]] MaterialAsset materialFromGltf(const tinygltf::Material& source, const std::vector<TextureAssetHandle>& textures) {
    MaterialAsset material;
    material.name = source.name;
    const auto& pbr = source.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() == 4) {
        material.baseColorFactor = {
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3]),
        };
    }
    material.metallicFactor = static_cast<float>(pbr.metallicFactor);
    material.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
    material.alphaCutoff = static_cast<float>(source.alphaCutoff);
    material.doubleSided = source.doubleSided ? 1u : 0u;
    if (source.alphaMode == "MASK") {
        material.alphaMode = 1u;
    } else if (source.alphaMode == "BLEND") {
        material.alphaMode = 2u;
    }
    if (source.emissiveFactor.size() == 3) {
        material.emissiveFactor = {
            static_cast<float>(source.emissiveFactor[0]),
            static_cast<float>(source.emissiveFactor[1]),
            static_cast<float>(source.emissiveFactor[2]),
        };
    }
    if (pbr.baseColorTexture.index >= 0 && static_cast<size_t>(pbr.baseColorTexture.index) < textures.size()) {
        material.baseColorTexture = textures[static_cast<size_t>(pbr.baseColorTexture.index)];
    }
    if (pbr.metallicRoughnessTexture.index >= 0 && static_cast<size_t>(pbr.metallicRoughnessTexture.index) < textures.size()) {
        material.metallicRoughnessTexture = textures[static_cast<size_t>(pbr.metallicRoughnessTexture.index)];
    }
    if (source.normalTexture.index >= 0 && static_cast<size_t>(source.normalTexture.index) < textures.size()) {
        material.normalTexture = textures[static_cast<size_t>(source.normalTexture.index)];
    }
    if (source.emissiveTexture.index >= 0 && static_cast<size_t>(source.emissiveTexture.index) < textures.size()) {
        material.emissiveTexture = textures[static_cast<size_t>(source.emissiveTexture.index)];
    }
    return material;
}

void finalizePrimitiveVertexFrames(
    MeshAsset& mesh,
    uint32_t firstVertex,
    uint32_t vertexCount,
    uint32_t firstIndex,
    uint32_t indexCount,
    bool hasNormals,
    bool hasTangents,
    bool hasTexcoords) {
    if (!hasNormals) {
        for (uint32_t i = 0; i < vertexCount; ++i) {
            mesh.vertices[firstVertex + i].normal = glm::vec3{0.0f};
        }
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
            const uint32_t i0 = mesh.indices[firstIndex + i + 0];
            const uint32_t i1 = mesh.indices[firstIndex + i + 1];
            const uint32_t i2 = mesh.indices[firstIndex + i + 2];
            const glm::vec3 p0 = mesh.vertices[i0].position;
            const glm::vec3 p1 = mesh.vertices[i1].position;
            const glm::vec3 p2 = mesh.vertices[i2].position;
            const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
            mesh.vertices[i0].normal += faceNormal;
            mesh.vertices[i1].normal += faceNormal;
            mesh.vertices[i2].normal += faceNormal;
        }
    }

    std::vector<glm::vec3> tangentAccum(vertexCount, glm::vec3{0.0f});
    if (!hasTangents && hasTexcoords) {
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
            const uint32_t i0 = mesh.indices[firstIndex + i + 0];
            const uint32_t i1 = mesh.indices[firstIndex + i + 1];
            const uint32_t i2 = mesh.indices[firstIndex + i + 2];
            const glm::vec3 p0 = mesh.vertices[i0].position;
            const glm::vec3 p1 = mesh.vertices[i1].position;
            const glm::vec3 p2 = mesh.vertices[i2].position;
            const glm::vec2 uv0 = mesh.vertices[i0].texcoord;
            const glm::vec2 uv1 = mesh.vertices[i1].texcoord;
            const glm::vec2 uv2 = mesh.vertices[i2].texcoord;
            const glm::vec3 e1 = p1 - p0;
            const glm::vec3 e2 = p2 - p0;
            const glm::vec2 duv1 = uv1 - uv0;
            const glm::vec2 duv2 = uv2 - uv0;
            const float det = duv1.x * duv2.y - duv1.y * duv2.x;
            if (std::abs(det) <= 1.0e-8f) {
                continue;
            }
            const glm::vec3 tangent = (e1 * duv2.y - e2 * duv1.y) / det;
            tangentAccum[i0 - firstVertex] += tangent;
            tangentAccum[i1 - firstVertex] += tangent;
            tangentAccum[i2 - firstVertex] += tangent;
        }
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        MeshVertex& vertex = mesh.vertices[firstVertex + i];
        const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
        vertex.normal = normalLen2 > 1.0e-12f ? vertex.normal / std::sqrt(normalLen2) : glm::vec3{0.0f, 1.0f, 0.0f};

        if (!hasTangents) {
            glm::vec3 tangent = hasTexcoords ? tangentAccum[i] : glm::vec3{0.0f};
            tangent -= vertex.normal * glm::dot(vertex.normal, tangent);
            const float tangentLen2 = glm::dot(tangent, tangent);
            if (tangentLen2 <= 1.0e-12f) {
                const glm::vec3 helper = std::abs(vertex.normal.y) < 0.95f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
                tangent = glm::normalize(glm::cross(helper, vertex.normal));
            } else {
                tangent /= std::sqrt(tangentLen2);
            }
            vertex.tangent = glm::vec4{tangent, 1.0f};
        }
    }
}

} // namespace

GltfLoader::GltfLoader(AssetManager& assets)
    : assets_(assets) {}

SceneAsset GltfLoader::load(const std::filesystem::path& path) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string error;
    std::string warning;
    const bool binary = path.extension() == ".glb";
    const bool ok = binary
        ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
        : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
    if (!warning.empty()) {
        std::cerr << "glTF load warning: " << warning << '\n';
    }
    if (!ok) {
        throw std::runtime_error("glTF load failed: " + path.string() + " " + error);
    }

    SceneAsset scene;
    scene.name = path.filename().string();
    scene.sourcePath = path;

    std::vector<bool> colorTextureUse(model.textures.size(), false);
    std::vector<bool> dataTextureUse(model.textures.size(), false);
    for (const tinygltf::Material& sourceMaterial : model.materials) {
        const auto& pbr = sourceMaterial.pbrMetallicRoughness;
        if (pbr.baseColorTexture.index >= 0 && static_cast<size_t>(pbr.baseColorTexture.index) < colorTextureUse.size()) {
            colorTextureUse[static_cast<size_t>(pbr.baseColorTexture.index)] = true;
        }
        if (sourceMaterial.emissiveTexture.index >= 0 && static_cast<size_t>(sourceMaterial.emissiveTexture.index) < colorTextureUse.size()) {
            colorTextureUse[static_cast<size_t>(sourceMaterial.emissiveTexture.index)] = true;
        }
        if (pbr.metallicRoughnessTexture.index >= 0 && static_cast<size_t>(pbr.metallicRoughnessTexture.index) < dataTextureUse.size()) {
            dataTextureUse[static_cast<size_t>(pbr.metallicRoughnessTexture.index)] = true;
        }
        if (sourceMaterial.normalTexture.index >= 0 && static_cast<size_t>(sourceMaterial.normalTexture.index) < dataTextureUse.size()) {
            dataTextureUse[static_cast<size_t>(sourceMaterial.normalTexture.index)] = true;
        }
    }

    std::vector<TextureAssetHandle> textureHandles;
    textureHandles.reserve(model.textures.size());
    for (size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex) {
        const tinygltf::Texture& sourceTexture = model.textures[textureIndex];
        TextureAsset texture;
        if (sourceTexture.source >= 0 && static_cast<size_t>(sourceTexture.source) < model.images.size()) {
            texture = textureFromImage(model.images[static_cast<size_t>(sourceTexture.source)], path);
        }
        texture.name = texture.name.empty() ? sourceTexture.name : texture.name;
        texture.srgb = colorTextureUse[textureIndex] && !dataTextureUse[textureIndex];
        texture.sampler = samplerFromGltf(model, sourceTexture);
        textureHandles.push_back(assets_.addTexture(std::move(texture)));
    }
    scene.textures = textureHandles;

    std::vector<MaterialAssetHandle> materialHandles;
    materialHandles.reserve(model.materials.size());
    for (const tinygltf::Material& sourceMaterial : model.materials) {
        materialHandles.push_back(assets_.addMaterial(materialFromGltf(sourceMaterial, textureHandles)));
    }
    if (materialHandles.empty()) {
        materialHandles.push_back(assets_.addMaterial(MaterialAsset{.name = "default"}));
    }
    scene.materials = materialHandles;

    std::vector<MeshAssetHandle> meshHandles;
    meshHandles.reserve(model.meshes.size());
    for (const tinygltf::Mesh& sourceMesh : model.meshes) {
        MeshAsset mesh;
        mesh.name = sourceMesh.name;

        for (const tinygltf::Primitive& primitive : sourceMesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                continue;
            }

            const uint32_t firstVertex = static_cast<uint32_t>(mesh.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
            const auto positionIt = primitive.attributes.find("POSITION");
            if (positionIt == primitive.attributes.end()) {
                continue;
            }

            const tinygltf::Accessor& posAccessor = model.accessors[static_cast<size_t>(positionIt->second)];
            const size_t vertexCount = posAccessor.count;
            mesh.vertices.resize(mesh.vertices.size() + vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                mesh.vertices[firstVertex + i].position = accessorElement<glm::vec3>(model, posAccessor, i);
            }

            const auto normalIt = primitive.attributes.find("NORMAL");
            const bool hasNormals = normalIt != primitive.attributes.end();
            if (normalIt != primitive.attributes.end()) {
                const tinygltf::Accessor& normalAccessor = model.accessors[static_cast<size_t>(normalIt->second)];
                for (size_t i = 0; i < vertexCount; ++i) {
                    mesh.vertices[firstVertex + i].normal = accessorElement<glm::vec3>(model, normalAccessor, i);
                }
            }

            const auto uvIt = primitive.attributes.find("TEXCOORD_0");
            const bool hasTexcoords = uvIt != primitive.attributes.end();
            if (uvIt != primitive.attributes.end()) {
                const tinygltf::Accessor& uvAccessor = model.accessors[static_cast<size_t>(uvIt->second)];
                for (size_t i = 0; i < vertexCount; ++i) {
                    mesh.vertices[firstVertex + i].texcoord = accessorElement<glm::vec2>(model, uvAccessor, i);
                }
            }

            const auto tangentIt = primitive.attributes.find("TANGENT");
            const bool hasTangents = tangentIt != primitive.attributes.end();
            if (tangentIt != primitive.attributes.end()) {
                const tinygltf::Accessor& tangentAccessor = model.accessors[static_cast<size_t>(tangentIt->second)];
                for (size_t i = 0; i < vertexCount; ++i) {
                    mesh.vertices[firstVertex + i].tangent = accessorElement<glm::vec4>(model, tangentAccessor, i);
                }
            }

            if (primitive.indices >= 0) {
                const tinygltf::Accessor& indexAccessor = model.accessors[static_cast<size_t>(primitive.indices)];
                for (size_t i = 0; i < indexAccessor.count; ++i) {
                    const uint32_t index = indexElement(model, indexAccessor, i);
                    mesh.indices.push_back(firstVertex + index);
                }
            } else {
                for (uint32_t i = 0; i < vertexCount; ++i) {
                    mesh.indices.push_back(firstVertex + i);
                }
            }

            MeshPrimitiveAsset prim;
            prim.firstVertex = firstVertex;
            prim.vertexCount = static_cast<uint32_t>(vertexCount);
            prim.firstIndex = firstIndex;
            prim.indexCount = static_cast<uint32_t>(mesh.indices.size()) - firstIndex;
            prim.material = primitive.material >= 0 && static_cast<size_t>(primitive.material) < materialHandles.size()
                ? materialHandles[static_cast<size_t>(primitive.material)]
                : materialHandles.front();
            finalizePrimitiveVertexFrames(mesh, firstVertex, prim.vertexCount, firstIndex, prim.indexCount, hasNormals, hasTangents, hasTexcoords);
            mesh.primitives.push_back(prim);
        }

        meshHandles.push_back(assets_.addMesh(std::move(mesh)));
    }
    scene.meshes = meshHandles;

    scene.nodes.reserve(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const tinygltf::Node& sourceNode = model.nodes[i];
        SceneNodeAsset node;
        node.name = sourceNode.name;
        node.transform = nodeTransform(sourceNode);
        if (sourceNode.mesh >= 0 && static_cast<size_t>(sourceNode.mesh) < meshHandles.size()) {
            node.mesh = meshHandles[static_cast<size_t>(sourceNode.mesh)];
        }
        if (sourceNode.camera >= 0 && static_cast<size_t>(sourceNode.camera) < model.cameras.size()) {
            const tinygltf::Camera& sourceCamera = model.cameras[static_cast<size_t>(sourceNode.camera)];
            node.hasCamera = true;
            if (sourceCamera.type == "perspective") {
                node.cameraYfov = static_cast<float>(sourceCamera.perspective.yfov);
                node.cameraNear = static_cast<float>(sourceCamera.perspective.znear);
                node.cameraFar = sourceCamera.perspective.zfar > 0.0
                    ? static_cast<float>(sourceCamera.perspective.zfar)
                    : 1000.0f;
            }
        }
        for (int child : sourceNode.children) {
            node.children.push_back(static_cast<uint32_t>(child));
        }
        scene.nodes.push_back(std::move(node));
    }
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        for (uint32_t child : scene.nodes[i].children) {
            if (child < scene.nodes.size()) {
                scene.nodes[child].parent = static_cast<int32_t>(i);
            }
        }
    }

    for (size_t i = 0; i < model.lights.size(); ++i) {
        const tinygltf::Light& srcLight = model.lights[i];
        SceneLightAsset light;
        light.color = srcLight.color.size() >= 3
            ? glm::vec3(static_cast<float>(srcLight.color[0]),
                         static_cast<float>(srcLight.color[1]),
                         static_cast<float>(srcLight.color[2]))
            : glm::vec3(1.0f);
        light.intensity = static_cast<float>(srcLight.intensity);
        if (light.intensity <= 0.0f) {
            light.intensity = srcLight.type == "directional" ? 1.0f : 100.0f;
        }
        light.type = srcLight.type == "directional" ? 0u
                   : srcLight.type == "spot" ? 2u
                   : 1u;
        light.enabled = true;
        if (srcLight.type == "directional") {
            light.sizeOrRadius = 0.0f;
        }
        for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
            if (model.nodes[ni].light == static_cast<int>(i)) {
                light.transform = nodeTransform(model.nodes[ni]);
                light.nodeIndex = static_cast<int32_t>(ni);
                break;
            }
        }
        scene.lights.push_back(light);
    }

    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (sceneIndex >= 0 && static_cast<size_t>(sceneIndex) < model.scenes.size()) {
        for (int root : model.scenes[static_cast<size_t>(sceneIndex)].nodes) {
            scene.rootNodes.push_back(static_cast<uint32_t>(root));
        }
    }
    return scene;
}

SceneAsset GltfLoader::loadWithCache(const std::filesystem::path& path) {
    const std::filesystem::path cachePath = SceneCache::cachePathFor(path);

    if (useCache_ && SceneCache::isCacheValid(path, cachePath)) {
        auto cached = SceneCache::load(cachePath);
        if (cached.has_value()) {
            uint64_t actualGltfMtime = SceneCache::fileMtime(path);
            if (cached->sourceMtime != actualGltfMtime) {
                cached.reset();
            } else {
                std::filesystem::path binPath = path.parent_path() / (path.stem().string() + ".bin");
                if (std::filesystem::exists(binPath)) {
                    uint64_t actualBinMtime = SceneCache::fileMtime(binPath);
                    if (cached->sourceBinMtime != actualBinMtime) {
                        cached.reset();
                    }
                }
            }
            if (cached.has_value() && !dependenciesValid(*cached)) {
                cached.reset();
            }

            if (cached.has_value()) {
                const auto start = std::chrono::high_resolution_clock::now();

                SceneAsset scene;
                scene.name = cached->name;
                scene.sourcePath = path;

                for (const auto& cachedTex : cached->textures) {
                    TextureAsset texture;
                    texture.name = cachedTex.name;
                    texture.sourcePath = cachedTex.sourcePath.empty() ? path : std::filesystem::path(cachedTex.sourcePath);
                    texture.width = cachedTex.width;
                    texture.height = cachedTex.height;
                    texture.channels = cachedTex.channels;
                    texture.srgb = cachedTex.srgb;
                    texture.fallback = cachedTex.fallback;
                    texture.rgba8 = cachedTex.rgba8;
                    texture.sampler.minFilter = static_cast<TextureFilter>(cachedTex.minFilter);
                    texture.sampler.magFilter = static_cast<TextureFilter>(cachedTex.magFilter);
                    texture.sampler.wrapS = static_cast<TextureWrap>(cachedTex.wrapS);
                    texture.sampler.wrapT = static_cast<TextureWrap>(cachedTex.wrapT);
                    scene.textures.push_back(assets_.addTexture(std::move(texture)));
                }

                auto textureHandleFor = [&](int32_t index) -> TextureAssetHandle {
                    if (index < 0 || static_cast<size_t>(index) >= scene.textures.size()) {
                        return TextureAssetHandle{};
                    }
                    return scene.textures[static_cast<size_t>(index)];
                };

                for (const auto& cachedMat : cached->materials) {
                    MaterialAsset material;
                    material.name = cachedMat.name;
                    material.baseColorFactor = cachedMat.baseColorFactor;
                    material.emissiveFactor = cachedMat.emissiveFactor;
                    material.metallicFactor = cachedMat.metallicFactor;
                    material.roughnessFactor = cachedMat.roughnessFactor;
                    material.alphaCutoff = cachedMat.alphaCutoff;
                    material.alphaMode = cachedMat.alphaMode;
                    material.doubleSided = cachedMat.doubleSided;
                    material.baseColorTexture = textureHandleFor(cachedMat.baseColorTextureIndex);
                    material.normalTexture = textureHandleFor(cachedMat.normalTextureIndex);
                    material.metallicRoughnessTexture = textureHandleFor(cachedMat.metallicRoughnessTextureIndex);
                    material.emissiveTexture = textureHandleFor(cachedMat.emissiveTextureIndex);
                    scene.materials.push_back(assets_.addMaterial(std::move(material)));
                }

                for (const auto& cachedMesh : cached->meshes) {
                    MeshAsset mesh;
                    mesh.name = cachedMesh.name;
                    mesh.vertices = cachedMesh.vertices;
                    mesh.indices = cachedMesh.indices;
                    for (const auto& cachedPrim : cachedMesh.primitives) {
                        MeshPrimitiveAsset prim;
                        prim.firstVertex = cachedPrim.firstVertex;
                        prim.vertexCount = cachedPrim.vertexCount;
                        prim.firstIndex = cachedPrim.firstIndex;
                        prim.indexCount = cachedPrim.indexCount;
                        if (cachedPrim.materialIndex >= 0 && static_cast<size_t>(cachedPrim.materialIndex) < scene.materials.size()) {
                            prim.material = scene.materials[static_cast<size_t>(cachedPrim.materialIndex)];
                        } else if (!scene.materials.empty()) {
                            prim.material = scene.materials.front();
                        }
                        mesh.primitives.push_back(prim);
                    }
                    scene.meshes.push_back(assets_.addMesh(std::move(mesh)));
                }

                for (uint32_t i = 0; i < cached->nodes.size(); ++i) {
                    const CachedNodeData& cachedNode = cached->nodes[i];
                    SceneNodeAsset node;
                    node.name = cachedNode.name;
                    node.transform = cachedNode.transform;
                    node.parent = cachedNode.parentIndex;
                    node.children = cachedNode.children;
                    node.hasCamera = cachedNode.hasCamera != 0;
                    node.cameraYfov = cachedNode.cameraYfov;
                    node.cameraNear = cachedNode.cameraNear;
                    node.cameraFar = cachedNode.cameraFar;
                    if (cachedNode.meshIndex >= 0 && static_cast<size_t>(cachedNode.meshIndex) < scene.meshes.size()) {
                        node.mesh = scene.meshes[static_cast<size_t>(cachedNode.meshIndex)];
                    }
                    scene.nodes.push_back(std::move(node));
                }
                scene.rootNodes = cached->rootNodes;

                const auto end = std::chrono::high_resolution_clock::now();
                const double loadMs = std::chrono::duration<double, std::milli>(end - start).count();
                std::cout << "Scene cache hit: loaded in " << loadMs << " ms (name: " << cached->name << ")\n";
                return scene;
            }
        }
    }

    const auto start = std::chrono::high_resolution_clock::now();
    SceneAsset scene = load(path);
    const auto end = std::chrono::high_resolution_clock::now();
    const double loadMs = std::chrono::duration<double, std::milli>(end - start).count();

    CachedScene cached = buildCachedScene(path, scene);
    if (SceneCache::save(cachePath, cached)) {
        std::cout << "Scene cache created: " << cachePath.string() << "\n";
    }
    std::cout << "glTF loaded (uncached): " << loadMs << " ms\n";
    return scene;
}

CachedScene GltfLoader::buildCachedScene(const std::filesystem::path& path, const SceneAsset& scene) {
    CachedScene cached;
    cached.name = scene.name;

    const auto getTextureIndex = [&](TextureAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        for (int32_t i = 0; i < static_cast<int32_t>(scene.textures.size()); ++i) {
            if (scene.textures[static_cast<size_t>(i)].index == handle.index) {
                return i;
            }
        }
        return -1;
    };

    const auto getMaterialIndex = [&](MaterialAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        for (int32_t i = 0; i < static_cast<int32_t>(scene.materials.size()); ++i) {
            if (scene.materials[static_cast<size_t>(i)].index == handle.index) {
                return i;
            }
        }
        return -1;
    };

    const auto getMeshIndex = [&](MeshAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        for (int32_t i = 0; i < static_cast<int32_t>(scene.meshes.size()); ++i) {
            if (scene.meshes[static_cast<size_t>(i)].index == handle.index) {
                return i;
            }
        }
        return -1;
    };

    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets_.texture(handle);
        if (texture == nullptr) {
            continue;
        }
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
        cached.textures.push_back(std::move(cachedTex));
    }

    for (MaterialAssetHandle handle : scene.materials) {
        const MaterialAsset* material = assets_.material(handle);
        if (material == nullptr) {
            continue;
        }
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
        cached.materials.push_back(std::move(cachedMat));
    }

    for (MeshAssetHandle handle : scene.meshes) {
        const MeshAsset* mesh = assets_.mesh(handle);
        if (mesh == nullptr) {
            continue;
        }
        CachedMeshData cachedMesh;
        cachedMesh.name = mesh->name;
        cachedMesh.vertices = mesh->vertices;
        cachedMesh.indices = mesh->indices;
        for (const MeshPrimitiveAsset& prim : mesh->primitives) {
            CachedPrimitiveData cachedPrim;
            cachedPrim.firstVertex = prim.firstVertex;
            cachedPrim.vertexCount = prim.vertexCount;
            cachedPrim.firstIndex = prim.firstIndex;
            cachedPrim.indexCount = prim.indexCount;
            cachedPrim.materialIndex = getMaterialIndex(prim.material);
            cachedMesh.primitives.push_back(std::move(cachedPrim));
        }
        cached.meshes.push_back(std::move(cachedMesh));
    }

    for (const SceneNodeAsset& node : scene.nodes) {
        CachedNodeData cachedNode;
        cachedNode.name = node.name;
        cachedNode.transform = node.transform;
        cachedNode.meshIndex = getMeshIndex(node.mesh);
        cachedNode.hasCamera = node.hasCamera ? 1u : 0u;
        cachedNode.cameraYfov = node.cameraYfov;
        cachedNode.cameraNear = node.cameraNear;
        cachedNode.cameraFar = node.cameraFar;
        cachedNode.parentIndex = node.parent;
        cachedNode.children = node.children;
        cached.nodes.push_back(std::move(cachedNode));
    }

    cached.rootNodes = scene.rootNodes;
    cached.sourceMtime = SceneCache::fileMtime(path);

    std::filesystem::path binPath = path.parent_path() / (path.stem().string() + ".bin");
    cached.sourceBinMtime = SceneCache::fileMtime(binPath);
    std::unordered_set<std::string> dependencyPaths;
    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets_.texture(handle);
        if (texture == nullptr || texture->sourcePath.empty() || texture->sourcePath == path) {
            continue;
        }
        addDependency(cached, texture->sourcePath, dependencyPaths);
    }

    return cached;
}

} // namespace rtv
