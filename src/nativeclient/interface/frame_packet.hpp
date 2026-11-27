// The frame packet: what the render core hands to a drawing backend (three.js on the website, Vulkan in the native client). Version: VERSION below.
// ONE flat, versioned, little-endian byte layout. No pointers, no JS / three.js concepts: sections are addressed by byte offset from the start of the packet, every struct is
// plain 4- and 8-byte fields (doubles for everything numeric, so a value is bit-identical on every platform), 8-byte aligned. Documented in FRAME-PACKET.md; the C++ structs
// here are the definition, test-packet-layout.mjs pins their sizes against the JS reader.
//
// Two byte layouts are defined: the SCENE INPUT a producer (the game) hands to the core (SceneHeader ...), and the FRAME PACKET the core hands to a backend (Header ...).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace rc { namespace fp {

constexpr uint32_t PACKET_MAGIC=0x50465251u;   // "QRFP" as little-endian bytes
constexpr uint32_t SCENE_MAGIC=0x4e435251u;    // "QRCN"
constexpr uint32_t VERSION=4;   // 4: the Vulkan dialect (UniformSlot grew to 48 bytes: block, set, binding, offset, size, stride; PF_VULKAN; scene flag SCENE_VULKAN; DIALECT_VULKAN variant key bit); 3: program variants   // 3: program VARIANTS (basic / lit / sprite / line / effect programs built on request by rc_variant_build, keys and uniform sources defined by the core); 2: program table (SEC_PROGRAMS / SEC_UNIFORMS, Material::programId) and map fog volumes in the scene input

// ---- enums (numeric values are part of the format) ----------------------------------------------------------------------------------------------------------------
enum Factor:uint32_t{F_ZERO=0,F_ONE=1,F_SRC_COLOR=2,F_ONE_MINUS_SRC_COLOR=3,F_DST_COLOR=4,F_ONE_MINUS_DST_COLOR=5,F_SRC_ALPHA=6,F_ONE_MINUS_SRC_ALPHA=7,F_DST_ALPHA=8,F_ONE_MINUS_DST_ALPHA=9};
enum BlendMode:uint32_t{BM_NONE=0,BM_NORMAL=1,BM_ADD=2,BM_MULTIPLY=3,BM_INVMULTIPLY=4};
// Which faces are DRAWN. CULL_BACK = the usual one-sided surface (front faces drawn); CULL_FRONT = back faces drawn; CULL_NONE = both in one pass.
enum Cull:uint32_t{CULL_BACK=0,CULL_FRONT=1,CULL_NONE=2};
enum LightmapMode:uint32_t{LM_NONE=0,LM_PAGE=1,LM_BAKED=2};   // LM_PAGE: the lightmap page named by the draw item; LM_BAKED: the material's own external lightmap texture
enum AlphaFunc:uint32_t{AF_NONE=0,AF_GE128=1,AF_GT0=2,AF_LT128=3};
enum TcGen:uint32_t{TG_BASE=0,TG_LIGHTMAP=1,TG_ENVIRONMENT=2,TG_VECTOR=3};
enum ColorGen:uint32_t{CG_IDENTITY=0,CG_CONST=1,CG_WAVE=2,CG_VERTEX=3,CG_EXACT_VERTEX=4,CG_ONE_MINUS_VERTEX=5,CG_OTHER=6};
enum TcOpType:uint32_t{TC_SCROLL=1,TC_SCALE=2,TC_TRANSFORM=3,TC_ROTATE=4,TC_STRETCH=5,TC_TURB=6};
enum SegKind:uint32_t{SEG_AFFINE=1,SEG_TURB=2};
enum PassKind:uint32_t{PASS_WORLD=1,PASS_VIEWMODEL=2,PASS_MIRROR=3};
enum UploadKind:uint32_t{UP_VERTEX_BUFFER=1,UP_INDEX_BUFFER=2,UP_TEXTURE_RGBA8=3,UP_LIGHTMAP_RGBA8=4};

