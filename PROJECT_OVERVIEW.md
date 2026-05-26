# Project Overview

This document summarizes the current structure and architecture of the native Vulkan renderer in `native/vulkan`.

The project is a C++20 / Vulkan 1.3 path-tracing renderer and editor. It builds a native Windows executable named `rtvulkan` and currently supports a Vulkan KHR hardware ray tracing path, glTF/GLB scene loading, HDR environment maps, progressive accumulation, temporal/spatial denoising, TAA, histogram auto exposure, compute tone mapping, selection outlines, ImGui editor panels, GPU timing, and renderer debug views.

The renderer is operational, but it is still in stabilization. Current development is focused on the hardware ray tracing backend and editor-scene synchronization.

## Repository Shape

Important top-level files and directories:

- `CMakeLists.txt`: CMake build definition for the `rtvulkan` executable.
- `README.md`: user-facing build/run instructions and current feature status.
- `HardwareRT_Implementation_plan.md`: detailed phased plan for Vulkan hardware ray tracing.
- `docs/`: architecture and migration notes.
- `include/rtv/`: public/internal C++ headers for renderer modules.
- `src/rtv/`: C++ implementation files.
- `src/main.cpp`: executable entry point and CLI parsing.
- `shaders/`: GLSL compute post-process, fullscreen, and hardware ray tracing shaders.
- `build/`: local CMake build directory.
- `Sponza/`: local sample scene assets.
- `citrus_orchard_road_puresky_4k.hdr`: local HDR environment asset.
- `rtv_editor.ini`: ImGui/editor layout state.
- `run_rtvulkan.bat`, `run_rtvulkan_debug.bat`: local launcher scripts.

At the time of inspection, the project contained roughly:

- 66+ C++ source files.
- 70+ header files.
- 30+ shader files, including ray tracing, denoising, tone mapping, TAA, atmosphere, and ReSTIR-related compute shaders.
- Several migration/stabilization documents.

## Build System

`CMakeLists.txt` defines a single executable:

```text
rtvulkan
```

The project requires:

- Windows.
- Visual Studio 2022 C++ tools.
- CMake.
- Vulkan SDK.
- vcpkg-provided dependencies.

Main dependencies:

- Vulkan SDK.
- Volk.
- GLFW.
- GLM.
- Vulkan Memory Allocator.
- stb.
- tinygltf.
- SPIRV-Reflect.
- ImGui docking sources.

The CMake file currently includes local fallback paths for the Vulkan SDK, vcpkg, and ImGui source tree. That makes the project convenient on the current machine, but not fully portable without path cleanup.

## Program Entry

`src/main.cpp` is intentionally small. It parses CLI options and starts the application.

Supported options include:

- `--frames <count>`: run for a fixed number of frames, useful for smoke tests.
- `--debug-view <name>`: start in a specific renderer debug view.
- `--gltf <path>`: load a glTF/GLB scene.
- `--hdr <path>`: load an HDR environment map.

After parsing, `main()` constructs `rtv::Application` and calls `run()`.

## Application Layer

`Application` is the top-level owner of the runtime. It is declared in `include/rtv/Application.h` and implemented in `src/rtv/Application.cpp`.

It owns:

- The GLFW window.
- Runtime input handling.
- Camera controller.
- Scene loading and async glTF reloads.
- Asset manager.
- Scene document.
- Vulkan context.
- Resource allocator.
- Upload context and buffer uploader.
- Swapchain.
- Command system.
- ImGui overlay.
- Path tracer renderer.

Startup flow:

1. Create the GLFW window.
2. Initialize Vulkan through `VulkanContext`.
3. Create VMA resource allocation and upload helpers.
4. Create the swapchain and command system.
5. Load a glTF scene if provided; otherwise initialize the fallback scene.
6. Convert the editor scene document into a GPU scene asset.
7. Create `PathTracerRenderer`.
8. Apply the active scene camera.
9. Create the ImGui overlay.
10. Enter the main frame loop.

The main loop:

1. Polls GLFW events.
2. Computes frame delta time.
3. Starts an ImGui frame.
4. Processes runtime keyboard/mouse controls.
5. Builds editor panels.
6. Applies safe pre-frame editor requests.
7. Records and submits the frame through `CommandSystem`.
8. Applies post-frame requests that may require resource rebuilds.
9. Polls async scene loading.
10. Updates the window title.

## Vulkan Core

### `VulkanContext`

`VulkanContext` owns the Vulkan instance and device-level setup.

Responsibilities:

- Instance creation.
- Validation layer/debug messenger setup.
- GLFW surface creation.
- Physical-device selection.
- Required extension and feature checks.
- Logical device creation.
- Graphics/present queue retrieval.
- Bindless capability discovery.
- Hardware ray tracing capability discovery.
- VMA-compatible device state.

Hardware RT capability tracking includes:

- `VK_KHR_acceleration_structure`.
- `VK_KHR_ray_tracing_pipeline`.
- `VK_KHR_deferred_host_operations`.
- `VK_KHR_buffer_device_address`.
- `VK_KHR_spirv_1_4`.
- `VK_KHR_shader_float_controls`.

### `Swapchain`

`Swapchain` owns:

- `VkSwapchainKHR`.
- Swapchain images.
- Image views.
- Swapchain format.
- Swapchain extent.
- Surface support queries.
- Surface format selection.
- Present mode selection.
- Resize/recreation.

### `CommandSystem`

`CommandSystem` owns frame submission.

It uses two frames in flight and manages:

- Command pools.
- Command buffers.
- Per-frame fences.
- Image-available semaphores.
- Render-finished semaphores.
- Swapchain recreation.
- Frame recording.
- Queue submission.
- Presentation.

It delegates actual renderer work to `PathTracerRenderer` and UI rendering to `UiOverlay`.

## Resource Layer

The project wraps Vulkan resources in small RAII-style C++ types.

### `ResourceAllocator`

`ResourceAllocator` wraps Vulkan Memory Allocator and stores device/physical-device state. It also exposes debug naming and whether buffer device address is supported.

### `Buffer`

`Buffer` wraps:

- `VkBuffer`.
- `VmaAllocation`.
- Optional persistent mapping.

It supports:

- GPU-only memory.
- Upload memory.
- Readback memory.
- Descriptor metadata.
- Writes/flushes/invalidates.
- Buffer resizing.
- Device address queries for hardware RT.

### `Image`

`Image` wraps:

- `VkImage`.
- `VmaAllocation`.
- Image view.
- Current tracked layout.

It supports:

- Sampled image descriptors.
- Storage image descriptors.
- Full subresource ranges.
- Resizing.
- Mipmap generation.

### Upload Helpers

`UploadContext`, `BufferUploader`, and `BatchUploader` provide staging upload paths for buffers, textures, and scene data. Upload work is fence-backed and designed to remain compatible with a future transfer-queue path.

### Barriers

`ImageBarrier` provides Synchronization2 helpers around `vkCmdPipelineBarrier2`. The renderer avoids legacy `vkCmdPipelineBarrier` and keeps image/buffer transitions explicit.

## Descriptor And Pipeline Layer

The renderer has custom helpers for descriptor and pipeline management.

Key modules:

- `DescriptorLayoutCache`: caches reusable descriptor set layouts.
- `DescriptorAllocator`: owns resettable descriptor pools.
- `DescriptorSet`: lightweight descriptor set wrapper.
- `DescriptorWriter`: batches writes for buffers, images, image arrays, samplers, and acceleration structures.
- `ShaderCompiler`: invokes `glslangValidator` and recompiles GLSL when needed.
- `ShaderReflection`: uses SPIRV-Reflect to discover descriptor bindings and push constants.
- `ShaderModule`: owns compiled SPIR-V modules.
- `PipelineCache`: owns Vulkan pipeline cache state.
- `ComputePipeline`: owns compute pipeline state.
- `GraphicsPipeline`: owns dynamic-rendering fullscreen graphics pipeline state.
- `RayTracingPipeline`: owns Vulkan ray tracing pipeline and shader binding tables.

