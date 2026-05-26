# Ray Tracing Engine Vulkan Port

This directory contains the native Vulkan 1.3 / C++20 port of the WebGPU path tracing renderer. Rendering is handled by the Vulkan KHR hardware ray tracing path.

The port is operational: it opens a native window, runs a Vulkan KHR hardware ray tracing path tracer on capable devices, denoises temporally/spatially, presents through compute tone mapping and a fullscreen pass, supports glTF/HDR inputs, exposes an ImGui editor, and includes GPU timing/debug views. It is now an editor-oriented renderer prototype with scene hierarchy, inspector, undo/redo, render settings persistence, and incremental scene-update paths, but it still needs more hardware-RT update/refit, material coverage, render-graph, and editor workflow hardening.

## Implemented

- GLFW window with Vulkan surface creation
- Vulkan 1.3 instance/device setup with validation debug messenger
- Physical-device selection requiring swapchain support, dynamic rendering, Synchronization2, and required descriptor features
- Logical device, graphics/present queues, and VMA allocator
- Swapchain creation, image views, resize handling, and per-swapchain-image present semaphores
- Per-frame command pools, command buffers, semaphores, and fences
- Dynamic rendering submitted through `vkQueueSubmit2`
- RAII `Buffer` abstraction for GPU-only, upload, readback, and persistently mapped buffers
- RAII `Image` abstraction for sampled textures, storage images, render targets, mip-capable images, image views, and descriptor metadata
- Synchronization2 image and buffer barrier helpers using `vkCmdPipelineBarrier2`
- Fence-backed `UploadContext` and `BufferUploader` for staging buffer, texture, and scene uploads
- `TextureLoader` and HDR environment upload paths
- Descriptor layout cache, descriptor allocator, descriptor writer, and descriptor set wrapper
- Per-frame descriptor arenas and transient CPU-visible uniform ring buffers
- GLSL to SPIR-V shader compilation through `glslangValidator`
- SPIR-V reflection for descriptor bindings, descriptor types, stage visibility, and push constants
- Pipeline cache, compute pipeline wrapper, graphics pipeline wrapper, ray tracing pipeline wrapper, and dynamic-rendering fullscreen pipeline
- Compact auxiliary buffers for variance, depth/normal, world position, and temporal history
- Temporal denoiser with reprojection, disocclusion rejection, luminance clipping, reactive masking, moving-camera preview support, and multi-scale a-trous filtering
- Temporal anti-aliasing with independent temporal frame tracking, TAA-only camera jitter, depth/world-position validation, and configurable sharpening
- Cornell-box scene and optional glTF/GLB import through `--gltf`
- glTF camera import into editable camera entities
- Radiance HDR environment loading through `--hdr`
- Environment row/column CDF generation and shader-side importance sampling
- Environment direct lighting / next-event estimation at every non-delta bounce, with a configurable per-hit environment sample count
- Separate lighting controls for procedural sky intensity, loaded HDR environment intensity, and camera-visible background intensity
- CPU BVH construction with Morton ordering, binned SAH, BVH4-style packed upload data, and rope traversal
- Scene buffers for materials, primitives, mesh records, instance records, light records, local mesh data, local BVHs, and TLAS nodes
- glTF texture residency through a fixed sampled texture array
- Shader-side base-color, normal, metallic-roughness, and emissive texture sampling
- Direct/emissive/environment lighting debug views plus PDF/MIS diagnostics
- GPU timestamp profiling for path tracing, denoising, and fullscreen passes
- ImGui editor with viewport, scene hierarchy, inspector, asset browser, render settings, debug/profiler, optional material editor, sample count, reset reason, resolution, and pass timings
- Scene hierarchy selection sync, selected-instance outline, transform gizmo preview/commit, active scene-camera piloting, and undo/redo-backed editor operations
- Inspector-backed transform, camera, light, mesh visibility, material assignment, cast-shadow, and visible-to-camera edits
- WASD/mouse camera controls, `Ctrl+L` sun rotation drag, pointer release on focus loss, and `F11` borderless fullscreen
- Primary Vulkan hardware ray tracing backend using `VK_KHR_acceleration_structure` and `VK_KHR_ray_tracing_pipeline`
- BLAS/TLAS construction for fallback Cornell-box and imported glTF triangle meshes
- Hardware RT path loop with raygen-owned multi-bounce tracing, direct/emissive/environment lighting, shadow rays, MIS/PDF diagnostics, denoiser auxiliary outputs, and alpha-aware any-hit shaders
- Hardware RT per-triangle material metadata so closest-hit and any-hit shaders use direct material lookup instead of scanning primitive records
- Hardware RT reduced ray payload: hit shaders return IDs, UVs, normals, and tangent basis; raygen performs material decode/texture evaluation after a hit is accepted
- Hardware RT optimized any-hit path that samples only alpha data when alpha is relevant and terminates shadow rays immediately for accepted opaque hits
- Conservative hardware RT opaque traversal fast path using `VK_GEOMETRY_OPAQUE_BIT_KHR` for meshes known to be alpha-free and double-sided
- Instance visibility flags for hidden, camera-hidden, and non-shadow-casting objects in ray queries
- AgX, ACES, PBR Neutral, Reinhard, Reinhard White, and Linear tone mappers
- Hardware RT debug/profiler stats for BLAS count, instance count, acceleration-structure memory, and SBT size