// Material::flags
enum MaterialFlag:uint32_t{
  MF_TRANSPARENT=1<<0,   // drawn in the sorted back-to-front pass; the surface alpha is used. Without it the output alpha is forced to 1 (three's OPAQUE define).
  MF_DEPTH_WRITE=1<<1,MF_DEPTH_TEST=1<<2,
  MF_ALPHA_TEST=1<<3,    // discard when the texel alpha < alphaTestRef
  MF_POLY_OFFSET=1<<4,
  MF_TWO_PASS=1<<5,      // transparent + double sided + not order independent: the core emits TWO draw items per instance, back faces first
  MF_SKY=1<<6,MF_PORTAL=1<<7,MF_HIDDEN=1<<8,MF_ENV=1<<9,MF_ANIMATED=1<<10,MF_NO_LIGHTMAP=1<<11,MF_INVERTED_SHELL=1<<12,
  MF_ORDER_INDEPENDENT=1<<13,  // the blend commutes between faces (additive / multiply), so one double sided pass is exact
  MF_SKY_BOX=1<<14             // MF_SKY: all six skybox faces exist in the archive; the box name (skyparms farbox, without _rt/_bk/_lf/_ft/_up/_dn) is the material's diffuse name. The stages are the cloud layers.
};
// Stage::flags
enum StageFlag:uint32_t{SF_DEPTH_WRITE=1<<0,SF_CLAMP=1<<1,SF_HAS_BLEND=1<<2,SF_IS_LIGHTMAP=1<<3,SF_IS_WHITE=1<<4,SF_ANIMATED_FRAMES=1<<5,SF_HAS_TC_VECTOR=1<<6};   // SF_HAS_TC_VECTOR: tcGen vector with its two vectors parsed (tcVector); all zeros is valid: both coordinates are 0, as in Quake 3

// ---- sections -----------------------------------------------------------------------------------------------------------------------------------------------------
enum Section:uint32_t{SEC_CAMERAS=0,SEC_MATERIALS,SEC_STAGES,SEC_TCOPS,SEC_FRAMES,SEC_DRAWS,SEC_UPLOADS,SEC_PASSES,SEC_PROGRAMS,SEC_UNIFORMS,SEC_BLOB,SEC_COUNT};
struct SectionRef{uint32_t offset,count;};   // byte offset from the packet start; element count (bytes for SEC_BLOB)

struct Header{
  uint32_t magic,version,flags,totalBytes;
  double time;                     // seconds: the clock every StageFrame was evaluated at
  SectionRef sec[SEC_COUNT];
};
static_assert(sizeof(Header)==112,"Header");

struct Camera{
  double view[16];                 // world -> eye, column-major (three's matrixWorldInverse)
  double proj[16];                 // eye -> clip, column-major
  double viewProj[16];             // proj * view, computed by the core in double
  double position[3];
  double nearZ,farZ,fovY;
  uint32_t flags,reserved;
};
static_assert(sizeof(Camera)==440,"Camera");
enum CameraFlag:uint32_t{CAM_NO_PVS=1u<<9};   // Camera.flags bit 9 (0x200; bits 0-7 are the pass kind, bit 8 clearDepth): skip the PVS (every world instance counts as seen; the frustum test still applies). For a whole-map draw such as the minimap bake

struct Material{
  uint32_t id;                     // = its index; the producer orders its keys, the core never reorders (these are the "material ids the core owns")
  uint32_t flags;                  // MaterialFlag
  uint32_t nameOffset,nameLen;     // shader name, blob
  uint32_t firstStage,stageCount;  // Stage table
  uint32_t blendMode;              // BlendMode (info)
  uint32_t blendSrc,blendDst,blendSrcAlpha,blendDstAlpha;   // Factor: the exact fixed-function blend of the whole surface; equation is always ADD
  uint32_t cull;                   // Cull
  uint32_t lightmapMode;           // LightmapMode
  uint32_t diffuseOffset,diffuseLen;   // texture file name of the surface's own image (blob), len 0 = none (a plain white surface)
  uint32_t envOffset,envLen;           // tcGen environment image (MF_ENV), len 0 = none
  uint32_t bakedLightmapOffset,bakedLightmapLen;   // LM_BAKED image
  uint32_t programId;              // index into the program table (SEC_PROGRAMS) of the shader program this surface is drawn with, NO_PROGRAM = none (explicit objects)
  double alphaTestRef;             // MF_ALPHA_TEST
  double polyFactor,polyUnits;     // MF_POLY_OFFSET (glPolygonOffset factor, units)
  double lightmapScale;            // final colour = diffuse * lightmap * lightmapScale (see FRAME-PACKET.md "Lightmap multiply")
  double envScale[2];
};
static_assert(sizeof(Material)==128,"Material");

constexpr uint32_t NO_PROGRAM=0xffffffffu;

