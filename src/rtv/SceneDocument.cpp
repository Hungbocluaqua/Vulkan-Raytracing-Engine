#include "rtv/SceneDocument.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace rtv {

namespace {

std::string escapeJson(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '"') {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    return result;
}

void writeVec3(std::ostream& out, glm::vec3 value) {
    out << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

glm::vec3 translationFromMatrix(const glm::mat4& matrix) {
    return glm::vec3(matrix[3]);
}

glm::vec3 scaleFromMatrix(const glm::mat4& matrix) {
    return {
        glm::length(glm::vec3(matrix[0])),
        glm::length(glm::vec3(matrix[1])),
        glm::length(glm::vec3(matrix[2])),
    };
}

glm::vec3 eulerFromMatrix(const glm::mat4& matrix) {
    glm::vec3 scale = scaleFromMatrix(matrix);
    glm::mat3 rotation{matrix};
    if (scale.x > 0.0f) {
        rotation[0] /= scale.x;
    }
    if (scale.y > 0.0f) {
        rotation[1] /= scale.y;
    }
    if (scale.z > 0.0f) {
        rotation[2] /= scale.z;
    }
    return glm::eulerAngles(glm::quat_cast(rotation));
}

} // namespace

void SceneDocument::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
    markDirty(SceneUpdateKind::EnvironmentOnly);
}

void SceneDocument::setRenderSettings(RenderSettings settings) {
    renderSettings_ = settings;
    markDirty(SceneUpdateKind::CameraOnly);
}

void SceneDocument::setActiveCamera(EntityId id) {
    if (activeCamera_ == id) {
        return;
    }
    activeCamera_ = id;
    for (Entity* entity : registry_.entities()) {
        if (entity->camera.has_value()) {
            entity->camera->active = entity->id == id;
        }
    }
    markDirty(SceneUpdateKind::CameraOnly);
}

void SceneDocument::setSourceGltfPath(std::optional<std::filesystem::path> path) {
    sourceGltfPath_ = std::move(path);
}

void SceneDocument::setSourceHdrPath(std::optional<std::filesystem::path> path) {
    sourceHdrPath_ = std::move(path);
    if (sourceHdrPath_.has_value()) {
        environment_.hdrPath = *sourceHdrPath_;
    }
}

void SceneDocument::markDirty(SceneUpdateKind kind) {
    dirty_ = true;
    pendingUpdate_ = combine(pendingUpdate_, kind);
    lastChangeReason_ = sceneUpdateKindName(kind);
}

void SceneDocument::clearDirty() {
    dirty_ = false;
    pendingUpdate_ = SceneUpdateKind::None;
    registry_.clearDirty();
}

SceneUpdateKind SceneDocument::pendingUpdate() const {
    return combine(pendingUpdate_, registry_.pendingUpdate());
}

void SceneDocument::importSceneAsset(const SceneAsset& scene) {
    registry_ = SceneRegistry{};
    activeCamera_ = {};
    sceneTextures_ = scene.textures;
    sceneMaterials_ = scene.materials;
    sceneMeshes_ = scene.meshes;

    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNodeAsset& node = scene.nodes[i];
        EntityId id = registry_.createEntity(node.name.empty() ? "Node " + std::to_string(i) : node.name);
        Entity* entity = registry_.entity(id);
        if (entity == nullptr) {
            continue;
        }
        entity->transform.position = translationFromMatrix(node.transform);
        entity->transform.rotationEuler = eulerFromMatrix(node.transform);
        entity->transform.scale = scaleFromMatrix(node.transform);
        entity->transform.dirty = true;

        if (node.mesh.valid()) {
            MeshRenderer renderer;
            renderer.mesh = node.mesh;
            renderer.materialSlots.clear();
            entity->meshRenderer = renderer;
        }
        if (node.hasCamera) {
            Camera camera;
            camera.verticalFovRadians = node.cameraYfov;
            camera.nearPlane = node.cameraNear;
            camera.farPlane = node.cameraFar;
            camera.active = !activeCamera_.valid();
            entity->camera = camera;
            if (camera.active) {
                activeCamera_ = id;
            }
        }
    }

    clearDirty();
    markDirty(SceneUpdateKind::FullSceneRebuild);
}