The descriptor and pipeline architecture is documented further in `docs/DESCRIPTOR_PIPELINE_SYSTEM.md`.

## Scene And Asset Model

The CPU-side asset model is handle-based and lives mainly in:

- `AssetManager`.
- `TextureAsset.h`.
- `MeshAsset.h`.
- `GltfLoader`.
- `SceneDocument`.
- `SceneRegistry`.
- `SceneComponents`.

### `AssetManager`

`AssetManager` owns vectors of:

- `TextureAsset`.
- `MaterialAsset`.
- `MeshAsset`.

Assets are referenced by lightweight handles:

- `TextureAssetHandle`.
- `MaterialAssetHandle`.
- `MeshAssetHandle`.

### Asset Types

`TextureAsset` stores texture metadata, sampler description, source path, residency state, and RGBA8 pixel data.

`MaterialAsset` stores:

- Base color factor.
- Emissive factor.
- Metallic factor.
- Roughness factor.
- Alpha cutoff.
- Alpha mode.
- Double-sided flag.
- Base color texture handle.
- Normal texture handle.
- Metallic-roughness texture handle.
- Emissive texture handle.

Imported metallic-roughness materials are uploaded as PBR/GGX materials even when `metallic == 0`, so dielectric roughness now affects stone, plastic, painted, and other non-metal surfaces. The shaders use a diffuse/specular mixture PDF for those materials instead of sampling only the GGX lobe.

`MeshAsset` stores:

- Vertices.
- Indices.
- Mesh primitives.
- Per-primitive material handles.

`SceneAsset` stores:

- Source path.
- Texture handles.
- Material handles.
- Mesh handles.
- Scene nodes.
- Scene lights.
- Root nodes.

### Editor Scene

`SceneDocument` is the editable scene representation. It owns:

- `SceneRegistry`.
- Environment settings.
- Render settings.
- Active camera.
- Source glTF/HDR paths.
- Dirty/update state.

`SceneRegistry` stores entities using generational `EntityId`s.

Entity components include:

- `Transform`.
- `MeshRenderer`.
- `Light`.
- `Camera`.

Scene changes are categorized by `SceneUpdateKind`:

- `None`.
- `MaterialOnly`.
- `TransformOnly`.
- `LightOnly`.
- `EnvironmentOnly`.
- `CameraOnly`.
- `VisibilityOnly`.
- `TopologyChanged`.
- `RendererSettingsOnly`.

### Scene Conversion

`SceneToGpuSceneBuilder` converts a `SceneDocument` into renderer-facing data. It returns:

- A generated `SceneAsset`.
- Instance entity mapping.
- Renderer settings.
- Accumulation reset reason.
- Whether a renderer rebuild is required.

This keeps editor edits separate from the lower-level GPU scene representation.

Current limitation: this conversion path is still evolving. Mesh entities, lights, render settings, visibility flags, environment settings, and active camera state reach the renderer, but topology-changing editor operations still require careful rebuild/refit routing.

## GPU Scene Representation

`GpuScene` is the main bridge between CPU scene data and shader-visible GPU buffers.

It owns buffers for:

- Vertices.
- Indices.
- BVH nodes.
- Triangle records.
- Materials.
- Analytic spheres.
- Mesh records.
- Primitive records.
- Instance records.
- Hardware RT triangle material IDs.
- Light records.
- Local mesh vertices.
- Local mesh indices.
- Instance bounds.
- Local BVH nodes.
- Local triangle data.
- TLAS nodes.
- TLAS instance indices.
- Environment row CDF.
- Environment column CDF.
- Mesh parameters uniform.
- Environment parameters uniform.

It also owns:

- Environment image.
- Environment sampler.
- Material texture table.
- Material texture samplers.
- Hardware ray tracing mesh build inputs.
- Hardware ray tracing instance build inputs.

This scene data feeds the hardware RT renderer and the post-processing passes.

