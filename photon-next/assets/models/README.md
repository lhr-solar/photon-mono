# 3D models

| File | Size | Role |
| --- | --- | --- |
| `new_dyn.glb` | 6.2 MB | Dynamics car (optimized, tinygltf-valid). **HTTP static** at `models/new_dyn.glb`. Chassis-only; Model combo. |
| `newCar.glb` | 32 MB | Display car (single mesh, no wheel nodes). **HTTP static**. Default 3D pick and OSM overlay. |
| `s26_simple_track.glb` | 7.9 MB | Lightweight track. **HTTP static**. Overlay "Track" toggle. |

## How they are served

Fonts are `--preload-file`'d into `photon.data`. The car/track GLBs are copied to `.artifacts-web/models/` at build time and fetched at runtime with `emscripten_async_wget2_data` (progress: `loading new_dyn.glb 42%`), then parsed from memory by tinygltf.

While the selected GLB is fetching, the 3D view shows that progress over an empty grid — no placeholder model.

`new_dyn.glb` nodes are all `empty_N` (no `Wheel_*` names). Wheels are hard-mapped from the dumped JSON plus the legacy AABB classifier (`Photon/photon/gpu/scene.cpp`):

- FL `empty_273`,`empty_274`  FR `empty_275`,`empty_276`
- REAR `empty_366`–`empty_371` (single axle, both RL/RR)
- `newCar.glb` is one node `empty_195` — rigid display mesh, no articulated nodes.

Front links (wishbones, pushrods, tie rods, shocks, rack, steering column) are classified spatially. Attach points are measured at load: inboard centroid of the medial 35% of each link's primitives, outboard-most vertex as the upright tip. IK then keeps those joints on the wheel as it steers and jounces.

Named `Body` / `Wheel_*` nodes still bind if a future GLB uses those names.
