# sdk/mii 2.0: drawing a Mii the standard way

The plan for sdk/mii 2.0.0 (HBC 1.12.0). It makes the right way to draw a
Mii the easy way, for every app (WiiStation first). Status: in progress on
branch `mii-sdk-2`, started from HBC 1.11.1.

## What "standard" means here

These patterns come from Nintendo's own Mii library (RFL, in the Wii Menu
decompilation: docs/research/mii-rendering.md, sections 1.5 and 1.6) and
from NW4R's model drawing:

- **The app owns the camera.** The library draws into the app's frame with
  the app's matrices, lights and blending (`RFLSetMtx`,
  `RFLLoadDrawSetting`).
- **Two passes.** The opaque pass (`RFLDrawOpa`) and the translucent pass
  (`RFLDrawXlu`) are separate calls, so an app draws all its opaque objects,
  then all its translucent ones.
- **Static geometry.** Display lists are built once; each frame only the
  matrices change.
- **Bodies skinned on the GPU.** The Mii Channel's MDL0 display lists carry
  per-vertex matrix indexes. Each frame the app loads about 20 bone matrices
  and a few weighted ones; the GPU poses the vertices.
- **Icons offscreen, once.** `RFLMakeIcon` uses a tight camera (near 500,
  far 700) and draws the translucent pass twice so the copied alpha is right.

## Where 1.1.x differs, and what 2.0 does

| | 1.1.x | 2.0 |
|---|---|---|
| Body | CPU skinning (0.8 ms), 54 KB of vertices through the FIFO a frame | the MDL0's own display lists, matrix palette on the GPU: about 1 KB a frame, no per-vertex CPU work. CPU skinning stays as the host tests' reference |
| Passes | `mii_draw` does both | `mii_draw_opa` and `mii_draw_xlu`; `mii_draw` = both, in order |
| GX state | about 15 changes the app must undo | `mii_draw_setup` / `mii_draw_done`: one documented state, set and handed back |
| Depth | the app must know to give a wide range | `mii_view` reports the range the Mii needs; documented and checked |
| A Mii in a UI rectangle | each app works it out (HBC's avatar) | `mii_draw_in_rect`, with the render mode the app chooses (below) |
| Icons | `mii_icon` / `mii_capture` | match `RFLMakeIcon`: tight camera, two-pass translucent alpha, RGB5A3 or RGBA8 |

## Render modes for a Mii in a rectangle

The app picks one:

- **`MII_RECT_IN_PLACE`**: drawn into the frame being built, with the whole
  depth range in front of the rectangle's depth, which is then put back.
  - Cheapest; hard edges (the EFB has no supersampling).
- **`MII_RECT_SUPERSAMPLED`**: drawn at twice the size into the EFB's
  corner, copied out box-filtered (smooth edges) as an RGBA8 texture, then
  drawn as a sprite.
  - Costs about 0.3 ms of GPU time a frame, an RGBA6 pass, and the GX state
    to restore after.
  - Call it before the frame, as `mii_capture` is called today.
  - The hangs 1.10.21 blamed on capturing were HBC's GX FIFO in MEM2, not
    the copy (REVIEW.md, 1.10.26).

HBC will offer both modes, with a setting.

## Steps

1. **GPU body.** Done (HBC 1.11.3): avatar CPU time 0.73 ms to 0.13 ms a
   frame, FIFO 54 KB to under 2 KB; the palette matches CPU skinning to
   0.00024 (positions) on both bodies; 5 minutes on the bench Wii. Also
   cross-fades between animations (`mii_pose.from_*`, `mii_fade_weight`),
   which the avatar uses into and out of its wave.
   - Keep the MDL0 objects' display lists and their position and normal
     arrays.
   - Each frame, build the bone and weighted matrices; GX loads them
     through the lists' own indexed matrix loads, from
     `GX_SetArray(GX_POSMTXARRAY/GX_NRMMTXARRAY)`.
   - Host test: posed by the palette, every vertex matches CPU skinning
     within 0.01.
2. **API:** `mii_draw_opa`, `mii_draw_xlu`, `mii_draw_setup`,
   `mii_draw_done`, `mii_draw_in_rect` with the two modes; `mii_view`'s
   depth needs.
3. **Icons** rebuilt as `RFLMakeIcon` does.
4. **Proof every time:**
   - Bench grabs of the face compared automatically with a clean icon
     render. That check would have caught the 1.11.0 face.
   - FIFO bytes and CPU time per Mii per frame, recorded.
   - Both render modes run for 5 minutes on the bench Wii.
5. **HBC moved onto it**, with a setting for the avatar's render mode.
   Docs; `MIIDRAW_VERSION` 2.0.0; HBC 1.12.0.
6. **WiiStation** integrates against 2.0, not 1.1.

## Not changing

- The face models stay the user's own (Save Mii faces).
- `mii_res_*`, `mii_db_*`, `mii_remote_*` and the `mii` struct stay as
  they are.