Lighting note: GPU light records are built from emissive mesh/sphere geometry and authored scene lights. Directional, point, and area lights are merged into the same light-record selection table as emissive geometry; sun and environment lighting remain separate shader paths.

## Path Tracer Renderer

`PathTracerRenderer` is the main rendering engine.

It owns:

- The active `GpuScene`.
- Render resolution state.
- Accumulation state.
- Camera uniforms.
- Renderer settings.
- Debug parameters.
- Raw render image.
- Denoised image.
- Presentation image.
- History image.
- TAA history and parameter state.
- Accumulation buffer.
- Variance buffer.
- Depth/normal buffer.
- World position buffers.
- Uniform buffers.
- Descriptor layout cache.
- Pipeline cache.
- Shader modules.
- Denoiser compute pipeline.
- Fullscreen graphics pipeline.
- Hardware ray tracing pipeline.
- Hardware ray tracing scene.
- Auto-exposure histogram/reduce pipelines.
- Tone-map compute pipeline.
- TAA compute pipeline.
- Selection-outline compute pipeline.
- Per-frame descriptor arenas.
- GPU profilers.
- Renderer validation log.

Main frame flow:

1. Begin the frame and update camera/settings uniforms.
2. Transition the raw output image for shader writes.
3. Run path tracing through hardware RT.
4. Barrier path tracing outputs for denoiser reads.
5. Run temporal/spatial denoising if enabled.
6. Run TAA if enabled and update temporal history.
7. Copy history resources for the next frame.
8. Optionally build/reduce the luminance histogram for auto exposure.
9. Tone map into the presentation image.
10. Optionally run the selection outline compute pass.
11. Render fullscreen presentation.
12. Render the editor overlay.

Accumulation resets when relevant state changes:

- Startup.
- Resize.
- Camera movement.
- Manual reset.
- Render settings change.
- Lighting change.
- Environment change.
- Denoiser setting change.
- TAA setting change.
- Debug view change.
- Scene change.
- Material change.
- Shader reload.

## Renderer Backend

The renderer requires Vulkan KHR hardware ray tracing and fails clearly during startup when the selected device lacks the required extensions or features.

The hardware RT backend uses:

- `RayTracingScene`.
- `AccelerationStructure`.
- `RayTracingPipeline`.
- `shaders/pathtrace.rgen`.
- `shaders/pathtrace.rchit`.
- `shaders/pathtrace.rahit`.
- `shaders/pathtrace.rmiss`.
- `shaders/pathtrace_shadow.rahit`.
- `shaders/pathtrace_shadow.rmiss`.
- `shaders/rt_common.glsl`.

`RayTracingScene` builds:

- One or more BLAS objects from mesh geometry.
- A TLAS from scene instances.

`RayTracingPipeline` builds:

- Ray generation group.
- Miss groups.
- Hit groups.
- Shader binding tables.

The ray generation shader owns the multi-bounce path loop. Hit shaders return compact hit information such as IDs, UVs, normals, and tangent basis. Material decoding and texture evaluation happen after hits are accepted.

The hardware RT descriptor set includes the TLAS and an RT triangle-material-ID buffer in addition to the shared scene/material/environment buffers. New material, lighting, traversal, scene-update, and performance work should be implemented here first.

## Shaders

Shader files:

- `demo_compute.comp`: simple compute demo.
- `denoiser.comp`: temporal/spatial denoiser.
- `taa.comp`: temporal anti-aliasing resolve and configurable sharpening.
- `luminance_histogram.comp`: auto-exposure histogram builder.
- `exposure_reduce.comp`: auto-exposure percentile reducer/adaptation pass.
- `tone_map.comp`: HDR exposure, tone mapping, color grading, and output encoding.
- `selection_outline.comp`: selected-instance outline overlay.
- `fullscreen.vert`: fullscreen triangle vertex shader.
- `fullscreen.frag`: fullscreen presentation copy shader.
- `pathtrace.rgen`: ray generation shader for hardware RT.
- `pathtrace.rchit`: closest-hit shader.
- `pathtrace.rahit`: primary any-hit shader.
- `pathtrace.rmiss`: primary miss shader.
- `pathtrace_shadow.rahit`: shadow any-hit shader.
- `pathtrace_shadow.rmiss`: shadow miss shader.
- `rt_common.glsl`: shared hardware RT shader declarations and helpers.