## Still In Progress

- Hardware RT material, lighting, temporal stability, and traversal stabilization
- Hardware RT acceleration-structure update/refit paths for transform-heavy editing
- Finer hardware RT geometry splitting so opaque single-sided and alpha-tested primitives can use different traversal/hit-group policies within the same mesh
- Further rough dielectric/specular sampling and MIS tuning
- Fully bindless material texture residency
- Broader glTF material extension support
- More complete scene instancing and transform update paths
- Automatic render graph resource-state tracking and barrier validation
- More denoiser/reprojection validation on the hardware RT output
- Continued editor workflow polish around component creation, camera activation, hierarchy reveal, and inspector edge cases

## Build

Requirements:

- Windows
- Visual Studio 2022 with C++ tools
- CMake
- Vulkan SDK
- vcpkg dependencies for this project

Configure and build from the repository root:

```powershell
cmake -S native/vulkan -B native/vulkan/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/Users/HomePc/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build native/vulkan/build --config Debug
```

If the terminal does not see `VULKAN_SDK`, pass the SDK paths explicitly:

```powershell
$env:VULKAN_SDK='C:\VulkanSDK\1.4.350.0'
cmake -S native/vulkan -B native/vulkan/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/Users/HomePc/vcpkg/scripts/buildsystems/vcpkg.cmake -DVulkan_INCLUDE_DIR=C:/VulkanSDK/1.4.350.0/Include -DVulkan_LIBRARY=C:/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib
cmake --build native/vulkan/build --config Debug
```

Run:

```powershell
native\vulkan\build\Debug\rtvulkan.exe
```

Smoke test:

```powershell
native\vulkan\build\Debug\rtvulkan.exe --frames 6
```

## Runtime Examples

```powershell
native\vulkan\build\Debug\rtvulkan.exe --debug-view direct
native\vulkan\build\Debug\rtvulkan.exe --debug-view indirect
native\vulkan\build\Debug\rtvulkan.exe --debug-view mis-weight
native\vulkan\build\Debug\rtvulkan.exe --gltf path\to\scene.glb
native\vulkan\build\Debug\rtvulkan.exe --hdr path\to\environment.hdr
native\vulkan\build\Debug\rtvulkan.exe --gltf path\to\scene.glb --hdr path\to\environment.hdr
```

Hardware RT requires `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_spirv_1_4`, and `VK_KHR_shader_float_controls`.
In hardware RT mode, raygen owns the path loop and launches primary, bounce, and shadow rays explicitly while keeping pipeline recursion depth at `1`.
Hardware RT uses a correctness-first material policy: alpha and single-sided material rules are enforced by any-hit where needed. Meshes that are known to be alpha-free and double-sided are marked opaque so Vulkan can skip any-hit traversal for that geometry.

## Runtime Controls

- Click captures mouse look.
- `Esc`, `Alt+Tab`, or focus loss releases mouse look.
- `WASD` moves the camera.
- `Q/E` or `Ctrl/Space` moves vertically.
- `Shift` increases camera speed.
- `F11` toggles borderless fullscreen.
- Hold `Ctrl+L` and drag to rotate the scene Primary Sun. Quick tapping `Ctrl+L` keeps the open-level shortcut behavior.
- `F1` cycles debug views.
- `0` returns to beauty view.
- `R` resets accumulation.
- `F2` toggles denoising.
- `F3` toggles denoising while the camera is moving.
- `F4` toggles the scene Primary Sun.
- `F5` toggles environment lighting.
- `F6` toggles direct lighting.
- `+/-` adjusts exposure.
- `</>` adjusts environment intensity.
- `[`/`]` rotates the environment.
- `PageUp/PageDown` changes max bounces.
- `Home/End` changes a-trous denoiser iterations.

The ImGui editor exposes the same core controls plus scene hierarchy, inspector editing, asset browsing, render settings, HDR path loading, profiler timing, hardware RT AS/SBT stats, sample count, resolution, debug view selection, accumulation reset reason, TAA sharpening, tone mapper selection, and environment lighting controls.

Environment control behavior:

- `Sky Intensity` controls the procedural fallback sky only.
- `Environment Intensity` controls loaded HDR environment lighting only.
- `Background Intensity` scales the camera-visible sky/background, so HDR lighting can be raised without immediately blowing out the visible sky.
- `Environment Samples` controls direct environment-light samples per surface hit. `1` is the interactive default; higher values reduce sky-light noise at a proportional performance cost.
- The procedural sky is uploaded through the same environment texture path as HDRs, but it is flagged separately on the GPU so it uses `Sky Intensity` instead of `Environment Intensity`.
- The scene-owned Primary Sun component is the source of truth for sun direction, illuminance, angular radius, and color temperature. Legacy directional sun lights are migrated on load.

