# Research: drawing Miis (faces and bodies) on a Wii

Gathered on 2026-10-24 for a Mii renderer in the HBC SDK. `sdk/mii` already
reads and writes the Mii data (`sdk/mii/mii.h`, [../mii.md](../mii.md)). This
note covers the other half: how a Wii turns those 74 bytes into a picture,
where the art comes from, and what we may use.

Each fact names its source. The sources are listed at the end. Paths such as
`RFL_Model.c` are in the Wii Menu decompilation's
`libs/RVLFaceLib/src/` (koopthekoopa/wii-ipl) unless another repository is
named. Where something was not checked, the note says so.

## Summary

- **RFL draws only a head.** It has no body (`RFL_Model.h` draws faceline,
  hair, cap, forehead, beard, nose, nose line, mask and glasses only). Every
  game brings its own body.
- **RFL's art is one file, `RFL_Res.dat`**: 18 archives of shapes (GX
  display-list source data) and textures (BTI-style headers). It is not
  compressed in the copy RFL reads. It is Nintendo's art: we must not ship it.
- **Every Wii has a copy.** The Wii Menu reads `/RFL_Res.dat` from a U8
  archive in one of its own title contents (`iplSystem.h`
  `loadRFLRes_()`: content index 4 in the 4.3U decompilation), so a homebrew
  app can read the user's own copy at run time through ES. Games also carry
  copies on their discs (Mario Kart Wii: `/contents/RFLRes01.arc`).
- **The whole pipeline is decompiled** in several CC0 game decompilations
  (Wii Menu, Wii Sports, Super Mario Galaxy 1 and 2, Brawl, Mario Kart Wii and
  more). It is small: about 2,500 lines for the model, the mask texture and
  the icon. It uses only fixed-function GX, which libogc exposes one to one.
- **Recommended path**: our own clean implementation of the same steps, fed by
  the user's `RFL_Res.dat` found on their Wii; first an offscreen "head icon"
  (as `RFLMakeIcon` does) that the HBC UI draws as a texture; then a live 3D
  head; a simple body of our own design last.

## 1. How a Wii renders a Mii (RFL)

### 1.1 Where the face resources are

- **RFL's own paths.** `RFL_NANDAccess.c` names
  `/shared2/menu/FaceLib/RFL_DB.dat` (the database) and
  `/shared2/menu/FaceLib/RFL_Res.dat` (the resource) as its two file types.
  `RFL_NANDLoader.c` opens `/shared2/FaceLib/RFL_Res.dat` directly
  (`scResFileFullPathName`, `NANDPrivateOpen`) when no resource is cached.
- **That loose file is normally absent.** The bench Wii's
  `/shared2/menu/FaceLib` holds only `RFL_DB.dat` ([../mii.md](../mii.md),
  "Where a Wii keeps its Miis"). GBAtemp users report the same from NAND
  dumps (GBAtemp thread, below; forum report, not checked by us).
- **Callers pass the resource in memory instead.** `RFLInitRes(work,
  resBuffer, resSize, useDeluxTex)` (`RFL_System.c`) caches a buffer
  (`loader->mCacheBuffer`). Then every part is a `memcpy` from it
  (`getCachedFile_`).
- **The Wii Menu's copy.** `RFLGetArcFilePath()` returns `"/RFL_Res.dat"`
  (`RFL_System.c`). The Wii Menu reads it with
  `readSharedAsync(heap, RFLGetArcFilePath(), 4)` (`include/system/iplSystem.h`,
  `loadRFLRes_`). `SharedFile` (`src/system/iplNandShared.cpp`) opens title
  content index 4 of the System Menu with `ES_OpenTitleContentFile`, reads
  the U8 `ARCHeader` and FST, `ARCOpen`s the name and reads the bytes raw (no
  decompression step). `iplNigaoeManager.cpp` then calls
  `RFLInitRes(work, mResFile->getBuffer(), mResFile->getLength(), FALSE)`.
  So the index is 4 in the 4.3U Wii Menu; other versions were not checked.
- **Discs.** Mario Kart Wii keeps it as the only file of
  `/contents/RFLRes01.arc`; "many Wii games that use Miis" carry one
  (mkwiiki, RFL_Res.dat page). Wii Sports and others load it from their own
  archives (not checked per game).
