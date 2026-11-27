#define _USE_MATH_DEFINES
#include "nc_world.h"
#include "nc_shader.h"
#include "nc_json.h"
#include <string_view>
#include "render_core.h"
#include "qrazy_core_api.h"
#include "frame_packet.hpp"
#include "object_state.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>
#include <future>
#include <map>
#include <stdexcept>
#include <vector>

using namespace rc::fp;
using rc::os::ObjectState;

namespace nc {

// ---- the core's plan records. frame_plan.hpp is internal to the core build, so the layouts are repeated here and checked against rc_plan_sizeof() at load.
struct PlanIn { Camera main, view; double mainWorld[16]; double timeMs; int32_t mainCluster; uint32_t flags; uint32_t mainMask, mirrorMask, portalMask, viewMask; };
struct PlanPass { Camera camera; double textureMatrix[16]; double cullViewProj[16]; uint32_t kind; int32_t surface; uint32_t flags, layerMask; int32_t pvsCluster, skipInstance; uint32_t visOffset, targetSize; };
static_assert(sizeof(PlanIn) == 1040, "PlanIn"); static_assert(sizeof(PlanPass) == 728, "Pass");
enum { PI_VIEW = 1, PI_MIRRORS = 2 };
enum { PF_WORLD = 1, PF_CLEAR_COLOR = 2, PF_CLEAR_DEPTH = 4, PF_LINEAR_TARGET = 8, PF_APERTURE = 16 };
struct FrameEntry { uint32_t kind, index, phase, reserved; double depthKey; };
static_assert(sizeof(FrameEntry) == 24, "FrameEntry");
constexpr int FROM_CAMERA = -2;
constexpr uint32_t MINIMAP_LAYER = 8u;

static std::string lower(std::string s) { return lowerStr(std::move(s)); }
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static const uint8_t* putCore(std::string_view bytes) {
  if (bytes.size() >= INT_MAX) fatal("core input too large");
  auto* p = static_cast<uint8_t*>(rc_alloc(static_cast<int>(bytes.size()) + 1));
  if (!p) fatal("could not allocate core input");
  if (!bytes.empty()) memcpy(p, bytes.data(), bytes.size());
  p[bytes.size()] = 0;
  return p;
}
static const uint8_t* putCore(const std::vector<uint8_t>& bytes) {
  return putCore(bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
#define RC(call) do { int r_ = (call); if (r_ != 0) fatal("core call failed: %s -> %d (%s)", #call, r_, rc_vulkan_error()); } while (0)

typedef std::array<double, 16> M4;
static M4 ident() { M4 m = {}; m[0] = m[5] = m[10] = m[15] = 1; return m; }

// the projection (from the camera's clips), position and fov
static void finishCamera(CameraIn& c, const double eye[3], double aspect, double fovY) {
  const double nz = c.nearZ, fz = c.farZ, t = 1 / tan(fovY * M_PI / 360); M4 p = {}; p[0] = t / aspect; p[5] = t; p[10] = (fz + nz) / (nz - fz); p[11] = -1; p[14] = 2 * fz * nz / (nz - fz);
  memcpy(c.proj, p.data(), 128); memcpy(c.pos, eye, 24); c.fovY = fovY;
}

CameraIn makeCamera(const double eye[3], double yawDeg, double pitchDeg, double aspect, double fovY) {
  const double yaw = yawDeg * M_PI / 180, pit = pitchDeg * M_PI / 180;
  const double f[3] = {cos(yaw) * cos(pit), sin(pit), -sin(yaw) * cos(pit)};
  return makeCameraFwd(eye, f, aspect, fovY);
}
CameraIn makeCameraFwd(const double eye[3], const double f[3], double aspect, double fovY) {
  double up0[3] = {0, 1, 0};
  double r[3] = {f[1] * up0[2] - f[2] * up0[1], f[2] * up0[0] - f[0] * up0[2], f[0] * up0[1] - f[1] * up0[0]}; const double rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); for (double& x : r) x /= rl;
  double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  CameraIn c; M4 v = ident();
  for (int i = 0; i < 3; ++i) { v[i * 4 + 0] = r[i]; v[i * 4 + 1] = u[i]; v[i * 4 + 2] = -f[i]; }
  v[12] = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]); v[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]); v[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  memcpy(c.view, v.data(), 128); finishCamera(c, eye, aspect, fovY); return c;
}
CameraIn makeCameraView(const double eye[3], const double quat[4], double aspect, double fovY) {
  CameraIn c; if (rc_camera_view(eye, quat, c.view) != 0) fatal("rc_camera_view failed");
  const double* clips = rc_camera_clips(); c.nearZ = clips[0]; c.farZ = clips[1];
  finishCamera(c, eye, aspect, fovY); return c;
}

// the inverse of a rigid view matrix
static void invertRigid(const double v[16], double out[16]) {
  for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) out[c * 4 + r] = v[r * 4 + c];
  for (int r = 0; r < 3; ++r) out[12 + r] = -(out[r] * v[12] + out[4 + r] * v[13] + out[8 + r] * v[14]);
  out[3] = out[7] = out[11] = 0; out[15] = 1;
}

// ---- programs
struct Attr { int loc; VkFormat format; uint32_t offset; };
struct Prog {
  VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE; VkDescriptorSetLayout set2 = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE;
  std::vector<Attr> attrs; std::vector<UniformSlot> slots; uint32_t stageVec4 = 0; bool ok = false; uint32_t stride = 56; const char* names = nullptr; std::vector<std::pair<std::string, int>> inputs;
};
struct MatGpu { VkDescriptorSet set = VK_NULL_HANDLE; size_t matOff = 0, stageOff = 0; bool ready = false; std::string diffuse; uint32_t stamp = 0; int surface = -1; };
struct SurfaceTarget { Image color, depth; VkFramebuffer fb = VK_NULL_HANDLE; int size = 0; };
struct Grabbed { std::vector<FrameEntry> ent; std::vector<uint8_t> blocks; std::vector<ObjectState> states; std::vector<DrawItem> draws; };

static const int MAX_PASS_SLOTS = 9, MINIMAP_SLOT = MAX_PASS_SLOTS - 1, GUN_SLOT = MINIMAP_SLOT - 1, PASS_SLOT = 16384, LIGHT_SLOT = 512, DRAW_STRIDE = 256, DRAW_CAP = 16384;

struct GunPass { Grabbed list; uint32_t drawBase = 0; bool on = false; };
struct TexturePixels {
  std::string name;
  std::vector<uint8_t> rgba;
  int32_t size[2] = {};
  Image replacement;

  bool load(const FileSource& fs, bool mip, int level) {
    int w = 0, h = 0;
    if (!loadImage(fs, name, rgba, w, h)) return false;
    size[0] = w;
    size[1] = h;
    if (mip && (rgba.size() > INT_MAX || rc_picmip_rgba(rgba.data(), static_cast<int>(rgba.size()), size, level) != 0))
      throw std::runtime_error("core picmip failed for " + name);
    return true;
  }

  Image upload(const Context& c) const {
    return makeTexture(c, rgba.data(), size[0], size[1], true, !getenv("QRAZY_NC_LINEARTEX"));
  }
};

struct World::Impl {
  std::vector<PlanPass> passes;
  Grabbed lists[MAX_PASS_SLOTS], objectList;
  std::vector<const PlanPass*> portalPasses;
  GunPass gun;

  Context c; const FileSource* fs = nullptr; VkRenderPass rpScreen = VK_NULL_HANDLE, rpTarget = VK_NULL_HANDLE; std::shared_ptr<void> fxp;   // the weapon effects (nc_world_fx.inl)
  WorldStats st; int picmip = 0; bool mapFx = true, volumeFog = true, fastSky = false; bool mirrors = true; bool worldless = false; bool gunShown = false; int fogMode = 0; float fogColor[3] = {0, 0, 0}, fogDensity = 0, clearColor[3] = {0, 0, 0};   // worldless: no map, objects only (the runway)
  std::string entities;
  // core tables copied out of the first packet
  double mapBounds[6] = {-1000, 0, -1000, 1000, 200, 1000};
  std::string blob; std::vector<Material> mats; std::vector<Stage> stages; std::vector<Prog> progs; std::vector<MatGpu> matGpu;
  std::vector<int32_t> batchRec; int batchW = 0; std::vector<uint32_t> stageOffsets; bool stageOk = false;
  // GPU
  Buf vbuf, ibuf, passBuf, lightBuf, drawBuf, matBuf; size_t matBump = 0;
  VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE, set1Layout = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkSampler samp = VK_NULL_HANDLE, sampClamp = VK_NULL_HANDLE, sampFlat = VK_NULL_HANDLE;   // sampFlat: repeat, linear, level 0 only (a record material with wrapS 10497: the trigger overlay labels)
  VkDescriptorSet set0[MAX_PASS_SLOTS], set1 = VK_NULL_HANDLE;
  Image white, grey; std::vector<Image> lmPages; std::map<std::pair<std::string, bool>, Image> texCache; std::vector<SurfaceTarget> targets;
  std::map<std::pair<uint32_t, uint32_t>, VkPipeline> pipes;
  uint32_t frameStamp = 0; int nSurfaces = 0; bool loggedObjects = false;
  struct Obj; std::unique_ptr<Obj> obj;
  int wantedPicmip = 0;
  size_t refreshCacheSize = 0;
  std::atomic<bool> cancelPicmip{false};
  std::future<std::vector<TexturePixels>> picmipJob;
  std::vector<TexturePixels> picmipUploads;
  size_t uploadedPicmip = 0;

  const Image& texByName(const std::string& nameIn, bool mip = true) {   // mip: r_picmip applies (not to baked lightmaps)
    const auto key = std::make_pair(lower(nameIn), mip);
    auto it = texCache.find(key); if (it != texCache.end()) return it->second;
    TexturePixels pixels{key.first};
    if (pixels.load(*fs, mip, picmip)) {
      ++st.texturesLoaded;
      return texCache[key] = pixels.upload(c);
    }
    ++st.texturesMissing; if (st.texturesMissing <= 12) logf("missing texture: %s", key.first.c_str());
    return texCache[key] = grey;
  }
  std::string blobStr(uint32_t off, uint32_t len) const { return std::string(blob.data() + off, len); }
  bool texFileExists(const std::string& stem) const { for (const char* e : {".tga", ".jpg", ".jpeg", ".png"}) if (fs->exists(stem + e)) return true; return false; }
  std::string diffuseNameOf(const Material& m) const {
    std::string d = blobStr(m.diffuseOffset, m.diffuseLen); if (!d.empty() || m.stageCount != 0) return d;
    const std::string n = lower(blobStr(m.nameOffset, m.nameLen)); return texFileExists(n) ? n : d;
  }

  void buildProgram(Prog& P, const Program& p, const UniformSlot* slots, const uint8_t* b, int idx, bool isWorld = true);
  void prepareMaterial(uint32_t id);
  void writeMaterialDescriptors(uint32_t id);
  void fillMaterial(uint32_t id, const double* stageFrames, const PlanPass* portalPass);
  VkPipeline pipelineFor(uint32_t progId, const ObjectState& s);
  void grab(Grabbed& list, bool packet);
  void createTargets();
};

static void putSlot(uint8_t* base, const UniformSlot& u, const double* v, int n) {
  uint8_t* d = base + u.offset;
  switch (u.type) {
    case UT_BOOL: *(uint32_t*)d = v[0] != 0; break;
    case UT_MAT3: for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) ((float*)d)[c * 4 + r] = (float)v[c * 3 + r]; break;
    default: for (int i = 0; i < n && (uint32_t)(i * 4) < (u.size ? u.size : 4); ++i) ((float*)d)[i] = (float)v[i]; break;
  }
}

// The world's interleaved vertex (the batcher's 14 doubles narrowed to float): position 0, uv 12, uv1 20, normal 28, shaderVertexColor 40, stride 56.
static bool worldAttribute(const std::string& name, VkFormat& fmt, uint32_t& off) {
  if (name == "position") { fmt = VK_FORMAT_R32G32B32_SFLOAT; off = 0; } else if (name == "uv") { fmt = VK_FORMAT_R32G32_SFLOAT; off = 12; } else if (name == "uv1") { fmt = VK_FORMAT_R32G32_SFLOAT; off = 20; }
  else if (name == "normal") { fmt = VK_FORMAT_R32G32B32_SFLOAT; off = 28; } else if (name == "shaderVertexColor") { fmt = VK_FORMAT_R32G32B32A32_SFLOAT; off = 40; } else return false;
  return true;
}

