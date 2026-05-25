#pragma once

#include <cstdint>
#include <string_view>

namespace rtv {

enum class RendererBackend : uint32_t {
    Auto,
    Compute,
    HardwareRayTracing,
};

[[nodiscard]] RendererBackend parseRendererBackend(std::string_view value);
[[nodiscard]] const char* rendererBackendName(RendererBackend backend);
[[nodiscard]] const char* rendererBackendDisplayName(RendererBackend backend);

} // namespace rtv