- **Version.** The file's second `u16` is a version. RFL asserts it is at
  least 0x39D ("RFL : resource is old.", `parseOnmemoryRes_`). mkwiiki
  reports 0x39D in Mario Kart Wii.

### 1.2 The `RFL_Res.dat` format

Big-endian. Layout from `RFL_NANDLoader.c` (`parseOnmemoryRes_`,
`getCachedFile_`, `getCachedLength_`) and the mkwiiki page, which agree:

| Offset | Type | Contents |
| --- | --- | --- |
| 0x00 | u16 | archive count (18) |
| 0x02 | u16 | version (0x39D) |
| 0x04 | u32 × 18 | offset of each archive from the start of the file |

Each archive:

| Offset | Type | Contents |
| --- | --- | --- |
| 0x00 | u16 | file count N |
| 0x02 | u16 | largest file's size |
| 0x04 | u32 × (N+1) | file offsets, relative to the end of this table; entry N is the end, so size(i) = off[i+1] - off[i] |
| 0x08 + 4N | | the files |

The archive order is `RFLiArcID` (`RFLi_NANDLoader.h`): Beard, Eye, Eyebrow,
Faceline, FaceTex, ForeHead, Glass, GlassTex, Hair, Mask, Mole, Mouth,
Mustache, Nose, Nline, NlineTex, Cap, CapTex.