struct Stage{
  uint32_t flags;                  // StageFlag
  uint32_t imageOffset,imageLen;   // texture name (blob); len 0 with SF_IS_LIGHTMAP / SF_IS_WHITE = the special images
  uint32_t frameOffset,frameCount; // animmap frame names: frameCount names of "len,bytes" in the blob (frameCount 1 = a single image)
  uint32_t blendSrc,blendDst;      // Factor (meaningful with SF_HAS_BLEND; the first stage always replaces)
  uint32_t alphaFunc,tcGen;        // AlphaFunc, TcGen
  uint32_t rgbGen,alphaGen;        // ColorGen
  uint32_t firstTcOp,tcOpCount;    // TcOp table
  uint32_t reserved;
  double animFrequency;
  double rgbGenArgs[4],alphaGenArgs[4];   // CG_CONST: rgb in [0..2] / alpha in [0]; CG_WAVE: [base, amplitude, phase, frequency] (the wave kind is in rgbWaveKind / alphaWaveKind)
  uint32_t rgbWaveKind,alphaWaveKind,reserved0,reserved1;
  double tcVector[6];              // TG_VECTOR s then t
};
static_assert(sizeof(Stage)==192,"Stage");

struct TcOp{uint32_t type,reserved;double p[6];};   // scroll: p0,p1 | scale: p0,p1 | transform: m00 m01 m10 m11 t0 t1 | rotate: p0 deg/s | stretch: kind, base, amp, phase, freq | turb: base, amp, phase, freq
static_assert(sizeof(TcOp)==56,"TcOp");

// What changes every frame, one per Stage (same index). The core evaluated everything that does not depend on the vertex.
constexpr int MAX_SEGS=6;
struct Seg{uint32_t kind,reserved;double v[6];};   // SEG_AFFINE: uv' = (v0*u + v2*v + v4, v1*u + v3*v + v5); SEG_TURB: uv += sin((vec2(pos.x+pos.z,pos.y)/1024 + v1 + v2) * 2pi) * v0
static_assert(sizeof(Seg)==56,"Seg");
struct StageFrame{
  double frameIndex;               // animmap frame to sample
  double rgb[3];                   // colour multiplier (identity / const / wave); vertex colour modes are applied by the backend
  double alpha;
  uint32_t segCount,reserved;
  Seg seg[MAX_SEGS];
};
static_assert(sizeof(StageFrame)==40+8+MAX_SEGS*56,"StageFrame");

struct DrawItem{
  uint32_t materialId,passIndex;
  uint32_t bufferId,firstIndex,indexCount;
  uint32_t cull;                   // Cull for THIS draw (a MF_TWO_PASS material yields two items: CULL_FRONT then CULL_BACK)
  int32_t lightmapId;              // -1 none
  int32_t moverIndex;              // -1 = static world (identity model)
  uint32_t objectId;               // the producer's stable id of the instance; the last sort tiebreak
  uint32_t reserved;
  double model[12];                // 3x4 row-major affine model matrix (identity for the static world)
  double depthKey;                 // the view depth the sort used
};
static_assert(sizeof(DrawItem)==144,"DrawItem");

struct Upload{uint32_t kind,id,width,height,blobOffset,byteLength,flags,reserved;};
static_assert(sizeof(Upload)==32,"Upload");

struct Pass{
  uint32_t kind,cameraIndex,firstDraw,drawCount;
  uint32_t clearDepth,clearColor,targetId,reserved;   // targetId 0xffffffff = the screen
  double clearRgba[4];
};
static_assert(sizeof(Pass)==64,"Pass");