void World::Impl::buildProgram(Prog& P, const Program& p, const UniformSlot* slots, const uint8_t* b, int idx, bool isWorld) {
  P.names = (const char*)b;
  const std::string vs((const char*)b + p.vertexOffset, p.vertexLen), fs_((const char*)b + p.fragmentOffset, p.fragmentLen);
  std::vector<uint32_t> sv, sf; std::string log;
  if (const char* dd = getenv("QRAZY_NC_DUMPGLSL")) { for (int k = 0; k < 2; ++k) { FILE* f = fopen((std::string(dd) + "/prog" + std::to_string(idx) + (k ? ".frag" : ".vert")).c_str(), "wb"); if (f) { const std::string& t = k ? fs_ : vs; fwrite(t.data(), 1, t.size(), f); fclose(f); } } }   // diagnostic: the core's GLSL of every program
  if (!compileGlsl(ShaderStage::Vertex, vs, sv, log)) { logf("program %d vertex shader failed:\n%s", idx, log.c_str()); return; }
  if (!compileGlsl(ShaderStage::Fragment, fs_, sf, log)) { logf("program %d fragment shader failed:\n%s", idx, log.c_str()); return; }
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = sv.size() * 4; ci.pCode = sv.data(); NC_CK(vkCreateShaderModule(c.dev, &ci, nullptr, &P.vs));
  ci.codeSize = sf.size() * 4; ci.pCode = sf.data(); NC_CK(vkCreateShaderModule(c.dev, &ci, nullptr, &P.fs));
  // vertex inputs: the location of an input = the sum of the locations of the lower set bits of the program's attribute mask, in the core's table order (NATIVE-CLIENT.md 8.2)
  int loc = 0; uint32_t floats = 0;
  for (int i = 0, n = rc_vulkan_attribute_count(); i < n; ++i) {
    if (!(p.attributes & (uint32_t)rc_vulkan_attribute(i, 0))) continue;
    const std::string nm = rc_vulkan_attribute_name(i);
    if (isWorld) {
      VkFormat f; uint32_t o;
      if (!worldAttribute(nm, f, o)) { logf("program %d reads vertex input '%s', which the world path has no data for", idx, nm.c_str()); return; }
      P.attrs.push_back({loc, f, o});
    } else {   // an object program: its inputs interleaved as floats in the core's table order; a mat4 (instancing) is not supported yet
      int nf = rc_vulkan_attribute(i, 1); if (nm == "color" && (p.flags & 32u)) nf = 4;
      if (rc_vulkan_attribute(i, 2) != 1) { logf("object program %d reads '%s' (%d locations): not supported yet", idx, nm.c_str(), rc_vulkan_attribute(i, 2)); return; }
      const VkFormat f = nf == 1 ? VK_FORMAT_R32_SFLOAT : nf == 2 ? VK_FORMAT_R32G32_SFLOAT : nf == 3 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
      P.attrs.push_back({loc, f, floats * 4}); P.inputs.push_back({nm, nf}); floats += (uint32_t)nf;
    }
    loc += rc_vulkan_attribute(i, 2);
  }
  if (!isWorld) P.stride = floats * 4;
  P.stageVec4 = p.stageDataVec4; std::vector<std::pair<uint32_t, uint32_t>> samplers;
  for (uint32_t k = 0; k < p.uniformCount; ++k) { const UniformSlot& u = slots[p.uniformFirst + k]; P.slots.push_back(u); if (u.block == BLK_SAMPLER) samplers.push_back({u.binding, u.count ? u.count : 1}); }
  std::vector<VkDescriptorSetLayoutBinding> bs; for (uint32_t k = 0; k < 2; ++k) bs.push_back({k, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
  for (auto& s : samplers) bs.push_back({s.first, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, s.second, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
  VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = (uint32_t)bs.size(); li.pBindings = bs.data(); NC_CK(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &P.set2));
  VkDescriptorSetLayout sets[3] = {set0Layout, set1Layout, P.set2}; VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 3; pl.pSetLayouts = sets;
  NC_CK(vkCreatePipelineLayout(c.dev, &pl, nullptr, &P.layout)); P.ok = true;
}

void World::Impl::prepareMaterial(uint32_t id) {
  MatGpu& g = matGpu[id]; if (g.ready) return; g.ready = true; const Material& m = mats[id]; if (m.programId == NO_PROGRAM || !progs[m.programId].ok) return; const Prog& P = progs[m.programId];
  g.diffuse = diffuseNameOf(m);
  const size_t stageBytes = ((std::max<size_t>(P.stageVec4 * 16, 16) + 255) / 256) * 256;
  g.matOff = matBump; matBump += 512; g.stageOff = matBump; matBump += stageBytes; if (matBump > matBuf.size) fatal("material buffer full (%zu of %zu)", matBump, matBuf.size);
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &P.set2; NC_CK(vkAllocateDescriptorSets(c.dev, &ai, &g.set));
  writeMaterialDescriptors(id);
}

void World::Impl::writeMaterialDescriptors(uint32_t id) {
  const MatGpu& g = matGpu[id]; const Material& m = mats[id]; const Prog& P = progs[m.programId];
  std::vector<VkWriteDescriptorSet> ws; VkDescriptorBufferInfo bi[2] = {{matBuf.buf, g.matOff, 512}, {matBuf.buf, g.stageOff, std::max<size_t>(P.stageVec4 * 16, 16)}}; std::vector<std::vector<VkDescriptorImageInfo>> ii; ii.reserve(P.slots.size() + 1);
  for (uint32_t b = 0; b < 2; ++b) { VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = g.set; w.dstBinding = b; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi[b]; ws.push_back(w); }
  for (auto& u : P.slots) {
    if (u.block != BLK_SAMPLER) continue;
    const Image* img = &white; VkSampler smp = samp; std::vector<const Image*> frames;
    if (u.source == US_LIGHTMAP || u.source == US_SKY_FACE) smp = sampClamp;
    switch (u.source) {
      case US_DIFFUSE: if (!g.diffuse.empty()) img = &texByName(g.diffuse); break;
      case US_LIGHTMAP:
        if (getenv("QRAZY_NC_NOLM")) break;   // diagnostic: every lightmap is white
        if (m.lightmapMode == LM_PAGE) { const int page = batchRec[(size_t)id * batchW + 14]; if (page >= 0 && page < (int)lmPages.size()) img = &lmPages[page]; }
        else if (m.lightmapMode == LM_BAKED && m.bakedLightmapLen) img = &texByName(blobStr(m.bakedLightmapOffset, m.bakedLightmapLen), false);
        break;
      case US_STAGE_FRAMES: {
        const uint32_t si = m.firstStage + u.arg; if (u.arg >= m.stageCount || si >= stages.size()) break; const Stage& s = stages[si];
        if (s.flags & SF_CLAMP) smp = sampClamp;
        if (s.frameCount > 1) {   // animmap: frameCount names of (uint32 length, bytes); the shader picks the frame from the stage table
          size_t at = s.frameOffset;
          for (uint32_t f = 0; f < s.frameCount && at + 4 <= blob.size(); ++f) { uint32_t len; memcpy(&len, blob.data() + at, 4); at += 4; if (at + len > blob.size()) break; frames.push_back(&texByName(blobStr((uint32_t)at, len))); at += len; }
        } else if (s.imageLen) img = &texByName(blobStr(s.imageOffset, s.imageLen));
        break; }
      case US_PORTAL_TARGET: if (g.surface >= 0 && g.surface < (int)targets.size()) img = &targets[g.surface].color; smp = sampClamp; break;
      case US_ENV_IMAGE: if (m.envLen) img = &texByName(blobStr(m.envOffset, m.envLen)); break;
      case US_SKY_FACE: { static const char* fx[6] = {"_rt", "_bk", "_lf", "_ft", "_up", "_dn"}; if (m.diffuseLen) img = &texByName(blobStr(m.diffuseOffset, m.diffuseLen) + fx[u.arg % 6]); break; }
      default: img = &grey; break;
    }
    ii.emplace_back(); for (uint32_t k = 0; k < (u.count ? u.count : 1); ++k) { const Image* im = frames.empty() ? img : frames[std::min<size_t>(k, frames.size() - 1)]; ii.back().push_back({smp, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}); }
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = g.set; w.dstBinding = u.binding; w.descriptorCount = u.count ? u.count : 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = ii.back().data(); ws.push_back(w);
  }
  vkUpdateDescriptorSets(c.dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
}

void World::Impl::fillMaterial(uint32_t id, const double* stageFrames, const PlanPass* portalPass) {
  MatGpu& g = matGpu[id]; const Material& m = mats[id]; const Prog& P = progs[m.programId];
  uint8_t* mb = matBuf.ptr + g.matOff; memset(mb, 0, 512);
  for (auto& u : P.slots) {
    if (u.block != BLK_MATERIAL) continue; double v[4] = {0, 0, 0, 0}; int n = 1;
    switch (u.source) {
      case US_LIGHTMAP_SCALE: case US_LIGHTMAP_INTENSITY: v[0] = m.lightmapScale; break;
      case US_ALPHA_REF: v[0] = m.alphaTestRef; break;
      case US_ENV_SCALE: v[0] = m.envScale[0]; v[1] = m.envScale[1]; n = 2; break;
      case US_PORTAL_MATRIX: if (portalPass) {   // the core's texture matrix is in the browser's convention (v = 0 at the bottom of the target); the vertex wrapper flips y in every pass, targets included, so the target's row 0 is the top of the view: sample with v' = 1 - v, i.e. row 1 := row 3 - row 1 (column-major)
        double tm[16]; memcpy(tm, portalPass->textureMatrix, sizeof tm); for (int c = 0; c < 4; ++c) tm[c * 4 + 1] = portalPass->textureMatrix[c * 4 + 3] - portalPass->textureMatrix[c * 4 + 1];
        putSlot(mb, u, tm, 16); } continue;
      case US_HAS_DIFFUSE: v[0] = !g.diffuse.empty(); break;
      case US_HAS_LIGHTMAP: v[0] = m.lightmapMode != LM_NONE; break;
      case US_SKY_HAS_BOX: v[0] = (m.flags & MF_SKY_BOX) != 0; break;
      default: continue;
    }
    putSlot(mb, u, v, n);
  }
  if (P.stageVec4 && stageOk && stageFrames && m.stageCount) {
    const uint32_t a = stageOffsets[m.firstStage], b = stageOffsets[m.firstStage + m.stageCount]; float* d = (float*)(matBuf.ptr + g.stageOff);
    memset(d, 0, P.stageVec4 * 16); for (uint32_t i = a; i < b && (i - a) < P.stageVec4 * 4; ++i) d[i - a] = (float)stageFrames[i];
  }
}

VkPipeline World::Impl::pipelineFor(uint32_t progId, const ObjectState& s) {
  const auto key = std::make_pair(progId, s.pipelineKey); auto it = pipes.find(key); if (it != pipes.end()) return it->second;
  const Prog& P = progs[progId];
  if (getenv("QRAZY_NC_DUMPGLSL")) logf("pipeline: program %u flags 0x%x blendMode %u blend %s src %u dst %u eq %u | alpha src %u dst %u | depth test %d write %d func %u | cull %u mask 0x%x", progId, (unsigned)s.flags, s.blendMode, (s.flags & rc::os::OS_BLEND_ENABLED) ? "on" : "off", s.blendSrc, s.blendDst, s.blendEq, s.blendSrcAlpha, s.blendDstAlpha, (s.flags & rc::os::OS_DEPTH_TEST) != 0, (s.flags & rc::os::OS_DEPTH_WRITE) != 0, s.depthFunc, s.cull, s.colorMask);
  VkPipelineShaderStageCreateInfo st2[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  st2[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st2[0].module = P.vs; st2[0].pName = "main"; st2[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st2[1].module = P.fs; st2[1].pName = "main";
  VkVertexInputBindingDescription vb = {0, P.stride, VK_VERTEX_INPUT_RATE_VERTEX}; std::vector<VkVertexInputAttributeDescription> va; for (auto& a : P.attrs) va.push_back({(uint32_t)a.loc, 0, a.format, a.offset});
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = (uint32_t)va.size(); vi.pVertexAttributeDescriptions = va.data();
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = s.topology == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : s.topology == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
  static const bool noCull = getenv("QRAZY_NC_NOCULL") != nullptr;   // diagnostic: draw both faces everywhere
  rs.cullMode = (noCull || s.cull == 2) ? VK_CULL_MODE_NONE : s.cull == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
  // Verified on a real GPU with hangtime-df: frontFace 0 is COUNTER_CLOCKWISE (NATIVE-CLIENT.md section 7). Section 8.7 says CLOCKWISE; with that most world faces are culled. QRAZY_NC_CW selects it, for diagnosis.
  static const bool cw = getenv("QRAZY_NC_CW") != nullptr;
  rs.frontFace = (s.frontFace == 0) == cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;   // NATIVE-CLIENT.md 8.7: the vertex wrapper flips y, so the packet's winding is clockwise on screen
  rs.depthBiasEnable = (s.flags & rc::os::OS_POLYGON_OFFSET) != 0;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = (s.flags & rc::os::OS_DEPTH_TEST) != 0; ds.depthWriteEnable = (s.flags & rc::os::OS_DEPTH_WRITE) != 0; ds.depthCompareOp = (VkCompareOp)s.depthFunc;
  VkPipelineColorBlendAttachmentState ba = {}; ba.blendEnable = (s.flags & rc::os::OS_BLEND_ENABLED) != 0; ba.srcColorBlendFactor = (VkBlendFactor)s.blendSrc; ba.dstColorBlendFactor = (VkBlendFactor)s.blendDst; ba.colorBlendOp = (VkBlendOp)s.blendEq;
  ba.srcAlphaBlendFactor = (VkBlendFactor)s.blendSrcAlpha; ba.dstAlphaBlendFactor = (VkBlendFactor)s.blendDstAlpha; ba.alphaBlendOp = (VkBlendOp)s.blendEqAlpha; ba.colorWriteMask = s.colorMask;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
  VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 4; dy.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount = 2; gi.pStages = st2; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs;
  gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = P.layout; gi.renderPass = rpScreen;
  VkPipeline pipe; NC_CK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe)); ++st.pipelines; return pipes[key] = pipe;
}

// The core's lists are overwritten by the next pass: keep a copy of this pass's.
void World::Impl::grab(Grabbed& L, bool packet) {
  const int n = rc_frame_entry_count(); const FrameEntry* e = (const FrameEntry*)rc_frame_entries(); L.ent.assign(e, e + n);
  const uint8_t* b = rc_frame_draw_blocks(); L.blocks.assign(b, b + (size_t)n * 192); const ObjectState* s = (const ObjectState*)rc_frame_states(); L.states.assign(s, s + n);
  if (packet) { const uint8_t* pk = rc_packet_ptr(); const Header* h = (const Header*)pk; const DrawItem* d = (const DrawItem*)(pk + h->sec[SEC_DRAWS].offset); L.draws.assign(d, d + h->sec[SEC_DRAWS].count); }
}

static void passSwitches(const World::Impl& w, uint8_t* slot) {   // the PassBlock's "fast" (r_fastSky, offset 192) and "volumeFogOn" (r_volumeFog, 224; the core set it to 1 where the map has fog)
  *(float*)(slot + 192) = w.fastSky ? 1.f : 0.f; if (!w.volumeFog) *(float*)(slot + 224) = 0.f;
}

#include "nc_world_objects.inl"
#include "nc_world_fx.inl"
#include "nc_post.inl"

World::World() : m(new Impl()) {}
World::~World() {
  m->cancelPicmip = true;
  if (m->picmipJob.valid()) m->picmipJob.wait();
}
const WorldStats& World::stats() const { return m->st; }
const double* World::mapBounds() const { return m->mapBounds; }

void World::warmPipelines() {
  Impl& w = *m;
  Camera cam{};
  cam.view[0] = cam.view[5] = cam.view[10] = cam.view[15] = 1;
  cam.proj[0] = cam.proj[5] = cam.proj[10] = 1e-9;
  cam.proj[15] = 1;
  cam.nearZ = 1; cam.farZ = 1e9;
  cam.flags = PASS_WORLD | CAM_NO_PVS;
  rc_frame_begin();
  const auto* camera = reinterpret_cast<const uint8_t*>(&cam);
  const int result = w.worldless ? rc_scene_frame(camera, MINIMAP_LAYER, -1)
                                 : rc_frame_objects(camera, 0, nullptr, 0, 0, MINIMAP_LAYER, -1);
  if (result < 0) { logf("minimap pipeline warmup: core returned %d", result); return; }
  const auto* entries = reinterpret_cast<const FrameEntry*>(rc_frame_entries());
  const auto* states = reinterpret_cast<const ObjectState*>(rc_frame_states());
  const DrawItem* draws = nullptr;
  if (!w.worldless) {
    const uint8_t* packet = rc_packet_ptr();
    const auto* header = reinterpret_cast<const Header*>(packet);
    draws = reinterpret_cast<const DrawItem*>(packet + header->sec[SEC_DRAWS].offset);
  }
  for (int i = 0; i < rc_frame_entry_count(); ++i) {
    if (entries[i].kind == 0) {
      const uint32_t id = draws[entries[i].index].materialId;
      if (w.matGpu[id].set) w.pipelineFor(w.mats[id].programId, states[i]);
    } else {
      const auto* ids = static_cast<const int32_t*>(rc_scene_array(A_OBJECT_ID));
      const int handle = ids[entries[i].index] - 100;
      if (handle >= 0 && handle < (int)w.obj->insts.size())
        w.pipelineFor(w.obj->models[w.obj->insts[handle].model].prog, states[i]);
    }
  }
}

std::string World::entityClass(int index) const {   // the classname of the BSP entity with this index (the order of the entity lump); empty when there is none
  const std::string& ents = m->entities; size_t p = 0; int k = 0;
  while ((p = ents.find('{', p)) != std::string::npos) {
    const size_t e = ents.find('}', p); if (e == std::string::npos) break; if (k++ != index) { p = e; continue; }
    const std::string b = ents.substr(p, e - p); const size_t q = b.find("\"classname\""); if (q == std::string::npos) return ""; const size_t a = b.find('"', q + 11); if (a == std::string::npos) return ""; const size_t z = b.find('"', a + 1); return z == std::string::npos ? "" : b.substr(a + 1, z - a - 1);
  }
  return "";
}

bool World::spawn(double eye[3], double& yaw) const {
  const std::string& ents = m->entities; size_t p = 0;
  auto val = [](const std::string& b, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\""; size_t q = b.find(k); if (q == std::string::npos) return false; q = b.find('"', q + k.size()); if (q == std::string::npos) return false; const size_t e = b.find('"', q + 1); out = b.substr(q + 1, e - q - 1); return true;
  };
  while ((p = ents.find('{', p)) != std::string::npos) {
    const size_t e = ents.find('}', p); const std::string b = ents.substr(p, e - p); p = e; std::string cn, org, ang;
    if (!val(b, "classname", cn) || (cn != "info_player_start" && cn != "info_player_deathmatch") || !val(b, "origin", org)) continue;
    double x, y, z; sscanf(org.c_str(), "%lf %lf %lf", &x, &y, &z); eye[0] = x; eye[1] = z + 8; eye[2] = -y; yaw = val(b, "angle", ang) ? atof(ang.c_str()) : 0; return true;
  }
  return false;
}

static std::string mapBaseName(const std::string& rel) {
  const size_t slash = rel.find_last_of("/\\");
  const std::string name = rel.substr(slash == std::string::npos ? 0 : slash + 1);
  return name.substr(0, name.find_last_of('.'));
}

// The GPU state every pass needs, with or without a map: descriptor layouts and pools, samplers, the pass / light / draw blocks and their sets, the white and grey texels.
static void gpuBase(World::Impl& w, const Context& ctx) {
  // ---- descriptor layouts and pools: set 0 = pass + lights (per pass slot), set 1 = draw block (dynamic offset), set 2 = material (per program)
  VkDescriptorSetLayoutBinding b0[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr}, {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr}};
  VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = b0; NC_CK(vkCreateDescriptorSetLayout(ctx.dev, &li, nullptr, &w.set0Layout));
  VkDescriptorSetLayoutBinding b1 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr}; li.bindingCount = 1; li.pBindings = &b1; NC_CK(vkCreateDescriptorSetLayout(ctx.dev, &li, nullptr, &w.set1Layout));
  VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 8192; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps; NC_CK(vkCreateDescriptorPool(ctx.dev, &dpi, nullptr, &w.pool));
  VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.maxLod = VK_LOD_CLAMP_NONE;
  if (ctx.props.limits.maxSamplerAnisotropy >= 8.f) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 8.f; }
  NC_CK(vkCreateSampler(ctx.dev, &si, nullptr, &w.samp)); si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; NC_CK(vkCreateSampler(ctx.dev, &si, nullptr, &w.sampClamp));
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; si.maxLod = 0.25f; si.anisotropyEnable = VK_FALSE; si.maxAnisotropy = 1.f; NC_CK(vkCreateSampler(ctx.dev, &si, nullptr, &w.sampFlat));

  w.passBuf = makeBuf(ctx, (size_t)MAX_PASS_SLOTS * PASS_SLOT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); w.lightBuf = makeBuf(ctx, (size_t)MAX_PASS_SLOTS * LIGHT_SLOT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  w.drawBuf = makeBuf(ctx, (size_t)2 * DRAW_CAP * DRAW_STRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); memset(w.lightBuf.ptr, 0, w.lightBuf.size);
  {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = w.pool; ai.descriptorSetCount = 1;
    for (int i = 0; i < MAX_PASS_SLOTS; ++i) {
      ai.pSetLayouts = &w.set0Layout; NC_CK(vkAllocateDescriptorSets(ctx.dev, &ai, &w.set0[i]));
      VkDescriptorBufferInfo bi[2] = {{w.passBuf.buf, (VkDeviceSize)i * PASS_SLOT, PASS_SLOT}, {w.lightBuf.buf, (VkDeviceSize)i * LIGHT_SLOT, LIGHT_SLOT}}; VkWriteDescriptorSet wr[2] = {};
      for (int k = 0; k < 2; ++k) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = w.set0[i]; wr[k].dstBinding = k; wr[k].descriptorCount = 1; wr[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[k].pBufferInfo = &bi[k]; }
      vkUpdateDescriptorSets(ctx.dev, 2, wr, 0, nullptr);
    }
    ai.pSetLayouts = &w.set1Layout; NC_CK(vkAllocateDescriptorSets(ctx.dev, &ai, &w.set1));
    VkDescriptorBufferInfo bi = {w.drawBuf.buf, 0, 192}; VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = w.set1; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(ctx.dev, 1, &wr, 0, nullptr);
  }
  const uint8_t whitePx[4] = {255, 255, 255, 255}, greyPx[4] = {128, 128, 128, 255}; w.white = makeTexture(ctx, whitePx, 1, 1, false); w.grey = makeTexture(ctx, greyPx, 1, 1, false);
}

bool World::load(const Context& ctx, const FileSource& fsrc, const std::string& bspRel, const std::string& scriptsFile, const std::string& filesList, VkRenderPass screenPass, const std::string& fixesJson) {
  Impl& w = *m; const auto t0 = std::chrono::steady_clock::now(); w.c = ctx; w.fs = &fsrc; w.rpScreen = screenPass;
  const auto prog = [&](const char* stage, double f) { if (onProgress) onProgress(stage, f); };
  prog("geometry", 0.02);
  std::vector<uint8_t> bsp, scripts, files;
  int selectedArchive = -1;
  std::string actualBsp;
  if (!fsrc.read(bspRel, bsp, &selectedArchive, &actualBsp) || bsp.size() < 8 + 17 * 8) {
    logf("cannot read %s under %s", bspRel.c_str(), fsrc.root.c_str()); return false;
  }
  const uint32_t modelOff = rd32(bsp.data() + 8 + 7 * 8), modelLen = rd32(bsp.data() + 12 + 7 * 8);
  if (modelLen >= 40 && (size_t)modelOff + 40 <= bsp.size()) {
    float b[6]; memcpy(b, bsp.data() + modelOff, sizeof b);
    const double bounds[6] = {b[0], b[2], -b[4], b[3], b[5], -b[1]};
    memcpy(w.mapBounds, bounds, sizeof bounds);
  }
  const std::string mapName = mapBaseName(actualBsp);
  std::vector<FileSource::ShaderSource> shaderSources;
  if (scriptsFile.empty()) {
    if (!fsrc.readShaderSources(selectedArchive, shaderSources)) {
      logf("could not read the map's shader scripts"); return false;
    }
    for (const std::string& name : fsrc.listArchives()) {
      files.insert(files.end(), name.begin(), name.end());
      files.push_back('\n');
    }
  }
  else {
    { FILE* f = fopen(scriptsFile.c_str(), "rb"); if (!f) { logf("cannot read %s", scriptsFile.c_str()); return false; } fseek(f, 0, SEEK_END); scripts.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET); if (fread(scripts.data(), 1, scripts.size(), f) != scripts.size()) {} fclose(f); }
    { FILE* f = fopen(filesList.c_str(), "rb"); if (!f) { logf("cannot read %s", filesList.c_str()); return false; } fseek(f, 0, SEEK_END); files.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET); if (fread(files.data(), 1, files.size(), f) != files.size()) {} fclose(f); }
  }
  if (rc_version() != 4) { logf("render core version %d, this client needs 4", rc_version()); return false; }
  { const struct { int what; size_t size; const char* name; } chk[] = {{0, 160, "SurfaceIn"},{1, sizeof(PlanPass), "Pass"}, {2, sizeof(PlanIn), "PlanIn"}};
    for (auto& k : chk) if (rc_plan_sizeof(k.what) != (int)k.size) { logf("core and client disagree on the size of %s (%d vs %zu)", k.name, rc_plan_sizeof(k.what), k.size); return false; } }
  const unsigned lumpOff = rd32(&bsp[8]), lumpLen = rd32(&bsp[12]); w.entities.assign((const char*)&bsp[lumpOff], lumpLen);
  const std::vector<uint8_t> entBytes(w.entities.begin(), w.entities.end());

  // ---- the core: map -> batches -> world scene in the Vulkan dialect. The core refuses a world with 10 or more fog volumes: retry with fewer.
  const std::vector<int> caps = getenv("QRAZY_NC_NOFOG") ? std::vector<int>{0} : std::vector<int>{8, 4, 2, 1, 0};   // QRAZY_NC_NOFOG: no map fog volumes (a diagnostic)
  for (int cap : caps) {
    rc_reset();
    RC(rc_bsp_load(putCore(bsp), (int)bsp.size()));
    if (scriptsFile.empty()) {
      for (const auto& source : shaderSources) {
        const char* text = reinterpret_cast<const char*>(putCore(source.bytes));
        RC(rc_map_shader_source(text, (int)source.bytes.size(), source.tier));
      }
    } else {
      RC(rc_parse_scripts(reinterpret_cast<const char*>(putCore(scripts)), (int)scripts.size()));
    }
    const char* fixes = reinterpret_cast<const char*>(putCore(fixesJson));
    const char* bspName = reinterpret_cast<const char*>(putCore(mapName));
    RC(rc_map_shader_fixes(fixes, (int)fixesJson.size(), bspName, (int)mapName.size()));
    RC(rc_tessellate());
    const char* fl = (const char*)putCore(files); RC(rc_build_batches_auto(fl, (int)files.size(), w.mapFx ? 1 : 0, (const char*)putCore(entBytes), (int)entBytes.size())); RC(rc_merge_batches());
    RC(rc_world_set_vulkan(1));
    const int nf = rc_map_fog(cap);
    if (nf < 0) fatal("core could not build map fog for %s", mapName.c_str());
    RC(rc_world_set_fog(rc_out_ptr(), rc_out_len(), nf));
    const int prep = rc_world_scene_prepare(fl, (int)files.size(), w.mapFx ? 1 : 0, nullptr, 0); if (prep != 0 && cap > 0) { logf("world scene refused (%d) with %d fog volumes, retrying with fewer", prep, nf); continue; } RC(prep);
    logf("fog volumes: %d", nf); break;
  }
  const int stageFloats = rc_stage_prepare(); rc_mark();
  w.nSurfaces = rc_plan_surface_count();
  const int nb = rc_batch_count(), nv = rc_batch_sizes(1), ni = rc_batch_sizes(2); w.batchW = rc_batch_sizes(0); w.batchRec.assign(rc_batch_records(), rc_batch_records() + (size_t)nb * w.batchW);
  w.st.batches = nb; w.st.vertices = nv; w.st.indices = ni; w.st.surfaces = w.nSurfaces;
  logf("map: %d batches, %d vertices, %d indices, %d lightmap pages, %d mirror/portal surfaces", nb, nv, ni, rc_page_count(), w.nSurfaces);

  prog("geometry", 0.1);
  gpuBase(w, ctx);
  // ---- geometry: the core's 14 doubles per vertex narrowed to floats (pos3 uv2 lm2 normal3 rgba4), indices as uint32
  w.vbuf = makeBuf(ctx, (size_t)nv * 56, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); w.ibuf = makeBuf(ctx, (size_t)ni * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  { const double* v = rc_batch_vertices(); float* o = (float*)w.vbuf.ptr; for (size_t i = 0; i < (size_t)nv * 14; ++i) o[i] = (float)v[i]; memcpy(w.ibuf.ptr, rc_batch_indices(), (size_t)ni * 4); }
  for (int i = 0; i < rc_page_count(); ++i) w.lmPages.push_back(makeTexture(ctx, rc_page_data(i), rc_page_width(i), rc_page_height(i), false));

  // ---- mirror / portal targets: one square image per registered surface, in the layout a sampler expects until the first pass renders into it
  {
    VkAttachmentDescription at[2] = {};
    at[0].format = VK_FORMAT_B8G8R8A8_UNORM; at[0].samples = VK_SAMPLE_COUNT_1_BIT; at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE; at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT; at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
    VkSubpassDependency dep[2] = {{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
                                  {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 2; rpi.pAttachments = at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 2; rpi.pDependencies = dep;
    NC_CK(vkCreateRenderPass(ctx.dev, &rpi, nullptr, &w.rpTarget));
  }
  w.targets.resize((size_t)w.nSurfaces);
  for (int s = 0; s < w.nSurfaces; ++s) {
    const double ts = *(const double*)(rc_plan_surface_in(s) + 136); const int size = std::max(16, std::min(2048, (int)ts)); SurfaceTarget& t = w.targets[s]; t.size = size;
    t.color = makeImage(ctx, size, size, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    t.depth = makeImage(ctx, size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView v[2] = {t.color.view, t.depth.view}; VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = w.rpTarget; fi.attachmentCount = 2; fi.pAttachments = v; fi.width = fi.height = size; fi.layers = 1;
    NC_CK(vkCreateFramebuffer(ctx.dev, &fi, nullptr, &t.fb));
    oneShot(ctx, [&](VkCommandBuffer cb) { imageBarrier(cb, t.color.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); });
  }

  // ---- the first frame carries the load tables (programs, materials, stages): read them from a priming frame at the spawn point
  double eye[3] = {0, 100, 0}, yaw = 0; spawn(eye, yaw);
  { const CameraIn ci = makeCamera(eye, yaw, 0, 16.0 / 9.0, 100); Camera cam = {}; memcpy(cam.view, ci.view, 128); memcpy(cam.proj, ci.proj, 128); memcpy(cam.position, ci.pos, 24); cam.nearZ = ci.nearZ; cam.farZ = ci.farZ; cam.fovY = ci.fovY; cam.flags = PASS_WORLD;
    rc_frame_begin(); uint8_t* cp = (uint8_t*)rc_alloc(sizeof cam); memcpy(cp, &cam, sizeof cam); RC(rc_frame_objects(cp, 0, nullptr, 0, 1, 1, -1));
    const uint8_t* pk = rc_packet_ptr(); const Header* h = (const Header*)pk;
    w.blob.assign((const char*)pk + h->sec[SEC_BLOB].offset, h->sec[SEC_BLOB].count);
    const Material* mm = (const Material*)(pk + h->sec[SEC_MATERIALS].offset); w.mats.assign(mm, mm + h->sec[SEC_MATERIALS].count);
    const Stage* ss = (const Stage*)(pk + h->sec[SEC_STAGES].offset); w.stages.assign(ss, ss + h->sec[SEC_STAGES].count);
    const Program* pr = (const Program*)(pk + h->sec[SEC_PROGRAMS].offset); const UniformSlot* sl = (const UniformSlot*)(pk + h->sec[SEC_UNIFORMS].offset);
    w.progs.resize(h->sec[SEC_PROGRAMS].count); for (size_t i = 0; i < w.progs.size(); ++i) { prog("shaders", 0.1 + 0.3 * (double)i / (double)std::max<size_t>(1, w.progs.size())); w.buildProgram(w.progs[i], pr[i], sl, (const uint8_t*)w.blob.data(), (int)i); }
    int bad = 0; for (auto& p : w.progs) if (!p.ok) ++bad; w.st.programs = (int)w.progs.size(); if (bad) logf("%d of %zu programs did not compile", bad, w.progs.size());
  }
  if (stageFloats >= 0) { w.stageOk = true; const uint32_t* so = rc_stage_offsets(); w.stageOffsets.assign(so, so + rc_stage_count() + 1); } else logf("rc_stage_prepare -> %d (animated stages off)", stageFloats);
  w.matGpu.assign(w.mats.size(), MatGpu()); w.st.materials = (int)w.mats.size();
  // which material is a surface's live portal material: the surface's own batch (material id = batch)
  for (int s = 0; s < w.nSurfaces; ++s) { const int b = rc_plan_surface_batch(s); if (b >= 0 && (size_t)b < w.matGpu.size()) w.matGpu[b].surface = s; }
  size_t need = 0; for (auto& mt : w.mats) { if (mt.programId == NO_PROGRAM || mt.programId >= w.progs.size()) continue; need += 512 + ((std::max<size_t>(w.progs[mt.programId].stageVec4 * 16, 16) + 255) / 256) * 256; }
  w.matBuf = makeBuf(ctx, need + (1u << 20), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  // every material's descriptors and textures at load, so nothing is read from disk while playing
  for (uint32_t i = 0; i < w.mats.size(); ++i) if (w.mats[i].programId != NO_PROGRAM && w.mats[i].programId < w.progs.size() && !(w.mats[i].flags & MF_HIDDEN)) { prog("textures", 0.4 + 0.55 * (double)i / (double)std::max<size_t>(1, w.mats.size())); w.prepareMaterial(i); }
  prog("objects", 0.95);
  objInit(w);
  w.st.loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  logf("world ready in %.1f s: %d programs, %d materials, %d textures (%d missing)", w.st.loadSeconds, w.st.programs, w.st.materials, w.st.texturesLoaded, w.st.texturesMissing);
  return true;
}

// One object entry of a frame list: its pipeline, vertex / index buffers, the pass's set 0, the entry's draw block (dynamic offset into the draw buffer) and each part's material set.
// ---- the view weapon: a second scene with its own camera (the gun camera: identity view, the main camera's fov and aspect, the core's weapon clips), drawn after the world with the depth cleared
static void prepareGun(World::Impl& w, const CameraIn& cam, int slot, uint32_t drawBase, double time, GunPass& g) {
  g.on = false; if (!w.obj || !w.gunShown) return;
  const double aspect = cam.proj[0] != 0 ? cam.proj[5] / cam.proj[0] : 16.0 / 9.0, t = 1.0 / tan(cam.fovY * M_PI / 360.0), nz = rc_camera_clips()[2], fz = rc_camera_clips()[3];
  Camera c = {}; for (int i = 0; i < 4; ++i) c.view[i * 5] = 1.0;
  c.proj[0] = t / aspect; c.proj[5] = t; c.proj[10] = (fz + nz) / (nz - fz); c.proj[11] = -1; c.proj[14] = 2 * fz * nz / (nz - fz); c.nearZ = nz; c.farZ = fz; c.fovY = cam.fovY; c.flags = PASS_VIEWMODEL | (1u << 8);   // bit 8: clear depth
  uint8_t* cp = (uint8_t*)rc_alloc(sizeof c); memcpy(cp, &c, sizeof c);
  if (rc_scene_frame(cp, 2, -1) < 0) return;
  w.grab(g.list, false); if (g.list.ent.empty()) return;
  const int pb = rc_pass_block(cp); uint8_t* sl = w.passBuf.ptr + (size_t)slot * PASS_SLOT; memset(sl, 0, PASS_SLOT); if (pb > 0 && pb <= PASS_SLOT) memcpy(sl, rc_pass_block_ptr(), (size_t)pb);
  *(float*)(sl + 140) = 1.f; *(float*)(sl + 184) = (float)time; *(float*)(sl + 188) = (float)time;
  memcpy(rc_lights_view(), cam.view, sizeof cam.view);
  const float* gunLights = rc_viewgun_lights(cam.pos[0], cam.pos[1], cam.pos[2], w.worldless ? 0 : 1);
  memcpy(w.lightBuf.ptr + (size_t)slot * LIGHT_SLOT, gunLights, 480);
  if (drawBase + g.list.ent.size() > (size_t)DRAW_CAP) g.list.ent.resize(DRAW_CAP - drawBase);
  for (size_t k = 0; k < g.list.ent.size(); ++k) memcpy(w.drawBuf.ptr + (size_t)(drawBase + k) * DRAW_STRIDE, g.list.blocks.data() + k * 192, 192);
  g.drawBase = drawBase; g.on = true;
}
static void recordObject(World::Impl& w, VkCommandBuffer cb, int passSlot, uint32_t drawSlot, const FrameEntry& e, const ObjectState& s, uint32_t& objSkipped);
static void recordGun(World::Impl& w, VkCommandBuffer cb, int sw, int sh, int slot, const GunPass& g) {
  if (!g.on) return; VkClearAttachment ca = {}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca.clearValue.depthStencil = {1.0f, 0}; VkClearRect cr = {{{0, 0}, {(uint32_t)sw, (uint32_t)sh}}, 0, 1}; vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
  uint32_t skipped = 0; for (size_t k = 0; k < g.list.ent.size(); ++k) if (g.list.ent[k].kind != 0) recordObject(w, cb, slot, g.drawBase + (uint32_t)k, g.list.ent[k], g.list.states[k], skipped);
}

static void recordObject(World::Impl& w, VkCommandBuffer cb, int passSlot, uint32_t drawSlot, const FrameEntry& e, const ObjectState& s, uint32_t& objSkipped) {
  const VkDeviceSize zero = 0;
        const int32_t* oid = (const int32_t*)rc_scene_array(A_OBJECT_ID); const int h = oid[e.index] - 100;
        if (!w.obj || h < 0 || h >= (int)w.obj->modelOfObjectId.size() || w.obj->modelOfObjectId[h] < 0) { ++objSkipped; return; }
        const ObjModel& om = w.obj->models[w.obj->modelOfObjectId[h]]; const Prog& Pr = w.progs[om.prog]; 
        const VkPipeline pipe = w.pipelineFor((uint32_t)om.prog, s);
        vkCmdBindVertexBuffers(cb, 0, 1, &om.vb.buf, &zero); vkCmdBindIndexBuffer(cb, om.ib.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 0, 1, &w.set0[passSlot], 0, nullptr);
        if (s.flags & rc::os::OS_POLYGON_OFFSET) vkCmdSetDepthBias(cb, (float)s.polygonUnits, 0.f, (float)s.polygonFactor);
        const uint32_t dyn = drawSlot * DRAW_STRIDE; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 1, 1, &w.set1, 1, &dyn);
        for (size_t q = 0; q < om.parts.size(); ++q) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 2, 1, &om.sets[q], 0, nullptr); vkCmdDrawIndexed(cb, om.parts[q].count, 1, om.parts[q].first, 0, 0); }
  ++w.st.objectDraws;
}

bool World::loadObjectsOnly(const Context& ctx, const FileSource& fsrc, VkRenderPass screenPass) {
  Impl& w = *m; w.c = ctx; w.fs = &fsrc; w.rpScreen = screenPass; w.worldless = true;
  if (rc_version() != 4) { logf("render core version %d, this client needs 4", rc_version()); return false; }
  rc_reset(); rc_mark();
  gpuBase(w, ctx);
  w.matBuf = makeBuf(ctx, 8u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);   // the objects' material blocks (no world materials)
  objInit(w);
  logf("objects-only scene ready (no map)");
  return true;
}

// A frame with no map: the resident scene's nodes only (rc_scene_frame), one pass to the screen. The programs and pipeline state of objects come from the core's variant calls, not from a world packet.
static void recordWorldless(World::Impl& w, VkCommandBuffer cb, VkFramebuffer mainFb, int sw, int sh, const CameraIn& cam, double time, const std::function<void(VkCommandBuffer)>& overlay) {
  const auto t0 = std::chrono::steady_clock::now(); ++w.frameStamp; w.st.worldDraws = 0; w.st.objectDraws = 0; w.st.passes = 1;
  rc_frame_begin();
  Camera c = {}; memcpy(c.view, cam.view, 128); memcpy(c.proj, cam.proj, 128); memcpy(c.position, cam.pos, 24); c.nearZ = cam.nearZ; c.farZ = cam.farZ; c.fovY = cam.fovY; c.flags = PASS_WORLD;
  uint8_t* cp = (uint8_t*)rc_alloc(sizeof c); memcpy(cp, &c, sizeof c);
  const int rc = rc_scene_frame(cp, 1 | 4, -1); if (rc < 0) fatal("rc_scene_frame -> %d", rc);
  Grabbed& L = w.objectList; w.grab(L, false);
  const int pb = rc_pass_block(cp); uint8_t* slot = w.passBuf.ptr; memset(slot, 0, PASS_SLOT);
  if (pb > 0 && pb <= PASS_SLOT) memcpy(slot, rc_pass_block_ptr(), (size_t)pb);
  *(float*)(slot + 140) = 1.f; *(float*)(slot + 184) = (float)time; *(float*)(slot + 188) = (float)time;   // outputSRGB (the screen encodes), shaderTime, time
  passSwitches(w, slot);
  if (w.fogMode) { memcpy(slot + 160, w.fogColor, 12); *(float*)(slot + 172) = w.fogDensity; }   // the scene fog: colour (linear) and density (FogExp2)
  objPackLights(cam.view); memcpy(w.lightBuf.ptr, rc_lights_block(), 480);
  if (L.ent.size() > (size_t)DRAW_CAP) L.ent.resize(DRAW_CAP);
  for (size_t k = 0; k < L.ent.size(); ++k) memcpy(w.drawBuf.ptr + k * DRAW_STRIDE, L.blocks.data() + k * 192, 192);
  GunPass& gun = w.gun; gun.on = false; prepareGun(w, cam, GUN_SLOT, (uint32_t)L.ent.size(), time, gun);
  const auto t1 = std::chrono::steady_clock::now();
  VkClearValue cv[2]; cv[0].color = {{w.clearColor[0], w.clearColor[1], w.clearColor[2], 1.f}}; cv[1].depthStencil = {1.0f, 0};
  VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = w.rpScreen; rb.framebuffer = mainFb; rb.renderArea = {{0, 0}, {(uint32_t)sw, (uint32_t)sh}}; rb.clearValueCount = 2; rb.pClearValues = cv;
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1}; VkRect2D sc = {{0, 0}, {(uint32_t)sw, (uint32_t)sh}}; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  const float bc0[4] = {0, 0, 0, 0}; vkCmdSetBlendConstants(cb, bc0); vkCmdSetDepthBias(cb, 0.f, 0.f, 0.f);
  uint32_t objSkipped = 0;
  for (size_t k = 0; k < L.ent.size(); ++k) if (L.ent[k].kind != 0) recordObject(w, cb, 0, (uint32_t)k, L.ent[k], L.states[k], objSkipped);
  recordGun(w, cb, sw, sh, GUN_SLOT, gun);
  if (overlay) overlay(cb);
  vkCmdEndRenderPass(cb);
  w.st.objectEntriesSkipped = (int)objSkipped;
  w.st.planMs = std::chrono::duration<double, std::milli>(t1 - t0).count(); w.st.recordMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
}

void World::setFog(int mode, const float linearRgb[3], float density, const float clearSrgb[3]) {
  Impl& w = *m; w.fogMode = mode; w.fogDensity = density; for (int k = 0; k < 3; ++k) { w.fogColor[k] = linearRgb[k]; w.clearColor[k] = clearSrgb[k]; }
}

int World::addBoard(const uint8_t* rgba, int w, int h, double z, double yCentre, double halfW, double halfH, double yaw) {
  Impl& im = *m; if (!im.obj) return -1; return objAddBoard(im, rgba, w, h, z, yCentre, halfW, halfH, yaw);
}

void World::setMirrors(bool on) { m->mirrors = on; }
void World::setLoadOptions(int picmip, bool mapFx) { m->wantedPicmip = m->picmip = std::clamp(picmip, 0, 10); m->mapFx = mapFx; }

void World::setPicmip(int level) {
  level = std::clamp(level, 0, 10);
  if (level == m->wantedPicmip) return;
  m->wantedPicmip = level;
  m->cancelPicmip = true;
}

void World::refreshPicmip() {
  Impl& w = *m;
  if (w.picmipJob.valid()) {
    if (w.picmipJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
      w.picmipUploads = w.picmipJob.get();
    } catch (const std::exception& e) {
      if (!w.cancelPicmip) {
        logf("picmip %d refresh failed: %s", w.wantedPicmip, e.what());
        w.wantedPicmip = w.picmip;
      }
    }
  }
  if (!w.picmipUploads.empty()) {
    // A material prepared during the refresh can have added a texture at the old level.
    if (!w.cancelPicmip && w.refreshCacheSize == w.texCache.size()) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
      do {
        auto& pixels = w.picmipUploads[w.uploadedPicmip++];
        pixels.replacement = pixels.upload(w.c);
        std::vector<uint8_t>().swap(pixels.rgba);
      } while (w.uploadedPicmip < w.picmipUploads.size() && std::chrono::steady_clock::now() < deadline);
      if (w.uploadedPicmip < w.picmipUploads.size()) return;

      for (auto& pixels : w.picmipUploads) {
        Image& current = w.texCache.at({pixels.name, true});
        std::swap(current, pixels.replacement);
      }
      w.picmip = w.wantedPicmip;
      for (uint32_t i = 0; i < w.matGpu.size(); ++i)
        if (w.matGpu[i].set) w.writeMaterialDescriptors(i);
      logf("picmip %d applied to %zu map textures", w.picmip, w.picmipUploads.size());
    }
    for (size_t i = 0; i < w.uploadedPicmip; ++i) {
      const Image& image = w.picmipUploads[i].replacement;
      vkDestroyImageView(w.c.dev, image.view, nullptr);
      vkDestroyImage(w.c.dev, image.img, nullptr);
      vkFreeMemory(w.c.dev, image.mem, nullptr);
    }
    w.uploadedPicmip = 0;
    w.picmipUploads.clear();
  }
  if (w.wantedPicmip == w.picmip) return;

  std::vector<TexturePixels> textures;
  for (const auto& [key, image] : w.texCache)
    if (key.second && image.img != w.grey.img) textures.push_back(TexturePixels{key.first});
  if (textures.empty()) {
    w.picmip = w.wantedPicmip;
    return;
  }
  w.refreshCacheSize = w.texCache.size();
  w.cancelPicmip = false;
  w.picmipJob = std::async(std::launch::async, [&cancel = w.cancelPicmip, fs = w.fs->snapshot(),
                                            level = w.wantedPicmip, textures = std::move(textures)]() mutable {
    for (auto& pixels : textures) {
      if (cancel) break;
      if (!pixels.load(fs, true, level)) throw std::runtime_error("could not reload " + pixels.name);
      // Picmip compacts the pixels in place. Keep only the resulting image while decoding the rest.
      const size_t size = static_cast<size_t>(pixels.size[0]) * pixels.size[1] * 4;
      if (size < pixels.rgba.size()) {
        std::vector<uint8_t> reduced(pixels.rgba.data(), pixels.rgba.data() + size);
        pixels.rgba.swap(reduced);
      }
    }
    return std::move(textures);
  });
}
void World::setRender(bool sortTranslucent, bool volumeFog, bool fastSky) { Impl& w = *m; w.volumeFog = volumeFog; w.fastSky = fastSky; if (!w.worldless) rc_world_set_sorted(sortTranslucent ? 1 : 0); }
void World::buildGhostPath(const double* rows, int n) {
  Impl& w = *m; if (!w.obj || w.obj->path[1].model < 0) return; World::Impl::Obj& o = *w.obj; o.pathCount = 0; if (n < 2) return;
  std::vector<double> s((size_t)n * 5); for (int i = 0; i < n; ++i) { const double* r = rows + (size_t)i * 14; for (int k = 0; k < 4; ++k) s[(size_t)i * 5 + k] = r[k]; s[(size_t)i * 5 + 4] = r[13] == r[13] ? r[13] : 0.0; }
  double* in = (double*)rc_alloc((int)(s.size() * 8)); if (!in) return; memcpy(in, s.data(), s.size() * 8);
  const int count = rc_ghost_path(in, n); if (count <= 0) return;
  const float* seg[2] = {rc_ghost_path_fwd(), rc_ghost_path_rev()}; const int* before = rc_ghost_path_before(); o.pathBefore.assign(before, before + n);
  NC_CK(vkDeviceWaitIdle(w.c.dev));
  static const float quad[8][3] = {{-1, 2, 0}, {1, 2, 0}, {-1, 1, 0}, {1, 1, 0}, {-1, -1, 0}, {1, -1, 0}, {-1, -2, 0}, {1, -2, 0}}; static const uint32_t qi[18] = {0, 2, 1, 2, 3, 1, 2, 4, 3, 4, 5, 3, 4, 6, 5, 6, 7, 5};
  for (int l = 0; l < 2; ++l) {
    ObjModel& om = o.models[(size_t)o.path[l].model]; const Prog& P = w.progs[om.prog]; const size_t floats = P.stride / 4;
    for (Buf* b : {&om.vb, &om.ib}) if (b->buf) { vkDestroyBuffer(w.c.dev, b->buf, nullptr); vkFreeMemory(w.c.dev, b->mem, nullptr); *b = Buf(); }
    om.vb = makeBuf(w.c, (size_t)count * 8 * floats * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); om.ib = makeBuf(w.c, (size_t)count * 18 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    float* vb = (float*)om.vb.ptr; uint32_t* ib = (uint32_t*)om.ib.ptr;
    for (int i = 0; i < count; ++i) { for (int v = 0; v < 8; ++v) { size_t at = ((size_t)i * 8 + v) * floats; for (auto& a : P.inputs) { for (int c = 0; c < a.second; ++c) vb[at + c] = a.first == "position" ? quad[v][c] : a.first == "instanceStart" ? seg[l][i * 6 + c] : a.first == "instanceEnd" ? seg[l][i * 6 + 3 + c] : 0.f; at += (size_t)a.second; } }
      for (int k = 0; k < 18; ++k) ib[(size_t)i * 18 + k] = (uint32_t)(i * 8) + qi[k]; }
  }
  o.pathCount = count; logf("ghost path: %d segments", count);
}
void World::drawGhostPath(bool on, int k, double width, double opacity, const double rgb[3], double resW, double resH) {
  Impl& w = *m; if (!w.obj || w.obj->path[1].node < 0) return; World::Impl::Obj& o = *w.obj; uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  on = on && o.pathCount > 0; const int done = on ? o.pathBefore[(size_t)std::max(0, std::min(k, (int)o.pathBefore.size() - 1))] : 0;
  for (int l = 0; l < 2; ++l) { const World::Impl::Obj::PathLine& L = o.path[l]; const int segs = !on ? 0 : l == 0 ? done : o.pathCount - done;
    if (segs <= 0) { flags[L.node] &= ~2u; continue; }
    L.res[0] = (float)resW; L.res[1] = (float)resH; *L.width = (float)width; *L.opacity = (float)opacity; for (int c = 0; c < 3; ++c) L.diffuse[c] = (float)rgb[c];
    o.models[(size_t)L.model].parts[0].count = (uint32_t)segs * 18; flags[L.node] |= 2u; }
}
void World::fxMark(int style, const double pos[3], const double normal[3], double radius) { if (m->obj) fxMarkImpl(*m, style, pos, normal, radius); }
void World::fxSparks(const double pos[3], const double normal[3]) { if (m->obj) fxSparksImpl(*m, pos, normal); }
void World::fxBlast(int weapon, const double pos[3], double radius, const double* normal, bool ghost) { if (m->obj) fxBlastImpl(*m, weapon, pos, radius, normal, ghost); }
void World::fxProjectile(int id, int weapon, const double pos[3], const double vel[3], bool spawn) { if (m->obj) fxProjectileImpl(*m, id, weapon, pos, vel, spawn); }
void World::fxProjectileEnd(int id) { if (m->obj) fxProjectileEndImpl(*m, id); }
void World::fxBeam(const double from[3], const double to[3]) { if (m->obj) fxBeamImpl(*m, from, to); }
void World::fxHook(const double from[3], const double to[3], double time, bool attached, int slot) { if (m->obj) fxHookImpl(*m, from, to, time, attached, slot); }
void World::fxHookEnd(int slot) { if (m->obj) fxHookEndImpl(*m, slot); }
void World::fxUpdate(double dt, const CameraIn& cam) { if (m->obj) fxUpdateImpl(*m, dt, cam); }
void World::fxSetMarkTime(double seconds) { if (m->obj) fxOf(*m).markTime = seconds; }

void World::setShadow(int slot, const double pos[3], const double quat[4], double size, const double u[6]) {
  Impl& w = *m; if (!w.obj || slot < 0 || slot >= SHADOWS) return; const Impl::Obj::Shadow& sh = w.obj->shadows[slot]; const int node = sh.node; if (node < 0) return;
  double* p = (double*)rc_scene_array(A_POS); double* q = (double*)rc_scene_array(A_QUAT); double* s = (double*)rc_scene_array(A_SCALE); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  for (int k = 0; k < 3; ++k) p[node * 3 + k] = pos[k]; for (int k = 0; k < 4; ++k) q[node * 4 + k] = quat[k]; s[node * 3] = s[node * 3 + 1] = size; s[node * 3 + 2] = 1; for (int k = 0; k < 6; ++k) *sh.u[k] = (float)u[k]; flags[node] |= 2u;
}
void World::hideShadow(int slot) { Impl& w = *m; if (!w.obj || slot < 0 || slot >= SHADOWS) return; const int node = w.obj->shadows[slot].node; if (node >= 0) ((uint32_t*)rc_scene_array(A_FLAGS))[node] &= ~2u; }

int World::addAvatarBody(const float rgb[3], int uniqueId) {
  Impl& w = *m; if (!w.obj) return -1; const double tint[3] = {rgb[0], rgb[1], rgb[2]};
  const int bm = objAddCoreModel(w, 13, false, 0, nullptr, nullptr, "ghost-body", tint, uniqueId); if (bm < 0) return -1; return objAddNode(w, bm, 160.0, w.obj->models[bm].transparent ? 6 : 1);   // front side only, like the page (QC_PS_* has no two-sided): double sided shows the eyes through the back
}
int World::addAvatarGun(int weapon) {
  Impl& w = *m; if (!w.obj) return -1; const std::string name = std::to_string(weapon);
  const int gm = objAddCoreModel(w, 14, false, 0, nullptr, nullptr, name.c_str(), nullptr, 0); if (gm < 0) return -1; return objAddNode(w, gm, 160.0, w.obj->models[gm].transparent ? 2 : 1);
}
int World::addAvatarAccessory(const float rgb[3], int uniqueId) {
  Impl& w = *m; if (!w.obj || rc_model_avatar_surface() < 0) return -1; const double tint[3] = {rgb[0], rgb[1], rgb[2]}; const std::string id = "avatar-surface-" + std::to_string(uniqueId);
  const auto f = w.obj->modelByFile.find(id); const int am = f != w.obj->modelByFile.end() ? f->second : objModelFromRecord(w, id, id, 1.0, false, false, nullptr, 0, nullptr, nullptr, nullptr, tint);
  if (am < 0) return -1; return objAddNode(w, am, 160.0, w.obj->models[am].transparent ? 6 : 1);
}
int World::addPlate(const uint8_t* rgba, int tw, int th) {
  Impl& w = *m; if (!w.obj) return -1; const std::vector<float> pos = {-.5f, .5f, 0, .5f, .5f, 0, -.5f, -.5f, 0, .5f, -.5f, 0}, uv = {0, 0, 1, 0, 0, 1, 1, 1}; const std::vector<uint32_t> idx = {0, 2, 1, 1, 2, 3}; const double white[3] = {1, 1, 1};
  const int model = objBasicModel(w, pos, uv, idx, rgba, tw, th, white, 1.0, true); return objAddNode(w, model, 400.0, 2);
}
void World::setNodePose(int handle, const double pos[3], const double quat[4], const double scale[3], bool visible) {
  Impl& w = *m; if (!w.obj || handle < 0 || handle >= (int)w.obj->insts.size()) return; const int node = w.obj->insts[handle].node;
  double* p = (double*)rc_scene_array(A_POS); double* q = (double*)rc_scene_array(A_QUAT); double* s = (double*)rc_scene_array(A_SCALE); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  for (int k = 0; k < 3; ++k) { p[node * 3 + k] = pos[k]; s[node * 3 + k] = scale[k]; } for (int k = 0; k < 4; ++k) q[node * 4 + k] = quat[k]; if (visible) flags[node] |= 2u; else flags[node] &= ~2u;
}
void World::setOpacity(int handle, double opacity) { Impl& w = *m; if (!w.obj || handle < 0 || handle >= (int)w.obj->insts.size()) return; float* o = w.obj->models[(size_t)w.obj->insts[handle].model].opacityPtr; if (o) *o = (float)opacity; }
static bool objParseGlb(const std::vector<uint8_t>& glb) { if (glb.empty()) return false; uint8_t* cp = (uint8_t*)rc_alloc((int)glb.size() + 1); memcpy(cp, glb.data(), glb.size()); if (rc_model_parse_glb(cp, (int)glb.size()) < 0) { logf("custom model: %s", rc_model_error()); return false; } return true; }
int World::addViewWeaponGlb(const std::vector<uint8_t>& glb, double scale) {
  Impl& w = *m; if (!w.obj) return -1; const double* f = rc_fit_standard(scale); if (!f) return -1; const double xf[5] = {f[4], f[0], f[1], f[2], f[3]}; if (!objParseGlb(glb)) return -1;
  const std::string id = "custom-gun-" + std::to_string(w.obj->models.size()); const int model = objModelFromRecord(w, id, id, 1.0, false, true, xf); if (model < 0) return -1;
  const int h = objAddNode(w, model, 90.0, 1, 2u); w.obj->gunHandles.push_back(h); return h;
}
int World::addObjectGlb(const std::vector<uint8_t>& glb, double size) {
  Impl& w = *m; if (!w.obj || !objParseGlb(glb)) return -1; const std::string id = "custom-object-" + std::to_string(w.obj->models.size()); const int model = objModelFromRecord(w, id, id, size, true); if (model < 0) return -1;
  return objAddNode(w, model, size * 0.9);
}

int World::addViewWeaponFile(const std::string& glbRel, int fitKind) {
  Impl& w = *m; if (!w.obj) return -1; const int model = objAddFittedWeapon(w, glbRel, fitKind); if (model < 0) return -1;
  const int h = objAddNode(w, model, 90.0, 1, 2u); w.obj->gunHandles.push_back(h); return h;
}
int World::addViewWeapon(int kind) {
  Impl& w = *m; if (!w.obj) return -1;
  const char* rotorName = kind == 1 ? "machinegun-rotor" : kind == 5 ? "gauntlet-rotor" : nullptr;   // the barrel / rotor spins (rc_mg_spin / rc_gauntlet_spin)
  const int model = objAddCoreModel(w, kind, true, rotorName ? 1 : 0, rotorName); if (model < 0) return -1;
  const int h = objAddNode(w, model, 90.0, 1, 2u); w.obj->gunHandles.push_back(h);
  if (kind == 6) w.obj->gunLocal[h] = {0, 0, rc_hook_model_z()};   // the grapple sits back in the rig
  if (rotorName) {
    double pivot[3] = {0, 0, 0}; const int rm = objAddCoreModel(w, kind, true, 2, rotorName, pivot);
    if (rm >= 0) { const int rh = objAddNode(w, rm, 90.0, 1, 2u); const int rnode = w.obj->insts[rh].node, gnode = w.obj->insts[h].node;
      int32_t* parent = (int32_t*)rc_scene_array(A_PARENT); double* pos = (double*)rc_scene_array(A_POS); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
      parent[rnode] = gnode; for (int c = 0; c < 3; ++c) pos[rnode * 3 + c] = pivot[c]; flags[rnode] |= 2u; w.obj->rotors.push_back({rnode, kind == 1 ? 2 : 1}); }
  }
  return h;
}
void World::spinViewRotors(double dt, bool machinegunFiring, bool gauntletAttacking) {
  Impl& w = *m; if (!w.obj) return; double* quat = (double*)rc_scene_array(A_QUAT);
  for (const auto& r : w.obj->rotors) {
    const double a = r.second == 2 ? rc_mg_spin(dt, machinegunFiring ? 1 : 0) : rc_gauntlet_spin(dt, gauntletAttacking ? 1 : 0), s = sin(a / 2), c = cos(a / 2);
    quat[r.first * 4] = 0; quat[r.first * 4 + 1] = r.second == 1 ? s : 0; quat[r.first * 4 + 2] = r.second == 2 ? s : 0; quat[r.first * 4 + 3] = c;
  }
}
// the rig: rc_viewgun_place (the core's numbers) -> every weapon node; only the held weapon is visible
static void eulerXYZToQuat(double x, double y, double z, double q[4]) {
  const double c1 = cos(x / 2), c2 = cos(y / 2), c3 = cos(z / 2), s1 = sin(x / 2), s2 = sin(y / 2), s3 = sin(z / 2);
  q[0] = s1 * c2 * c3 + c1 * s2 * s3; q[1] = c1 * s2 * c3 - s1 * c2 * s3; q[2] = c1 * c2 * s3 + s1 * s2 * c3; q[3] = c1 * c2 * c3 - s1 * s2 * s3;
}
void World::hookOrigin(const CameraIn& cam, const double* p, double out[3]) {
  double* b = rc_hook_origin_buffer(); invertRigid(cam.view, b);   // the camera's world matrix
  const bool shown = p[0] != 0;
  if (shown) {   // the grapple model to view: the rig's turn and place, then the model's offset along the rig's z
    double q[4]; eulerXYZToQuat(p[4], p[5], p[6], q); const double x = q[0], y = q[1], z = q[2], s = q[3]; double* g = b + 16;
    g[0] = 1 - 2 * (y * y + z * z); g[1] = 2 * (x * y + s * z); g[2] = 2 * (x * z - s * y); g[3] = 0;
    g[4] = 2 * (x * y - s * z); g[5] = 1 - 2 * (x * x + z * z); g[6] = 2 * (y * z + s * x); g[7] = 0;
    g[8] = 2 * (x * z + s * y); g[9] = 2 * (y * z - s * x); g[10] = 1 - 2 * (x * x + y * y); g[11] = 0;
    const double off = rc_hook_model_z(); for (int k = 0; k < 3; ++k) g[12 + k] = p[1 + k] + g[8 + k] * off; g[15] = 1;
  }
  rc_hook_origin(shown ? 1 : 0); for (int k = 0; k < 3; ++k) out[k] = b[32 + k];
}
void World::placeViewGun(int heldHandle, const double* p) {
  Impl& w = *m; if (!w.obj) return; const bool visible = p[0] != 0 && heldHandle >= 0;
  double q[4]; eulerXYZToQuat(p[4], p[5], p[6], q); w.gunShown = visible;
  double* pos = (double*)rc_scene_array(A_POS); double* quat = (double*)rc_scene_array(A_QUAT); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  for (int h : w.obj->gunHandles) { const int node = w.obj->insts[h].node; double off[3] = {0, 0, 0}; const auto lo = w.obj->gunLocal.find(h); if (lo != w.obj->gunLocal.end()) { const double* v = lo->second.data(); const double tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]); off[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty); off[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz); off[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx); }   // the offset turned by the rig
    for (int k = 0; k < 3; ++k) pos[node * 3 + k] = p[1 + k] + off[k]; for (int k = 0; k < 4; ++k) quat[node * 4 + k] = q[k]; if (visible && h == heldHandle) flags[node] |= 2u; else flags[node] &= ~2u; }
}

int World::addCoreModel(int kind, double radius) {
  Impl& w = *m; if (!w.obj) return -1; const int model = objAddCoreModel(w, kind); if (model < 0) return -1;
  return objAddNode(w, model, radius, 1, 1u | (kind == 16 ? MINIMAP_LAYER : 0u));
}

static void recordDraws(World::Impl& w, VkCommandBuffer cb, int i, uint32_t base, size_t count,
                        const FrameEntry* entries, const ObjectState* states, const DrawItem* draws, uint32_t& objSkipped) {
  VkDeviceSize zero = 0;
  VkPipeline cur = VK_NULL_HANDLE; const void* curSet = nullptr;
  bool worldBound = false;
  for (size_t k = 0; k < count; ++k) {
    const FrameEntry& e = entries[k];
    if (e.kind != 0) {   // an object: a scene-store node. Its model is found through the node's object id.
      recordObject(w, cb, i, base + (uint32_t)k, e, states[k], objSkipped); worldBound = false; cur = VK_NULL_HANDLE; curSet = nullptr; continue;
    }
    if (!worldBound) { vkCmdBindVertexBuffers(cb, 0, 1, &w.vbuf.buf, &zero); vkCmdBindIndexBuffer(cb, w.ibuf.buf, 0, VK_INDEX_TYPE_UINT32); worldBound = true; }
    const DrawItem& d = draws[e.index]; const Material& mt = w.mats[d.materialId]; if (mt.programId == NO_PROGRAM || (mt.flags & MF_HIDDEN) || !w.progs[mt.programId].ok) continue;
    const Prog& Pr = w.progs[mt.programId]; const ObjectState& s = states[k]; const VkPipeline pipe = w.pipelineFor(mt.programId, s);
    if (pipe != cur) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); cur = pipe; curSet = nullptr; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 0, 1, &w.set0[i], 0, nullptr); }
    if (s.flags & rc::os::OS_POLYGON_OFFSET) vkCmdSetDepthBias(cb, (float)s.polygonUnits, 0.f, (float)s.polygonFactor);
    const uint32_t dyn = (base + (uint32_t)k) * DRAW_STRIDE; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 1, 1, &w.set1, 1, &dyn);
    const VkDescriptorSet ms = w.matGpu[d.materialId].set; if (curSet != (const void*)ms) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Pr.layout, 2, 1, &ms, 0, nullptr); curSet = (const void*)ms; }
    const int32_t* br = &w.batchRec[(size_t)d.bufferId * w.batchW];
    vkCmdDrawIndexed(cb, d.indexCount, 1, (uint32_t)br[9] + d.firstIndex, br[7], 0); ++w.st.worldDraws;
  }
}

