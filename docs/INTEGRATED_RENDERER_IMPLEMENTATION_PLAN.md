# Integrated Renderer Implementation Plan

This document converts the 81-phase renderer improvement roadmap into an executable implementation plan.

The plan is intentionally staged. Early batches fix correctness and low-risk performance issues in the current megakernel renderer. Middle batches harden temporal, denoising, ReSTIR, and material systems. Late batches are architecture v2 work and must not begin until the renderer is stable, profiled, and regression-tested.

## Execution Rules

### Phase Exit Gates

Every phase must pass these gates before the next dependent phase begins:

- Builds in Debug and Release.
- Runs without Vulkan validation errors.
- Produces stable output in the validation scenes.
- Has at least one targeted debug view, screenshot, GPU capture, or profiler counter proving the intended behavior.
- Does not regress static-scene accumulation.
- Does not add unbounded memory growth, descriptor leaks, or command-buffer lifetime hazards.

### Batch Exit Gates

Every batch must additionally pass:

- 120-frame static accumulation stability test.
- 120-frame camera-motion test.
- GPU timestamp comparison against the previous batch.
- RenderDoc or equivalent capture with clean barriers and descriptor state.
- Manual visual check on at least: Cornell/simple emissive scene, glossy metal scene, foliage/alpha scene, Sponza, outdoor HDR environment.

### Implementation Pattern

Each phase should follow the same sequence:

1. Add a compile-time or runtime feature flag if the change can affect frame output.
2. Add debug/profiler instrumentation before changing behavior.
3. Implement the smallest isolated code path.
4. Validate against a known baseline scene.
5. Enable by default only after the phase acceptance criteria pass.
6. Remove dead fallback code only after the next batch is stable.

### Phase 49-54 Scope

Phases 49-54 are treated as architecture and memory hardening before OMM, wavefront, and SER:

- Phase 49: Async compute overlap for denoiser, TAA/TSR, tone map, histogram, and post passes.
- Phase 50: Reservoir compression for ReSTIR DI/GI.
- Phase 51: Memory aliasing for temporal swap buffers and reservoir ping-pong buffers.
- Phase 52: Async entity picking.
- Phase 53: Replace avoidable `vkDeviceWaitIdle`.
- Phase 54: VMA budget control and descriptor pool tuning.

## Batch Overview

| Batch | Phases | Goal | Risk |
|-------|--------|------|------|
| 1 | 1-6, 15, 46 | Critical correctness and trivial quality fixes | Low |
| 2 | 7-10, 14, 16-17, 29-30 | Quick performance and BSDF quality wins | Low-Medium |
| 3 | 11-13, 18 | Sampling, denoiser, adaptive quality | Medium |
| 4 | 19-22, 49 | Temporal super resolution, display/output split, async compute overlap | Medium-Large |
| 5 | 23-28 | Production denoising | Large |
| 6 | 31-33 | ReSTIR DI correctness and light BVH quality | Medium-Large |
| 7 | 34-39 | ReSTIR GI | Large |
| 8 | 40-48, 55 | Material and texture system improvements | Medium-Large |
| 9 | 50-54 | Reservoir compression, memory aliasing, stall removal, memory-budget hardening | Large |
| 10 | 56-60 | Opacity micromaps | Medium-Large |
| 11 | 61-74 | Wavefront path tracing architecture v2 | Very Large |
| 12 | 75-77 | SER integration | Large |
| 13 | 78-81 | Advanced rendering features | Very Large |

## Batch 1: Critical Fixes

Goal: remove known correctness errors that bias the path tracer or waste trivial GPU work.

### Phase 1: Continue Paths After Emissive Hits [DONE]

Target files:

- `shaders/pathtrace.rgen`

Steps:

1. Locate the emissive-hit contribution path around the current emissive termination logic.
2. Replace unconditional path termination with normal throughput/radiance handling.
3. Only terminate when the sampled BSDF, path depth, or Russian roulette says to terminate.
4. Ensure next-event estimation and MIS state are still updated after the emissive contribution.
5. Add a debug mode that colors paths reaching an emissive and continuing past it.

Acceptance criteria:

- Emissive surfaces still contribute directly when hit.
- Paths can bounce away from emissive geometry.
- No energy spike from double-counting emissive direct light.
- Cornell box with emissive ceiling converges without dark secondary bounce loss.

Implementation notes:

- `shaders/pathtrace.rgen` now contributes hit emission without unconditionally terminating the path.
- Direct-light NEE is skipped on the emissive hit itself to avoid self-light double counting; subsequent BSDF continuation updates the usual next-hit MIS state.
- Added `emissive-continuation` debug view (`RendererDebugView::EmissiveContinuation`, value 38) to color paths that hit emissive geometry and successfully continue.
- Verified `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view emissive-continuation`.

### Phase 2: Fix Delta Classification Roughness Check [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`

Steps:

1. Find the delta/specular branch currently based on material or lobe type only.
2. Add `roughness < 0.001` as the delta threshold.
3. Clamp roughness for numerical stability separately from delta classification.
4. Ensure MIS PDF logic treats true delta lobes as non-MIS-sampled where appropriate.
5. Add a roughness ramp validation scene or debug material override.

Acceptance criteria:

- Roughness `0.0` and `0.0005` behave as delta.
- Roughness `0.001` and above use glossy sampling/PDF logic.
- No NaN or firefly increase on near-perfect mirrors.

Implementation notes:

- Added `MATERIAL_DELTA_ROUGHNESS_THRESHOLD = 0.001` and `material_is_delta(...)` in `shaders/rt_common.glsl`.
- Texture roughness is preserved down to `0.0`; GGX numerical stability now uses `ggx_safe_roughness(...)` separately.
- Direct-light delta skipping and path-scatter delta branches now use the threshold; rough specular surfaces fall through to glossy BRDF sampling/PDF.
- True delta lobes return zero from `pdf_brdf(...)`, keeping MIS from treating them as area-sampled glossy events.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view roughness`.

### Phase 3: Remove Double Fresnel From Multi-Scatter Compensation [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Inspect the GGX multi-scatter compensation path around the current specular accumulation.
2. Verify whether Fresnel is already included in the compensation term.
3. Change the accumulation to add the multi-scatter compensation directly instead of multiplying Fresnel twice.
4. Compare white furnace and metallic furnace results before and after.

Acceptance criteria:

- White furnace test remains energy-conserving.
- Metals do not darken incorrectly at grazing angles.
- Multi-scatter compensation remains bounded for roughness near 1.

Implementation notes:

- `heitz_ms_ggx(...)` already includes average Fresnel through `f_avg`; `eval_ggx_brdf(...)` now adds the returned multi-scatter term directly instead of multiplying by Schlick Fresnel again.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug beauty smoke run.

### Phase 4: Remove `indirect_strength` From Russian Roulette Decisions [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Find where `indirect_strength` scales throughput before Russian roulette probability is computed.
2. Move `indirect_strength` application to final indirect contribution only.
3. Keep Russian roulette based on physical throughput luminance.
4. Validate that setting `indirect_strength` to 0 does not change path survival before final contribution scaling.

Acceptance criteria:

- `indirect_strength` changes appearance only, not path survival statistics.
- GPU time is stable when varying `indirect_strength`.
- No biased brightness shift in scenes with low indirect strength.

Implementation notes:

- Removed `camera.indirect_strength` from path throughput updates in `shaders/pathtrace.rgen`.
- Applied `indirect_strength` only when adding non-primary-bounce lighting contributions to radiance and debug components.
- Russian roulette now uses physical throughput rather than the artistic indirect multiplier.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug beauty smoke run.

### Phase 5: MIS-Weight Sun Disk Hits [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `shaders/environment_sampling.glsl`
- `shaders/atmosphere_lighting.glsl`

Steps:

1. In `pathtrace.rgen`, audit both sun paths: explicit sun sampling through `sample_sun_light` and environment miss handling where `environment_sun_disk_radiance` contributes.
2. Treat `rt_common.glsl` as the canonical shared BSDF/PDF implementation target; update `environment_sampling.glsl` and `atmosphere_lighting.glsl` only when the helper function lives there in the current checkout.
3. Compute the BSDF PDF and sun/light PDF in matching solid-angle measure.
4. Apply `power_heuristic` to the sun disk contribution when a BSDF-sampled ray hits/misses into the sun disk.
5. Verify the explicit-light path and BSDF/environment-miss path use symmetric MIS weights.
6. Store the previous event PDF and event type in path state so sun disk MIS can distinguish delta, BSDF, environment, and light samples.
7. Add a debug view for sun MIS weight, sun light PDF, and previous BSDF PDF.

Acceptance criteria:

- Sun disk brightness is stable across roughness values.
- No double-bright sun highlight when both BSDF and light sampling can hit the disk.
- Outdoor HDR scene converges with fewer sun fireflies.

Implementation notes:

- Added `analytical_sun_pdf(...)` in `shaders/rt_common.glsl` so explicit sun sampling and BSDF/environment-miss sun hits share the same solid-angle PDF.
- `shaders/pathtrace.rgen` now tracks previous path event type (`BSDF` vs `DELTA`) and applies `power_heuristic(previousBsdfPdf, sunPdf)` when a non-delta BSDF ray misses into the analytical sun disk.
- Added sun-specific debug state and debug views: `sun-mis-weight`, `sun-light-pdf`, and `sun-previous-bsdf-pdf`.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view sun-mis-weight`.

### Phase 6: Use Effective RIS PDF For MIS [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`
- `shaders/environment_sampling.glsl`

Steps:

1. In `pathtrace.rgen`, find RIS candidate selection around direct environment/light sampling and the MIS code consuming the selected candidate PDF.
2. Treat `rt_common.glsl` as the canonical shared data-layout and BRDF/PDF target; use `restir_common.glsl` only for reservoir helper functions that are actually shared by ReSTIR passes.
3. Track raw proposal PDF, candidate target value, candidate count, reservoir weight sum, selected candidate probability, and final effective RIS PDF.
4. Store the effective PDF in the selected sample or reservoir payload instead of recomputing from the raw proposal.
5. Use the effective RIS PDF in MIS for direct lighting, environment lighting, and sun-disk competition.
6. Add debug output for raw proposal PDF, effective RIS PDF, and their ratio.
7. Add a test mode with fixed candidate count 1; it must match the non-RIS direct-light estimator.

Acceptance criteria:

- RIS estimator remains unbiased in a single-light test.
- Changing RIS candidate count changes variance, not mean brightness.
- No brightness shift between RIS disabled and RIS enabled after enough samples.

Implementation notes:

- The emissive RIS path now computes `effectiveLightPdf = rawLightPdf * candidateCount * selectedProxy / proxyWeightSum`.
- MIS and the direct-light contribution divide use the effective RIS PDF rather than the raw proposal PDF.
- First-bounce ReSTIR reservoir payloads receive the effective direct-light PDF through `components.first_light_pdf`.
- Added debug views: `ris-raw-light-pdf`, `ris-effective-light-pdf`, and `ris-pdf-ratio`.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view ris-effective-light-pdf`.

### Phase 15: Lower GGX Roughness Floor [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Locate the current roughness floor, expected near `0.02`.
2. Lower the floor to `0.001`.
3. Keep denominator guards for GGX D, G, and PDF functions.
4. Add NaN guards in debug builds if shader debug support exists.

Acceptance criteria:

- Mirror-like materials become visibly sharper.
- No NaN/Inf pixels in a roughness-0 validation scene.
- Denoiser does not smear sharp reflections more than before.

Implementation notes:

- Lowered `MATERIAL_MIN_GGX_ROUGHNESS` in `shaders/rt_common.glsl` from `0.02` to `0.001`.
- Kept the existing denominator guards in GGX D, G, PDF, and sampling functions.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view roughness`.

### Phase 46: Enable Anisotropic Texture Filtering [DONE]

Target files:

- `src/rtv/TextureLoader.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`

Steps:

1. Query `samplerAnisotropy` support during physical-device feature setup.
2. Enable `samplerAnisotropy` when available.
3. Create material texture samplers with anisotropy enabled and a device-limit clamp.
4. Add a renderer setting for anisotropy level if one does not exist.
5. Fall back to isotropic filtering when unsupported.

Acceptance criteria:

- Oblique floor/wall textures remain sharper.
- Validation layers report sampler feature usage as valid.
- Unsupported devices run without crashing.

Implementation notes:

- `VulkanContext` now queries `samplerAnisotropy`, enables it at device creation when supported, and exposes the device max anisotropy.
- `ResourceAllocator` carries sampler-anisotropy support and max anisotropy to resource creation code.
- Added `RendererSettings::materialTextureAnisotropy`, scene JSON persistence, and a Render Settings UI slider for material texture anisotropy.
- Material texture samplers in `GpuScene` now use the renderer-controlled anisotropy level, clamp to the device limit, and fall back to 1x when unsupported.
- `GpuScene` recreates material samplers when the anisotropy setting changes and defers destruction of retired samplers until enough frames have passed for in-flight descriptor use to finish.
- Combined image sampler descriptors now use the per-texture sampler array when available, preserving imported sampler state while applying the anisotropy level.
- Verified Debug build, Release build, and a 3-frame Debug smoke run.

## Batch 2: Quick Wins

Goal: improve frame responsiveness and sampling quality without changing architecture.

### Phase 7: Clamp Delta Time [DONE]

Target files:

- `src/rtv/Application.cpp`
- `src/rtv/CameraController.cpp`
- `include/rtv/RendererSettings.h`

Steps:

1. Identify where frame delta time is computed.
2. Clamp simulation/render delta time to a sane maximum, for example 1/30s or 1/15s.
3. Keep raw frame time available for profiler UI.
4. Ensure camera smoothing and adaptive quality use clamped delta, not raw stall spikes.

Acceptance criteria:

- Window drag, breakpoint resume, and shader compile stalls do not cause huge camera jumps.
- Adaptive quality does not enter a long feedback loop after one slow frame.

Implementation notes:

- `Application` now computes both raw and clamped frame delta; raw frame time remains visible in the editor/profiler UI, while runtime controls and renderer frame timing use the clamped value.
- Added `RendererSettings::maxFrameDeltaSeconds` with a default clamp of `1/30s`, sanitized in `PathTracerRenderer::applySettings`.
- `CameraController` defensively clamps its input delta to `1/30s` before applying keyboard movement.
- Auto-exposure adaptation receives the clamped frame delta through `PathTracerRenderer::setFrameDeltaSeconds`.
- Verified Debug build, Release build, and a 3-frame Debug smoke run.

### Phase 8: Increase Frames In Flight From 2 To 3 [DONE]

Target files:

- `include/rtv/FrameResources.h`
- `src/rtv/FrameResources.cpp`
- `include/rtv/CommandSystem.h`
- `src/rtv/CommandSystem.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Locate the global frames-in-flight constant.
2. Increase from 2 to 3.
3. Audit per-frame buffers, descriptor pools, command buffers, fences, and query pools.
4. Ensure accumulation frame index remains logical-frame based, not frame-resource-index based.
5. Verify resize and swapchain recreation wait on all frame fences.

Acceptance criteria:

- No frame-resource overwrite hazards.
- CPU frame time improves or remains stable.
- Accumulation does not reset incorrectly every third frame.

Implementation notes:

- Increased `CommandSystem::framesInFlight` from 2 to 3, giving the command pools, command buffers, acquire semaphores, and fences three independent frame slots.
- `PathTracerRenderer` now allocates three per-frame descriptor/uniform resources and three GPU profiler query sets.
- Updated `PipelineDemo` to three per-frame descriptor/uniform resources as part of the frame-resource aliasing audit.
- Accumulation continues to use logical sample/frame counters (`frameCount_` and `temporalFrameIndex_`) rather than the modulo frame-resource index.
- Resize and swapchain recreation paths still use device-idle waits, which cover all frame fences for this phase.
- Verified Debug build, Release build, and a 5-frame Debug smoke run to exercise frame slot wraparound.

### Phase 9: Eliminate Per-Frame Picking Ray [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/EditorSelection.h`

Steps:

1. Treat `pathtrace.rgen` as the active ray tracing implementation target; the current picking path is driven through `trace_surface_with_mode(..., picking)` and `payload.picking`.
2. Identify whether the renderer dispatches picking every frame or only when the editor requests selection.
3. Change picking to run only on click/selection request.
4. Store the last pick result until the next request.
5. Route picking through an explicit request object containing screen coordinate, frame id, scene revision, and requested entity mask.
6. Keep alpha-tested picking correctness by preserving the `payload.picking` behavior in any-hit shaders.
8. Add profiler markers for pick dispatch and pick readback separately.

Acceptance criteria:

- No picking ray work in normal frames.
- Click selection still works on opaque and alpha-tested geometry.
- GPU time improves in path tracing by the expected small percentage.

Implementation notes:

- Completed by reusing the alpha-tested primary visibility result for the entity-id buffer instead of tracing a second per-pixel picking ray.
- `shaders/pathtrace.rgen` now writes `entity_id_buffer` from the first path-trace hit stored in `PathComponents`.
- Existing editor click selection continues to read the latest entity-id buffer; a dedicated asynchronous click/request dispatch remains deferred to Phase 52.
- Verified Debug build, Release build, and a 5-frame Debug smoke run.

### Phase 10: Precompute Normal Matrices [DONE]

Target files:

- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `include/rtv/GpuScene.h`
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rchit`

Steps:

1. Add a normal matrix field to the GPU instance or geometry record.
2. Compute inverse-transpose transform on CPU during scene upload/refit.
3. Replace shader-side normal matrix reconstruction.
4. Keep support for non-uniform scale.
5. Validate transformed normals in a debug-normal view.

Acceptance criteria:

- Normal debug view matches previous output.
- Non-uniformly scaled meshes shade correctly.
- Shader instruction count decreases.

Implementation notes:

- Added `normalTransform` to `GpuInstanceRecord` and `normal_transform` to the GLSL `InstanceRecord` layout.
- `makeInstanceRecord(...)` now computes the inverse transform once and stores the CPU-side inverse-transpose normal transform for scene upload and transform refit paths.
- GPU cache restore reconstructs `normalTransform` from cached inverse transforms, so existing cache data can still load through the new GPU layout.
- Replaced shader-side `transpose(mat3(instance.inverse_transform))` reconstruction in closest-hit, primary any-hit, shadow any-hit, and emissive-light sampling with `mat3(instance.normal_transform)`.
- Verified Debug build, Release build, a 5-frame Debug normals smoke run, and a 5-frame Debug normals run against `scenes/validation/transform_stress.rtlevel`.

### Phase 14: VNDF Sampling For GGX [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Add a visible-normal GGX sampling function.
2. Return sampled half-vector, BSDF value, and matching PDF.
3. Replace old GGX NDF sampling for glossy reflection.
4. Preserve delta path for roughness below the Phase 2 threshold.
5. Validate PDF by comparing BSDF sampling histograms or furnace output.

Acceptance criteria:

- Grazing-angle reflections converge faster.
- No brightness shift in rough metal scenes.
- PDF and eval functions are measure-consistent.

Implementation notes:

- Added tangent-frame helpers and an isotropic Heitz visible-normal GGX sampler in `shaders/rt_common.glsl`.
- `sample_ggx_brdf(...)` now samples visible GGX normals in the view-aligned stretched space and reflects around the sampled half-vector.
- `pdf_ggx_brdf(...)` now evaluates the matching visible-normal reflection PDF, `D(h) * G1(v) / (4 * NdotV)`, instead of the old NDF half-vector PDF.
- The existing Phase 2 delta path remains outside `sample_brdf(...)`, so roughness below `0.001` still uses the explicit delta reflection/refraction branches.
- Verified Debug build, Release build, a 5-frame Debug beauty smoke run, and a 5-frame Debug run against `scenes/validation/material_grid.rtlevel`.

### Phase 16: Height-Correlated Smith G2 [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Add height-correlated Smith masking-shadowing function.
2. Replace uncorrelated G2 for GGX eval.
3. Keep numerical guards for `NdotV` and `NdotL`.
4. Validate with roughness/angle sweeps.

Acceptance criteria:

- Specular lobe shape matches expected GGX behavior.
- Energy remains bounded in furnace tests.

Implementation notes:

- Added `smith_ggx_lambda(...)` and replaced the uncorrelated `smith_g1(v) * smith_g1(l)` product with height-correlated Smith `G2 = 1 / (1 + lambdaV + lambdaL)`.
- Kept `smith_g1(...)` for the Phase 14 VNDF visible-normal PDF, where the sampled visible-normal distribution still needs the view masking term.
- Retained numerical guards for low `NdotV`, low `NdotL`, and near-zero tangent denominators.
- Verified Debug build, Release build, and a 5-frame Debug material-grid beauty run.

### Phase 17: Diffuse Energy Compensation [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Estimate energy lost to specular Fresnel for mixed diffuse/specular materials.
2. Reduce diffuse contribution by the physically appropriate specular allocation.
3. Apply consistently in BSDF eval and sampling weight.
4. Validate white furnace and colored dielectric scenes.

Acceptance criteria:

- Dielectrics do not exceed unit energy.
- Diffuse albedo remains visually stable for low-specular materials.

Implementation notes:

- Added shared PBR helpers for `f0`, Schlick average Fresnel, and diffuse energy allocation in `shaders/rt_common.glsl`.
- Mixed diffuse/specular BRDF evaluation now scales the diffuse term by `(1 - averageFresnel) * (1 - metallic)` instead of using the half-vector Fresnel term directly.
- The diffuse/specular sampling probability now uses the same compensated diffuse energy term, while keeping view Fresnel for the specular sampling weight.
- Pure diffuse material closures remain unchanged.
- Verified Debug build, Release build, and a 5-frame Debug material-grid beauty run.

### Phase 29: Reduce Environment Samples [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Locate the fixed environment sample loop.
2. Reduce default from 8 to 1 or 2.
3. Make the sample count user-configurable for profiling.
4. Ensure MIS and ReSTIR paths still receive correct PDFs.

Acceptance criteria:

- GPU time drops in environment-lit scenes.
- Variance increase is acceptable and preferably offset by RIS/ReSTIR.

Implementation notes:

- `RendererSettings`, `RenderSettings`, and `CameraUniform` default `environmentDirectSamples` to `1`.
- The Render Settings panel exposes a user-configurable `Environment Samples` slider from `1` to `8` for profiling.
- `shaders/pathtrace.rgen` clamps `camera.environment_direct_samples` to `1..8`, loops over that count, divides the accumulated contribution by the sample count, and keeps MIS PDFs in solid-angle measure for each environment sample.
- First-sample environment PDFs continue to populate the direct-light debug/ReSTIR payload fields through `sampledLightPdf`, `sampledBsdfPdf`, and `sampledMisWeight`.
- Verified as part of the Phase 17 Debug/Release builds and material-grid smoke run.

### Phase 30: Tune Russian Roulette [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`

Steps:

1. Set Russian roulette start depth based on current bounce configuration.
2. Compute survival probability from throughput luminance with a minimum clamp.
3. Scale throughput by inverse survival probability only when the path survives.
4. Profile path length distribution.

Acceptance criteria:

- 5-15 percent GPU reduction in bounce-heavy scenes.
- No measurable mean brightness bias.

Implementation notes:

- Added `RendererSettings::russianRouletteMinSurvival` with a default of `0.10`, clamped in `PathTracerRenderer::applySettings`.
- Stored the RR minimum survival value in `CameraUniform::renderControls.w`.
- `shaders/pathtrace.rgen` now derives RR start depth from `max_bounces` (`max_bounces / 2`, clamped to path depths 3-5) and skips RR on the final possible bounce.
- Survival probability now uses physical throughput luminance with the configured minimum clamp and a `0.95` maximum, replacing the previous max-RGB survival heuristic.
- Throughput is divided by the survival probability only after the path survives.
- Verified Debug build, Release build, and a 5-frame Debug smoke run with `--debug-view path-length`.

## Batch 3: Core Performance

Goal: improve sample quality, denoiser cost, and interactive responsiveness.

### Phase 13: Sobol, Owen Scrambling, And STBN [DONE]

Target files:

- `shaders/blue_noise.glsl`
- `shaders/pathtrace.rgen`
- `shaders/restir_*.comp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Add Sobol sequence tables or generated buffers.
2. Add per-pixel/per-frame Owen scrambling.
3. Integrate spatiotemporal blue-noise texture or procedural STBN source.
4. Route all major sampling dimensions through a shared sampler API.
5. Assign fixed dimensions for camera, BSDF, light, environment, RR, ReSTIR candidates, and denoiser stochastic taps.
6. Add debug views for sample dimensions and scrambling.

Acceptance criteria:

- Static scenes converge with visibly less structured noise.
- Temporal noise does not shimmer during camera motion.
- ReSTIR and path tracing do not accidentally reuse correlated dimensions.

Implementation notes:

- Added a shared shader sampling API in `blue_noise.glsl` with fixed dimension IDs, Sobol-style low-discrepancy values, Owen-style hash scrambling, and procedural spatiotemporal blue-noise rotation.
- Routed path-tracing seed setup, direct-light sampling, environment debug samples, BSDF sampling, dielectric decisions, and Russian roulette through fixed dimension streams.
- Routed ReSTIR RIS candidate selection and reservoir acceptance through the shared sampler.
- Routed ReSTIR spatial neighbor offsets through the shared sampler using frame-indexed dimensions.
- Added sample-dimension and sample-scramble debug views to the renderer enum, parser, editor combo, and raygen debug output.
- Kept CPU-provided TAA jitter as the camera ray offset so motion vectors and reprojection remain consistent with existing uniforms.
- Verified Debug build, beauty smoke, sample-dimension smoke, sample-scramble smoke, and a ReSTIR smoke run.
- Added `RTV_USE_DIMENSIONED_SAMPLER=0` as a shader compile toggle for comparing against the cheaper hash-based dimension fallback. The shader compiler now tracks shader option signatures so define changes force SPIR-V recompilation.
- Local 150-frame Debug Cornell smoke at frame 120: dimensioned sampler on `path=8.56 ms`, fallback sampler `path=8.47 ms`. Treat this as a quick spot check rather than a final benchmark.

### Phase 18: Denoiser Shared Memory Optimization [DONE]

Target files:

- `shaders/denoiser.comp`

Steps:

1. Profile current denoiser global-memory reads.
2. Tile neighborhood data into shared memory.
3. Add halo loading for filter radius.
4. Avoid bank conflicts where practical.
5. Keep fallback path behind a shader define until validated.

Acceptance criteria:

- 1-3 ms GPU improvement on denoiser-heavy resolutions.
- Output matches the baseline within expected floating-point tolerance.
- No out-of-bounds reads at image edges.

Implementation notes:

- Added `RTV_DENOISER_SHARED_TILE` as a shader define with a global-memory fallback path.
- Tiles the first 5x5 A-trous pass into 20x20 workgroup shared memory for color and packed depth/normal reads.
- Keeps later wider-step A-trous passes on the original global-memory path to avoid oversized shared-memory tiles.
- Handles partial workgroups by loading shared tiles before the in-bounds return so barriers remain valid at image edges.
- Verified Debug build and a 5-frame Debug smoke run with the beauty view.
- `RTV_DENOISER_SHARED_TILE=0` can compile the original global-memory path for A/B profiling.
- Local 150-frame Debug Cornell smoke at frame 120: shared-tile denoiser `denoise=0.258 ms`, global-memory fallback `denoise=0.240 ms`, so this scene/GPU currently favors the fallback by a small margin.

### Phase 11: Dynamic Quality Manager [DONE]

Target files:

- `include/rtv/RendererSettings.h`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `include/rtv/GpuProfiler.h`
- `src/rtv/GpuProfiler.cpp`

Steps:

1. Add frame-time target and quality state.
2. Track smoothed GPU frame time, CPU frame time, queue wait time, and camera/editor motion state.
3. Pull timings from `GpuProfiler` rather than wall-clock estimates so path tracing, denoising, TAA/TSR, tone map, ReSTIR, and UI cost can be controlled independently.
4. Dynamically adjust internal resolution, samples per pixel, max bounces, environment direct samples, denoiser passes, ReSTIR reuse count, and expensive debug features.
5. Add hysteresis to prevent oscillation: separate upscale/downscale thresholds, minimum dwell time, and cooldown after scene edits.
6. Expose override modes: off, conservative, balanced, aggressive.
7. Persist the selected mode in editor preferences or scene render settings.

Acceptance criteria:

- 30-50 percent GPU reduction during camera motion.
- Static camera returns to full quality after a stable delay.
- No visible resolution pumping during minor mouse movement.

Implementation notes:

- Added a default-off adaptive quality mode with conservative, balanced, and aggressive policies.
- Persisted adaptive mode and GPU frame-time target in scene render settings.
- Uses `GpuProfiler` frame timings plus camera/still-frame state to reduce effective max bounces, environment samples, and denoiser A-trous iterations while moving.
- Keeps render resolution fixed in this phase to avoid visible pumping before the later TSR/internal-resolution phases.
- Static frames recover to full settings after a deterministic stable-frame delay.
- Exposed adaptive quality mode and GPU frame target in the render settings panel.
- Verified Debug build, a default-off 5-frame smoke run, and an aggressive adaptive-quality validation smoke run.

### Phase 12: Motion-Adaptive Pass Skipping [DONE]

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/TemporalSystem.cpp`
- `shaders/denoiser.comp`

Steps:

1. Define pass skip policy for camera motion and scene edits.
2. Skip or simplify denoiser, expensive ReSTIR reuse, high-cost debug views, and optional post passes while moving.
3. Preserve mandatory history invalidation and output presentation.
4. Add profiler markers showing skipped passes.

Acceptance criteria:

- Motion responsiveness improves without stale-frame artifacts.
- Static quality recovers deterministically.

Implementation notes:

- Balanced/aggressive adaptive quality can skip ReSTIR spatial reuse while moving.
- Aggressive adaptive quality can skip the denoiser while moving; history-copy fallback still updates required temporal resources and presentation.
- Validation pass markers record adaptive skips for ReSTIR spatial and denoiser paths.
- Verified through the aggressive adaptive-quality validation smoke run.

## Batch 4: Temporal Super Resolution

Goal: split internal render resolution from display resolution and convert TAA into TSR.

### Phase 19: Add Display Extent And Split Resources [DONE]

Target files:

- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `shaders/temporal_common.glsl`

Steps:

1. Add separate `renderExtent` and `displayExtent`.
2. Allocate accumulation, G-buffer, velocity, variance, and path state at render extent.
3. Allocate presentation, UI composite, and final tone-map targets at display extent.
4. Audit every dispatch size and image barrier.
5. Add debug overlay showing both extents and scale ratio.

Acceptance criteria:

- 1.0 scale produces identical output to the pre-TSR renderer.
- Non-1.0 scale does not cause out-of-bounds reads/writes.

Implementation notes:

- Added explicit renderer `renderExtent` and `displayExtent`.
- Path tracing, accumulation, variance, G-buffer, velocity, ReSTIR, picking, and denoiser history remain render-resolution resources.
- Presentation LDR image is allocated at display extent; Phase 20 moves TSR output/history to display extent.
- Tone mapping dispatches at display extent and samples the render-resolution HDR input with normalized coordinates to avoid out-of-bounds reads at non-1.0 scale.
- Editor viewport now tracks display extent for the presented texture while still exposing render extent for diagnostics.
- Selection outline is skipped when render and display extents differ; display-space outline alignment remains a later upgrade.
- Verified Debug build plus native-scale and 0.5-scale smoke runs.

### Phase 20: Convert TAA To TSR [DONE]

Target files:

- `shaders/taa.comp`
- `shaders/temporal_common.glsl`
- `src/rtv/TemporalSystem.cpp`

Steps:

1. Convert TAA resolve to output display-resolution pixels.
2. Reproject display pixels into render-resolution history.
3. Add jitter-aware reconstruction filter.
4. Add disocclusion rejection, reactive mask support, and variance-aware clamping.
5. Preserve camera-cut reset behavior.

Acceptance criteria:

- Edges resolve over multiple frames instead of blurring.
- Disocclusions do not drag history.
- Static 0.67x render scale approaches native quality after accumulation.

Implementation notes:

- TAA now resolves display-resolution pixels while sampling render-resolution color, depth/normal, and velocity inputs.
- The shader reconstructs current color with a clamped 3x3 render-resolution filter and keeps all history reads/writes at display resolution.
- Render-space velocity is scaled into display pixels before reprojection, so TSR history remains stable when render scale changes.
- History clipping uses the existing neighborhood min/max clamp with luminance sigma/reactive checks for disocclusion stability.
- Camera-cut and history-valid resets are preserved through the existing TAA uniform path.
- Verified native-scale TAA smoke and 0.5-scale TSR smoke.

### Phase 21: Update C++ Pipeline For TSR [DONE]

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/RenderGraph.cpp`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Add render scale setting and quality presets.
2. Wire TSR pass resources into the render graph.
3. Recreate render-resolution resources when scale changes.
4. Keep display-resolution resources tied to swapchain size.
5. Reset or validate history on scale changes.

Acceptance criteria:

- Runtime render-scale changes are stable.
- No stale descriptors after resource recreation.
- GPU profiler attributes cost to render-resolution and display-resolution passes separately.

Implementation notes:

- `PathTracerRenderer::beginFrame` now receives render and display extents; render-resolution resources are recreated when the internal scale changes.
- TAA/TSR output and history images are allocated at display extent, while denoiser, velocity, G-buffer, and path-tracing resources stay at render extent.
- The render graph binds the TSR pass against display-sized output/history resources and dispatches it at display extent.
- Render settings expose TSR presets (`Native`, `Quality`, `Balanced`, `Performance`) plus the existing raw render-resolution scale slider.
- Existing profiler ranges continue to isolate render-resolution passes such as path trace/denoiser from display-resolution post passes such as TAA/TSR and tone map.
- Verified runtime resource recreation through 0.5-scale validation scene smoke.

### Phase 22: Update Tone Map And Presentation [DONE]

Target files:

- `shaders/tone_map.comp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/ViewportPanel.cpp`

Steps:

1. Ensure tone mapping consumes display-resolution TSR output.
2. Ensure viewport presentation and screenshots use display extent.
3. Keep luminance histogram resolution policy explicit.
4. Validate UI overlays and selection outline alignment.

Acceptance criteria:

- Presentation is sharp at native display resolution.
- Selection outline and picking coordinates match displayed pixels.
- Screenshots capture the final display-resolution image.

Implementation notes:

- Tone mapping consumes the current post-temporal HDR source; with TSR enabled that source is display-resolution TSR output.
- Tone mapping dispatches at display extent and samples HDR input by normalized coordinates, so native and sub-native render scales share the same presentation path.
- Viewport presentation uses the display extent for texture validation, viewport fitting, and HUD diagnostics.
- Auto-exposure histogram policy is explicit: histogram sampling and exposure reduction use the current post-temporal HDR source extent.
- Picking maps viewport UVs into the render-resolution entity buffer; selection outline remains enabled only when render and display extents match to avoid misaligned display-space outlines until the outline pass is upgraded.
- No separate screenshot capture path is present in the current codebase; the final presentation image is display-resolution.
- Verified 0.5-scale TSR plus auto-exposure smoke run.

### Phase 49: Async Compute Overlap

Target files:

- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`
- `include/rtv/CommandSystem.h`
- `src/rtv/CommandSystem.cpp`
- `include/rtv/RenderGraph.h`
- `src/rtv/RenderGraph.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/GpuProfiler.cpp`

Queue partition targets:

| Work | Preferred queue | Dependency | Notes |
|------|-----------------|------------|-------|
| Path tracing RT dispatch | Graphics/ray tracing | TLAS, descriptors, accumulation state | Producer for denoiser and temporal passes |
| Denoiser | Compute | Path tracing output, G-buffer/path data | First async candidate |
| TAA/TSR | Compute | Denoiser or raw color, velocity, histories | Async candidate after TSR graph is explicit |
| Luminance histogram | Compute | TSR or pre-tone-map HDR | Can overlap with some UI/graphics work |
| Exposure reduce | Compute | Histogram | Short pass; include only if synchronization overhead is lower than saved time |
| Tone map | Compute | Exposure, HDR input | Last async candidate before presentation |
| UI composite/presentation | Graphics | Tone-map output | Must wait for compute completion |

Steps:

1. Profile the post-path-trace workload first: denoiser, TAA/TSR, tone map, luminance histogram, exposure reduce, selection outline, sky reprojection, and any pure compute debug passes.
2. Add compute queue discovery in `VulkanContext`: prefer same-family compute-capable queue first, then separate async compute queue if available.
3. Expose `computeQueue`, `computeQueueFamilyIndex`, and capability flags.
4. Add timeline semaphore support for cross-queue dependencies.
5. Extend `CommandSystem` to allocate and submit compute command buffers independently from graphics/ray-tracing command buffers.
6. Extend `RenderGraph` pass metadata with queue domain: graphics, ray tracing, compute, transfer, or same-family compute.
7. Teach the graph compiler to partition post-path-trace compute passes after path tracing and before presentation.
8. Use same-family queue submissions without queue-family ownership transfers when graphics and compute share the same family.
9. Use `VkImageMemoryBarrier2` and `VkBufferMemoryBarrier2` with queue ownership transfers only for true cross-family queues.
10. Start with denoiser plus tone map overlap; add TAA/TSR, histogram, and exposure only after the first overlap path validates.
11. Add `--disable-async-compute` and `--single-queue-fallback` flags for deterministic debugging.
12. Add profiler lanes for graphics, ray tracing, compute, and queue wait time.
13. Compare against a single-queue baseline on the same scene, resolution, and quality settings.

Acceptance criteria:

- Denoiser/TAA/TSR/tone-map compute work overlaps with ray tracing or graphics work where hardware supports it.
- Total frame time improves by at least 10 percent on a GPU-bound scene, or the capture documents why overlap is unavailable.
- Target savings are tracked against the original 1.6-4.2 ms / 10-26 percent goal.
- `--single-queue-fallback` produces equivalent output to async mode under fixed RNG seed.
- Vulkan synchronization validation is clean with both same-family and cross-family queue paths.
- Unsupported devices fall back to the existing single-queue path.

Implementation notes:

- Compute queue discovery now scans all queue families and keeps a compute-capable queue available when the device exposes one.
- `VulkanContext` exposes compute queue, queue family, timeline semaphore, and capability helpers for async-compute preparation.
- Timeline semaphore support is now queried before device creation; devices without the feature skip timeline semaphore creation and remain on the single-queue fallback path.
- `CommandSystem` accepts `--disable-async-compute` and `--single-queue-fallback` runtime options through `main`/`Application`.
- Per-frame compute command pools and command buffers are allocated when async compute is enabled and a timeline semaphore is available.
- `CommandSystem` now has explicit graphics and compute submit helpers; graphics submits can signal the timeline semaphore, and compute submits can wait/signal timeline values once split command recording is enabled.
- `CommandSystem` now asks the renderer to record async compute work into the per-frame compute command buffer and submits it only when the renderer reports recorded work; the current renderer keeps returning false until a pass is safely split out of the graphics command buffer.
- `RenderGraphPass` now carries explicit queue-domain metadata, and `RenderGraph::hasAsyncCompute()` now reflects actual queue/semaphore wiring instead of transient-resource availability.
- The render graph compiler now infers queue domains for compiled passes, groups contiguous queue-domain segments, and logs pass/segment queue assignments for validation.
- Render graph barriers now retain before/after queue domains and can emit queue-family ownership transfers when a future async execution path supplies distinct graphics and compute queue families.
- GPU timing data now exposes graphics-lane, compute-lane, and queue-wait fields in profiler panels and the viewport HUD; queue wait remains zero until timeline submit/wait points are wired.
- This phase is not complete yet: moving real passes into async compute recording, active cross-queue execution, queue-family ownership-transfer validation, real queue-overlap profiling, and validation captures still need to be implemented before marking Phase 49 done.

## Batch 5: Denoiser Production Quality

Goal: move denoising from generic color filtering toward path-aware diffuse/specular temporal filtering.

### Phase 23: Expose Path Data [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/denoiser.comp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Add output buffers/images for direct diffuse, direct specular, indirect diffuse, indirect specular, albedo, roughness, hit distance, and confidence.
2. Write path data in the path tracer with clear units and resolution.
3. Update descriptor layouts and render graph resources.
4. Add debug views for each channel.
5. Keep old single-color denoiser path behind a compatibility flag.

Acceptance criteria:

- Debug channels match expected scene content.
- No significant path tracing cost increase beyond the added writes.
- Denoiser can consume the new channels without changing visual output yet.

Implementation notes:

- Added a compact path-data storage buffer with direct diffuse, direct specular, indirect diffuse, indirect specular, albedo, roughness, hit distance, and confidence metadata.
- Path tracing writes the path-data buffer at render resolution, and both the render graph and denoiser descriptor layouts now declare the buffer dependency.
- Added debug views for path direct diffuse/specular, path indirect diffuse/specular, path albedo, and path metrics.
- Verified Debug build plus beauty, path-direct-diffuse, path-direct-specular, path-indirect-diffuse, path-data-albedo, and path-data-metrics smoke runs.

### Phase 24: Roughness-Aware Kernel Sizing [DONE]

Target files:

- `shaders/denoiser.comp`

Steps:

1. Use roughness and normal variance to choose filter radius.
2. Keep glossy/low-roughness reflections on smaller kernels.
3. Use larger kernels on diffuse/high-roughness regions.
4. Add debug view for chosen radius.

Acceptance criteria:

- Sharp specular detail is preserved better.
- Diffuse noise still reduces effectively.

Implementation notes:

- The denoiser now derives a per-pixel spatial filter radius from first-hit roughness and variance, keeping low-roughness pixels on the smaller kernel while allowing noisy/high-roughness regions to use the wider kernel.
- Added the `denoiser-kernel-radius` debug view and exposed it through CLI parsing and the editor debug view list.
- Verified Debug build plus a `denoiser-kernel-radius` smoke run.

### Phase 25: Hit-Distance Filtering

Target files:

- `shaders/denoiser.comp`
- `shaders/temporal_common.glsl`

Steps:

1. Store primary and secondary hit distance from path tracing.
2. Normalize hit distance into a stable range.
3. Use hit distance to adapt spatial and temporal filter radius.
4. Reject history when hit-distance mismatch exceeds threshold.

Acceptance criteria:

- Near-field detail is preserved.
- Distant diffuse regions denoise more aggressively.
- Disocclusion halos are reduced.

### Phase 26: Virtual Motion For Specular

Target files:

- `shaders/denoiser.comp`
- `shaders/temporal_common.glsl`
- `src/rtv/TemporalSystem.cpp`

Steps:

1. Estimate reflected hit point or virtual surface position for specular paths.
2. Compute virtual motion vector for specular history reprojection.
3. Fall back to surface motion when confidence is low.
4. Add debug view for specular virtual velocity.

Acceptance criteria:

- Specular ghosting is reduced during camera motion.
- Low-confidence areas do not smear or explode.

### Phase 27: Split Diffuse And Specular Histories

Target files:

- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `shaders/denoiser.comp`
- `shaders/taa.comp`

Steps:

1. Allocate separate diffuse and specular temporal histories.
2. Apply channel-specific validation and clamping.
3. Recombine after denoising and before tone mapping.
4. Add debug views for history length and confidence per channel.

Acceptance criteria:

- Diffuse converges smoothly without over-blurring specular.
- Specular rejects history more aggressively when needed.
- Memory impact is measured and documented.

### Phase 28: Anti-Flicker For Emissives

Target files:

- `shaders/denoiser.comp`
- `shaders/pathtrace.rgen`

Steps:

1. Mark emissive contributions in path data.
2. Detect high-intensity small-area emissive samples.
3. Clamp temporally using luminance history and confidence.
4. Avoid suppressing legitimate lighting changes.

Acceptance criteria:

- Small emissive objects flicker less.
- Moving emissive objects do not leave long trails.

## Batch 6: ReSTIR DI Fixes

Goal: remove bias and leaking from direct-light ReSTIR before building ReSTIR GI.

### Phase 31: Pairwise MIS For Temporal ReSTIR

Target files:

- `shaders/restir_common.glsl`
- `shaders/restir_temporal.comp`
- `shaders/restir_final.comp`

Steps:

1. Audit temporal reservoir reuse math.
2. Store all fields needed for pairwise MIS: target function, source PDF, sample count, confidence, and visibility state.
3. Implement pairwise MIS weighting for temporal candidates.
4. Reject incompatible reservoirs using depth, normal, material, and motion confidence.
5. Validate with moving camera and changing light intensity.

Acceptance criteria:

- Temporal ReSTIR mean matches non-temporal reference over enough frames.
- No persistent over-bright or under-bright bias after camera motion.

### Phase 32: Visibility Reuse For Spatial ReSTIR

Target files:

- `shaders/restir_spatial.comp`
- `shaders/restir_final.comp`
- `shaders/pathtrace_shadow.*`

Steps:

1. Track whether reservoir visibility is known, unknown, or invalid.
2. Reuse visibility only when receiver and occluder assumptions are compatible.
3. Cast validation rays for reused samples when required.
4. Reject spatial samples across large depth/normal discontinuities.

Acceptance criteria:

- Light leaking across walls is eliminated or significantly reduced.
- Spatial reuse still reduces variance in open areas.

### Phase 33: Improve Light BVH SAH Quality

Target files:

- `include/rtv/LightBvh.h`
- `src/rtv/LightBvh.cpp`
- `shaders/restir_common.glsl`

Steps:

1. Profile current light BVH build and traversal quality.
2. Improve SAH splitting or binning.
3. Store tighter bounds and better power estimates.
4. Add debug stats: tree depth, leaf count, traversal count, selected light distribution.

Acceptance criteria:

- Light selection variance improves in many-light scenes.
- Build cost remains acceptable for scene edits.

## Batch 7: ReSTIR GI

Goal: add reservoir-based indirect illumination reuse after DI ReSTIR is correct.

### Phase 34: ReSTIR GI Reservoir Struct

Target files:

- `shaders/restir_common.glsl`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Define GI reservoir layout around the 80-byte per-pixel target.
2. Include hit point, normal, radiance, target PDF, weight sum, sample count, age, and flags.
3. Allocate triple-buffered reservoir storage.
4. Add clear and debug-view passes.

Acceptance criteria:

- Reservoir buffers allocate and clear correctly.
- Debug view can inspect reservoir validity and age.

### Phase 35: ReSTIR GI Initial Sampling

Target files:

- `shaders/pathtrace.rgen`
- `shaders/restir_gi_init.comp`

Steps:

1. Generate candidate secondary hits from BSDF sampling.
2. Evaluate target function at candidate hit points.
3. Store selected candidate in the GI reservoir.
4. Keep regular path tracing contribution available for comparison.

Acceptance criteria:

- Initial GI reservoirs match one-bounce indirect lighting distribution.
- Reservoir disabled/enabled comparison has matching mean over time.

### Phase 36: ReSTIR GI Temporal Reuse

Target files:

- `shaders/restir_gi_temporal.comp`
- `shaders/temporal_common.glsl`

Steps:

1. Reproject previous GI reservoirs using hit-point reprojection.
2. Validate using depth, normal, material, motion, and hit-distance confidence.
3. Merge current and previous reservoirs with correct weights.
4. Age out stale reservoirs.

Acceptance criteria:

- Temporal reuse reduces indirect noise.
- Camera motion does not create persistent ghosted GI.

### Phase 37: ReSTIR GI Spatial Reuse

Target files:

- `shaders/restir_gi_spatial.comp`
- `shaders/pathtrace_shadow.*`

Steps:

1. Select blue-noise spatial neighbors.
2. Validate compatible surfaces.
3. Cast visibility rays for reused indirect samples where required.
4. Merge reservoirs with correct target-function evaluation at the current receiver.

Acceptance criteria:

- Indirect light does not leak through thin walls.
- Spatial reuse visibly reduces low-frequency noise.

### Phase 38: ReSTIR GI Final Shading

Target files:

- `shaders/restir_gi_final.comp`
- `shaders/pathtrace.rgen`

Steps:

1. Convert GI reservoir result into final indirect contribution.
2. Combine with regular path tracing or replace selected bounce path according to mode.
3. Apply generalized MIS consistently.
4. Add debug toggles for GI initial, temporal, spatial, and final contribution.

Acceptance criteria:

- Final GI contribution is unbiased relative to path tracing reference.
- Debug modes isolate each reuse stage.

### Phase 39: ReSTIR GI Tuning

Target files:

- `shaders/restir_*.comp`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Tune neighbor count, temporal age, confidence thresholds, and visibility ray budget.
2. Add half-resolution mode.
3. Identify candidate reservoir fields for later Phase 50 compression, but do not change reservoir layout in this tuning phase.
4. Profile memory bandwidth and GPU time.
5. Record pre-compression reservoir memory footprint so Phase 50 has a reliable baseline.

Acceptance criteria:

- ReSTIR GI has a documented quality/performance preset table.
- Half-resolution mode is stable and does not shimmer excessively.

## Batch 8: Materials And Textures

Goal: improve BRDF fidelity and texture correctness before architecture v2.

### Phase 40: Oren-Nayar Diffuse

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Add Oren-Nayar diffuse evaluation.
2. Map material roughness to diffuse roughness.
3. Preserve Lambert fallback for low roughness or performance mode.
4. Ensure PDF remains cosine-weighted unless sampling changes.

Acceptance criteria:

- Rough diffuse surfaces look less plastic.
- Energy remains bounded.

### Phase 41: Conductor Fresnel

Target files:

- `shaders/rt_common.glsl`
- `include/rtv/SceneComponents.h`
- `src/rtv/MaterialEditorPanel.cpp`

Steps:

1. Add conductor Fresnel using eta/k parameters.
2. Provide presets for common metals.
3. Map existing metallic materials to approximate conductor parameters.
4. Keep artist-friendly base-color metallic path as fallback.

Acceptance criteria:

- Gold, copper, aluminum, and silver presets render plausibly.
- Existing metallic assets do not break.

### Phase 42: Wire glTF Extensions

Target files:

- `src/rtv/GltfLoader.cpp`
- `include/rtv/MeshAsset.h`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `shaders/rt_common.glsl`

Steps:

1. Add import support for selected glTF material extensions handled by existing or near-term closures: `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_specular`, `KHR_materials_sheen`, `KHR_materials_emissive_strength`, and `KHR_texture_transform`.
2. Do not implement `KHR_materials_anisotropy` shading in this phase; parse and store its raw fields only if needed so Phase 43 can consume them.
3. Store extension data in CPU material structs with explicit `hasExtension` flags and default values.
4. Convert supported extension data to GPU material fields or closure fields.
5. Add asset-level diagnostics for unsupported extensions.
6. Add import tests or sample assets for each supported extension.

Acceptance criteria:

- Extension-heavy glTF test assets load with expected material properties.
- Unsupported extensions produce explicit warnings, not silent wrong materials.

### Phase 43: Anisotropic GGX

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add tangent/bitangent orientation support if not already reliable.
2. Implement anisotropic GGX eval and VNDF sampling.
3. Use glTF anisotropy extension fields.
4. Add brushed metal validation scene.

Acceptance criteria:

- Anisotropic highlights rotate correctly with tangent direction.
- Isotropic materials remain unchanged.

### Phase 44: Occlusion Texture

Target files:

- `src/rtv/GltfLoader.cpp`
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Import glTF occlusion texture and strength.
2. Bind texture through material texture indices.
3. Apply occlusion to indirect diffuse only, not direct lighting.
4. Add debug view for AO texture.

Acceptance criteria:

- AO maps visibly affect creases without darkening direct light incorrectly.
- Missing AO maps default to neutral.

### Phase 45: Specular AA / Toksvig

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rchit`

Steps:

1. Estimate normal-map variance from mip level or derivatives where available.
2. Increase effective roughness for high-frequency normal variation.
3. Clamp only the specular lobe, not material base roughness.
4. Add setting to compare on/off.

Acceptance criteria:

- Normal-mapped glossy surfaces shimmer less.
- Reflections are not globally over-blurred.

### Phase 47: Fix KTX2 Mipmap Loading

Target files:

- `src/rtv/TextureLoader.cpp`
- `include/rtv/TextureLoader.h`

Steps:

1. Audit KTX2 mip level enumeration and upload offsets.
2. Preserve all mip levels and correct row/slice layout.
3. Ensure image view and sampler use full mip range.
4. Validate with a colored-mip test texture.

Acceptance criteria:

- Correct mip appears at each distance.
- Compressed mip upload passes validation.

### Phase 48: 16-Bit And HDR Textures

Target files:

- `src/rtv/TextureLoader.cpp`
- `include/rtv/TextureAsset.h`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add loader support for 16-bit UNORM, 16F, and HDR formats where relevant.
2. Choose Vulkan formats by texture semantic: normal, emissive, color, data.
3. Preserve color-space handling.
4. Add memory budget warnings for high-precision textures.

Acceptance criteria:

- HDR emissive and high-precision normal textures load correctly.
- Color management remains correct for sRGB textures.

### Phase 55: Sheen And Thin-Film Sampling

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add sheen lobe evaluation and sampling.
2. Add thin-film interference approximation for applicable materials.
3. Integrate lobe selection probabilities into BSDF sampling.
4. Validate MIS weights for multi-lobe sampling.

Acceptance criteria:

- Sheen materials converge without excessive variance.
- Thin-film effect is stable under camera motion.

## Batch 9: Architecture Hardening

Goal: prepare renderer infrastructure for OMM, wavefront, SER, and long-running editor sessions.

### Phase 50: Reservoir Compression

Target files:

- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`
- `shaders/restir_temporal.comp`
- `shaders/restir_spatial.comp`
- `shaders/restir_final.comp`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/PathTracerRenderer.h`

Compression targets:

| Field group | Baseline representation | Candidate representation | Risk |
|-------------|-------------------------|--------------------------|------|
| Metadata | Multiple uint/float fields | Packed `uvec4` bitfields | Low |
| Normal | `vec3` fp32 | Octahedral 2x16-bit | Low-Medium |
| Hit distance / position | `vec3` fp32 world position | fp16 distance plus local basis or quantized offset | Medium |
| Radiance/sample value | `vec3` fp32 | fp16 RGB or RGBE | Medium |
| Target PDF / weight sum | fp32 | fp16 only if bias tests pass | High |
| Confidence / age / flags | fp32/uint | 8-16 bit packed integers | Low |

Steps:

1. Measure the current reservoir footprint at target resolutions.
2. Use the working target from the source roadmap as the baseline: DI reservoirs about 285 MB and GI reservoirs about 475 MB, roughly 760 MB total before compression at high resolution/triple buffering.
3. Inventory all reservoir fields: sample id, light id, hit position, normal, radiance/sample value, target PDF, weight sum, sample count `M`, confidence, age, flags, and visibility state.
4. Split reservoir data into hot fields used every pass and cold/debug fields used only for validation.
5. Pack metadata into `uvec4` or narrower integer fields: light index, pixel/sample id, age, flags, visibility state, and sample count.
6. Quantize normals using octahedral encoding into 2x16-bit or 2x10-bit fields depending on visual error.
7. Quantize hit distance or hit position relative to primary surface where possible instead of storing full `vec3` world position.
8. Store radiance/sample value in `f16vec3` or packed RGBE when the error budget is acceptable.
9. Store target PDF and weight sum in fp16 only after validating no bias or instability; otherwise keep fp32 for those fields.
10. Add versioned reservoir structs: uncompressed debug layout and compressed production layout.
11. Add conversion shaders or compile-time switch so A/B testing can run both layouts on the same scene.
12. Update ReSTIR DI and GI passes to read/write the compressed layout consistently.
13. Add debug views that decode and display compressed fields: age, confidence, M, light id, normal, hit distance, and validity.
14. Add GPU memory accounting for DI, GI, previous, current, spatial, and scratch reservoir buffers.

Acceptance criteria:

- Reservoir memory is reduced by at least 40 percent from the uncompressed baseline.
- High-resolution target memory moves materially below the original ~760 MB reservoir footprint.
- Mean lighting matches the uncompressed layout within a documented tolerance.
- No extra temporal flicker from quantized confidence, normal, hit distance, or weight fields.
- Debug mode can switch back to uncompressed reservoirs for validation.

### Phase 51: Memory Aliasing For Temporal And Reservoir Buffers

Target files:

- `include/rtv/RenderGraph.h`
- `src/rtv/RenderGraph.cpp`
- `include/rtv/RenderGraphResource.h`
- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`
- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Aliasing targets:

| Resource group | Candidate aliasing | Expected saving | Prerequisite |
|----------------|--------------------|-----------------|--------------|
| `worldPositionBuffer_` / `previousWorldPositionBuffer_` | Temporal ping-pong alias when previous is not read after current write | ~32 MB | Exact temporal pass lifetimes |
| ReSTIR DI current/previous/spatial scratch | Alias non-overlapping temporal/spatial buffers | Part of ~285 MB | Phase 50 layout inventory |
| ReSTIR GI current/previous/spatial scratch | Alias non-overlapping GI buffers | Part of ~285 MB | Phase 39 final pass schedule |
| TSR intermediate HDR/output buffers | Alias pre/post resolve intermediates | Scene/resolution dependent | Render graph lifetime analysis |
| Denoiser intermediate ping-pong buffers | Alias separable filter temporaries | Scene/resolution dependent | Denoiser pass declarations |

Steps:

1. Inventory temporal swap resources and reservoir resources with their exact frame lifetimes.
2. Specifically evaluate aliasing for `worldPositionBuffer_` and `previousWorldPositionBuffer_`, with the original memory target of about 32 MB saved when lifetimes do not overlap.
3. Specifically evaluate aliasing for the three reservoir buffers, with the original memory target of about 285 MB saved when DI/GI ping-pong or scratch lifetimes do not overlap.
4. Add lifetime intervals to render graph resources: first writer, last reader, queue domain, access mask, image layout, and frame index.
5. Add an aliasing eligibility check: equal or compatible size, format, usage flags, sample count, tiling, queue ownership, and no overlapping lifetime.
6. Add alias groups for temporal current/previous resources only when the graph proves they are not both read in the same pass.
7. Add alias groups for reservoir current/previous/spatial/scratch resources only after ReSTIR temporal and spatial passes declare exact reads and writes.
8. Use VMA allocation aliasing or explicit placed allocation strategy supported by the existing allocator abstraction.
9. Add resource debug names that include alias group id and active logical resource name for GPU captures.
10. Add a debug mode that disables aliasing at runtime for A/B validation.
11. Add validation assertions that a logical resource is not accessed outside its lifetime interval.
12. Add graph visualization or log output showing which resources alias and estimated memory saved.

Acceptance criteria:

- Temporal world-position aliasing saves the expected ~32 MB when enabled.
- Reservoir aliasing saves the expected ~285 MB where the final ReSTIR schedule permits it.
- No validation errors from layout, access, or queue ownership hazards.
- `--disable-resource-aliasing` produces visually equivalent output under fixed RNG seed.
- GPU captures show clear physical allocation reuse and logical resource names.

### Phase 52: Async Entity Picking

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/EditorSelection.h`
- `src/rtv/EditorSelection.cpp`
- `src/rtv/ViewportPanel.cpp`

Steps:

1. Replace synchronous pick readback with a per-frame readback ring.
2. Submit pick request with frame index and screen coordinate.
3. Read result only after the corresponding fence signals.
4. Return pending state to the editor instead of blocking.
5. Keep last valid selection visible while a request is pending.

Acceptance criteria:

- Click selection never calls `vkDeviceWaitIdle`.
- Selection result latency is acceptable, usually 1-3 frames.
- No stale result is applied after the scene changes incompatibly.

### Phase 53: Replace `vkDeviceWaitIdle`

Target files:

- `src/rtv/CommandSystem.cpp`
- `src/rtv/GpuScene.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/VulkanContext.cpp`
- `src/rtv/UiOverlay.cpp`

Steps:

1. Inventory every `vkDeviceWaitIdle` call and classify it: resize, upload, scene buffer replacement, environment load, picking, shutdown, or debug-only.
2. Replace per-frame/editor interactions with fences, timeline semaphores, deferred deletion, or per-resource lifetime tracking.
3. Keep device idle only for shutdown and unavoidable full device teardown.
4. Add debug logging when any device-idle path is hit.

Acceptance criteria:

- Normal editing, picking, environment load, and resize do not stall the whole device.
- Shutdown remains safe.
- GPU profiler shows fewer long gaps.

### Phase 54: VMA Budget Control And Descriptor Pool Tuning

Target files:

- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`
- `include/rtv/DescriptorAllocator.h`
- `src/rtv/DescriptorAllocator.cpp`
- `include/rtv/DescriptorLayoutCache.h`
- `src/rtv/DescriptorLayoutCache.cpp`
- `include/rtv/UiOverlay.h`
- `src/rtv/UiOverlay.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`

Budget categories:

| Category | Examples | Required diagnostics |
|----------|----------|----------------------|
| Acceleration structures | BLAS, TLAS, scratch | build/update peak, resident size |
| Textures | base color, normal, emissive, HDR env | format, mip count, compressed/uncompressed size |
| Temporal histories | TAA/TSR, denoiser, velocity, depth, normal | render/display extent, precision, history count |
| ReSTIR reservoirs | DI, GI, previous/current/spatial/scratch | compressed size, alias group, resolution |
| Render graph transients | intermediate images/buffers | lifetime, alias group, physical allocation |
| Wavefront queues | ray, hit, shadow, compaction, sort | capacity, high-water mark |
| Staging/upload | texture and buffer staging | transient peak and lifetime |
| Descriptors | renderer, bindless, UI, temporal, ReSTIR | pool size, allocated sets, failures |

Steps:

1. Enable `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` when `VK_EXT_memory_budget` is available.
2. Query heap budget, heap usage, allocation count, and block fragmentation through VMA each frame or at a throttled diagnostics rate.
3. Add memory-budget categories: acceleration structures, textures, temporal histories, reservoirs, render graph transients, wavefront queues, staging, UI, and miscellaneous buffers.
4. Add high-water marks and per-frame delta tracking for GPU memory.
5. Add warning thresholds at 70, 85, and 95 percent of reported budget.
6. Add emergency quality-budget hooks: reduce render scale, disable optional histories, reduce reservoir resolution, reduce denoiser history precision, and reject new large texture uploads.
7. Audit descriptor pool sizing for renderer, bindless resources, ImGui/UI, temporal passes, ReSTIR, and wavefront.
8. Replace fixed descriptor pool guesses with pool sizing derived from pass/resource counts where practical.
9. Add descriptor pool usage stats: allocated sets, free sets, pool count, failed allocations, fragmentation, and peak use.
10. Add descriptor pool growth policy with a hard cap and explicit error reporting.
11. Add descriptor lifetime validation for hot reload, scene reload, swapchain recreation, and renderer shutdown.
12. Add diagnostics UI or log output for VMA budget and descriptor pool state.