// ---- shader programs: the GLSL a backend compiles, decided by the core (glsl_gen.hpp) --------------------------------------------------------------------------------------------------
// The text is GLSL 1.00 DIALECT (attribute / varying / texture2D / gl_FragColor, `#include <colorspace_fragment>` = the output colour-space encode of the platform): a WebGL2 backend wraps it
// (glsl-compat.js), a native one adds its own #version header and replaces the include. Programs are keyed by shader STRUCTURE: every number that changes in time or between materials of one structure
// is read from the stage table (stage_values.hpp, bound per material as `shaderStageData`), never inlined.
enum ProgramKind:uint32_t{PK_WORLD=1,PK_ANIMATED=2,PK_ENV=3,PK_SKY=4,PK_PORTAL=5,PK_BASIC=6,PK_LIT=7,PK_SPRITE=8,PK_LINE=9,PK_EFFECT=10};   // 6-10: the object pass's programs (glsl_variants.hpp), made on request, not per map material
enum ProgramFlag:uint32_t{PF_OPAQUE=1,PF_ALPHA=2,PF_FOG=4,PF_STAGE_TABLE=8,PF_VULKAN=16,PF_COLOR_RGBA=32};   // PF_COLOR_RGBA (Vulkan dialect only): the `color` vertex input is a vec4 (an RGBA vertex colour), else a vec3. PF_VULKAN: the text is the Vulkan dialect (complete GLSL 450, glsl_vulkan.hpp) and the slots carry their block / offset layout   // PF_ALPHA: PK_WORLD texel alpha + alphaRef discard; PF_OPAQUE: PK_ANIMATED output alpha forced to 1; PF_FOG: volume fog code included
enum AttributeBit:uint32_t{AB_POSITION=1,AB_NORMAL=2,AB_UV=4,AB_UV1=8,AB_VERTEX_COLOR=16,AB_CENTER=32,AB_AXIS=64,AB_CORNER=128,AB_COLOR=256,AB_INSTANCE_MATRIX=512,AB_INSTANCE_START=1024,AB_INSTANCE_END=2048,AB_ATLAS_ROW=4096,AB_INSTANCE_COLOR_START=8192,AB_INSTANCE_COLOR_END=16384,AB_INSTANCE_DISTANCE_START=32768,AB_INSTANCE_DISTANCE_END=65536};   // (the last four: the fat lines' optional inputs; the GL dialect's masks never set them, the Vulkan dialect's list every declared input)   // AB_COLOR = the `color` attribute (vec3 / vec4: the program says which), AB_INSTANCE_MATRIX = instanceMatrix (mat4)   // AB_VERTEX_COLOR = shaderVertexColor (vec4)
struct Program{
  uint32_t kind,flags;
  uint32_t vertexOffset,vertexLen,fragmentOffset,fragmentLen;   // blob
  uint32_t attributes;                                          // AttributeBit: what the vertex shader reads
  uint32_t uniformFirst,uniformCount;                           // UniformSlot table
  uint32_t fogPlanes;                                           // length of volumeFogCameraDistances (0 = no fog code)
  uint32_t stageDataVec4;                                       // length of shaderStageData in vec4 (the material's slice of the stage table), 0 = not used
  uint32_t reserved;
};
static_assert(sizeof(Program)==48,"Program");
enum UniformType:uint32_t{UT_FLOAT=1,UT_VEC2=2,UT_VEC3=3,UT_VEC4=4,UT_MAT3=5,UT_MAT4=6,UT_SAMPLER2D=7,UT_SAMPLER2D_ARRAY=8,UT_VEC4_ARRAY=9,UT_FLOAT_ARRAY=10,UT_VEC3_ARRAY=11,UT_BOOL=12};   // UT_BOOL: Vulkan dialect only, a `bool` uniform is a uint32 0 / 1 in the block
// where a uniform's value comes from (arg refines it)
enum UniformSource:uint32_t{
  US_PLATFORM=1,          // projectionMatrix / modelViewMatrix / modelMatrix / viewMatrix / cameraPosition: the executor's own per-pass / per-draw values, by name
  US_CLOCK=2,             // seconds since the map's animation clock started (shaderTime / time)
  US_STAGE_DATA=3,        // the material's slice of the stage table
  US_STAGE_FRAMES=4,      // arg = stage index: the stage's animmap frames (or its single image)
  US_LIGHTMAP=5,          // the lightmap the draw uses (the page, or the material's baked one)
  US_LIGHTMAP_SCALE=6,    // Material::lightmapScale
  US_LIGHTMAP_INTENSITY=7,// Material::lightmapScale (env surfaces: intensity without 1/pi)
  US_DIFFUSE=8,           // the material's own image
  US_ALPHA_REF=9,         // Material::alphaTestRef
  US_ENV_IMAGE=10,US_ENV_SCALE=11,US_HAS_DIFFUSE=12,US_HAS_LIGHTMAP=13,
  US_SKY_FACE=14,         // arg = face 0..5 in the order rt bk lf ft up dn
  US_SKY_HAS_BOX=15,US_SKY_FAST=16,
  US_FOG_ON=17,US_FOG_DISTANCES=18,   // the map's fog switch; camera distance to every fog plane (count = Program::fogPlanes)
  US_PORTAL_MATRIX=19,US_PORTAL_TARGET=20,   // textureMatrix of the portal / mirror pass, the pass's render target
  // ---- the object pass's programs (version 3): values of a material, of the scene and of the pass; `arg` = array index where the name has one ----
  US_MAT_COLOR=21,US_MAT_EMISSIVE=22,       // diffuse (vec3, linear), emissive colour * emissiveIntensity (vec3)
  US_MAT_ROUGHNESS=23,US_MAT_METALNESS=24,US_MAT_OPACITY=25,
  US_MAT_MAP=26,US_MAT_MAP_TRANSFORM=27,    // the colour map (uv 0) and its uv transform (mat3)
  US_MAT_EMISSIVE_MAP=28,US_MAT_EMISSIVE_MAP_TRANSFORM=29,
  US_MAT_ALPHA_TEST=30,US_MAT_LIGHTMAP_INTENSITY=31,   // (the lightmap texture itself is US_LIGHTMAP)
  US_SPRITE_ROTATION=32,US_SPRITE_CENTER=33,
  US_SCENE_FOG_COLOR=34,US_SCENE_FOG_DENSITY=35,US_SCENE_FOG_NEAR=36,US_SCENE_FOG_FAR=37,   // the scene's fog (FogExp2 / Fog), colour already in the pass's output space
  US_LIGHT_AMBIENT=38,US_LIGHT_PROBE=39,    // ambientLightColor (vec3), lightProbe (9 x vec3)
  US_LIGHT_DIR_DIRECTION=40,US_LIGHT_DIR_COLOR=41,US_LIGHT_HEMI_DIRECTION=42,US_LIGHT_HEMI_SKY=43,US_LIGHT_HEMI_GROUND=44,   // arg = light index; direction in view space
  US_GLOW_POWERUPS=45,US_GLOW_TIME=46,      // weaponPowerups (vec3 quad / enviro / haste, 0 or 1), weaponPowerupTime (seconds)
  US_OUTPUT_SRGB=47,US_NORMAL_MATRIX=48,US_ORTHOGRAPHIC=49,   // per pass: 1 when the pass encodes to sRGB; per draw: normalMatrix (mat3); per pass: isOrthographic (bool)
  US_OVERLAY_MAP=51,US_OVERLAY_S=52,US_OVERLAY_T=53,   // a fog overlay stage (basic family): its image and the two texcoord vectors (game axes)
  US_EFFECT=50                            // an effect program's own parameter: arg = index in the effect's parameter list (glsl_variants.hpp effectParams)
};
// The last six fields are filled for the Vulkan dialect only (zero otherwise): where the uniform lives in the fixed descriptor layout. `count` is the element count of an array (1 for a scalar), `stride` the std140 array stride
// in bytes (0 for a non-array), `size` the bytes the uniform occupies (a sampler: 0). block: BlockId. Sampler slots: set / binding of the combined image sampler; every other slot: set / binding of its block + the byte offset in it.
struct UniformSlot{uint32_t nameOffset,nameLen,type,count,source,arg,block,set,binding,offset,size,stride;};
static_assert(sizeof(UniformSlot)==48,"UniformSlot");
// The fixed Vulkan descriptor layout (glsl_vulkan.hpp): set 0 = pass (block binding 0, lights binding 1), set 1 = draw (binding 0), set 2 = material (block binding 0, stage table binding 1, samplers from binding 2).
enum BlockId:uint32_t{BLK_NONE=0,BLK_PASS=1,BLK_LIGHT=2,BLK_DRAW=3,BLK_MATERIAL=4,BLK_STAGE=5,BLK_SAMPLER=6};
constexpr uint32_t VK_SET_PASS=0,VK_SET_DRAW=1,VK_SET_MATERIAL=2,VK_BIND_PASS=0,VK_BIND_LIGHT=1,VK_BIND_DRAW=0,VK_BIND_MATERIAL=0,VK_BIND_STAGE=1,VK_BIND_FIRST_SAMPLER=2;