The hardware RT shader set is the renderer path.

## Denoising

The denoiser is implemented in `shaders/denoiser.comp`.

It reads:

- Raw color.
- Packed depth/normal.
- Variance.
- History color.
- Current world position.
- Previous world position.
- Previous/current camera data.

It writes:

- Denoised color.

The denoiser performs temporal reprojection and spatial filtering. It can be disabled, run only when the camera is still, or run while moving depending on settings.

## Presentation

Presentation is split between compute and graphics passes.

`tone_map.comp` reads the raw, denoised, or TAA-resolved HDR renderer output, applies manual or auto exposure, runs the selected tone mapper, applies color grading, and writes the SDR presentation image. Tone mappers currently include Linear, Reinhard, Reinhard White, ACES, PBR Neutral, and AgX.

Auto exposure is scene-wide and histogram-based. `luminance_histogram.comp` bins log luminance from the denoised image, and `exposure_reduce.comp` chooses a configured percentile luminance, clamps the exposure target, and temporally adapts the current exposure.

`fullscreen.vert` emits a fullscreen triangle.

`fullscreen.frag` samples the already tone-mapped presentation image and writes it to the active render target.

`selection_outline.comp` can run after tone mapping to draw the selected-instance outline using the entity/instance ID buffer and packed depth/normal data.

After the fullscreen pass, the ImGui editor overlay is rendered.

## Editor And UI

The editor is ImGui-based and organized into panels:

- Viewport.
- Scene hierarchy.
- Inspector.
- Asset browser.
- Material editor.
- Render settings.
- Debug/profiler panel.
- Dockspace.

The editor builds `EditorRequests` rather than directly mutating every renderer subsystem. `Application` applies those requests at safe points in the frame.

Editor requests include:

- Renderer setting changes.
- Accumulation reset.
- glTF load.
- HDR load.
- Scene JSON save/load.
- Material update.
- Scene update.
- Entity creation/deletion/duplication/reparenting.
- Component creation for light, camera, and mesh renderer.
- Light, camera, transform, visibility, cast-shadow, and visible-to-camera edits.
- Transform gizmo live preview and undoable commit.
- Camera speed change.
- Camera reset.
- Shader reload.
- Layout reset.
- Denoiser toggle.
- Debug view toggle.
- Exit.

Current editor integration limits:

- Camera updates apply through the active-camera path and scene-camera piloting, but camera/editor workflows still need more manual validation.
- Creating or editing lights and cameras is routed through editor requests and scene operations; topology-changing edits require renderer rebuild/refit paths.
- Several Inspector branches for legacy/imported/fallback selections still show placeholder controls or temporary local values. Those controls can move in the UI without changing renderer state.
- The Inspector is the main editing surface for selected entities. The standalone Material Editor exists but is hidden by default.
- Material edits update `AssetManager` material data and attempt a GPU material-buffer update through `PathTracerRenderer::updateMaterials`.
- `Ctrl+L` rotates the scene-owned Primary Sun, but there is intentionally no separate sun drag indicator overlay at the moment.

## Runtime Controls

The app supports keyboard/mouse controls for:

- Camera movement.
- Mouse look.
- `Ctrl+L` Primary Sun drag rotation.
- Fullscreen toggle.
- Debug view cycling.
- Accumulation reset.
- Denoiser toggles.
- Sun/environment/direct-light toggles.
- Exposure adjustment.
- Environment intensity/rotation.
- Bounce count.
- A-trous denoiser iterations.

Files can be dropped onto the window:

- `.hdr` files load as environment maps.
- `.gltf` and `.glb` files trigger scene reload.

## Debugging And Profiling

`RendererDebugView` currently defines 38 parse-compatible debug views. The editor exposes a filtered set of useful/implemented views:

- Beauty.
- Variance.
- Normals.
- Reprojection confidence.
- Denoiser rejection.
- Depth.
- Roughness.
- Direct lighting.
- Indirect lighting.
- Emissive contribution.
- Environment contribution.
- Traversal steps.
- BVH depth.
- Instance ID.
- Mesh ID.
- TLAS steps.
- Traversal mismatch.
- Light PDF.
- BSDF PDF.
- MIS weight.
- Direct sample type.
- Albedo.
- Clay material.
- First-bounce throughput.
- Secondary environment miss.
- Bounce count.
- Secondary environment radiance.
- White environment transport.
- Motion vectors.
- Atmosphere sky view.
- Atmosphere transmittance.
- Atmosphere aerial perspective.
- Atmosphere multi-scatter.
- Temporal reactive mask.
- Temporal history weight.
- ReSTIR reservoir age.
- ReSTIR reservoir confidence.
- ReSTIR reservoir M.

`GpuProfiler` records GPU timings for major passes:

- Path tracing.
- Denoising.
- Fullscreen/presentation work.

The renderer validation log records finer pass names, including hardware RT tracing, auto-exposure histogram/reduce, tone map compute, selection outline, history copy, and editor viewport presentation.

The editor also exposes hardware RT stats:

- Active/inactive status.
- BLAS count.
- Instance count.
- Acceleration-structure memory.
- Shader binding table size.

## Documentation

Existing docs:

- `docs/RESOURCE_SYSTEM.md`: Vulkan resource ownership and barrier rationale.
- `docs/DESCRIPTOR_PIPELINE_SYSTEM.md`: descriptor lifetime, reflection, and pipeline strategy.
- `docs/PATH_TRACER_MIGRATION.md`: GPU scene and path tracer migration details.
- `docs/RENDERER_STABILIZATION.md`: MIS, debug visualization, asset pipeline, bindless groundwork, and validation notes.
- `docs/MIGRATION_ROADMAP.md`: WebGPU-to-Vulkan migration roadmap.
- `HardwareRT_Implementation_plan.md`: detailed hardware RT implementation plan.

## Current Development State

The renderer is functional but not finished. The strongest parts of the architecture are:

- Clear application/runtime ownership.
- RAII resource wrappers.
- Explicit Synchronization2 barriers.
- Shared GPU scene representation.
- Hardware RT backend.
- Shader reflection-driven descriptor layout construction.
- Editor request flow separated from immediate renderer mutation.
- Undo/redo command stack for editor operations.
- Scene-document-backed renderer settings and JSON persistence.
- Rich debug and profiling support.

Main active or future work areas:

- Hardware RT acceleration structure updates/refits.
- Hardening editor camera propagation and active-camera edge cases.
- Hardening authored light updates, GPU light-record weighting, and light editing workflows.
- Continuing to replace placeholder Inspector controls with controls backed by actual scene/renderer state.
- Finer geometry splitting for opaque, alpha-tested, and single-sided traversal paths.
- Further rough dielectric/specular sampling and MIS tuning in the hardware RT path.
- Fully bindless material texture residency.
- Broader glTF material extension support.
- More complete scene instancing and transform update paths.
- Render graph resource-state tracking and barrier validation.
- Denoiser, TAA, and reprojection validation against hardware RT output and the WebGPU reference.

## Important Architectural Takeaway

The project is not just a minimal Vulkan sample. It is a staged renderer/editor migration with a real scene pipeline, GPU scene abstraction, hardware ray tracing backend, denoiser, debug tooling, and editor integration.

The central data flow is:

```text
CLI / editor input
    -> Application
    -> SceneDocument / AssetManager
    -> SceneToGpuSceneBuilder
    -> GpuScene
    -> PathTracerRenderer
    -> hardware RT backend
    -> denoiser
    -> fullscreen presentation
    -> ImGui editor overlay
```

This separation lets the project continue improving editor features, scene import, GPU data layout, and renderer tracing without rewriting the full application shell.