SceneAsset SceneDocument::toSceneAsset() const {
    SceneAsset scene;
    scene.name = sourceGltfPath_.has_value() ? sourceGltfPath_->filename().string() : "SceneDocument";
    if (sourceGltfPath_.has_value()) {
        scene.sourcePath = *sourceGltfPath_;
    }
    scene.textures = sceneTextures_;
    scene.materials = sceneMaterials_;
    scene.meshes = sceneMeshes_;

    std::vector<const Entity*> entities = registry_.entities();
    scene.nodes.reserve(entities.size());
    for (const Entity* entity : entities) {
        SceneNodeAsset node;
        node.name = entity->name;
        node.transform = entity->transform.localMatrix();
        if (entity->meshRenderer.has_value() && entity->meshRenderer->visible && entity->meshRenderer->visibleToCamera) {
            node.mesh = entity->meshRenderer->mesh;
            if (node.mesh.valid()) {
                scene.meshes.push_back(node.mesh);
            }
            for (const MaterialSlot& slot : entity->meshRenderer->materialSlots) {
                MaterialAssetHandle material = slot.resolvedMaterial();
                if (material.valid()) {
                    scene.materials.push_back(material);
                }
            }
        }
        scene.nodes.push_back(node);
        scene.rootNodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));
    }

    std::sort(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    scene.meshes.erase(std::unique(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }), scene.meshes.end());
    std::sort(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    scene.materials.erase(std::unique(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }), scene.materials.end());
    return scene;
}