// ---- scene input (producer -> core) -------------------------------------------------------------------------------------------------------------------------------
struct SceneHeader{
  uint32_t magic,version,flags,poseOffset;   // flags: 1 = r_mapFx (external lightmaps / vector overlays / lightmap+vector stages kept)
  double time;
  uint32_t nCameras,nKeys,nDraws,nUploads;
  uint32_t blobBytes,filesOffset,filesLen,scriptOffset;   // blob: shader names, then the '\n' separated lowercase archive file list (files*), then the shader script text (scriptOffset, scriptLen)
  uint32_t scriptLen,poseCount;                           // poses: Pose[poseCount] at blob offset poseOffset
  uint32_t fogOffset,fogCount;                            // map fog volumes at blob offset fogOffset: FogVolume[fogCount], then the planes (4 doubles each: normal xyz, d), 0 volumes = a map without fog
};
static_assert(sizeof(SceneHeader)==72,"SceneHeader");
// One fog brush: the colour / opaque distance of its shader's fogparms (or the built-in fallback), and its planes (a point is inside when n.p - d <= 0 for every plane).
struct FogVolume{double color[3],depth;uint32_t planeFirst,planeCount;};
static_assert(sizeof(FogVolume)==40,"FogVolume");
// then: Camera[nCameras] (viewProj filled by the core), SceneKey[nKeys], SceneDraw[nDraws], Upload[nUploads], blob[blobBytes]
enum SceneKeyFlag:uint32_t{SK_BUTTON=1,SK_HAS_LIGHTMAP_PAGE=2,SK_BAKED_LIGHTMAP=4,SK_BSP_SKY=8,SK_OVERLAY_BAKED_FX=16,SK_EXPLICIT=32,SK_PORTAL_ORDINARY=64};   // PORTAL_ORDINARY: a portal / mirror shader compiled as the ordinary surface it is without a live view (world_scene.hpp: the material a portal batch falls back to)
// SK_EXPLICIT (phase E4-E6: marks, sprays, the view weapon, ghost / peer / pickup models): an object that has NO shader script. nameOffset / nameLen then point at an ExplicitMaterial in the blob
// (nameLen == sizeof): the producer states the material, the core only orders, double-passes and carries it. forceSinglePass is three's flag (ShaderMaterials and decals set it).
struct ExplicitMaterial{
  uint32_t flags;                  // MaterialFlag bits the producer states: TRANSPARENT, DEPTH_WRITE, DEPTH_TEST, ALPHA_TEST, POLY_OFFSET (the rest is derived)
  uint32_t blendSrc,blendDst,blendSrcAlpha,blendDstAlpha;   // Factor
  uint32_t imageOffset,imageLen;   // the object's texture name (blob), len 0 = none
  uint32_t forceSinglePass,reserved,reserved1;
  double alphaTestRef,polyFactor,polyUnits;
};
static_assert(sizeof(ExplicitMaterial)==64,"ExplicitMaterial");
// A pose is the game's own state of a moving thing, three.js Object3D style: local = translate(pos) * rotateXYZ(euler) * scale, world = parent's world * local (parent < own index, -1 = none).
// A draw whose moverIndex is a pose index is placed at world(pose) * draw.model (draw.model = the rest placement); any other moverIndex keeps draw.model as the producer's final matrix.
// Movers: pos = offset, euler = the one rotating axis' angle. Rigid models (avatars: body yaw, held weapon pitch as a child pose) use the same records.
struct Pose{double pos[3],euler[3],scale;int32_t parent;uint32_t reserved;};
static_assert(sizeof(Pose)==64,"Pose");
struct SceneKey{uint32_t nameOffset,nameLen,side,flags;};   // side: 0 front, 1 back, 2 double (three's constants); the shader name (blob) is looked up in the script text
static_assert(sizeof(SceneKey)==16,"SceneKey");
struct SceneDraw{
  uint32_t keyIndex,passIndex,bufferId,firstIndex,indexCount,objectId;
  int32_t lightmapId,moverIndex;
  double model[12];
};
static_assert(sizeof(SceneDraw)==128,"SceneDraw");

// ---- resident world (cl_wasmrender phase C): visibility ----------------------------------------------------------------------------------------------------------------
// Loaded once per map (rc_world_load): every cullable instance with its world box and the BSP clusters it can be seen from, plus the map's visibility table. Every frame
// rc_world_cull(cluster, viewProj) answers which instances are visible: no per-frame instance list crosses the boundary in either direction.
constexpr uint32_t WORLD_MAGIC=0x4c575251u;   // "QRWL"
struct WorldHeader{uint32_t magic,version,nInstances,nClusterIds,numClusters,clusterBytes,visBytes,reserved;};
static_assert(sizeof(WorldHeader)==32,"WorldHeader");
enum WorldInstanceFlag:uint32_t{WI_MOVING=1};   // a moving brush model: its box changes every frame, so it is never frustum culled (PVS only); three.js still culls it by its bounding sphere
struct WorldInstance{double bmin[3],bmax[3];uint32_t clusterOffset,clusterCount,flags,reserved;};
static_assert(sizeof(WorldInstance)==64,"WorldInstance");
// then: uint32 clusterIds[nClusterIds] (-1 = outside the vis structure = always visible), uint8 vis[visBytes] (numClusters x clusterBytes)

}}  // namespace rc::fp