**Textures** (`RFLTexture`, `RFLi_Texture.h`) have a 0x20-byte header shaped
like a BTI: format, transparency, width, height, wrap S/T, palette fields,
LOD fields and `imageOffset` (from the header's start). The parts textures
are eye, eyebrow, mouth, mustache and mole (`RFLiLoadTexture`); the shape
textures are face (make-up/wrinkles), cap, nose line and glasses
(`RFLiLoadShpTexture`).

**Shapes** (`RFLiInitShapeRes`, `RFL_Model.c`):

1. A 4-byte tag: `nose`, `frhd`, `face`, `hair`, `cap_`, `berd`, `nsln`,
   `mask`, `glas`.
2. `face` only: three float triples, the nose, beard and hair anchor
   positions, used to place those parts.
3. `u16` position count, then positions as `s16` x/y/z. RFL loads them with
   `GX_POS_XYZ, GX_S16`, 8 fraction bits (`RFLLoadVertexSetting`).
4. `u16` normal count, then `s16` normals, 14 fraction bits.
5. Except nose, forehead, hair and beard: `u16` texcoord count, then `s16`
   S/T, 13 fraction bits.
6. A `u8` primitive count; each primitive is a `u8` vertex count, a `u8`
   `GXPrimitive` value, then per vertex a `u8` position index, `u8` normal
   index and (with texcoords) a `u8` texcoord index. RFL replays these
   between `GXBeginDisplayList`/`GXEndDisplayList` with
   `GXPosition1x8`/`GXNormal1x8`/`GXTexCoord1x8`, so the draw is indexed
   (`GX_INDEX8`) from arrays bound with `GXSetArray`.

A shape with position count 0 is empty (no beard, no cap). The hair, cap and
forehead of one hair type are separate archives with the same index.

### 1.3 From a Mii to a model

`RFLiConvertRaw2Info` (`RFL_Database.c`) copies the 74-byte fields into an
`RFLiCharInfo` with no remapping: the stored part numbers are the archive
indexes. Then `RFLiInitCharModel` (`RFL_Model.c`) builds one
`RFLiCharModelRes` (about 33 KB: `RFLi_Model.h`) and one mask texture per
expression:

- **Shapes and their placement** (`RFLiInitCharModelRes`):
  - Faceline: shape `face[faceline.type]`, no transform; it supplies the
    nose, beard and hair anchors.
  - Cap, hair, forehead: index `hair.type`, translated to the hair anchor,
    mirrored in X when `hair.flip`.
  - Beard: `beard.type`, at the beard anchor.
  - Nose and nose line: `nose.type`, scale `0.4 + 0.175 * nose.scale`,
    Y = anchor Y `- 1.5 * (nose.y - 8)`.
  - Mask: `mask[faceline.type]`, the surface the 2D features are drawn on.
  - Glasses: one shape for every type, scale `0.4 + 0.15 * glass.scale`,
    Y = nose anchor Y `+ 5 - 1.5 * (glass.y - 11)`, Z `+ 2`.
  - Transforms are applied to the `s16` vertices once, at load, in 8.8 fixed
    point (`s = 256 * posScale`; `RFLiInitShapeRes`). `RFLSetCoordinate` can
    swap axes for games with another "up".
- **Shape textures**: face texture `faceline.texture`, cap texture
  `hair.type`, nose-line texture `nose.type`, glasses texture `glass.type`
  (`RFLiInitTexRes`).
- **Colours** are constant tables indexed by the Mii's fields:
  `cFacelineColor[6]`, `cHairColor[8]`, `cBeardColor[8]`, `cGlassColor[6]`,
  `cFavoriteColor[12]` (`RFL_Model.c`), and the eye, eyebrow, mouth, beard
  and mole tables in `RFLi_FaceConfig/colors.h`.

### 1.4 The mask texture: the 2D face features

`RFLiMakeTexture` and `RFLiCapture` (`RFL_MakeTex.c`) draw the eyes,
eyebrows, mouth, mustache and mole as textured quads into the EFB, then
`GXCopyTex` them into a square `GX_TF_RGB5A3` texture of 32, 64, 128 or 256
pixels (mipmapped for the `...M` resolutions). That texture is mapped onto the
`mask` shape.

- **Placement** (`RFLiSetFaceParts`): constants in a 64-unit square
  (`BASE_EYE_Y`, `BASE_MOUTH_Y`, ...), X step `0.88961464`, Y step
  `1.16 * 0.9276675`, scale `1 + 0.4 * scale`, rotation in 32 steps of
  11.25 degrees plus a per-type offset (`RFLi_EYE_ROT_OFFSET`,
  `RFLi_EYEBROW_ROT_OFFSET` in `RFLi_FaceConfig/offsets.h`). Eyes and
  eyebrows are mirrored pairs; the left one's angle is `360 - a`.
- **Colouring with TEV**: eye and mouth textures carry several masks in their
  channels. Three TEV stages with swap tables mix them: stage 0 selects one
  channel times colour 0, stage 1 another times colour 1, stage 2 adds white
  (`RFLiSetTev4Eye`, `RFLiSetTev4Mouth`). Eyebrow, mustache and mole are one
  stage: a constant colour with the texture's alpha.
- **Alpha** comes from a two-pass trick with destination alpha:
  `GX_BL_INVDSTALPHA, GX_BL_DSTALPHA, GX_LO_SET`, then
  `GX_BL_INVDSTALPHA, GX_BL_ONE`. This needs an EFB with alpha
  (`GX_PF_RGBA6_Z24`) while the capture runs. The HBC sets
  `GX_PF_RGB8_Z24` (`channel/channelapp/source/gfx.c`), so the renderer
  must switch format for its offscreen pass and switch back.
- **Expressions** are other parts drawn into further mask textures
  (`RFLiMakeTexture`): smile uses eye 48, surprise eye 49 and eyebrow Y - 2,
  blink eye 26, open mouth mouth 24, anger mouth 10, sorrow mouth 12; anger
  and sorrow also turn the eyebrows and eyes. `RFLSetExpression` only switches which mask
  texture is bound.

### 1.5 Drawing

`RFLDrawOpaCore` and `RFLDrawXluCore` (`RFL_Model.c`):

- One vertex format: indexed `s16` position, normal and texcoord.
- One TEV stage with a constant colour (`GXSetTevKColor`), lit by colour
  channel 0 (`RFLLoadDrawSetting`: a light mask, diffuse function and
  ambient).
- **Opaque pass**: beard (beard colour), nose and forehead (skin colour), hair
  (hair colour), cap (texture modulated by the favourite colour), then the
  faceline with the face texture over skin colour.
- **Translucent pass**: the mask with the expression's mask texture, the nose
  line, the glasses (texture times glasses colour).
- The game supplies the matrices (`RFLSetMtx`), lights and blend settings.

### 1.6 Icons: `RFLMakeIcon`

`RFLiMakeIcon` (`RFL_Icon.c`) is the reference for an offscreen head picture:

- 16 to 512 pixels square or not; resolution 64, 128 or 256 depending on
  size.
- Background: the favourite colour or a given colour, alpha 0.
- Perspective with fovy `2 * atan2(43.2, 500)`, near 500, far 700; camera at
  (0, 34.5, 600) looking at (0, 34.5, 0).
- One white light at (1600, 1500, 6000), ambient (160, 160, 160).
- Opaque pass; translucent pass twice (colour only, then alpha only with
  `GX_BL_ONE, GX_BL_INVSRCALPHA`) so the copied alpha is right.
- `GXCopyTex` to `GX_TF_RGB5A3`, then `GXDrawDone`.

The Wii Menu uses this for its Mii icons (`src/system/iplNigaoe.cpp`,
`make_icon`: `RFLiMakeIconFromData` or `RFLMakeIcon`).

### 1.7 Memory

`RFLGetWorkSize`: 0x4B000 (300 KB), or 0x64000 with the 256-pixel "delux"
textures (`RFL_System.c`). One model: `RFLiCharModelRes` (0x8250 bytes) plus
its mask textures. The resource held in memory adds its own size (not
measured; the per-archive offsets give it).

## 2. Decompilations with RFL or Mii rendering

All found with GitHub code search for `RFLiInitShapeRes` and `RFLMakeIcon`
(2026-10-24).

| Repository | Game | RFL | Other Mii code | Licence |
| --- | --- | --- | --- | --- |
| koopthekoopa/wii-ipl | Wii Menu 4.3U | full `libs/RVLFaceLib` | `iplNigaoe*.cpp` (icons), `iplSystem.h` (resource) | CC0-1.0 |
| koopthekoopa/RFL | RFL alone (20080306 DBG/REL) | full | | none stated |
| doldecomp/ogws | Wii Sports | `src/RVLFaceLib` | `RPSysKokeshi*.cpp` (Mii avatars, bodies), `RPGrpModelRfl.cpp`, `nw4r/g3d/g3d_scnrfl.h` | CC0-1.0 |
| SMGCommunity/Petari | Super Mario Galaxy | `src/RVLFaceLib` | `src/Game/NPC/MiiFaceRecipe.cpp` | CC0-1.0 |
| SMGCommunity/Garigari | Super Mario Galaxy 2 | `src/RVLFaceLib` | | CC0-1.0 |
| doldecomp/brawl | Super Smash Bros. Brawl | `include/OpenRVL/src/RVLFaceLib` | | CC0-1.0 |
| snailspeed3/mkw | Mario Kart Wii | `lib/rfl` | | CC0-1.0 |
| conhlee/rhf | Rhythm Heaven Fever | headers | `src/Game/RFLManager.cpp` | CC0-1.0 |
| yomcube/cmwp-decomp, FortuneStreetModding/boom-street-decomp, yannicksuter/mscharged-decomp, SwareJonge/IEStrikers, orangedude27/excite-truck | other games | RFL sources or headers | mscharged: `MiiManager.cpp` | CC0-1.0 where checked |
| Wii4Mii/Miivolution | PC port layer over Galaxy's RFL | rewritten in C++ | | none stated |

- Wii Sports Resort, Wii Party, Animal Crossing: City Folk and the Mii
  Channel itself were not found as decompilations with Mii code.
- **Licence status.** CC0 on a decompilation covers the contributors' work.
  It cannot license Nintendo's copyright in the code it reconstructs. Treat
  these as documentation: read them, describe the behaviour in our own words,
  write our own code. Do not paste their functions into the public-domain
  SDK. (A judgement, not legal advice.)
- **`RFL_Res.dat` is Nintendo's art** and is in none of these repositories.
  It must be read from the user's console at run time, never shipped.

## 3. Wii U, 3DS and Switch successors

- **FFL (Wii U, Cafe Face Library)**: aboood40091/ffl is a decompilation of
  FFL 1.3.10 from New Super Mario Bros. U (no licence stated).
  aboood40091/FFL-Testing and ariankordi/FFL-Testing (no licence stated) are
  renderers on top of it. ariankordi/ffl is a 64-bit/Linux port.
  ariankordi/FFL.js (AGPL-3.0) is a three.js renderer.
  ariankordi/ffl-raylib-samples (Unlicense) are raylib samples.
  jaames/mii-assets (no licence) extracts FFL/AFL archives to PNG and glTF
  (`FFLResHigh`: 317 textures, 857 shapes).
- **History and lessons**: Arian Kordi's blog post on Mii rendering history
  (ariankordi/blog, `content/posts/2026-mii-rendering-history.md`):
  - FFL, like RFL, has no body. Wii U titles add a separate `FFLBodyRes.dat`
    in the same resource format with two shapes (male, female); the class is
    `FFLIconWithBody`.
  - "MakeIcon" is "basically same in CFL/FFL/nn::mii" as `RFLMakeIcon`.
  - The Switch editor scales the body per bone ("VariableIconBody").
- **Arian Kordi's coming library** (blog, "2026 Quarter 2 Update") will parse
  Mii data and resources of all generations, Wii (RFL) included, with a
  planned MIT-compatible licence. It is not released. It does not convert
  newer Miis down to the Wii format.
- **Wii data to FFL/Studio**: HEYimHeroic/mii2studio (AGPL-3.0) converts a
  Wii Mii to Mii Studio data (`mii2studio.py`). Most fields pass through
  unchanged (types, positions, sizes, hair type). The differences: the Wii
  `facial_feature` splits into make-up (`{1:1, 2:6, 3:9, 9:10}`) and wrinkles
  (`{4:5, 5:2, 6:3, 7:7, 8:8, 10:9, 11:11}`); colour 0 of hair, eyebrows and
  beard becomes 8; eye colour +8; glasses colour +13; mouth colour +19;
  stretch fields get 3. The same repository has Kaitai specs:
  `gen1_wii.ksy` (Wii/DS) and `gen2_wiiu_3ds_miitomo.ksy`.
- **Body scale formula (Wii U/Switch, not Wii)**: FFL.js `getBodyScale`, from
  `nn::mii::detail::GetBodyScale` and Mii Maker's `ffl_app.rpx`:
  `x = z = build * (height * 0.47/128 + 0.4)/128 + height * 0.23/128 + 0.4`,
  `y = height * 0.77/128 + 0.5`, with height and build 0-127.
- **What we can reuse for GX**: the architecture and the knowledge, not the
  code or art:
  - FFL's design is RFL's, grown: shapes, a mask texture, a faceline texture,
    opaque then translucent passes, icons as offscreen renders.
  - Its shaders (GLSL/GX2) do not apply; the Wii path is fixed-function
    TEV, which RFL already shows.
  - Its art (`FFLResHigh.dat` and the rest) is as proprietary as
    `RFL_Res.dat`, is a different, larger set, and is not on a Wii.
  - The body scale formula above is a usable starting point for a body of
    our own.

## 4. Bodies

RFL has no body (section 1.5). What games do:

- **Wii Sports (and Wii Play, Wii Party on the same "Pack" engine)**
  (doldecomp/ogws):
  - `RPSysKokeshi::LoadResource` (`src/Pack/RPKernel/RPSysKokeshi.cpp`)
    loads a body model, two hand models and clothes textures from the
    "kokeshi" archive.
  - It picks the body by
    `IRPSysKokeshiBodyManager::GetBodyModelIndex(bodyType, sex, height,
    build)`: a discrete model index chosen from height and build, not a
    continuous scale. The implementations (`RPSportsBodyManager`,
    `RPPartyBodyManager`) are headers only in ogws today, so the thresholds
    are not known yet.
  - Clothes: a player colour (P1 blue, P2 red, P3 yellow, P4 green), the
    favourite colour, or random for friends (`RPSysKokeshiManager.h`
    `ClothesType`). `SetMatColor(model, favColor, skinColor)` tints body and
    hands.
  - The head is an RFL model in an NW4R scene node (`RPGrpModelRfl.cpp`,
    `nw4r::g3d::ScnRfl`), placed at the body's head joint
    (`GetHeadJointMtx`).
- **Pants colour** (WiiBrew, Mii data): gold for a special Mii (ID bit 31
  clear), blue for a "foreign" Mii (bit 30, which RFL calls the DS flag,
  `RFLiIsDSID`), red for a favourite, otherwise black/grey (the page
  contradicts itself between black and grey).
- **Shirt**: the favourite colour (`cFavoriteColor` above; Wii Sports
  `GetFavoriteColor`).
- **Height and build** are 0-128 in RFL's check ([../mii.md](../mii.md));
  WiiBrew says 0-127.
- **Mii Channel and Mario Kart Wii bodies**: no decompilation or
  documentation of the Mii Channel's body or its scaling was found. Mario
  Kart Wii's three Mii weight classes come from height and weight
  (fan wikis); the formula was not found.

## 5. Existing renderers and emulators

- **On the Wii**: no libogc/GX homebrew that draws real Miis was found.
  libmii (WiiBrew, Google Code) reads Mii data only. Its "Rendering Miis"
  page composes 2D sprite sheets with GRRLIB (fan-drawn art, no licence
  stated).
- **PC**: Mii-Database-Editor (Java, `RFL_DB.dat` editor) shows pictures via
  `ImageDownloader.java`, presumably from an online renderer (not checked).
  RFL_Res.dat Editor by Atlas (Windows, mkwiiki) edits head models and
  textures. "RFL_Res Web" by Gab is listed on the same page.
- **Dolphin** (`C:\projects\dolphin-src`): Mii code only copies `RFL_DB.dat`
  (`Source/Core/Core/WiiRoot.cpp`, `NetPlayClient.cpp`, `NetPlayServer.cpp`,
  `Common/NandPaths.cpp`) and checks it (`WiiUtils.cpp` `CheckNAND`). It does
  not render Miis; games do that through RFL on the emulated GPU.
- **Azahar (Citra's successor)**: Mii selector applets only
  (`src/core/frontend/applets/mii_selector.*`). Ryujinx and Cemu were not
  checked.

## 6. Recommended plan for an `sdk/mii` renderer

### 6.1 What we may use

- **The user's own `RFL_Res.dat`, read at run time on their Wii.** Never
  shipped, never cached to SD by default.
- **Facts about formats and behaviour**: offsets, field meanings, index
  rules, colour values, placement formulas. They are needed for
  interoperability, and `docs/mii.md` already works this way. Write them up
  in our own words with sources; write our own code.
- **Our own art** for anything RFL does not supply (body, hands, a
  placeholder head).
- **Not**: code pasted from decompilations (even CC0 ones), FFL or AFL
  resource files, Wii Sports body models, or the AGPL code of mii2studio and
  FFL.js inside the public-domain SDK (read them, do not copy them).

### 6.2 Finding `RFL_Res.dat`

Try in this order:

1. A path the app gives (for example on SD), for tests and for Wiis without a
   System Menu copy.
2. The System Menu: title 00000001-00000002. Read its TMD
   (`ES_GetStoredTMD`) and ticket views (`ES_GetTicketViews`). Open contents
   with `ES_OpenTitleContent`, `ES_ReadContent` and `ES_SeekContent` (libogc
   `gc/ogc/es.h`). Look for a U8 archive whose FST has `RFL_Res.dat`. Try
   index 4 first (4.3U, `iplSystem.h`), but scan, because other Wii Menu
   versions were not checked. Read the file raw (no compression in the Wii
   Menu's path).
3. `/shared2/menu/FaceLib/RFL_Res.dat` and `/shared2/FaceLib/RFL_Res.dat`
   (RFL's own NAND paths), in case a Wii has one.

Check: count 18, version >= 0x39D, every offset inside the file. Fail with a
clear error, never a crash.

**Checked on the bench Wii** (2026-10-24, `tests/mii_res_probe` through
`tests/wii_mii_res.py`, System Menu v513, IOS58 with AHBPROT open): an app
reaches none of these.

| Path | Result |
| --- | --- |
| `/shared2/menu/FaceLib/RFL_Res.dat` | -106: not there |
| `ES_OpenTitleContent` on each of the System Menu's 9 contents | -1026 for every one |
| `/shared1/content.map` (where its 6 shared contents are named) | -102: no permission |
| `/title/00000001/00000002/content/*.app` | -102: no permission |

The System Menu's 9 contents: index 0 (64 bytes) and 1 and 8 (6.2 and 3.7
MiB) are its own; 2 to 7 are shared (TMD type 0x8001), so they live in
`/shared1`. Index 4 is a shared content of 686,468 bytes. Which one holds
`RFL_Res.dat` was then shown by HBC 1.10.20's copy: content 4 (686,468
bytes) is a U8 archive holding `RFL_Res.dat`, 686,372 bytes, version 0x39D.

So an SDK app gets the file only by one of these:

- **A file the user supplies** (step 1 above): copied off their own Wii or
  game disc by a tool with NAND or disc rights (a BootMii NAND backup and an
  extractor; Mario Kart Wii's `/contents/RFLRes01.arc`).
- **NAND rights at run time**: an IOS with its ISFS permission check lifted
  (with AHBPROT, as HBC's play log does, or a cIOS's patch).

**Chosen (2026-10-24): the user supplies the file, and HBC makes that one
press.** HOME > HBC > Save Mii faces (`hbc.py mii-faces`) lifts the check
for its reads only, copies the file to `sd:/mii/RFL_Res.dat` and puts the
check back ([../mii.md](../mii.md#the-face-models-rfl_resdat)). SDK apps
read the SD copy and never lift the check themselves.
- **No resource**: a placeholder or a renderer of our own (step 5).

### 6.3 Steps

1. **Parser and PC tool** (`-DMII_NO_OGC`, like `sdk/mii`). Read the
   resource and print each archive's count. Decode one shape to OBJ and one
   texture to PNG. Test against a resource file the developer provides from
   their own Wii, kept in an ignored folder like `keys/`, never committed.
   Tests with no file skip, not fail.
2. **Head icon** (the minimal useful result): `mii_icon(&mii, w, h, bg,
   rgb5a3_out)`, a clean version of `RFLMakeIcon`:
   - Build the mask texture with quads and TEV in an offscreen EFB pass
     (`GX_PF_RGBA6_Z24`).
   - Build display lists for the shapes.
   - Draw opaque, then translucent twice, and copy to an RGB5A3 texture.
   - Restore the HBC's GX state (pixel format, viewport, scissor, matrices,
     TEV) afterwards.
   - Run it outside the frame loop: once per Mii, cached in memory.
   - This gives the HBC a picture per Mii (for example for `mboard` or a Mii
     list) at the cost of one texture each.
   - A 2D-only picture made from the mask texture alone shows no face shape
     or hair, so it is not worth doing first.
3. **Live 3D head**: expose the model (`mii_model_init`, `mii_model_draw(mtx,
   expression)`) for apps that want a turning head, blinking (expression
   textures) and lighting.
4. **Body**:
   - Our own low-poly body and hands, built in code (a kokeshi-style capsule
     body and sphere hands, as Wii Sports' `HandType_Sphere`).
   - Shirt in the favourite colour; pants from the ID flags and the favourite
     flag (section 4); skin from `cFacelineColor`.
   - Scale X/Z and Y from height and build. Start from the Wii U formula
     (section 3) and tune by eye against the Mii Channel on the bench Wii.
     The Wii's own formula is not known.
   - Head placed at the body's neck point.
5. **Fallback** with no `RFL_Res.dat`: a flat placeholder (a circle in the
   skin colour on a favourite-colour background), so callers always get an
   icon.

### 6.4 Testing

- **Host**: parser tests against the developer's own file (skipped
  otherwise); range checks on every index a Mii can hold (types up to the
  limits in `docs/mii.md`, plus the expression parts: eyes 26, 48, 49 and
  mouths 10, 12, 24).
- **Dolphin**: the emulated NAND needs a System Menu for step 2 of 6.2, or
  the SD path. Render icons of the six default guests and of
  `tests/dolphin_mboard.py`'s `test_mii()`. Compare frame dumps with the Wii
  Menu's own icons of the same Miis (Address Book, Message Board). Dolphin's
  EFB copies and destination alpha are close to the hardware's, but compare
  on the Wii too.
- **Wii** (bench queue): the same Miis, captured with the bench capture card;
  check icon alpha edges, glasses and mask transparency, and the restored GX
  state (the HBC UI must look unchanged after an icon is made). Time an icon;
  it should stay well under a frame's budget per Mii, or it runs in idle
  frames.
- **Review trigger**: if any step needs a constant that is not documented in
  this note, record it here with its source before using it.

## Sources

Decompilations (GitHub):

- koopthekoopa/wii-ipl (CC0-1.0): `libs/RVLFaceLib/src/RFL_NANDLoader.c`,
  `RFL_NANDAccess.c`, `RFL_System.c`, `RFL_Model.c`, `RFL_MakeTex.c`,
  `RFL_Icon.c`, `RFL_Database.c`;
  `libs/RVLFaceLib/include/internal/RFLi_NANDLoader.h`, `RFLi_Texture.h`,
  `RFLi_Model.h`, `RFLi_MakeTex.h`, `RFLi_FaceConfig/colors.h`,
  `RFLi_FaceConfig/offsets.h`; `include/RFL_Model.h`, `RFL_Types.h`,
  `RFL_Icon.h`; `include/system/iplSystem.h`, `src/system/iplNandShared.cpp`,
  `src/system/iplNandManager.cpp`, `src/system/iplNigaoe.cpp`,
  `src/system/iplNigaoeManager.cpp`.
  <https://github.com/koopthekoopa/wii-ipl>
- koopthekoopa/RFL: <https://github.com/koopthekoopa/RFL>
- doldecomp/ogws (CC0-1.0): `src/Pack/RPKernel/RPSysKokeshi.cpp`,
  `include/Pack/RPKernel/IRPSysKokeshiBodyManager.h`,
  `include/Pack/RPKernel/RPSysKokeshiManager.h`,
  `include/Pack/RPSports/RPSportsCommon/RPSportsBodyManager.h`,
  `src/Pack/RPGraphics/RPGrpModelRfl.cpp`, `src/RVLFaceLib/`.
  <https://github.com/doldecomp/ogws>
- SMGCommunity/Petari, SMGCommunity/Garigari, doldecomp/brawl,
  snailspeed3/mkw, conhlee/rhf, yomcube/cmwp-decomp,
  FortuneStreetModding/boom-street-decomp, yannicksuter/mscharged-decomp,
  SwareJonge/IEStrikers, orangedude27/excite-truck, Wii4Mii/Miivolution (all
  on GitHub, found by code search for `RFLiInitShapeRes` / `RFLMakeIcon`).

Formats and data:

- mkwiiki (formerly wiki.tockdom.com), RFL_Res.dat:
  <https://mkwiiki.org/wiki/RFL_Res.dat>
- RFL_Res.dat Editor: <https://wiki.tockdom.com/wiki/RFL_Res.dat_Editor>
- GBAtemp, "Where is RFL_Res.dat? How to edit Mii models?":
  <https://gbatemp.net/threads/where-is-rfl_res-dat-how-to-edit-mii-models.683818/>
  (from search results; the page itself refused the fetch)
- WiiBrew, Mii data: <https://wiibrew.org/wiki/Mii_data>
- WiiBrew, libmii: <https://wiibrew.org/wiki/Libmii>, and
  <https://wiibrew.org/wiki/Libmii/Rendering_Miis>
- [../mii.md](../mii.md) and `sdk/mii/mii.h` in this repository.

Successors and renderers:

- aboood40091/ffl: <https://github.com/aboood40091/ffl>
- aboood40091/FFL-Testing, ariankordi/FFL-Testing, ariankordi/ffl:
  <https://github.com/ariankordi/FFL-Testing>
- ariankordi/FFL.js (`ffl.js`, `getBodyScale`):
  <https://github.com/ariankordi/FFL.js>
- ariankordi/ffl-raylib-samples:
  <https://github.com/ariankordi/ffl-raylib-samples>
- ariankordi/blog, `content/posts/2026-mii-rendering-history.md`:
  <https://github.com/ariankordi/blog>
- Arian Kordi, "2026 Quarter 2 Update":
  <https://ariankordi.net/posts/2026-q2-update/>
- jaames/mii-assets: <https://github.com/jaames/mii-assets>
- HEYimHeroic/mii2studio (`mii2studio.py`, `gen1_wii.ksy`):
  <https://github.com/HEYimHeroic/mii2studio>
- Alex9303/Mii-Database-Editor:
  <https://github.com/Alex9303/Mii-Database-Editor>
- azahar-emu/azahar: <https://github.com/azahar-emu/azahar>

Local:

- `C:\projects\dolphin-src\Source\Core\Core\WiiRoot.cpp`, `WiiUtils.cpp`,
  `NetPlayClient.cpp`, `NetPlayServer.cpp`,
  `Source\Core\Common\NandPaths.cpp`.
- `C:\projects\libogc3-src\gc\ogc\es.h` (`ES_OpenTitleContent`,
  `ES_ReadContent`, `ES_SeekContent`, `ES_GetTicketViews`,
  `ES_GetStoredTMD`).
- `channel/channelapp/source/gfx.c` (the HBC's EFB pixel format).
