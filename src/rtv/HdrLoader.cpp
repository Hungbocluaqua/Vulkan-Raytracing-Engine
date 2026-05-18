#include "rtv/HdrLoader.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace rtv {

HdrImageData HdrLoader::loadRadiance(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    float* loaded = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
    if (loaded == nullptr) {
        throw std::runtime_error("Failed to load HDR environment: " + path.string());
    }

    HdrImageData result;
    result.width = static_cast<uint32_t>(width);
    result.height = static_cast<uint32_t>(height);
    result.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    for (uint32_t y = 0; y < result.height; ++y) {
        const uint32_t dstY = result.height - 1u - y;
        for (uint32_t x = 0; x < result.width; ++x) {
            const size_t src = (static_cast<size_t>(y) * result.width + x) * 4u;
            const size_t dst = (static_cast<size_t>(dstY) * result.width + x) * 4u;
            result.rgba[dst + 0] = std::clamp(loaded[src + 0], 0.0f, 65504.0f);
            result.rgba[dst + 1] = std::clamp(loaded[src + 1], 0.0f, 65504.0f);
            result.rgba[dst + 2] = std::clamp(loaded[src + 2], 0.0f, 65504.0f);
            result.rgba[dst + 3] = 1.0f;
        }
    }

    stbi_image_free(loaded);
    return result;
}

HdrImageData HdrLoader::createProcedural(uint32_t width, uint32_t height) {
    HdrImageData result;
    result.width = std::max(width, 1u);
    result.height = std::max(height, 1u);
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4u);

    for (uint32_t y = 0; y < result.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(result.height);
        const float lat = (v - 0.5f) * 3.1415926535f;
        const float dirY = std::sin(lat);
        const float skyT = std::pow(std::max(dirY, 0.0f), 0.35f);
        for (uint32_t x = 0; x < result.width; ++x) {
            const float r = 0.70f * (1.0f - skyT) + 0.56f * skyT;
            const float g = 0.74f * (1.0f - skyT) + 0.68f * skyT;
            const float b = 0.80f * (1.0f - skyT) + 0.92f * skyT;
            const size_t idx = (static_cast<size_t>(y) * result.width + x) * 4u;
            result.rgba[idx + 0] = r;
            result.rgba[idx + 1] = g;
            result.rgba[idx + 2] = b;
            result.rgba[idx + 3] = 1.0f;
        }
    }

    return result;
}

} // namespace rtv