bool SceneDocument::saveJson(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << std::setprecision(9);
    out << "{\n";
    out << "  \"sourceGltf\": \"" << escapeJson(sourceGltfPath_.has_value() ? sourceGltfPath_->string() : "") << "\",\n";
    out << "  \"sourceHdr\": \"" << escapeJson(sourceHdrPath_.has_value() ? sourceHdrPath_->string() : "") << "\",\n";
    out << "  \"environment\": {\"hdrPath\":\"" << escapeJson(environment_.hdrPath.string()) << "\",\"intensity\":" << environment_.intensity
        << ",\"rotation\":" << environment_.rotation
        << ",\"backgroundIntensity\":" << environment_.backgroundIntensity
        << ",\"enabled\":" << (environment_.enabled ? "true" : "false") << "},\n";
    out << "  \"renderSettings\": {\"pathTracingEnabled\":" << (renderSettings_.pathTracingEnabled ? "true" : "false")
        << ",\"directLightingEnabled\":" << (renderSettings_.directLightingEnabled ? "true" : "false")
        << ",\"maxBounces\":" << renderSettings_.maxBounces
        << ",\"environmentDirectSamples\":" << renderSettings_.environmentDirectSamples
        << ",\"exposure\":" << renderSettings_.exposure
        << ",\"sunlightEnabled\":" << (renderSettings_.sunlightEnabled ? "true" : "false")
        << ",\"sunIntensity\":" << renderSettings_.sunIntensity
        << ",\"skyIntensity\":" << renderSettings_.skyIntensity
        << ",\"sunAngularRadius\":" << renderSettings_.sunAngularRadius
        << ",\"indirectStrength\":" << renderSettings_.indirectStrength
        << ",\"denoiserEnabled\":" << (renderSettings_.denoiserEnabled ? "true" : "false")
        << ",\"atrousIterations\":" << renderSettings_.atrousIterations << ",\"denoiserStrength\":" << renderSettings_.denoiserStrength
        << ",\"debugView\":" << static_cast<uint32_t>(renderSettings_.debugView)
        << ",\"resolutionScale\":" << renderSettings_.resolutionScale << "},\n";
    out << "  \"entities\": [\n";
    const std::vector<const Entity*> entities = registry_.entities();
    for (size_t i = 0; i < entities.size(); ++i) {
        const Entity& entity = *entities[i];
        out << "    {\"id\":{\"index\":" << entity.id.index << ",\"generation\":" << entity.id.generation << "},";
        out << "\"name\":\"" << escapeJson(entity.name) << "\",";
        out << "\"transform\":{\"position\":";
        writeVec3(out, entity.transform.position);
        out << ",\"rotationEuler\":";
        writeVec3(out, entity.transform.rotationEuler);
        out << ",\"scale\":";
        writeVec3(out, entity.transform.scale);
        out << "}";
        if (entity.meshRenderer.has_value()) {
            out << ",\"meshRenderer\":{\"mesh\":" << entity.meshRenderer->mesh.index
                << ",\"visible\":" << (entity.meshRenderer->visible ? "true" : "false")
                << ",\"castShadow\":" << (entity.meshRenderer->castShadow ? "true" : "false")
                << ",\"visibleToCamera\":" << (entity.meshRenderer->visibleToCamera ? "true" : "false")
                << ",\"materialSlots\":[";
            for (size_t slotIndex = 0; slotIndex < entity.meshRenderer->materialSlots.size(); ++slotIndex) {
                const MaterialSlot& slot = entity.meshRenderer->materialSlots[slotIndex];
                out << "{\"name\":\"" << escapeJson(slot.name) << "\",\"material\":" << slot.material.index;
                if (slot.overrideMaterial.has_value()) {
                    out << ",\"overrideMaterial\":" << slot.overrideMaterial->index;
                }
                out << "}";
                if (slotIndex + 1u < entity.meshRenderer->materialSlots.size()) {
                    out << ',';
                }
            }
            out << "]}";
        }
        if (entity.light.has_value()) {
            out << ",\"light\":{\"type\":" << static_cast<uint32_t>(entity.light->type) << ",\"color\":";
            writeVec3(out, entity.light->color);
            out << ",\"intensity\":" << entity.light->intensity << ",\"sizeOrRadius\":" << entity.light->sizeOrRadius
                << ",\"enabled\":" << (entity.light->enabled ? "true" : "false") << "}";
        }
        if (entity.camera.has_value()) {
            out << ",\"camera\":{\"verticalFovRadians\":" << entity.camera->verticalFovRadians
                << ",\"nearPlane\":" << entity.camera->nearPlane << ",\"farPlane\":" << entity.camera->farPlane
                << ",\"active\":" << (entity.camera->active ? "true" : "false") << "}";
        }
        out << "}";
        if (i + 1u < entities.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

bool SceneDocument::loadJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string json = buffer.str();
    auto stringValue = [&](std::string_view key) -> std::optional<std::string> {
        const std::string needle = "\"" + std::string(key) + "\": \"";
        size_t begin = json.find(needle);
        if (begin == std::string::npos) {
            return std::nullopt;
        }
        begin += needle.size();
        const size_t end = json.find('"', begin);
        if (end == std::string::npos) {
            return std::nullopt;
        }
        return json.substr(begin, end - begin);
    };

    if (auto gltf = stringValue("sourceGltf"); gltf.has_value() && !gltf->empty()) {
        sourceGltfPath_ = *gltf;
    }
    if (auto hdr = stringValue("sourceHdr"); hdr.has_value() && !hdr->empty()) {
        sourceHdrPath_ = *hdr;
        environment_.hdrPath = *hdr;
    }

    auto numberAfter = [](const std::string& text, std::string_view key, float fallback) {
        const std::string needle = "\"" + std::string(key) + "\":";
        size_t begin = text.find(needle);
        if (begin == std::string::npos) {
            return fallback;
        }
        begin += needle.size();
        return std::strtof(text.c_str() + begin, nullptr);
    };
    auto uintAfter = [&](const std::string& text, std::string_view key, uint32_t fallback) {
        return static_cast<uint32_t>(std::max(0.0f, numberAfter(text, key, static_cast<float>(fallback))));
    };
    auto boolAfter = [](const std::string& text, std::string_view key, bool fallback) {
        const std::string needle = "\"" + std::string(key) + "\":";
        size_t begin = text.find(needle);
        if (begin == std::string::npos) {
            return fallback;
        }
        begin += needle.size();
        return text.compare(begin, 4, "true") == 0 ? true : text.compare(begin, 5, "false") == 0 ? false : fallback;
    };
    auto vec3After = [](const std::string& text, std::string_view key, glm::vec3 fallback) {
        const std::string needle = "\"" + std::string(key) + "\":[";
        size_t begin = text.find(needle);
        if (begin == std::string::npos) {
            return fallback;
        }
        begin += needle.size();
        glm::vec3 value = fallback;
        value.x = std::strtof(text.c_str() + begin, nullptr);
        begin = text.find(',', begin);
        if (begin == std::string::npos) {
            return value;
        }
        value.y = std::strtof(text.c_str() + begin + 1u, nullptr);
        begin = text.find(',', begin + 1u);
        if (begin == std::string::npos) {
            return value;
        }
        value.z = std::strtof(text.c_str() + begin + 1u, nullptr);
        return value;
    };
    auto nameAfter = [](const std::string& text, std::string_view key, std::string fallback) {
        const std::string needle = "\"" + std::string(key) + "\":\"";
        size_t begin = text.find(needle);
        if (begin == std::string::npos) {
            return fallback;
        }
        begin += needle.size();
        const size_t end = text.find('"', begin);
        return end == std::string::npos ? fallback : text.substr(begin, end - begin);
    };

    environment_.enabled = boolAfter(json, "enabled", environment_.enabled);
    environment_.intensity = numberAfter(json, "intensity", environment_.intensity);
    environment_.rotation = numberAfter(json, "rotation", environment_.rotation);
    environment_.backgroundIntensity = numberAfter(json, "backgroundIntensity", environment_.backgroundIntensity);

    renderSettings_.pathTracingEnabled = boolAfter(json, "pathTracingEnabled", renderSettings_.pathTracingEnabled);
    renderSettings_.directLightingEnabled = boolAfter(json, "directLightingEnabled", renderSettings_.directLightingEnabled);
    renderSettings_.maxBounces = uintAfter(json, "maxBounces", renderSettings_.maxBounces);
    renderSettings_.environmentDirectSamples = uintAfter(json, "environmentDirectSamples", renderSettings_.environmentDirectSamples);
    renderSettings_.exposure = numberAfter(json, "exposure", renderSettings_.exposure);
    renderSettings_.sunlightEnabled = boolAfter(json, "sunlightEnabled", renderSettings_.sunlightEnabled);
    renderSettings_.sunIntensity = numberAfter(json, "sunIntensity", renderSettings_.sunIntensity);
    renderSettings_.skyIntensity = numberAfter(json, "skyIntensity", renderSettings_.skyIntensity);
    renderSettings_.sunAngularRadius = numberAfter(json, "sunAngularRadius", renderSettings_.sunAngularRadius);
    renderSettings_.indirectStrength = numberAfter(json, "indirectStrength", renderSettings_.indirectStrength);
    renderSettings_.denoiserEnabled = boolAfter(json, "denoiserEnabled", renderSettings_.denoiserEnabled);
    renderSettings_.atrousIterations = uintAfter(json, "atrousIterations", renderSettings_.atrousIterations);
    renderSettings_.denoiserStrength = numberAfter(json, "denoiserStrength", renderSettings_.denoiserStrength);
    renderSettings_.debugView = static_cast<RendererDebugView>(uintAfter(json, "debugView", static_cast<uint32_t>(renderSettings_.debugView)));
    renderSettings_.resolutionScale = numberAfter(json, "resolutionScale", renderSettings_.resolutionScale);

    registry_ = SceneRegistry{};
    sceneMeshes_.clear();
    sceneMaterials_.clear();
    activeCamera_ = {};
    std::istringstream lines(json);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("\"id\"") == std::string::npos || line.find("\"transform\"") == std::string::npos) {
            continue;
        }

        const EntityId id = registry_.createEntity(nameAfter(line, "name", "Entity"));
        Entity* entity = registry_.entity(id);
        if (entity == nullptr) {
            continue;
        }
        entity->transform.position = vec3After(line, "position", entity->transform.position);
        entity->transform.rotationEuler = vec3After(line, "rotationEuler", entity->transform.rotationEuler);
        entity->transform.scale = vec3After(line, "scale", entity->transform.scale);

        if (line.find("\"meshRenderer\"") != std::string::npos) {
            MeshRenderer renderer;
            renderer.mesh = MeshAssetHandle{uintAfter(line, "mesh", UINT32_MAX)};
            renderer.visible = boolAfter(line, "visible", true);
            renderer.castShadow = boolAfter(line, "castShadow", true);
            renderer.visibleToCamera = boolAfter(line, "visibleToCamera", true);
            if (renderer.mesh.valid()) {
                sceneMeshes_.push_back(renderer.mesh);
            }
            const uint32_t material = uintAfter(line, "material", UINT32_MAX);
            if (material != UINT32_MAX) {
                renderer.materialSlots.push_back(MaterialSlot{.name = "slot 0", .material = MaterialAssetHandle{material}});
                sceneMaterials_.push_back(MaterialAssetHandle{material});
            }
            entity->meshRenderer = std::move(renderer);
        }
        if (line.find("\"light\"") != std::string::npos) {
            Light light;
            light.type = static_cast<LightType>(uintAfter(line, "type", static_cast<uint32_t>(LightType::Point)));
            light.color = vec3After(line, "color", light.color);
            light.intensity = numberAfter(line, "intensity", light.intensity);
            light.sizeOrRadius = numberAfter(line, "sizeOrRadius", light.sizeOrRadius);
            light.enabled = boolAfter(line, "enabled", true);
            entity->light = light;
        }
        if (line.find("\"camera\"") != std::string::npos) {
            Camera camera;
            camera.verticalFovRadians = numberAfter(line, "verticalFovRadians", camera.verticalFovRadians);
            camera.nearPlane = numberAfter(line, "nearPlane", camera.nearPlane);
            camera.farPlane = numberAfter(line, "farPlane", camera.farPlane);
            camera.active = boolAfter(line, "active", false);
            entity->camera = camera;
            if (camera.active) {
                activeCamera_ = id;
            }
        }
    }

    std::sort(sceneMeshes_.begin(), sceneMeshes_.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    sceneMeshes_.erase(std::unique(sceneMeshes_.begin(), sceneMeshes_.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }), sceneMeshes_.end());
    std::sort(sceneMaterials_.begin(), sceneMaterials_.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    sceneMaterials_.erase(std::unique(sceneMaterials_.begin(), sceneMaterials_.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }), sceneMaterials_.end());
    markDirty(SceneUpdateKind::FullSceneRebuild);
    return true;
}

SceneUpdateKind SceneDocument::combine(SceneUpdateKind current, SceneUpdateKind next) {
    if (next == SceneUpdateKind::None) {
        return current;
    }
    if (current == SceneUpdateKind::None) {
        return next;
    }
    if (current == next) {
        return current;
    }
    if (current == SceneUpdateKind::FullSceneRebuild || next == SceneUpdateKind::FullSceneRebuild) {
        return SceneUpdateKind::FullSceneRebuild;
    }
    return SceneUpdateKind::FullSceneRebuild;
}

} // namespace rtv