void World::record(VkCommandBuffer cb, VkFramebuffer mainFb, int sw, int sh, const CameraIn& cam, double time, const uint8_t* poses, int nPoses, const std::function<void(VkCommandBuffer)>& overlay) {
  if (m->worldless) { recordWorldless(*m, cb, mainFb, sw, sh, cam, time, overlay); return; }
  Impl& w = *m; const auto t0 = std::chrono::steady_clock::now(); ++w.frameStamp; w.st.worldDraws = 0; w.st.objectDraws = 0;
  // ---- plan
  rc_frame_begin();
  PlanIn* in = (PlanIn*)rc_plan_in(); memset(in, 0, sizeof *in);
  Camera& mc = in->main; memcpy(mc.view, cam.view, 128); memcpy(mc.proj, cam.proj, 128); memcpy(mc.position, cam.pos, 24); mc.nearZ = cam.nearZ; mc.farZ = cam.farZ; mc.fovY = cam.fovY; mc.flags = PASS_WORLD;
  invertRigid(cam.view, in->mainWorld); in->timeMs = time * 1000.0; in->mainCluster = FROM_CAMERA; in->flags = w.mirrors ? PI_MIRRORS : 0; in->mainMask = 1 | 4; in->mirrorMask = 1; in->portalMask = 1; in->viewMask = 2;
  uint8_t* live = rc_plan_live(); for (int s = 0; s < w.nSurfaces; ++s) live[s] = 1;
  // the core reads the pose bytes while it plans and while it builds each pass's list: copy them into its memory once for the frame
  const uint8_t* corePoses = nullptr; if (poses && nPoses > 0) { uint8_t* p = (uint8_t*)rc_alloc(nPoses * 64); memcpy(p, poses, (size_t)nPoses * 64); corePoses = p; rc_plan_poses(corePoses, nPoses); }
  const int nPass = rc_plan_run(nullptr, nullptr); w.st.passes = nPass; if (nPass < 0) fatal("rc_plan_run -> %d", nPass);
  const PlanPass* passes = (const PlanPass*)rc_plan_passes(); const uint8_t* drawn = rc_plan_drawn();

  // ---- every pass's list and uniform blocks are written first (the core's buffers are overwritten by the next pass), then all passes are recorded
  const double* stageFrames = w.stageOk ? rc_stage_frames(time) : nullptr;
  auto& pp = w.passes; pp.assign(passes, passes + nPass);
  auto& lists = w.lists;
  uint32_t drawBase[MAX_PASS_SLOTS] = {}, drawNext = 0;
  auto& portalPassOf = w.portalPasses; portalPassOf.assign((size_t)std::max(1, w.nSurfaces), nullptr); for (auto& p : pp) if (p.surface >= 0 && p.surface < w.nSurfaces) portalPassOf[p.surface] = &p;
  for (int i = 0; i < nPass && i < GUN_SLOT; ++i) {
    if (rc_plan_pass_frame(i, time, corePoses, corePoses ? nPoses : 0, 0, -1) < 0) fatal("rc_plan_pass_frame(%d) failed", i);
    w.grab(lists[i], (pp[i].flags & PF_WORLD) != 0);
    const int pb = rc_pass_block((const uint8_t*)&pp[i].camera); uint8_t* slot = w.passBuf.ptr + (size_t)i * PASS_SLOT; memset(slot, 0, PASS_SLOT);
    if (pb > 0 && pb <= PASS_SLOT) memcpy(slot, rc_pass_block_ptr(), (size_t)pb);
    *(float*)(slot + 140) = ((pp[i].flags & PF_LINEAR_TARGET) || getenv("QRAZY_NC_LINEAR")) ? 0.f : 1.f;   // outputSRGB: the screen encodes, a linear target does not
    *(float*)(slot + 184) = (float)time; *(float*)(slot + 188) = (float)time;   // shaderTime, time
    passSwitches(w, slot);
    objPackLights(pp[i].camera.view); memcpy(w.lightBuf.ptr + (size_t)i * LIGHT_SLOT, rc_lights_block(), 480);   // the lit objects' LIGHT block for this pass's view
    const size_t n = lists[i].ent.size(); drawBase[i] = drawNext; if (drawNext + n > (size_t)DRAW_CAP) { logf("draw block buffer full"); lists[i].ent.resize(DRAW_CAP - drawNext); }
    for (size_t k = 0; k < lists[i].ent.size(); ++k) memcpy(w.drawBuf.ptr + (size_t)(drawNext + k) * DRAW_STRIDE, lists[i].blocks.data() + k * 192, 192);
    drawNext += (uint32_t)lists[i].ent.size();
    for (const FrameEntry& e : lists[i].ent) {
      if (e.kind != 0) continue; const DrawItem& d = lists[i].draws[e.index]; MatGpu& g = w.matGpu[d.materialId]; if (g.stamp == w.frameStamp) continue;
      const Material& mt = w.mats[d.materialId]; if (mt.programId == NO_PROGRAM || !w.progs[mt.programId].ok) continue;
      w.prepareMaterial(d.materialId); g.stamp = w.frameStamp; w.fillMaterial(d.materialId, stageFrames, g.surface >= 0 ? portalPassOf[g.surface] : nullptr);
    }
  }
  GunPass& gun = w.gun; gun.on = false; prepareGun(w, cam, GUN_SLOT, drawNext, time, gun);
  const auto t1 = std::chrono::steady_clock::now();

  // ---- record
  uint32_t objSkipped = 0;
  for (int i = 0; i < nPass && i < GUN_SLOT; ++i) {
    const PlanPass& P = pp[i]; const bool toScreen = P.surface < 0; const int size = toScreen ? 0 : w.targets[P.surface].size; const int rw = toScreen ? sw : size, rh = toScreen ? sh : size;
    VkClearValue cv[2]; cv[0].color = {{0.f, 0.f, 0.f, 1.f}}; cv[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = toScreen ? w.rpScreen : w.rpTarget; rb.framebuffer = toScreen ? mainFb : w.targets[P.surface].fb;
    rb.renderArea = {{0, 0}, {(uint32_t)rw, (uint32_t)rh}}; rb.clearValueCount = 2; rb.pClearValues = cv; vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)rw, (float)rh, 0, 1}; VkRect2D sc = {{0, 0}, {(uint32_t)rw, (uint32_t)rh}}; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    const float bc0[4] = {0, 0, 0, 0}; vkCmdSetBlendConstants(cb, bc0); vkCmdSetDepthBias(cb, 0.f, 0.f, 0.f);
    const Grabbed& L = lists[i];
    recordDraws(w, cb, i, drawBase[i], L.ent.size(), L.ent.data(), L.states.data(), L.draws.data(), objSkipped);
    if (toScreen) recordGun(w, cb, sw, sh, GUN_SLOT, gun);
    if (toScreen && overlay) overlay(cb);
    vkCmdEndRenderPass(cb);
  }
  (void)drawn;
  if (objSkipped && !w.loggedObjects) { w.loggedObjects = true; logf("note: %u object entries in the frame list have no model and are skipped", objSkipped); }
  w.st.objectEntriesSkipped = (int)objSkipped;
  w.st.planMs = std::chrono::duration<double, std::milli>(t1 - t0).count(); w.st.recordMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
}

