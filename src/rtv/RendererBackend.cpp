#include "rtv/RendererBackend.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace rtv {

RendererBackend parseRendererBackend(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized == "auto") {
        return RendererBackend::Auto;
    }
    if (normalized == "compute") {
        return RendererBackend::Compute;
    }
    if (normalized == "rt" || normalized == "hardware-ray-tracing" || normalized == "hardware") {
        return RendererBackend::HardwareRayTracing;
    }
    throw std::runtime_error("Unknown renderer backend: " + std::string(value));
}

const char* rendererBackendName(RendererBackend backend) {
    switch (backend) {
    case RendererBackend::Auto: return "auto";
    case RendererBackend::Compute: return "compute";
    case RendererBackend::HardwareRayTracing: return "rt";
    }
    return "unknown";
}

const char* rendererBackendDisplayName(RendererBackend backend) {
    switch (backend) {
    case RendererBackend::Auto: return "Auto";
    case RendererBackend::Compute: return "Compute";
    case RendererBackend::HardwareRayTracing: return "Hardware Ray Tracing";
    }
    return "Unknown";
}

} // namespace rtv
