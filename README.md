# VBuffer Animated Sail

A DirectX 12 deferred-rendering experiment with two GPU-animated sails, a compact effects V-buffer, tiled compute classification, and indirect per-effect dispatch.

The sample keeps material rasterization independent from special effects: the sail pixel shader writes ordinary albedo and surface data, while later GPU passes classify visible effect IDs and apply signed additive changes to the G-buffer. Dear ImGui provides live wind, cloth, effect, and buffer-inspection controls.

## Highlights

- GPU vertex-shader sail deformation with wind, gust, tension, billow, and sag controls
- Two instanced sails with independently selectable effects
- Deferred albedo, normal/material, depth, and effects V-buffer outputs
- GPU-built 8x8 tile lists with no CPU readback
- Separate compute PSO and indirect dispatch for each effect
- Iridescent wind, frost crystal, and storm-lightning effects
- Procedural cloth texture and smiley design with no runtime assets
- Debug views for every G-buffer and V-buffer channel
- Self-contained embedded HLSL and pinned Dear ImGui dependency

## Pass and resource flow

All HLSL is embedded in `src/shaders.h` and compiled with `D3DCompile` at shader model 5.1.

1. **Geometry/G-buffer pass**
   - `SailVS` instances and deforms the static 48x36 sail grid twice, with a small phase variation, and computes animated view-space normals analytically.
   - `SailGBufferPS` samples the procedural cloth texture and adds a large, centered smiley face directly to ordinary base albedo. Its gold face, dark eyes, and curved smile are generated from separate non-repeating 0..1 sail UVs, derivative/smoothstep anti-aliased, and blended so the cloth weave remains visible. It performs no lighting or V-buffer special-effect math.
   - The mast uses `StaticSceneVS` and `MastGBufferPS` and writes the same G-buffer layout.
   - Outputs:
     - Base albedo: `R8G8B8A8_UNORM`
     - Encoded view-space normal/material: `R16G16B16A16_FLOAT`; RGB is `normal * 0.5 + 0.5`, A is material ID (`1` sail, `2` mast)
     - Effects V-buffer: `R32G32B32A32_UINT`
     - Shared depth: `R32_TYPELESS`, viewed as `D32_FLOAT` for depth and `R32_FLOAT` for sampling

2. **Per-effect tile classification**
   - Three classifier PSOs dispatch one 8x8 compute group per screen tile, one classifier for each supported nonzero effect ID.
   - A group-shared flag ensures each matching tile is appended once to that effect's own GPU `RWStructuredBuffer<uint>` tile list.
   - Each effect also owns a separate three-uint indirect argument buffer. An atomic increment writes dispatch X; `FinalizeIndirectArgs` sets Y and Z to one.
   - All three counters are cleared every frame. UAV barriers order list/counter writes and argument finalization before each argument buffer transitions to `INDIRECT_ARGUMENT`.
   - There is no CPU readback. Three `ExecuteIndirect` calls target three separate effect PSOs; effects with empty lists dispatch zero groups.

3. **Effect compute**
   - **1 - Iridescent wind sheen:** `IridescentWindSheenCS` combines view-normal/direction facing, logical UV, wind, and time into moving cyan/magenta/gold bands with both positive highlights and negative cloth-color variation.
   - **2 - Frost crystal:** `FrostCrystalCS` advances an animated noisy crystallization front through logical UV, using decoded barycentric triangle edges and billow-normal response for icy buildup, bright facets, and dark gaps.
   - **3 - Storm lightning:** `StormLightningCS` seeds animated branching bolts from logical UV, primitive ID, and barycentrics, producing a blue-white core, triangle veins, and a dark signed halo.
   - Output is a separate `R16G16B16A16_FLOAT` signed-delta UAV. It never modifies the G-buffer in place and never multiplies effect logic into the base material shader.
   - The delta target is cleared once. Effect IDs are exclusive per pixel, and every shader validates its ID before writing, so the separate dispatches cannot race. The base cloth and smiley remain visible beneath all effects.

4. **Deferred lighting/composition**
   - `CompositePS` reads albedo, normal/material, depth, effects data, and signed delta.
   - It applies the base directional lighting, adds the signed delta, and writes the swap chain.
   - Dear ImGui is drawn after composition.

Size-dependent G-buffer, depth, signed-delta, tile-list, indirect-argument, RTV, DSV, SRV, and UAV resources are recreated after waiting for in-flight GPU work. Explicit transitions cover render-target, depth-write, non-pixel SRV, pixel SRV, UAV, indirect-argument, back-buffer, and presentation states.

## Effects V-buffer encoding

Each sail writes its independently selected effect ID:

| Component | Encoding |
|---|---|
| X | Effect ID (`0` none, `1` iridescent sheen, `2` frost crystal, `3` storm lightning) |
| Y | `SV_PrimitiveID + 1` (`0` remains the cleared/background sentinel) |
| Z | Sail parameter UV packed as two IEEE-754 half values |
| W | First two barycentric coordinates packed as two UNORM16 values; the third is `1 - x - y` |

Native `SV_Barycentrics` is unavailable through the sample's deliberately retained `D3DCompile` SM5.1 path. The pixel shader reconstructs equivalent barycentrics from the interpolated non-repeating sail parameter UV, the known 48x36 regular grid, and `SV_PrimitiveID` triangle parity. This is deterministic for this static topology and avoids a DXC/runtime dependency. The mast writes effect ID zero.

## Controls and debug views

The UI preserves all wind, gust, billow, tension, relaxation, sag, pause, reset, and resize behavior. Separate dropdowns select the left and right sail effect IDs; both share the view-space direction and signed strength controls. Debug views are:

- Final composite
- Base albedo
- Encoded view-space normal/material
- Effects V-buffer overview
- Effects V-buffer effect ID
- Effects V-buffer primitive ID
- Effects V-buffer logical UV
- Effects V-buffer barycentrics
- Signed delta (zero-centered at gray)

## Build

Requirements: Windows 10/11, CMake 3.24+, Visual Studio 2022 with Desktop development with C++. CMake FetchContent downloads pinned Dear ImGui `v1.91.8`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
cmake --build build --config Release
.\build\Release\VBufferAnimatedSail.exe
```