bool World::recordMinimap(VkCommandBuffer cb, int wPixels, int hPixels, const float box[4],
                          const QcMinimapOut& view, double playerY, double minY,
                          double time, const uint8_t* poses, int nPoses, VkQueryPool timestamps) {
  const int x = (int)std::lround(box[0]), y = (int)std::lround(box[1]);
  const int bw = (int)std::lround(box[2]), bh = (int)std::lround(box[3]);
  const int left = std::max(0, x), top = std::max(0, y);
  const int right = std::min(wPixels, x + bw), bottom = std::min(hPixels, y + bh);
  if (bw < 8 || bh < 8 || right <= left || bottom <= top) return false;

  // The core supplies OpenGL clip coordinates. World shaders convert them to Vulkan.
  static_assert(sizeof(Camera) == 440);
  alignas(8) Camera cam{};
  auto* camera = reinterpret_cast<uint8_t*>(&cam);
  if (rc_minimap_live_camera(&view, playerY, minY, camera) != sizeof(cam)) return false;
  Impl& w = *m;
  rc_frame_begin();
  const int result = w.worldless ? rc_scene_frame(camera, MINIMAP_LAYER, -1)
                                 : rc_frame_objects(camera, time, poses, nPoses, 0, MINIMAP_LAYER, -1);
  if (result < 0) return false;
  const int count = rc_frame_entry_count();
  const uint8_t* blocks = rc_frame_draw_blocks();
  for (int i = 0; i < count; ++i)
    memcpy(w.drawBuf.ptr + (size_t)(DRAW_CAP + i) * DRAW_STRIDE, blocks + (size_t)i * 192, 192);
  const int pb = rc_pass_block(camera);
  uint8_t* slot = w.passBuf.ptr + MINIMAP_SLOT * PASS_SLOT;
  memset(slot, 0, PASS_SLOT);
  if (pb > 0 && pb <= PASS_SLOT) memcpy(slot, rc_pass_block_ptr(), (size_t)pb);
  *(float*)(slot + 140) = getenv("QRAZY_NC_LINEAR") ? 0.f : 1.f;
  *(float*)(slot + 184) = *(float*)(slot + 188) = (float)time;
  passSwitches(w, slot);
  objPackLights(cam.view);
  memcpy(w.lightBuf.ptr + MINIMAP_SLOT * LIGHT_SLOT, rc_lights_block(), 480);

  const DrawItem* draws = nullptr;
  if (!w.worldless) {
    const uint8_t* packet = rc_packet_ptr();
    const Header* header = reinterpret_cast<const Header*>(packet);
    draws = reinterpret_cast<const DrawItem*>(packet + header->sec[SEC_DRAWS].offset);
    const double* stageFrames = w.stageOk ? rc_stage_frames(time) : nullptr;
    const auto* entries = reinterpret_cast<const FrameEntry*>(rc_frame_entries());
    for (int i = 0; i < count; ++i) {
      if (entries[i].kind != 0) continue;
      const uint32_t id = draws[entries[i].index].materialId;
      MatGpu& gpu = w.matGpu[id];
      if (!gpu.set || gpu.stamp == w.frameStamp) continue;
      gpu.stamp = w.frameStamp;
      w.fillMaterial(id, stageFrames, gpu.surface >= 0 ? w.portalPasses[gpu.surface] : nullptr);
    }
  }

  if (timestamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2);
  VkRect2D scissor = {{left, top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
  VkClearAttachment clear[2] = {};
  clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  clear[0].clearValue.color = {{10.f / 255, 18.f / 255, 22.f / 255, 1.f}};
  clear[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  clear[1].clearValue.depthStencil = {1.f, 0};
  VkClearRect rect = {scissor, 0, 1};
  vkCmdClearAttachments(cb, 2, clear, 1, &rect);
  VkViewport viewport = {(float)x, (float)y, (float)bw, (float)bh, 0, 1};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  vkCmdSetScissor(cb, 0, 1, &scissor);
  const float blend[4] = {};
  vkCmdSetBlendConstants(cb, blend);
  vkCmdSetDepthBias(cb, 0, 0, 0);
  uint32_t skipped = 0;
  recordDraws(w, cb, MINIMAP_SLOT, DRAW_CAP, count, reinterpret_cast<const FrameEntry*>(rc_frame_entries()),
              reinterpret_cast<const ObjectState*>(rc_frame_states()), draws, skipped);
  if (timestamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 3);
  return true;
}

// ---- the !showtriggers overlay (interface 8): rc_trigger_overlay's model record as scene nodes, one per mesh, on the transparent single-sided class 6; fills and edges (line segments) at the core's render orders (rc_trigger_order),
// so the core's sort draws them after the world's translucent surfaces. Built hidden; nothing in it moves.
bool World::buildTriggerOverlay(const std::string& fixes, const std::string& name, bool vql, bool runway) {
  Impl& w = *m; if (!w.obj) return false; Impl::Obj& o = *w.obj;
  if (!o.trig.empty()) {   // the previous overlay (a mode change): its nodes, buffers and label images go; descriptor sets and material blocks are kept for the next one
    vkDeviceWaitIdle(w.c.dev);
    for (int h : o.trig) { Impl::Obj::Inst& in = o.insts[(size_t)h]; rc_scene_destroy(in.node); objReleaseModel(w, in.model); o.modelOfObjectId[(size_t)h] = -1; }
    o.trig.clear();
  }
  const std::string fx = fixes.empty() ? "{}" : fixes, nm = name.empty() ? "map" : name;   // (the page's defaults, trigger-overlay.js readTriggerRecord)
  char* fp = (char*)rc_alloc((int)fx.size() + 1); memcpy(fp, fx.data(), fx.size()); fp[fx.size()] = 0; char* np = (char*)rc_alloc((int)nm.size() + 1); memcpy(np, nm.data(), nm.size()); np[nm.size()] = 0;
  const int len = runway ? rc_trigger_overlay("", 0, "", 0, 0, 1) : rc_trigger_overlay(fp, (int)fx.size(), np, (int)nm.size(), vql ? 1 : 0, 0);
  if (len < 0) { logf("trigger overlay: %s", rc_model_error()); return false; }
  const std::vector<uint8_t> rec(rc_model_ptr(), rc_model_ptr() + len);   // copied out at once: the next model call replaces it
  const uint8_t* r = rec.data(); const uint32_t nNodes = rd32(r + 12), nMeshes = rd32(r + 16); const uint8_t* mshs = r + 40 + 280 * nNodes; const uint8_t* mats = mshs + 48 * nMeshes; const uint32_t matStride = rd32(r + 4) >= 2 ? 104 : 88;
  int fills = 0, edges = 0; size_t texBytes = 0; static int generation = 0; ++generation;
  for (uint32_t k = 0; k < nMeshes; ++k) {
    const uint8_t* mk = mshs + 48 * k; if (rd32(mk + 16) == 0) continue; const bool line = rd32(mats + matStride * rd32(mk + 12) + 40) == 4;
    const int model = objModelFromRecord(w, "trigger-overlay-" + std::to_string(generation) + "-" + std::to_string(k), "trigger overlay", 1.0, false, false, nullptr, 0, nullptr, nullptr, nullptr, nullptr, (int)k, r); if (model < 0) continue;
    const int h = objAddNode(w, model, 1.0e7, 6); if (h < 0) { objReleaseModel(w, model); break; } const int node = o.insts[(size_t)h].node;   // one sphere round everything: never culled, and every node sorts at the origin as the page's do
    int32_t* order = (int32_t*)rc_scene_array(A_RENDER_ORDER); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
    order[node] = rc_trigger_order(line ? 1 : 0); if (line) flags[node] |= NODE_LINES; if (!o.trigShown) flags[node] &= ~2u;
    o.trig.push_back(h); if (line) ++edges; else ++fills; for (const GeoPart& g : o.models[(size_t)model].parts) if (g.hasTex) texBytes += (size_t)g.tex.w * g.tex.h * 4;
  }
  logf("trigger overlay: %d fill meshes, %d edge meshes, %.1f MB of label images (%s)", fills, edges, (double)texBytes / 1048576.0, o.trigShown ? "shown" : "hidden");
  return true;
}
void World::showTriggerOverlay(bool on) {
  Impl& w = *m; if (!w.obj || w.obj->trigShown == on) return; w.obj->trigShown = on; uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  for (int h : w.obj->trig) { const int node = w.obj->insts[(size_t)h].node; if (on) flags[node] |= 2u; else flags[node] &= ~2u; }
}

int World::addPickup(uint32_t weapon, const std::string& glbRel, const char* icons, double size,
                     uint32_t marker, double radius, double stretch) {
  Impl& w = *m;
  if (!w.obj) return -1;
  auto& cache = w.obj->modelByFile;
  if (weapon || !glbRel.empty()) {
    const std::string name = std::to_string(weapon);
    const std::string id = (weapon ? "pickup-weapon:" + name : "pickup-file:" + glbRel + ":" + icons)
                           + "@" + std::to_string(size);
    auto found = cache.find(id);
    int model = found == cache.end() ? -1 : found->second;
    if (found == cache.end()) {
      if (weapon) {
        if (rc_model_build(14) >= 0)
          model = objModelFromRecord(w, id, id, size, true, false, nullptr, 0, nullptr, nullptr, name.c_str());
      } else {
        model = objAddPickupFile(w, id, glbRel, size, icons);
      }
      cache[id] = model;
    }
    if (model >= 0) return objAddNode(w, model, size * 0.9, w.obj->models[model].transparent ? 2 : 1);
  }

  const std::string id = "pickup-marker:" + std::to_string(marker) + ":" +
                         std::to_string(radius) + ":" + std::to_string(stretch);
  auto found = cache.find(id);
  int model;
  if (found != cache.end()) {
    model = found->second;
  } else {
    const float r = (float)radius, y = (float)(radius * stretch);
    const std::vector<float> pos = {r, 0, 0, 0, 0, r, -r, 0, 0, 0, 0, -r, 0, y, 0, 0, -y, 0};
    const std::vector<uint32_t> idx = {4, 1, 0, 4, 2, 1, 4, 3, 2, 4, 0, 3,
                                       5, 0, 1, 5, 1, 2, 5, 2, 3, 5, 3, 0};
    double rgb[3];
    for (int c = 0; c < 3; ++c) rgb[c] = srgbToLinear(((marker >> (16 - 8 * c)) & 255) / 255.0);
    model = objBasicModel(w, pos, {}, idx, nullptr, 0, 0, rgb, 1.0, false);
    cache.emplace(id, model);
    logf("pickup marker: #%06x, radius %.1f, Y stretch %.2f", marker, radius, stretch);
  }
  return objAddNode(w, model, radius * std::max(1.0, stretch));
}
void World::setObject(int handle, const double pos[3], double yaw, bool visible) {
  Impl& w = *m; if (!w.obj || handle < 0 || handle >= (int)w.obj->insts.size()) return; const int node = w.obj->insts[handle].node;
  double* p = (double*)rc_scene_array(A_POS); double* q = (double*)rc_scene_array(A_QUAT); uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS);
  for (int k = 0; k < 3; ++k) p[node * 3 + k] = pos[k];
  q[node * 4] = 0; q[node * 4 + 1] = sin(yaw * 0.5); q[node * 4 + 2] = 0; q[node * 4 + 3] = cos(yaw * 0.5);   // about +y
  if (visible) flags[node] |= 2u; else flags[node] &= ~2u;   // F_VISIBLE
}

}  // namespace nc