Acceptance criteria:

- VMA budget reporting is active on devices supporting `VK_EXT_memory_budget`.
- Memory pressure warnings appear before allocation failure.
- Descriptor pools no longer exhaust during long editor sessions, scene reloads, or material hot reload.
- Descriptor pool sizes are documented and tied to renderer feature counts.
- No descriptor leaks are observed after repeated scene load/unload cycles.

## Batch 10: Opacity Micromaps

Goal: reduce any-hit cost for alpha-tested foliage on RTX 40+ hardware while preserving fallback paths.

### Phase 56: Extension Detection And Feature Query

Target files:

- `src/rtv/VulkanContext.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/RayTracingScene.cpp`

Steps:

1. Query opacity micromap extension support and feature structs.
2. Expose renderer capability flags.
3. Add settings UI with disabled reason when unsupported.
4. Ensure non-NVIDIA or pre-RTX-40 devices use fallback alpha any-hit path.

Acceptance criteria:

- Capability detection is correct and visible.
- Unsupported devices run unchanged.

### Phase 57: Mark Alpha-Tested Geometry

Target files:

- `src/rtv/GltfLoader.cpp`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `include/rtv/MeshAsset.h`
- `include/rtv/RayTracingScene.h`

Steps:

1. Add `containsAlphaTestedGeometry` or equivalent mesh/material flag.
2. Propagate alpha mode and cutoff from glTF to GPU scene.
3. Split flags by primitive where needed.
4. Add debug stats for opaque, alpha-tested, and blended geometry counts.

Acceptance criteria:

- Alpha-tested foliage is identified correctly.
- Opaque geometry is not incorrectly routed through OMM.

### Phase 58: Generate OMM Data From Alpha Textures

Target files:

- `src/rtv/TextureLoader.cpp`
- `src/rtv/RayTracingScene.cpp`
- new OMM preprocessing helper files if needed

Steps:

1. Sample alpha textures at the required subdivision level.
2. Classify micro-triangles as opaque, transparent, unknown, or mixed.
3. Build micromap arrays and usage counts.
4. Cache OMM data per mesh/material/alpha texture.
5. Add CPU-side validation and debug visualization.

Acceptance criteria:

- Generated OMM data matches alpha cutoff visually.
- Preprocessing time and memory are measured.

### Phase 59: Integrate OMM Into BLAS Builds

Target files:

- `src/rtv/AccelerationStructure.cpp`
- `src/rtv/RayTracingScene.cpp`

Steps:

1. Attach OMM descriptors to eligible BLAS geometry.
2. Use extension build structs in the BLAS build chain.
3. Preserve fallback BLAS build path.
4. Validate build flags and memory requirements.

Acceptance criteria:

- BLAS builds succeed with OMM enabled.
- Any-hit invocation count drops on foliage scenes.

### Phase 60: Geometry Splitting

Target files:

- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `src/rtv/RayTracingScene.cpp`
- `shaders/pathtrace.rahit`

Steps:

1. Split opaque, alpha-tested, and blended geometry into separate BLAS geometry groups.
2. Use OMM only for alpha-tested geometry.
3. Keep blended geometry on existing any-hit path if needed.
4. Profile traversal and any-hit counts.

Acceptance criteria:

- Foliage shadow rays improve by 2-4x on supported hardware.
- Any-hit invocations drop by 50-80 percent.
- Fallback path remains correct.

## Batch 11: Wavefront Path Tracing

Goal: fork the renderer into a queue-based architecture where compute handles generation/shading and thin ray tracing pipelines handle hardware traversal.

Wavefront work is architecture v2. Do not start until the megakernel path tracer, temporal system, denoiser, ReSTIR DI/GI, materials, bindless descriptors, and render graph hardening are stable.

### Phase 61: Expand Wavefront Common Structs

Target files:

- `shaders/wavefront_common.glsl`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Define `WavefrontRay`, `WavefrontHit`, `WavefrontShadowRay`, and `WavefrontPixelState`.
2. Include radiance, throughput, RNG state, MIS state, path depth, atmosphere transmittance, material id, instance id, primitive id, barycentrics, and flags.
3. Define queue headers with atomic counters and capacity.
4. Define dynamic queue sizing based on render extent and max path depth.
5. Add buffer allocation and clear passes.

Acceptance criteria:

- Struct layouts match between C++ and GLSL.
- Queue counters clear and increment correctly in a test shader.

### Phase 62: Camera Ray Generation Compute

Target files:

- new `shaders/wavefront_generate.comp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Generate primary camera rays in compute.
2. Initialize per-pixel state.
3. Append rays into the ray queue.
4. Support jitter, depth of field placeholders, and camera cut reset.

Acceptance criteria:

- Generated ray directions match the megakernel camera path.
- Queue count equals active render pixels.

### Phase 63: Thin Ray Trace Wrapper

Target files:

- new `shaders/wavefront_trace.rgen`
- update `shaders/pathtrace.rchit`
- update `shaders/pathtrace.rahit`
- update `shaders/pathtrace.rmiss`
- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Read rays from the wavefront ray queue.
2. Call `traceRayEXT`.
3. Write closest-hit, any-hit alpha decision, and miss result into the hit queue.
4. Keep payload minimal.
5. Preserve megakernel shaders through separate entry points or compile definitions.

Acceptance criteria:

- Primary hit/miss data matches the megakernel path for the same camera.
- No shading is performed in the trace wrapper except required alpha handling.

### Phase 64: Wavefront Shade Compute

Target files:

- new `shaders/wavefront_shade.comp`
- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`

Steps:

1. Read hit queue entries.
2. Decode material and textures.
3. Evaluate direct lighting and enqueue shadow rays.
4. Sample BSDF and enqueue next-bounce rays.
5. Apply Russian roulette.
6. Update per-pixel radiance, throughput, MIS state, and path state.
7. Handle miss/environment contribution.

Acceptance criteria:

- One-sample output statistically matches megakernel output.
- Bounce depth and path termination stats match expected distribution.

### Phase 65: Dedicated Shadow Trace

Target files:

- new `shaders/wavefront_shadow_trace.rgen`
- `shaders/pathtrace_shadow.*`

Steps:

1. Trace queued shadow rays with terminate-on-first-hit flags.
2. Write visibility/occlusion result into per-pixel state or shadow result buffer.
3. Support alpha-tested geometry and OMM fallback.
4. Add debug view for shadow queue occupancy.

Acceptance criteria:

- Direct lighting visibility matches megakernel path.
- Shadow queue cost is separately profiled.

### Phase 66: Queue Compaction

Target files:

- new `shaders/wavefront_compact.comp`

Steps:

1. Remove terminated or invalid rays from queues.
2. Compact live rays into next queue buffer.
3. Preserve mapping to pixel state.
4. Add prefix-sum or atomic append implementation, whichever profiles better first.

Acceptance criteria:

- Queue count decreases as paths terminate.
- No live ray is dropped or duplicated.

### Phase 67: Frame Allocators For Queue Memory

Target files:

- `include/rtv/FrameResources.h`
- `src/rtv/FrameResources.cpp`
- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`

Steps:

1. Add arena allocator for per-frame wavefront queues and scratch buffers.
2. Allocate all queue memory from frame-local arenas.
3. Free all wavefront transient memory at frame end after fence completion.
4. Track high-water marks.

Acceptance criteria:

- No per-frame heap churn.
- Queue memory high-water mark is visible.

### Phase 68: Synchronization And Barriers

Target files:

- `src/rtv/RenderGraph.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Model generate, compact, sort, trace, shadow trace, shade, and write passes in the render graph.
2. Add buffer barriers between queue producers and consumers.
3. Add image barriers for final output.
4. Validate each bounce iteration.
5. Add deterministic single-queue fallback.

Acceptance criteria:

- Vulkan synchronization validation is clean.
- Output is deterministic under fixed RNG seed and single-queue fallback.

### Phase 69: ReSTIR Integration

Target files:

- `shaders/wavefront_shade.comp`
- `shaders/restir_*.comp`

Steps:

1. Move temporal reuse access into shade stage for bounce 0.
2. Preserve reservoir update order.
3. Ensure DI and GI reservoirs are indexed by pixel state.
4. Validate against megakernel ReSTIR output.

Acceptance criteria:

- ReSTIR debug views work in wavefront mode.
- Mean lighting matches megakernel mode.

### Phase 70: Debug View Adaptation

Target files:

- `src/rtv/RendererDebug.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `shaders/wavefront_write.comp`

Steps:

1. List all existing debug views.
2. Map each to wavefront state, hit queues, or final write outputs.
3. Add queue-specific debug views: occupancy, path depth, live rays, terminated rays, material bucket.
4. Disable unsupported debug views with clear UI text until implemented.

Acceptance criteria:

- Existing core debug views work in wavefront mode.
- Queue-specific debugging is sufficient to diagnose stuck paths.

### Phase 71: Material And Ray Sorting

Target files:

- new `shaders/wavefront_sort.comp`
- `shaders/wavefront_common.glsl`

Steps:

1. Add sort keys by material type, roughness bucket, hit distance bucket, and ray type.
2. Implement a simple GPU radix or bucket sort.
3. Sort hit queue before shading.
4. Measure occupancy and shader divergence.

Acceptance criteria:

- Divergent material scenes improve.
- Sorting overhead does not exceed saved shading time.

### Phase 72: Queue Memory Bandwidth Optimization

Target files:

- `shaders/wavefront_common.glsl`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Compress queue fields where safe.
2. Use half precision for selected radiance/throughput/state fields only after error testing.
3. Add half-resolution queue mode for selected secondary effects.
4. Reduce redundant per-ray data by moving persistent fields to pixel state.

Acceptance criteria:

- Queue bandwidth drops measurably.
- Visual error is documented and acceptable.

### Phase 73: Dynamic Work Stealing And Queue Balancing

Target files:

- `shaders/wavefront_*.comp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Track queue occupancy per stage and bounce.
2. Adjust dispatch sizes based on live queue counts.
3. Add work stealing or persistent-thread strategy only after simple dispatch sizing is profiled.
4. Avoid starving shadow or shade queues.

Acceptance criteria:

- GPU occupancy improves on divergent scenes.
- No queue starvation or long-tail frame spikes.

### Phase 74: Validation And Profiling

Target files:

- `src/rtv/ValidationSceneSuite.cpp`
- `src/rtv/GpuProfiler.cpp`
- docs and test assets as needed

Steps:

1. Add side-by-side megakernel versus wavefront validation mode.
2. Record path length, queue occupancy, occupancy estimates, bandwidth, and pass timings.
3. Capture representative scenes.
4. Document regressions and decide whether wavefront becomes default or remains opt-in.

Acceptance criteria:

- Wavefront improves divergent scenes by 20-40 percent or has documented blockers.
- Correctness is statistically equivalent to megakernel mode.
- All debug and temporal systems work in wavefront mode.

## Batch 12: Shader Execution Reordering

Goal: enable SER after wavefront exposes large reorderable batches.

### Phase 75: Query Invocation Reorder Depth

Target files:

- `src/rtv/VulkanContext.cpp`
- `include/rtv/VulkanContext.h`

Steps:

1. Query SER-related NVIDIA extension support.
2. Query `maxRayTracingInvocationReorderDepth`.
3. Expose capability to renderer settings/debug UI.
4. Disable SER path on unsupported hardware.

Acceptance criteria:

- Capability reporting is correct.
- Unsupported devices run unchanged.

### Phase 76: Full SER With Wavefront

Target files:

- `shaders/wavefront_trace.rgen`
- `shaders/wavefront_shade.comp`
- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Identify stage boundaries where large batches can be reordered.
2. Add SER hints/grouping by hit distance, material type, and ray type.
3. Use `reorderThreadNV()` where appropriate.
4. Profile with and without sorting to avoid redundant work.
5. Keep feature flag and GPU capability guard.

Acceptance criteria:

- Divergent scenes improve by the expected 20-40 percent on supported hardware.
- SER does not change image output beyond floating-point noise.

### Phase 77: Pipeline Flag For Reorder Capture

Target files:

- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Add `VK_PIPELINE_CREATE_RAY_TRACING_SHADER_GROUP_HANDLE_CAPTURE_REORDER_BIT_NV` when SER is enabled and supported.
2. Keep fallback pipeline creation path.
3. Validate pipeline cache compatibility.

Acceptance criteria:

- SER pipeline creates successfully on supported hardware.
- Non-SER pipeline remains valid on all devices.

## Batch 13: Advanced Rendering Features

Goal: add high-impact features after the renderer architecture is stable.

### Phase 78: Depth Of Field

Target files:

- `include/rtv/PhysicalCamera.h`
- `src/rtv/PhysicalCamera.cpp`
- `shaders/pathtrace.rgen`
- `shaders/wavefront_generate.comp`

Steps:

1. Add aperture radius, focus distance, blade count, and bokeh settings.
2. Sample thin lens in camera ray generation.
3. Adjust ray differentials or sampling dimensions.
4. Add focus picking or UI controls.

Acceptance criteria:

- Defocus blur is physically plausible.
- Focus plane remains sharp.
- Works in megakernel and wavefront modes if both are still supported.

### Phase 79: Motion Blur

Target files:

- `include/rtv/SceneComponents.h`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `src/rtv/RayTracingScene.cpp`
- `shaders/pathtrace.rgen`

Steps:

1. Store previous and current transforms per instance.
2. Add shutter open/close settings.
3. Sample ray time per camera ray.
4. Build or update acceleration structures with motion support where available.
5. Validate per-instance motion and camera motion separately.

Acceptance criteria:

- Moving objects blur correctly.
- Static objects remain sharp.
- Temporal systems receive correct velocity/history data.

### Phase 80: Volumetric Path Tracing

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- wavefront shaders if enabled
- new volume data structures as needed

Steps:

1. Add homogeneous medium support first.
2. Add phase function sampling.
3. Add transmittance tracking in path state.
4. Add volume direct lighting and shadow transmittance.
5. Later extend to heterogeneous grids.

Acceptance criteria:

- Homogeneous fog renders with correct transmittance.
- Volumetric shadows are stable.
- Path throughput remains unbiased.

### Phase 81: Caustics And MNEE

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- wavefront shade if enabled

Steps:

1. Identify SDS path cases currently missing or high variance.
2. Implement manifold next-event estimation for selected specular paths.
3. Add robust Jacobian and visibility handling.
4. Keep feature behind a quality setting.
5. Validate with glass-caustic reference scenes.

Acceptance criteria:

- Caustics converge faster than baseline path tracing.
- No catastrophic fireflies or biased energy gain.

## Detailed Execution Runbooks

This section defines how to execute the phase plan in practice. The phase sections above define what to build. The runbooks below define the exact operating procedure for baselining, implementation, validation, and handoff.

### Repository Preflight

Run this before starting any batch:

1. Confirm the working tree state and identify unrelated user changes.
2. Build Debug.
3. Build Release.
4. Run the renderer for a short smoke test with the default scene.
5. Run a 120-frame static accumulation test.
6. Run a 120-frame camera-motion test.
7. Capture baseline GPU timings for path trace, denoiser, TAA/TSR, ReSTIR, tone map, histogram, and UI.
8. Capture baseline VRAM usage by category if diagnostics already exist.
9. Capture at least one baseline screenshot per validation scene.
10. Save the renderer settings used for the baseline.
11. Record GPU, driver version, resolution, render scale, max bounces, samples per pixel, denoiser mode, ReSTIR mode, and debug flags.
12. Ensure Vulkan validation is enabled for Debug validation runs.
13. Ensure shader debug names and resource names appear in GPU captures.
14. Disable adaptive quality for correctness baselines unless the phase explicitly tests adaptive quality.
15. Pin random seed for A/B comparisons when the phase affects sampling.

Suggested local commands:

```powershell
cmake --build build --config Debug
cmake --build build --config Release
.\build\Debug\rtvulkan.exe --frames 120
.\build\Release\rtvulkan.exe --frames 120
```

Adjust executable paths if the active build directory differs.

### Per-Phase Work Item Template

Every phase should produce a small implementation record with these fields:

| Field | Required content |
|-------|------------------|
| Phase | Number and title |
| Owner branch | Branch or changelist name |
| Feature flag | Runtime setting, shader define, or reason no flag is needed |
| Files touched | Exact files changed |
| Baseline artifact | Screenshot, capture, timing table, or metric snapshot |
| Implementation notes | Design decisions and rejected alternatives |
| Validation artifacts | Screenshots, GPU timings, validation output, memory stats |
| Rollback plan | Flag disable, commit revert, or fallback path |
| Follow-up debt | Known limitations not blocking phase exit |

Phase implementation sequence:

1. Create or select a feature flag before changing behavior.
2. Add debug/profiler counters first.
3. Add data layout changes with old behavior still active.
4. Add shader changes behind the flag.
5. Add C++ descriptor/resource wiring.
6. Run a single-frame smoke test.
7. Run fixed-seed A/B output comparison.
8. Run validation scenes.
9. Capture GPU timings and memory deltas.
10. Enable by default only if acceptance criteria pass.
11. Leave a setting to disable the feature until the next batch is stable.
12. Document any deviation from this plan in the phase record.

### Feature Flag Policy

Use feature flags to isolate renderer-risk changes:

| Change type | Required flag type |
|-------------|--------------------|
| Shader math correctness fix | Debug comparison flag until validated |
| Sampling strategy change | Runtime renderer setting and fixed-seed A/B mode |
| Temporal history change | Runtime setting plus history reset path |
| Memory layout change | Compile-time layout define plus runtime debug fallback when practical |
| Queue/synchronization change | Runtime setting with single-queue fallback |
| Hardware-specific feature | Capability flag plus explicit unsupported-device fallback |
| Architecture fork | Renderer mode selection, for example megakernel versus wavefront |

Naming convention:

1. Renderer settings use `enableX`.
2. Shader defines use `RTV_ENABLE_X`.
3. Debug fallbacks use `--disable-x` command-line flags.
4. Architecture modes use positive selection such as `--renderer-mode wavefront`.
5. Temporary A/B switches must be removed or moved to debug-only UI after the next batch stabilizes.

### Baseline Metrics Schema

Record this table before and after every batch:

| Metric | Unit | Notes |
|--------|------|-------|
| Total GPU frame time | ms | Median, p95, p99 over 120 frames |
| CPU frame time | ms | Median, p95, p99 over 120 frames |
| Path trace pass | ms | Include RT dispatch only |
| Denoiser pass | ms | Split temporal/spatial if possible |
| TAA/TSR pass | ms | Include resolve/reconstruction |
| ReSTIR DI | ms | Init, temporal, spatial, final |
| ReSTIR GI | ms | Init, temporal, spatial, final |
| Tone map and exposure | ms | Histogram, exposure reduce, tone map |
| Queue wait time | ms | Required after Phase 49 |
| VRAM total | MB | VMA reported usage where available |
| Reservoir memory | MB | Required after Phase 34 |
| Temporal history memory | MB | Required after Phase 19 |
| Descriptor sets allocated | count | Required after Phase 54 |
| Any-hit invocations | count | Required for alpha scenes |
| Average path length | bounces | Required for RR and wavefront work |
| Live wavefront queue high-water | rays | Required after Phase 61 |

### Batch 1 Runbook: Critical Fixes

Execution order:

1. Build a fixed-seed baseline for emissive, sun, glossy, and RIS scenes.
2. Implement Phase 2 before Phase 14 so delta classification is correct before VNDF sampling.
3. Implement Phase 15 immediately after Phase 2 because roughness floor and delta classification interact.
4. Implement Phase 3 with a white furnace comparison before touching emissive continuation.
5. Implement Phase 1 and verify emissive continuation does not double-count direct emissive hits.
6. Implement Phase 5 after Phase 1 because sun disk MIS depends on previous-event PDF state.
7. Implement Phase 6 after Phase 5 because RIS effective PDF must participate in the same MIS path.
8. Implement Phase 4 after MIS fixes so Russian roulette validation uses the final throughput logic.
9. Implement Phase 46 last because it is independent and easy to disable.
10. Run all Batch 1 validation scenes with fixed seed and then with normal stochastic sampling.

Batch 1 validation details:

1. Cornell emissive mean luminance must not decrease after emissive continuation.
2. A mirror looking at an emissive surface must not become over-bright.
3. Roughness ramp must show delta behavior only below `0.001`.
4. White furnace and metallic furnace must remain energy bounded.
5. Sun glints must not double-brighten when both BSDF and light sampling can sample the sun.
6. RIS candidate count changes must affect variance but not long-run mean.
7. `indirect_strength` changes must not change path survival statistics.
8. Anisotropic filtering must be disabled cleanly on unsupported devices.

### Batch 2 Runbook: Quick Wins

Execution order:

1. Implement Phase 7 first to stop long frame delta spikes from contaminating later responsiveness measurements.
2. Implement Phase 8 and run resource-lifetime validation for three frames in flight.
3. Implement Phase 9 and confirm picking work is absent from normal frames.
4. Implement Phase 10 and compare normal debug view before/after.
5. Implement Phase 14 only after Phase 2 is merged and validated.
6. Implement Phase 16 after Phase 14 so GGX eval and sampling remain paired.
7. Implement Phase 17 and re-run furnace tests.
8. Implement Phase 29 and Phase 30 together because environment sample count and RR both affect variance/performance tradeoffs.
9. Re-profile static and moving scenes.

Batch 2 validation details:

1. Camera motion after a breakpoint or window drag must not jump.
2. Three frames in flight must not corrupt accumulation or descriptor updates.
3. Picking requests must produce the same entity id as the previous per-frame path.
4. Non-uniform scale normal debug view must remain correct.
5. VNDF sampling must preserve mean brightness versus old GGX after enough samples.
6. RR tuning must reduce average path length without luminance bias.

### Batch 3 Runbook: Core Performance

Execution order:

1. Add sampling-dimension documentation before implementing Phase 13.
2. Implement Sobol/Owen/STBN behind a sampler-mode setting.
3. Reserve fixed dimensions for camera jitter, lens, BSDF lobe, BSDF direction, light selection, environment selection, ReSTIR candidates, RR, and denoiser stochastic taps.
4. Implement Phase 18 shared-memory denoiser as a shader variant.
5. Implement Phase 11 dynamic quality with adaptive behavior disabled by default.
6. Implement Phase 12 pass skipping only after Phase 11 exposes motion state and quality state.
7. Profile each feature independently and then together.

Batch 3 validation details:

1. Sample dimension reuse must be visible in debug output.
2. STBN must reduce structured noise without creating temporal shimmer.
3. Shared-memory denoiser must match baseline output within tolerance.
4. Dynamic quality must not oscillate under small mouse movement.
5. Pass skipping must never skip mandatory history invalidation.

### Batch 4 Runbook: TSR And Async Compute

Execution order:

1. Implement Phase 19 resource split with render scale fixed to 1.0.
2. Validate that 1.0 render scale is identical to the pre-split renderer.
3. Implement Phase 20 TSR reconstruction with a native-scale compatibility mode.
4. Implement Phase 21 C++ pipeline wiring and render-scale UI.
5. Implement Phase 22 presentation, tone map, screenshot, and selection alignment.
6. Build a pass dependency table for all post-path-trace compute work.
7. Implement Phase 49 same-family compute submission first.
8. Add cross-family ownership transfers only after same-family async is correct.
9. Add timeline semaphore profiling and single-queue fallback.
10. Run all TSR validation before enabling async compute by default.

Batch 4 validation details:

1. Render scale 1.0 must match baseline.
2. Render scale changes must recreate resources without stale descriptors.
3. Selection outline and picking coordinates must match displayed pixels.
4. TSR history must reset correctly on camera cut and scale change.
5. Async compute must produce equivalent output to single-queue fallback.
6. GPU captures must show actual overlap or explain why hardware scheduling prevents it.

### Batch 5 Runbook: Production Denoiser

Execution order:

1. [DONE] Implement Phase 23 path data outputs without changing denoiser behavior.
2. [DONE] Add debug views for every path data channel.
3. [DONE] Implement Phase 24 roughness-aware kernel sizing.
4. Implement Phase 25 hit-distance filtering.
5. Implement Phase 26 virtual motion for specular behind a conservative flag.
6. Implement Phase 27 split histories only after diffuse/specular channel confidence is reliable.
7. Implement Phase 28 emissive anti-flicker last because it depends on stable path data classification.
8. Re-run temporal motion tests after each phase.

Batch 5 validation details:

1. Path data channels must be explainable for every validation scene.
2. Specular highlights must not smear after roughness-aware filtering.
3. Hit-distance filtering must reduce halos at disocclusions.
4. Virtual specular motion must improve moving-camera glossy scenes.
5. Split histories must not exceed memory budget without Phase 50/51 mitigation notes.
6. Emissive anti-flicker must not suppress real animated emissive changes.

### Batch 6 Runbook: ReSTIR DI Fixes

