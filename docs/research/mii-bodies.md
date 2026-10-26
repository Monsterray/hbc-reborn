# Research: how Nintendo builds, scales and draws a Mii's body

Gathered on 2026-10-24 to make `sdk/mii/miidraw.c` (`make_body`) look like
the Wii's own Miis. This note continues [mii-rendering.md](mii-rendering.md)
section 4, which already covers RFL (head only), Wii Sports' kokeshi loader
and the trouser colour rules. Read that first. Every claim below names its
source. "Not found" means that it was searched for and not found. "Inference"
means that the claim is our reading and not a source's.

Nothing here was copied into the repository. FFL.js and its helpers are
AGPL-3.0, the decompilations are CC0, and the models are Nintendo's. Describe
them in our own words and write our own code.

## Summary

- **No Wii title's body code that computes scale from height and build is
  public.** Wii Sports (doldecomp/ogws) shows the loader. The body is a
  *discrete model*, `GetBodyModelIndex(bodyType, sex, height, build)`, from
  one body BRRES per body type and sex. The function that does this,
  `RPSportsBodyManager`, is still undecompiled assembly.
- **Mario Kart Wii also uses discrete sizes.** It has small, medium and large
  Mii drivers, chosen from height + build against two thresholds (see
  section 2).
- **The best public description of how a Mii body scales is the editor body's
  per-bone scaling.** It was reversed from the Switch SDK
  (`nn::mii::detail::UpdateScale`, `VariableIconBodyImpl`) and Wii U Mii
  Maker. Arian Kordi's FFL.js documents it in `helpers/ModelScaleDesc.js`.
  It has these parts:
  - a single scale vector from height and build;
  - per-bone rules that use that vector in different ways;
  - scale that is **not inherited** by child bones;
  - a correction at the root that keeps the feet on the ground;
  - a head that rides the head bone but keeps its own scale (1.4x the
    body's).

  The same file says that this rule set was **tested with the Wii Mii Channel
  body model**, so that body has the same skeleton family.
- **Why our body looks wrong:** `make_body` scales the whole mesh by
  `body_scale[0]` and `[1]`. Nintendo scales per bone. A height change
  lengthens the limbs and torso. A build change thickens the segments. The
  joints (shoulders, knees, wrists, ankles) and so the hands and feet only
  follow the build value, uniformly. The arms swap the axes. That is why the
  pure formula looked like "a stick" or "a box" when we applied it to the
  whole mesh (the comment at `sdk/mii/miidraw.c:820`).
- **Recommendation:** first apply Nintendo's per-bone rules to our own
  segmented body. This is cheap and needs no proprietary data. If faithful
  art is wanted, load the user's own Mii Channel body at run time from NAND
  title `00010002-48414341` (HACA), as we already do with `RFL_Res.dat`. MDL0
  geometry is already GX display lists. The steps and the open items are in
  section 5.

## 1. Decompilations

### 1.1 Wii Sports: doldecomp/ogws (CC0)

Repository: https://github.com/doldecomp/ogws, checked at commit `27fa94a`
(2026-09-19).

- **`RPSysKokeshi::LoadResource`** (`src/Pack/RPKernel/RPSysKokeshi.cpp`) does
  the following steps:
  1. It gets the kokeshi archive `Common/Kokeshi/common.carc`
     (`RPSysResourceManager::KOKESHI_ARCHIVE_PATH`,
     `src/Pack/RPSystem/RPSysResourceManager.cpp`).
  2. It asks the body manager for `GetBodyFileName(bodyType, sex)`.
  3. It creates the body with `CreateModel(file, GetBodyModelIndex(bodyType,
     sex, height, build), ...)`. `CreateModel`
     (`RPSysKokeshiManager.cpp`) takes the MDL0 at that **index** in the
     BRRES (`Type_ResMdl`, `index`). So the height and build select one of
     several MDL0s in a single file. They are not a continuous transform in
     this code.
  4. It loads two hand models from `HAND_FILE_NAMES`:
     `"G3D/ava_handA.brres"` and `"G3D/ava_handB.brres"`. The enum
     `HandType_Human` is "Model with fingers" and `HandType_Sphere` is
     "Spherical model" (`RPSysKokeshiManager.h`). The hands are separate
     models, placed with `GetLeftHandJointMtx` and `GetRightHandJointMtx`
     from the body's joints. Wii Sports' hands float. There are no arms
     (Wikipedia "Mii", game appearances).
  5. It gets the clothes from `GetClothesFileName()` and
     `GetClothesName(clothesType, sex, &a, &b)`. It replaces the body's and
     hands' textures `"dummy_tex_a"` and `"dummy_tex_b"` (`ChangeTexture`).
     `ClothesType` is P1 blue, P2 red, P3 yellow, P4 green, the favourite
     colour, or random for friends.
  6. It calls `SetMatColor(model, favColor, skinColor)`. On material
     `"mt_main"` this sets **TEV colour register 0 = body (favourite)
     colour** and **register 1 = skin colour**
     (`RPSysKokeshiManager::SetMatColor`). So one material tints shirt and
     skin from two registers, and the clothes texture supplies the pattern
     and shading.
  7. It replaces the light-map textures `lm_0` and `lm_1` on the face, body
     and hands (`ApplyLightTexture`). Wii Sports lights with sphere-map style
     light textures, not GX vertex lighting.
- **The head** is an RFL scene node at a base scale of **0.08**
  (`RPSysKokeshiManager` constructor: `mBaseScale = 0.08f;
  RPGrpModelRfl::SetBaseScale`). It is placed at `GetHeadJointMtx`. The
  manager can remove the model's own scale from a joint matrix
  (`GetModelBaseScaleInvalidatedMtx`), so that the head does not inherit the
  body's scale. The FFL editor body does the same (section 3).
- **Height and build** come from `RFLAdditionalInfo` (`height : 7`,
  `build : 7`, `include/RVLFaceLib/RFL_DataUtility.h`). Range 0-127,
  `RFL_MAX_BODY_HEIGHT` = 128 (`RFL_Config.h`). Wii Sports randomises both
  for middle-DB filler Miis (`RPSysAvatar::Setup`).
- **The interface** is `IRPSysKokeshiBodyManager`
  (`include/Pack/RPKernel/IRPSysKokeshiBodyManager.h`): friend clothes range,
  body file name, body model index, clothes file and names, and head and hand
  joint matrices.
- **What is not decompiled:** `RPSportsBodyManager.cpp` is `NonMatching`
  (`configure.py`), and its header body is `// . . .`. It is in main.dol at
  `.text 0x801D26E0-0x801D29D8` (`config/RSPE01_01/splits.txt`), 11 small
  functions (`fn_801D26E0` and the rest in `symbols.txt`). Its `.rodata`
  `0x803841D0-0x80384560` holds one 17-character string, 45 strings of 12
  characters and 10 strings of 20 characters (`symbols.txt` sizes 0x12, 0xD
  and 0x15). *Inference:* the 20-character strings are body file names
  (`"G3D/ava_handA.brres"` is 19 characters) and the 12-character strings are
  clothes texture names. `.sdata2 0x804C1D60` holds four 16-bit values. These
  are possibly the height and build thresholds. *Unverified.* To get the
  rule, disassemble those 11 functions from the user's own Wii Sports DOL in
  Dolphin. Nobody has published this.
- **Wii Play** (the "Party Pack") uses the same engine with
  `RPPartyBodyManager`. It is a header only in ogws, and also in
  akiramusic000/Rhae (a Wii Play decompilation,
  https://github.com/akiramusic000/Rhae).

### 1.2 Other Wii titles

| Title | Status | Source |
|---|---|---|
| Wii Menu (koopthekoopa/wii-ipl) | Draws **no bodies**. `iplNigaoe.cpp` only makes RGB5A3 head icons with `RFLMakeIcon` / `RFLiMakeIconFromData`. | `src/system/iplNigaoe.cpp`, `iplNigaoeManager.cpp` |
| Mario Kart Wii | Discrete S/M/L Mii drivers with outfits A/B/C × male/female = 18 character IDs (`MII_S_A_MALE` 0x18 … `MII_L_C_FEMALE` 0x29). Size class from height + build (section 2). Racing suit in the favourite colour (mariowiki). doldecomp/mkw `src/system/Mii.cpp` has no body code yet. | MelgMKW/Pulsar `GameSource/MarioKartWii/Mii/Mii.hpp` (`enum MiiBuild`), `GlobalFunctions.hpp` (`GetMiiCharacterId` 0x80831af4) |
| Wii Sports Resort, Wii Fit, Wii Party | No useful decompilation was found (unicycleparrot2/-Wii-Sports-Resort-decompilation and Jaydenyahoo64/Wii-Fit-decomp have no code). Wii Fit sets body weight from the measured BMI (Wikipedia "Mii"). | GitHub search |
| Smash Bros. Brawl, Animal Crossing: City Folk | Brawl has no Miis. City Folk uses Mii faces as masks only. *No body code was searched for.* | — |
| Mii Channel (HACA) | **No decompilation or reverse engineering of its body code was found.** | GitHub and web search |

## 2. Height and build to body scale: the formulas

### 2.1 Wii

- **Wii Sports / Wii Play:** a discrete model index from (body type, sex,
  height, build). The thresholds are unknown (section 1.1).
- **Mario Kart Wii:** three size classes. Pulsar's header comment reads
  `MII_MEDIUM_BUILD, //if (104 - height) > build` and
  `MII_HEAVY_BUILD //if (150 - height) > build`. The direction of the
  comparison is written inconsistently there. A Mario Kart 7 mod that copies
  the same rule (Bsquo/mk7-patch-collection,
  `source/examples/variable_mii_size/variable_mii_size.cpp`,
  `getMyMiiWeightClass`) reads: large if `150 - height < build`, else medium
  if `104 - height < build`, else small. In other words:
  **height + build > 150 → large; > 104 → medium; else small.** This is the
  form that makes sense. It is from mods, not from a decompiled function.
- **Mii Channel:** **no formula was found.** It visibly scales the body
  smoothly in the editor and in the plaza. Its body uses the editor skeleton
  that the FFL rules in 2.2 were tested on (section 3).

### 2.2 Wii U / Switch (and the editor body generally)

Sources: FFL.js `getBodyScale`
(https://github.com/ariankordi/FFL.js/blob/HEAD/ffl.js), the fiddle
`ariankordi/my-jsfiddles/threejs-mii-accurate-body-scaling/script.js`
(comment: from `nn::mii::detail::(anon)::GetBodyScale(Float3*, int, int)`),
and a second independent copy in TeamWheelWizard/WheelWizard
`NativeMiiRenderer.cs` `CalculateBodyScale`. All three give the same values:

```
x = z = build * (height * 0.47/128 + 0.4) / 128 + height * 0.23/128 + 0.4
y     = height * 0.77/128 + 0.5            (height, build 0..127)
```

At the middle (64, 64), x = 0.8325 and y = 0.885. At (0, 0), x = 0.4 and
y = 0.5. At (127, 127), x is about 1.49 and y is about 1.26.

**Per-bone application** ("editor body", `editorBodyScaleDesc` in
https://github.com/ariankordi/FFL.js/blob/HEAD/helpers/ModelScaleDesc.js;
reference `mii_VariableIconBodyImpl.o`,
`nn::mii::detail::UpdateScale(Vector3f*, VriableIconBodyBoneKind,
const Float3&)`):

| Bones | Scale applied to the bone |
|---|---|
| every other bone (default) | (x, y, z) |
| `head` | (x, max(y, 1), z): the neck never gets shorter |
| `arm_l1`, `arm_l2`, `arm_r1`, `arm_r2`, `elbow_l`, `elbow_r` | (y, x, z): X and Y swapped. The arms lie along X in the bind pose, so height lengthens them and build thickens them. |
| `wrist_l/r`, `shoulder_l/r`, `ankle_l/r`, `knee_l/r` | (x, x, x): uniform, from build only |
| `all_root`, `body` | none |

Mechanics (from `helpers/SkeletonScalingExtensions.js`, same repository):

- **The scale is local and not inherited.** For each bone, the world matrix
  is multiplied by that bone's own scale, and only the *positions* of its
  children are moved to follow. The children's orientation and scale are not
  multiplied by the parent's scale. So a fat torso does not make fat hands.
- **Root correction.** For each direct child of `skl_root`, the translation
  is scaled by (y, y, x) for (tx, ty, tz), and then `ty += x - y`. The
  comment says this "keep[s] the model's root (legs) planted".
- **Head.** The head is an attachment on the `head` bone. It follows the
  bone's scaled position and rotation but keeps its own scale.
  `headToBodyScale = 1.4`, with the comment "literally the value used in
  ffl_app.rpx" (fiddle `script.js`). The head is set to `0.1 * 1.4` for a
  body normalised to world scale 1.
- **Colours** (fiddle): pants `0x40474E` (grey), favourite-colour table from
  FFL `FFLiColor.cpp`. WheelWizard uses the same pants value (0.251, 0.275,
  0.306).
- **History** (https://ariankordi.net/posts/2026-mii-rendering-history/):
  - Wii U NWF titles draw a fixed body "always the same size with no
    scaling", from `FFLBodyRes.dat` (2 shapes, male and female).
  - Mii Maker is "pretty much the only title that scales the Mii model
    properly".
  - Without scaling, full-body renders look "lanky".
  - The per-bone scaling "took months".
- **Miitomo / Tomodachi Life ("arch" body)** uses different rules
  (`archBodyScaleDesc`): every bone gets (y, x, z) by default, ankles and
  wrists are uniform, and the bones are capitalised (`Skl_Root`, `Head`).
  This is not relevant to the Wii.

## 3. The Mii Channel's body model

**Read from the bench Wii (2026-10-24, an HBC 1.10.26 development build).** The Mii Channel
(title 00010002-48414341, HACA, v6 on the bench Wii) keeps its bodies in
content `00000019.app`, a U8 archive: `Channel/Character.szs` (Yaz0, then
U8) holds `maleBody.brres` (180,352 bytes), `femaleBody.brres` (180,864),
`maleBodyEmpty.brres`, `femaleBodyEmpty.brres`, `graund_model.brres`,
`road_model.brres`, `editBG.brres` and effects. HBC's Save Mii faces copies
the two bodies to `sd:/mii` (`channel/channelapp/source/miires.c`).

- **MDL0 version 8.** 20 bones: `all_root`, `aslope_root`, `position_root`,
  `skl_root` (Y 70, rotated 90 degrees about Y), `chest` (rotated 90 about Z:
  its X runs up), `arm_l1`, `arm_l2`, `wrist_l` and the right ones (two
  24.85 segments, held out level: the model is in a T-pose), `head` (38.86
  up the chest: Y 108.86), `hip`, `foot_l1`, `foot_l2`, `ankle_l` and the
  right ones (two 22.06 segments down to Y 10), `shadow_model`. Every limb
  runs along its bone's X.
- **Two skinned meshes** and a shadow quad: `body__pants` (207 positions,
  404 triangles in strips) and `body__outer` (179 positions, 344 triangles:
  the shirt and the round hands). Each vertex is bound to one bone through
  a position-matrix index (XF loads put bone matrices in slots); positions
  are in that bone's space. No vertex colours or texture coordinates for
  the body; materials `outer` and `pants` (an environment texture each).
  The female shirt is a flared tunic, longer at the hem.
- **21 CHR0 animations, version 3**: `bodyWait`, `bodyGreeting` (a wave),
  `bodyWalk`, `bodyRun`, `bodyTalk`, `bodyTalk_01`, `bodyCatnap_00/01`,
  `bodyGetDown`, `bodySlap_00` to `_06`, `bodySlip_00/01`,
  `bodysnart_00/01`, `bodyWallow`. Header: frames at 0x1C (`bodyWait` 120,
  `bodyGreeting` 96, `bodyWalk` 40), bones, loop, scaling rule. Each bone:
  a name, a code word (bit 1 identity; 5, 6 rotation and translation zero;
  13-21 constant X Y Z of scale, rotation, translation; 22-24 has scale,
  rotation, translation; 25-26 the rotation keys' format), then a constant
  float or an offset to keyframes per animated axis. Translation keys and
  rotation format 3: f32 frame, value, slope; rotation format 2: u16 frame
  in 32nds, s16 angle and slope in 65536ths of a turn; cubic Hermite between
  keys. `bodySlip_00` and `bodyWallow` use another rotation format (1) that
  `sdk/mii` does not read yet.
- **The roots place the Mii in the plaza**: `aslope_root` is rotated -45
  degrees about X in every animation (the sloped ground and camera); a
  renderer leaves the three roots at the bind pose.
- **The head**: RFL's head at scale 1 on the head bone, its bind pose
  upright, looks right against these bodies (the Mii Channel's own code
  for it is not decompiled).
- **The per-bone scale** (section 2.2) fits this skeleton as "each bone
  long by height and thick by build, along its own X": chest, hip, arms and
  legs (sy, sx, sx); wrists and ankles (sx, sx, sx); the head bone (max(sy,
  1), sx, sx); not inherited, a child following its parent's scaled offset.
  Checked by eye at tiny, medium and large, both bodies.

The search-result notes this section first held (The Models Resource's
male and female packs) agree with the above.

## 4. Proportions in numbers

There are no published numbers for the Wii body's lengths. What is known:

- Head-to-body world-scale ratio 1.4 (Wii U Mii Maker, section 2.2), with
  the head's own scale not affected by the body's scale.
- The head sits on the `head` bone, whose Y scale is clamped to at least 1.
- Wii Sports: RFL head at base scale 0.08 on the body's head joint.
- For real lengths, measure the user's own body MDL0 (bone translations in
  the MDL0 bone section, offset 0x38 in each 0xD0-byte bone entry). Or
  measure frames from the bench's capture card of the Mii Channel plaza.
  Measure only. Do not commit Nintendo data.

## 5. Recommendation for a homebrew GX renderer

**Step 1 (do now, no Nintendo data).** Rebuild `make_body` as a small rig
that follows Nintendo's rules instead of scaling the whole mesh:

- Use separate segments: torso, hips, upper and lower leg, upper and lower
  arm, shoulder, elbow, wrist with hand, knee, ankle with foot. Each has its
  own matrix.
- Make the lengths along a bone follow the height scale and the girths
  follow the build scale. The arms use (y, x, z). The joints, hands and feet
  use the uniform x. `all_root` and `body` do not scale.
- Do not let a parent's scale multiply into a child. Move the children only
  by the scaled offset.
- Plant the feet at the root (the root correction in section 2.2).
- Attach the head at the head bone with its own fixed scale (1.4 head-to-body
  as Mii Maker). Clamp the neck's Y scale to at least 1.
- Use the 0..127 formula unchanged. Remove the `powf(.., 0.6)` workaround,
  which was only needed because the mesh was scaled whole.
- Tune the bind-pose lengths by eye against the plaza with the capture card.

**Step 2 (optional, most faithful).** Load the body that the user's own Wii
already has, at run time, the way the SDK loads `RFL_Res.dat`:

1. Find the file. With a bench job (see the bench memory notes), list the
   `.app` contents of `/title/00010002/48414341/content/`, then the U8
   archives and BRRES inside. Check whether HBC's IOS gives ISFS read access
   to that directory. Never commit or ship the file. This step is **not done
   yet**. If the Mii Channel's body cannot be read, Wii Sports'
   `Common/Kokeshi/common.carc` is on the disc, not on NAND, so it is a
   poorer source.
2. Parse U8 → BRRES → MDL0. References:
   - https://mkwiiki.org/wiki/MDL0_(File_Format): section indices 0 draw
     lists, 1 bones, 2 vertices, 3 normals, 4 colours, 5 UVs, 8 materials,
     9 TEV, 10 objects. Bone entries are 0xD0 bytes: S/R/T at
     0x20/0x2C/0x38, parent and child links at 0x5C-0x68, MTX34 and inverse
     at 0x70/0xA0. The draw-list opcodes are 0x02 node, 0x03 weighting,
     0x04 draw, 0x05 indexing, 0x06 duplicate matrix.
   - noclip.website `src/rres/brres.ts` and `src/WiiSports/` (Wii Sports
     scenes). Check that repository's licence before you reuse anything.
   - The Rust `brres` crate (https://docs.rs/brres/) notes that early titles
     such as Wii Sports may use an older MDL0 version than it supports.
3. Draw it. An MDL0 object's vertex data is already a **GX display list**
   with indexed attributes and position-matrix-index loads. On a Wii, do the
   following:
   - point `GX_SetArray` at the vertex, normal and UV arrays;
   - load the bone matrices (with section 2.2's per-bone scale, no
     inheritance) into the matrix slots that the object's bone table names
     (`GX_LoadPosMtxImm` / `GX_LoadNrmMtxImm`);
   - `GX_CallDispList` the object;
   - set TEV from the material: favourite colour and skin into the colour
     registers, as Wii Sports does.

   (sdk/mii does not: it skins on the CPU and sends the posed vertices
   through the FIFO. The GPU hangs seen while it was written were HBC's
   GX FIFO in MEM2, not this choice: REVIEW.md, 1.10.26.)

   Weighted (multi-bone) vertices need the draw list's weighting matrices,
   which are blended on the CPU (opcode 0x03).
4. Keep step 1 as the fallback when the file is missing, as the SDK already
   does for RFL.

The trade-off: step 1 costs little and probably gives most of the "looks
like a Mii" gain, because it changes how the proportions scale. Step 2 gives
Nintendo's exact silhouette, but it needs file discovery on the bench, an
MDL0 parser and a skinning path, and its scale formula on the Wii is still
an assumption borrowed from Wii U and Switch.

## Sources

- doldecomp/ogws (CC0), https://github.com/doldecomp/ogws:
  - `src/Pack/RPKernel/RPSysKokeshi.cpp`, `RPSysKokeshiManager.cpp`,
    `RPSysAvatar.cpp`;
  - `include/Pack/RPKernel/IRPSysKokeshiBodyManager.h`, `RPSysKokeshiManager.h`;
  - `include/Pack/RPSports/RPSportsCommon/RPSportsBodyManager.h`;
  - `src/Pack/RPSystem/RPSysResourceManager.cpp`;
  - `config/RSPE01_01/splits.txt`, `symbols.txt`, `configure.py`;
  - `include/RVLFaceLib/RFL_DataUtility.h`, `RFL_Config.h`.
- koopthekoopa/wii-ipl: `src/system/iplNigaoe.cpp`, `iplNigaoeManager.cpp`.
- akiramusic000/Rhae (Wii Play decompilation).
- MelgMKW/Pulsar: `GameSource/MarioKartWii/Mii/Mii.hpp`,
  `GlobalFunctions.hpp`.
- Bsquo/mk7-patch-collection:
  `source/examples/variable_mii_size/variable_mii_size.cpp`.
- doldecomp/mkw: `src/system/Mii.cpp` (no body code).
- ariankordi/FFL.js (AGPL-3.0): `ffl.js`, `helpers/ModelScaleDesc.js`,
  `helpers/SkeletonScalingExtensions.js`.
- ariankordi/my-jsfiddles: `threejs-mii-accurate-body-scaling/script.js`,
  `index.html`.
- TeamWheelWizard/WheelWizard:
  `WheelWizard/Features/MiiRendering/Services/NativeMiiRenderer.cs`.
- https://ariankordi.net/posts/2026-mii-rendering-history/
- https://wiibrew.org/wiki/Title_database
- https://models.spriters-resource.com/wii/miichannel/asset/348210/
  (search-result text only)
- https://mkwiiki.org/wiki/MDL0_(File_Format)
- https://github.com/magcius/noclip.website (`src/rres/`, `src/WiiSports/`)
- https://en.wikipedia.org/wiki/Mii