## Debug Views

Supported debug views include:

- `beauty`
- `variance`
- `normals`
- `depth`
- `roughness`
- `reprojection-confidence`
- `denoiser-rejection`
- `direct-lighting`
- `indirect-lighting`
- `environment-contribution`
- `emissive-contribution`
- `instance-id`
- `mesh-id`
- `light-pdf`
- `bsdf-pdf`
- `mis-weight`
- `direct-sample-type`
- `albedo`
- `clay-material`
- `first-bounce-throughput`
- `secondary-environment-miss`
- `bounce-count`
- `secondary-environment-radiance`
- `white-environment-transport`
- `motion-vectors`
- `atmosphere-sky-view`
- `atmosphere-transmittance`
- `atmosphere-aerial-perspective`
- `atmosphere-multi-scatter`
- `temporal-reactive-mask`
- `temporal-history-weight`
- `restir-reservoir-age`
- `restir-reservoir-confidence`
- `restir-reservoir-m`

Some parse-compatible low-level traversal views remain available for saved settings and CLI compatibility, but the editor filters out placeholder/no-op views from normal cycling.

## Architecture

### Application Layer

`Application` owns the native window, top-level event loop, camera input, fullscreen switching, UI forwarding, and frame execution.

### Vulkan Core

`VulkanContext` owns instance, debug messenger, surface, physical-device selection, logical device, queues, enabled features, and the VMA allocator.

`Swapchain` owns swapchain images, views, format, extent, acquisition, presentation, and resize recreation.

`CommandSystem` owns per-frame command pools, command buffers, fences, and synchronization primitives.

### Resource Layer

`Buffer` owns `VkBuffer` plus `VmaAllocation`. It supports GPU-only buffers, CPU upload buffers, readback buffers, persistently mapped buffers, and descriptor-ready metadata.

`Image` owns `VkImage`, `VmaAllocation`, and image views. It tracks the current image layout so compute, transfer, sampled, and presentation transitions remain explicit.

`UploadContext` and `BufferUploader` create staging resources, record copy commands, submit upload work, and synchronize completion with fences. The design keeps the upload path compatible with a future transfer queue.

`ImageBarrier` wraps Synchronization2 barriers and intentionally avoids legacy `vkCmdPipelineBarrier`.

### Descriptor And Pipeline Layer

`DescriptorLayoutCache` owns reusable `VkDescriptorSetLayout` objects keyed by binding/type/count/stage.

`DescriptorAllocator` owns resettable descriptor pools. Frame-local allocators avoid descriptor lifetime hazards while command buffers are in flight.

`DescriptorWriter` batches descriptor writes for uniform buffers, storage buffers, sampled images, storage images, samplers, and combined image samplers.

`ShaderCompiler` invokes `glslangValidator` and recompiles shaders when sources are newer than generated SPIR-V outputs.

`ShaderReflection` uses SPIRV-Reflect to extract descriptor bindings and push constants. Reflection metadata is used to create compatible descriptor set layouts and pipeline layouts.

`PipelineCache`, `ComputePipeline`, and `GraphicsPipeline` own Vulkan pipeline state and dynamic-rendering-compatible layouts.

### Renderer Layer

The renderer executes this frame flow:

1. Upload changed uniforms and scene/environment data.
2. Dispatch path tracing through Vulkan KHR hardware RT.
3. Barrier path tracing outputs for denoiser reads.
4. Dispatch temporal/a-trous denoising compute.
5. Update/copy history resources for the next frame.
6. Present through a fullscreen dynamic-rendering graphics pass.
7. Render the ImGui overlay.

Accumulation resets when camera pose, resize, material, lighting, environment, denoiser/TAA, debug, shader, or scene state changes. Temporal/TAA frame tracking is separate from accumulation sample count so editor preview can keep temporal history while path-tracing accumulation resets.

### Scene Layer

The Vulkan scene path is moving from flattened demo data toward scalable GPU scene representation:

- Mesh records
- Primitive records
- Instance records
- Material records
- Light records
- Local mesh vertex/index buffers
- Local mesh triangle buffers
- Hardware RT triangle material ID buffer
- Instance bounds

This keeps shader logic independent from hardcoded scene geometry and prepares the hardware RT renderer for larger imported scenes, instancing, streaming, and editor-driven scene updates.

## Development Direction

Recommended next work:

1. Stabilize the hardware RT path across Cornell, Sponza, HDR, alpha-cutout, emissive, and normal-map scenes.
2. Continue rough dielectric/specular BSDF, PDF, and MIS tuning in the hardware RT shaders.
3. Add hardware RT TLAS/BLAS refit or rebuild paths for transform-heavy editing.
4. Split mixed-material hardware RT meshes into opaque, alpha, and single-sided geometry classes so more triangles can use fast opaque traversal without losing glTF material correctness.
5. Move fixed texture arrays toward a full bindless descriptor model.
6. Harden render graph state tracking and barrier validation.
7. Expand glTF material support and tune temporal reprojection/denoising against hardware RT output.