Execution order:

1. Freeze a non-ReSTIR path tracing reference for many-light scenes.
2. Implement Phase 31 pairwise MIS with debug mode showing candidate weights.
3. Validate temporal reservoir reuse with camera still, camera moving, and light intensity changing.
4. Implement Phase 32 visibility reuse with conservative rejection first.
5. Loosen spatial reuse only after light leaks are eliminated.
6. Implement Phase 33 Light BVH improvements after DI estimator correctness is stable.
7. Compare variance, mean, and GPU cost against pre-batch ReSTIR.

Batch 6 validation details:

1. Pairwise MIS must remove temporal brightness bias.
2. Visibility reuse must not leak light through walls.
3. Light BVH changes must affect variance and traversal counts, not mean brightness.
4. Reservoir debug views must remain valid after every pass.

### Batch 7 Runbook: ReSTIR GI

Execution order:

1. Implement Phase 34 GI reservoir layout and clear/debug passes only.
2. Add memory accounting before generating any GI samples.
3. Implement Phase 35 initial sampling and compare to one-bounce path tracing.
4. Implement Phase 36 temporal reuse with strict rejection thresholds.
5. Implement Phase 37 spatial reuse with visibility rays enabled by default.
6. Implement Phase 38 final shading with side-by-side path tracing reference mode.
7. Implement Phase 39 tuning only after correctness is stable.
8. Record reservoir memory footprint for Phase 50 compression.

Batch 7 validation details:

1. Initial reservoirs must match bounce-1 distribution.
2. Temporal reuse must reduce noise without ghosted GI.
3. Spatial reuse must not leak through walls or across strong normal/depth discontinuities.
4. Final shading must be unbiased relative to reference.
5. Half-resolution tuning must have documented quality loss.

### Batch 8 Runbook: Materials And Textures

Execution order:

1. Add material validation scenes before changing BRDF code.
2. Implement Phase 40 Oren-Nayar with Lambert fallback.
3. Implement Phase 41 conductor Fresnel with preset values and legacy fallback.
4. Implement Phase 42 glTF extension import without anisotropic shading.
5. Implement Phase 43 anisotropic GGX after tangent basis validation.
6. Implement Phase 44 occlusion texture and verify it affects indirect diffuse only.
7. Implement Phase 45 specular AA as a roughness adjustment, not a material mutation.
8. Implement Phase 47 KTX2 mip fix with colored-mip validation.
9. Implement Phase 48 HDR/16-bit texture support with color-space tests.
10. Implement Phase 55 sheen/thin-film sampling after multi-lobe MIS is stable.

Batch 8 validation details:

1. Legacy materials must render acceptably after every material change.
2. Extension-heavy glTF assets must report unsupported extensions explicitly.
3. Anisotropic highlights must rotate with tangents.
4. AO must not incorrectly darken direct light.
5. KTX2 mips must display the expected colored mip at distance.
6. HDR emissive textures must not clamp unexpectedly.
7. Multi-lobe sampling must preserve energy.

### Batch 9 Runbook: Memory And Stall Hardening

Execution order:

1. Implement Phase 50 after ReSTIR GI has final reservoir semantics.
2. Keep uncompressed reservoirs as a debug layout until Phase 51 passes.
3. Implement Phase 51 aliasing with aliasing disabled by default.
4. Enable aliasing per resource group, starting with the lowest-risk temporal intermediates.
5. Implement Phase 52 async picking before Phase 53 so picking-specific waits have a replacement path.
6. Implement Phase 53 by replacing one `vkDeviceWaitIdle` class at a time.
7. Implement Phase 54 after memory categories are final enough to budget.
8. Run long editor-session tests with repeated scene loads, material edits, environment loads, resizes, and picking.

Batch 9 validation details:

1. Reservoir compression must reduce memory without mean-lighting bias.
2. Aliasing must be provably lifetime-safe in graph logs and GPU captures.
3. Async picking must never block the device.
4. Normal editing flows must not hit `vkDeviceWaitIdle`.
5. VMA budget warnings must appear before allocation failure.
6. Descriptor pools must survive repeated hot reload and scene reload.

### Batch 10 Runbook: OMM

Execution order:

1. Implement Phase 56 feature detection and leave OMM disabled by default.
2. Implement Phase 57 alpha-tested geometry classification.
3. Validate classification on foliage, cutout decals, opaque meshes, and blended meshes.
4. Implement Phase 58 OMM preprocessing with CPU-side debug visualization.
5. Implement Phase 59 BLAS build integration with fallback build path.
6. Implement Phase 60 geometry splitting after BLAS integration works.
7. Profile any-hit invocation count and shadow ray cost on foliage scenes.

Batch 10 validation details:

1. Unsupported hardware must behave exactly like the fallback path.
2. OMM alpha classification must match the alpha cutoff.
3. BLAS builds must remain valid after scene reload and material edits.
4. Any-hit invocations should drop by 50-80 percent on foliage.
5. Foliage shadow rays should improve by the expected 2-4x where supported.

### Batch 11 Runbook: Wavefront

Execution order:

1. Create a separate renderer mode for wavefront and keep megakernel as reference.
2. Implement Phase 61 layouts and C++/GLSL layout tests before any tracing.
3. Implement Phase 62 primary generation and validate queue counts.
4. Implement Phase 63 trace wrapper and compare hit/miss buffers to megakernel.
5. Implement Phase 64 shade compute for one bounce before multiple bounces.
6. Implement Phase 65 shadow trace and validate direct lighting.
7. Implement Phase 66 compaction with debug counters.
8. Implement Phase 67 frame allocator and memory high-water stats.
9. Implement Phase 68 synchronization after all early stages exist.
10. Implement Phase 69 ReSTIR integration only after base wavefront shading is correct.
11. Implement Phase 70 debug views before optimization phases.
12. Implement Phase 71 sorting, Phase 72 bandwidth optimization, and Phase 73 work balancing only after correctness is locked.
13. Implement Phase 74 validation and decide default renderer mode based on data.

Batch 11 validation details:

1. Wavefront primary rays must match megakernel rays.
2. Wavefront hit buffers must match megakernel payload outputs.
3. One-bounce wavefront output must match megakernel statistically.
4. Multi-bounce path length distribution must match megakernel.
5. Queue counters must never exceed capacity.
6. Fixed-seed single-queue wavefront must be deterministic.
7. Debug views must diagnose queue occupancy, material buckets, path depth, and terminated rays.
8. Divergent scenes should improve by 20-40 percent or blockers must be documented.

### Batch 12 Runbook: SER

Execution order:

1. Implement Phase 75 capability query and UI reporting.
2. Add SER pipeline creation flags behind capability checks.
3. Implement Phase 77 pipeline flag in a no-op behavior mode first.
4. Implement Phase 76 reordering hints in wavefront mode only.
5. Profile with sorting disabled, sorting enabled, SER disabled, and SER enabled.
6. Keep unsupported hardware on the exact same wavefront path without SER.

Batch 12 validation details:

1. SER unsupported devices must not create SER pipeline structs.
2. SER output must match non-SER wavefront output within floating-point noise.
3. Reordering must improve divergent scenes enough to justify complexity.
4. SER must not conflict with wavefront sorting or debug captures.

### Batch 13 Runbook: Advanced Features

Execution order:

1. Implement Phase 78 depth of field in camera ray generation with deterministic lens sampling.
2. Validate DoF in megakernel and wavefront if both are active.
3. Implement Phase 79 motion blur after per-instance previous/current transform infrastructure is stable.
4. Validate motion blur independently from temporal velocity buffers.
5. Implement Phase 80 homogeneous volumes before heterogeneous volumes.
6. Implement Phase 81 MNEE only after specular/refraction material paths are stable.
7. Keep every advanced feature behind quality settings.

Batch 13 validation details:

1. DoF focus plane must remain sharp.
2. Motion blur must not blur static objects.
3. Volume transmittance must match analytic homogeneous-medium expectations.
4. MNEE caustics must improve convergence without introducing persistent fireflies.

### Phase Handoff Checklist

Before a phase is considered complete:

1. The feature has a clear enabled/disabled state.
2. The fallback path still works.
3. Debug UI or logs expose the new state.
4. GPU resources have debug names.
5. Validation scenes pass.
6. GPU timings are recorded.
7. Memory deltas are recorded.
8. Known limitations are written into the phase record.
9. Any new shader layout is documented in C++ and GLSL.
10. Any new descriptor binding is reflected in layout creation and update code.
11. Any new temporal history has reset, resize, and camera-cut behavior.
12. Any new queue submission has a single-queue fallback.

### Regression Triage Procedure

Use this order when a phase fails validation:

1. Re-run with the phase feature flag disabled.
2. Re-run with fixed seed.
3. Re-run at one sample per pixel and one bounce if the failure is shading-related.
4. Re-run with temporal accumulation disabled if the failure is ghosting/flicker.
5. Re-run with ReSTIR disabled if the failure is direct or indirect lighting bias.
6. Re-run with denoiser disabled if the failure is blur, haloing, or trails.
7. Re-run with async compute disabled if the failure is nondeterministic.
8. Re-run with resource aliasing disabled if the failure is corruption.
9. Compare debug views for depth, normal, world position, roughness, velocity, reservoir age, and reservoir confidence.
10. Take a GPU capture only after the smallest failing configuration is identified.

## Dependency Graph Summary

```text
Batch 1 critical fixes
  -> Batch 2 quick wins
  -> Batch 3 core performance
  -> Batch 4 TSR + async compute overlap
  -> Batch 5 denoiser
  -> Batch 6 ReSTIR DI
  -> Batch 7 ReSTIR GI
  -> Batch 8 materials/textures
  -> Batch 9 memory/stall hardening
  -> Batch 10 OMM
  -> Batch 11 wavefront
  -> Batch 12 SER
  -> Batch 13 advanced features
```

Parallel-safe groups:

- Batch 1 phases are mostly parallel, but Phase 2 should land before Phase 14.
- Batch 2 phases can run in parallel except Phase 11 depends on Phase 8 and Phase 12 depends on Phase 11.
- Batch 4 phases 19-22 are sequential; Phase 49 begins after the TSR pass graph and post stack are explicit enough to partition by queue.
- Batch 5 phases 24-28 depend on Phase 23.
- Batch 7 is sequential because each ReSTIR GI stage depends on the previous reservoir semantics.
- Batch 9 order is Phase 50 before Phase 51 for ReSTIR resources, Phase 52 before Phase 53 for picking stalls, and Phase 54 after Phases 50-51 so budget control sees final memory categories.
- Batch 10 is sequential because OMM build integration depends on capability detection, classification, and data generation.
- Batch 11 is mostly sequential through Phases 61-64, then Phases 65, 67, 69, and 70 can branch.

## Validation Scene Matrix

Use these scenes for every batch:

| Scene | Purpose |
|-------|---------|
| Cornell emissive | Emissive continuation, MIS, denoising stability |
| Mirror/glossy roughness ramp | Delta threshold, VNDF, roughness floor, specular denoising |
| Outdoor HDR + sun | Sun MIS, environment sampling, TSR, exposure |
| Foliage alpha scene | Alpha any-hit, OMM, shadow performance |
| Sponza | Full scene stress test, texture filtering, ReSTIR, material system |
| Many-light scene | Light BVH, ReSTIR DI/GI |
| Moving camera/object scene | Velocity, TSR, denoiser, motion blur readiness |

## Metrics To Track

Record these after each batch:

- GPU frame time at native resolution.
- GPU frame time at TSR quality scale.
- Path tracing pass time.
- Denoiser pass time.
- ReSTIR pass time.
- Tone-map/presentation time.
- Async compute overlap time, queue wait time, and single-queue fallback delta after Phase 49.
- Accumulation stability over 120 static frames.
- Mean luminance against reference.
- Number of validation-layer warnings/errors.
- VRAM usage by category: images, buffers, histories, descriptors, acceleration structures, reservoirs, wavefront queues, staging, render graph transients.
- Reservoir memory before/after compression and aliasing after Phases 50-51.
- VMA heap budget, heap usage, allocation count, and high-water marks after Phase 54.
- Descriptor pool usage, growth count, failed allocations, and high-water marks after Phase 54.
- Any-hit invocation count for alpha scenes.
- Wavefront queue occupancy once available.

## Stop Conditions

Stop the current phase and fix before continuing if any of these occur:

- A correctness change shifts mean luminance without a documented estimator reason.
- Vulkan validation reports synchronization or descriptor errors.
- A temporal change introduces persistent ghosting in validation scenes.
- A performance optimization worsens GPU time by more than 5 percent in common scenes.
- A new architecture feature requires rewriting an unstable earlier subsystem.
- A fallback path is broken on unsupported hardware.
