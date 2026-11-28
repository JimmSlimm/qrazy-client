// Included by nc_world.cpp after World::Impl: the object side of the executor. Objects are nodes of the core's resident scene store; the core culls, sorts and merges them into the frame list
// (kind 1 entries), together with the pipeline state it derives from each node's material class. This file owns what the core does not: vertex/index buffers, textures, descriptor sets.
// Models come from the core's GLB reader (rc_model_parse_glb); programs are the core's LIT variants in the Vulkan dialect.

struct GeoPart { uint32_t first = 0, count = 0; Image tex; bool hasTex = false, flat = false, clamp = false; /* flat: sampled with repeat wrapping, level 0 only (Material.wrapS 10497) */ double color[3] = {1, 1, 1}; double rough = 1, metal = 0, opacity = 1; };
struct ObjModel { bool transparent = false, hasEmission = false; float* opacityPtr = nullptr; float* diffusePtr = nullptr; int prog = -1; Buf vb, ib; std::vector<GeoPart> parts; std::vector<VkDescriptorSet> sets; std::vector<size_t> matOffs; double size = 1; std::vector<float*> glowPtrs, glowTimePtrs; };   // matOffs: each part's material block (768 bytes: 512 material + 256 stage); glow*: each part's powerup glow uniforms (view weapons)
enum { NODE_MESH = 16, NODE_SPRITE = 32, NODE_CLAIMED = 256, NODE_LINES = 4096 };   // NODE_LINES: the mesh is line segments (scene_store.hpp F_LINES -> the state's topology)
constexpr int PICKUP_GLOW_MATERIAL = 9;
enum { A_POS = 0, A_QUAT = 1, A_SCALE = 2, A_SPHERE = 5, A_PARENT = 16, A_RENDER_ORDER = 17, A_MATERIAL = 18, A_OBJECT_ID = 19, A_FLAGS = 20, A_LAYERS = 21, A_CLAIM_MASK = 23, A_MAT_FLAGS = 32, A_MAT_ID = 33 };

struct World::Impl::Obj {
  std::map<std::string, uint32_t> C;                       // the core's variant constants by name
  std::vector<ObjModel> models; std::map<std::string, int> modelByFile;
  Image pickupGlowTexture;
  std::map<std::pair<uint32_t, bool>, int> pickupGlowModels;
  struct Inst { int node, model, glowNode = -1; };
  std::vector<Inst> insts; std::vector<int> modelOfObjectId;   // objectId = 100 + handle -> model
  int nDir = 0, nHemi = 0; std::vector<int> gunHandles; std::map<int, std::array<double, 3>> gunLocal;   /* gunLocal: a weapon's own offset inside the rig (the page's hook choice group sits at z -5) */ std::vector<std::pair<int, int>> rotors; bool ready = false; /* rotors: scene node, spin axis (1 y, 2 z) */ std::map<uint32_t, int> litByKey; Image checker;   // litByKey: variant key -> program
  uint32_t objDraws = 0;
  std::map<int, std::vector<std::pair<VkDescriptorSet, size_t>>> freeSlots;   // program -> (descriptor set, material block) of released models, used again by the next model of that program
  struct Shadow { int node = -1; float* u[6] = {}; } shadows[World::SHADOWS];   // the players' ground shadows; u points into the slot's material block
  struct PathLine { int node = -1, model = -1; float *res = nullptr, *width = nullptr, *px = nullptr, *edge = nullptr, *diffuse = nullptr, *opacity = nullptr; } path[2];   // the ghost path: passed, ahead
  std::vector<int> pathBefore; int pathCount = 0;
  std::vector<int> trig; bool trigShown = false;   // the !showtriggers overlay: its node handles; shown or hidden
};

// The game's own rig (backend-util.js addSkyLights): a hemisphere light (sky c5eeff, ground 334252, intensity 2) and a white sun (intensity 2.2) at (-500, 1800, 1200); colours are linear x intensity. No ambient light.
static const double OBJ_LIGHTS[2][15] = {{2, 0, 0, 1.117, 1.708, 2.0, 0.066, 0.109, 0.169, 0, 1, 0, 0, 0, 0}, {3, 0, 0, 2.2, 2.2, 2.2, 0, 0, 0, -500, 1800, 1200, 0, 0, 0}};
// ambient + hemisphere + one directional light (placeholder rig: the game's own lights are not read yet); the directions are in view space, so the view goes in every pass
static void objPackLights(const double view[16]) {
  const int REC = rc_lights_info(4); double* in = rc_lights_in(); double* lv = rc_lights_view();
  for (int i = 0; i < 2; ++i) for (int k = 0; k < 15; ++k) in[i * REC + k] = OBJ_LIGHTS[i][k];
  for (int k = 0; k < 16; ++k) lv[k] = view[k];
  rc_lights_pack(2);
}

static void objInitShadows(World::Impl& w);
static void objInitGhostPath(World::Impl& w);
static int objAddNode(World::Impl& w, int model, double radius, int materialClass = 1, uint32_t layer = 1);
static void objInit(World::Impl& w) {
  auto o = std::make_unique<World::Impl::Obj>();
  for (int i = 0, n = rc_variant_const_count(); i < n; ++i) o->C[rc_variant_const_name(i)] = rc_variant_const_value(i);
  rc_scene_reset();
  int32_t* matId = (int32_t*)rc_scene_array(A_MAT_ID); uint32_t* matFlags = (uint32_t*)rc_scene_array(A_MAT_FLAGS);
  for (int i = 0; i < 9; ++i) { matFlags[i] = 0; matId[i] = 10 + i; }   // class 1 = opaque lit meshes
  // the pipeline state of class 1: three.js's MeshBasicMaterial defaults (an opaque, depth-tested, depth-writing, front-face material); the core derives the record
  std::map<std::string, int> factIndex; for (int i = 0, n = rc_object_state_fact_count(); i < n; ++i) factIndex[rc_object_state_fact_name(i)] = i;
  const char* defaults = "transparent=0 blending=1 blendEquation=100 blendSrc=204 blendDst=205 blendEquationAlpha=-1 blendSrcAlpha=-1 blendDstAlpha=-1 blendColorR=0 blendColorG=0 blendColorB=0 blendAlpha=0 premultipliedAlpha=0 depthTest=1 depthWrite=1 depthFunc=3 colorWrite=1 polygonOffset=0 polygonOffsetFactor=0 polygonOffsetUnits=0 side=0 allowTwoPass=1 alphaTest=0 spriteRotation=0 lineWidth=1 dashSize=1 gapSize=1 dashScale=1 dashOffset=0 dashed=0 worldUnits=0";
  { double* fa = rc_object_state_facts(); const std::string all(defaults); size_t a = 0;
    while (a < all.size()) { size_t b = all.find(' ', a); if (b == std::string::npos) b = all.size(); const std::string tok = all.substr(a, b - a); a = b + 1; if (tok.empty()) continue; const size_t e = tok.find('=');
      auto it = factIndex.find(tok.substr(0, e)); if (it == factIndex.end()) fatal("unknown object state fact %s", tok.c_str()); fa[it->second] = atof(tok.c_str() + e + 1); }
    if (rc_scene_material_set(1) != 0) fatal("rc_scene_material_set(1) failed");
    // class 2: a transparent, depth-tested, non-depth-writing, double-sided material (MeshBasicMaterial transparent + side double): text boards
    fa[factIndex["transparent"]] = 1; fa[factIndex["depthWrite"]] = 0; fa[factIndex["side"]] = 2;
    if (rc_scene_material_set(2) != 0) fatal("rc_scene_material_set(2) failed");
    // class 3: an impact mark (transparent, no depth write, double sided, pulled towards the camera by a polygon offset); class 4: the same added to the picture; class 5: an added sprite (sparks), no offset
    fa[factIndex["polygonOffset"]] = 1; fa[factIndex["polygonOffsetFactor"]] = -4; fa[factIndex["polygonOffsetUnits"]] = -6;
    if (rc_scene_material_set(3) != 0) fatal("rc_scene_material_set(3) failed");
    fa[factIndex["blending"]] = 2; if (rc_scene_material_set(4) != 0) fatal("rc_scene_material_set(4) failed");
    fa[factIndex["polygonOffset"]] = 0; fa[factIndex["polygonOffsetFactor"]] = 0; fa[factIndex["polygonOffsetUnits"]] = 0; if (rc_scene_material_set(5) != 0) fatal("rc_scene_material_set(5) failed");
    // class 6: transparent, depth-tested, no depth write, FRONT side only, normal blending (the trigger overlay: every face is in its mesh twice, once per side)
    fa[factIndex["blending"]] = 1; fa[factIndex["side"]] = 0; if (rc_scene_material_set(6) != 0) fatal("rc_scene_material_set(6) failed");
    // class 7: as 6 but double sided in one pass (the player shadows, player-shadows.js)
    fa[factIndex["side"]] = 2; fa[factIndex["allowTwoPass"]] = 0; if (rc_scene_material_set(7) != 0) fatal("rc_scene_material_set(7) failed");
    // class 8: the ghost path (ghost-path.js lineMaterial: transparent, no depth write, front side, polygon offset -4 / -4)
    fa[factIndex["side"]] = 0; fa[factIndex["allowTwoPass"]] = 1; fa[factIndex["polygonOffset"]] = 1; fa[factIndex["polygonOffsetFactor"]] = -4; fa[factIndex["polygonOffsetUnits"]] = -4; if (rc_scene_material_set(8) != 0) fatal("rc_scene_material_set(8) failed");
    // Pickup halos use the sprite pass: additive, depth tested, no depth write.
    matFlags[PICKUP_GLOW_MATERIAL] = 1;   // MC_TRANSPARENT
    matId[PICKUP_GLOW_MATERIAL] = 10 + PICKUP_GLOW_MATERIAL;
    fa[factIndex["blending"]] = 2; fa[factIndex["polygonOffset"]] = 0;
    if (rc_scene_material_set(PICKUP_GLOW_MATERIAL) != 0) fatal("pickup glow material failed");
  }
  const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; objPackLights(ident); o->nDir = rc_lights_info(1); o->nHemi = rc_lights_info(2);
  if (rc_lights_info(0) == 0) logf("note: the placeholder light rig does not fit the core's light packing; lit objects would be left out");
  const uint8_t px[4] = {255, 255, 255, 255}; o->checker = makeTexture(w.c, px, 1, 1, false);
  o->ready = true; w.obj = std::move(o);
  objInitShadows(w);
  objInitGhostPath(w);
  logf("objects ready: %d directional, %d hemisphere lights", w.obj->nDir, w.obj->nHemi);
}

// The LIT program for a material's facts (FACT_MAP, FACT_STANDARD, FACT_OPAQUE ...), the scene's light counts and fog mode: the key is the core's (rc_variant_key), Vulkan dialect.
static int objProgram(World::Impl& w, const char* family, uint32_t facts, int nDir, int nHemi, int fogMode) {
  World::Impl::Obj& o = *w.obj;
  const int k = rc_variant_key((int)o.C[family], facts, nDir, nHemi, fogMode); if (k < 0) fatal("rc_variant_key(%s, 0x%x) refused", family, facts);
  const uint32_t key = (uint32_t)k | o.C["DIALECT_VULKAN"]; { auto f = o.litByKey.find(key); if (f != o.litByKey.end()) return f->second; }
  const int len = rc_variant_build((int)o.C[family], key); if (len < 0) fatal("rc_variant_build(%s, 0x%x): %s", family, key, rc_vulkan_error());
  const std::string rec((const char*)rc_variant_ptr(), (size_t)len); Program p; memcpy(&p, rec.data(), sizeof p);
  const UniformSlot* slots = (const UniformSlot*)(rec.data() + sizeof(Program)); const std::string* blobp = new std::string(rec.data() + sizeof(Program) + p.uniformCount * sizeof(UniformSlot), rec.data() + rec.size());   // kept for the life of the program: slot names point into it
  const int idx = (int)w.progs.size(); w.progs.emplace_back(); w.buildProgram(w.progs[idx], p, slots, (const uint8_t*)blobp->data(), 1000 + idx, false);
  if (!w.progs[idx].ok) fatal("the %s object program did not compile", family);
  return o.litByKey[key] = idx;
}
static int objLitProgram(World::Impl& w, uint32_t facts) { return objProgram(w, "FAMILY_LIT", facts, w.obj->nDir, w.obj->nHemi, w.fogMode); }

// A model from the core's GLB record, merged with its node matrices, centred on its bounds and scaled so its largest side is `size` (item-models.js fitModel), laid out for the lit program.
// fit = true: centred on the bounds and scaled so the largest side is `size` (pickups); false: the record's own coordinates, as the core placed them (the runway, the view weapon rig).
// xf (may be null): turn about y, scale, then translation x y z (the fit of a contributed weapon file: rc_fit_weapon), applied to the baked vertices and normals
// part (with rotorName): 0 the whole record, 1 everything but the named rotor node and what hangs below it, 2 only that rotor, its vertices relative to the rotor node's origin (pivotOut); a rotor is a child node the host spins.
static int objModelFromRecord(World::Impl& w, const std::string& id, const std::string& glbRel, double size, bool fit, bool gun = false, const double* xf = nullptr, int part = 0, const char* rotorName = nullptr, double* pivotOut = nullptr, const char* meshName = nullptr, const double* tint = nullptr, int meshIndex = -1, const uint8_t* recIn = nullptr, std::string_view iconMaterial = {}) {
  World::Impl::Obj& o = *w.obj;
  const uint8_t* rec = recIn ? recIn : rc_model_ptr(); const uint32_t nNodes = rd32(rec + 12), nMeshes = rd32(rec + 16), nMaterials = rd32(rec + 20), nImages = rd32(rec + 24), blobBytes = rd32(rec + 28), total = rd32(rec + 32);
  const uint8_t* blob = rec + total - blobBytes; const uint8_t* mshs = rec + 40 + 280 * nNodes; const uint8_t* mats = mshs + 48 * nMeshes; const uint32_t matStride = rd32(rec + 4) >= 2 ? 104 : 88;   /* Header.version 2 adds metalness and roughness to the 88-byte material (rc_layout(27)) */
  if (rd32(rec + 4) >= 2 && rc_layout(27) != 104) logf("note: the core's Material is %d bytes, this client reads 104", rc_layout(27));
  const uint8_t* imgs = mats + matStride * nMaterials;
  std::vector<float> pos, nrm, uv, col; std::vector<uint32_t> idx; ObjModel m; m.size = size; bool anyColors = false, basic = false, transparent = false, line = false;   // col: vertex colours (a mesh whose normal slot holds colours, hasNormal == 2)
  m.hasEmission = (rd32(rec + 8) & 1u) != 0;
  struct Raw { uint32_t first, count; double color[3]; double rough = 1, metal = 0, opacity = 1; std::vector<uint8_t> rgba; int iw, ih; bool raw = false, flat = false, icon = false; }; bool standard = false, anyMap = false, haveIcon = false; std::vector<Raw> raws;
  int rotorNode = -1; double pivot[3] = {0, 0, 0};
  if (rotorName) for (uint32_t n = 0; n < nNodes; ++n) { const uint8_t* nd = rec + 40 + 280 * n; const uint32_t no = rd32(nd), nl = rd32(nd + 4); if (nl == strlen(rotorName) && !memcmp(blob + no, rotorName, nl)) { rotorNode = (int)n; break; } }
  if (rotorNode >= 0) { double Wn[16]; memcpy(Wn, rec + 40 + 280 * rotorNode + 152, 128); for (int c = 0; c < 3; ++c) pivot[c] = Wn[12 + c]; if (pivotOut) for (int c = 0; c < 3; ++c) pivotOut[c] = pivot[c]; }
  auto underRotor = [&](uint32_t node) { for (int guard = 0; guard < 32; ++guard) { if ((int)node == rotorNode) return true; const int32_t parent = (int32_t)rd32(rec + 40 + 280 * node + 8); if (parent < 0) return false; node = (uint32_t)parent; } return false; };
  for (uint32_t k = 0; k < nMeshes; ++k) {
    const uint8_t* mk = mshs + 48 * k; if (meshIndex >= 0 && (uint32_t)meshIndex != k) continue; if (meshName) { const uint32_t nl = rd32(mk + 4); if (nl != strlen(meshName) || memcmp(blob + rd32(mk), meshName, nl)) continue; }
    if (part && rotorNode >= 0) { const bool r = underRotor(rd32(mk + 8)); if ((part == 1 && r) || (part == 2 && !r)) continue; } const uint32_t vc = rd32(mk + 16), ic = rd32(mk + 20), po = rd32(mk + 24), no = rd32(mk + 28), uo = rd32(mk + 32), io = rd32(mk + 36), hn = rd32(mk + 40), hu = rd32(mk + 44);
    double W[16]; memcpy(W, rec + 40 + 280 * rd32(mk + 8) + 152, 128);   // the node's world matrix, column-major
    const uint32_t firstIdx = (uint32_t)idx.size(); const size_t base = pos.size() / 3; const float* P = (const float*)(blob + po); const float* N = (const float*)(blob + no); const float* U = (const float*)(blob + uo);
    for (uint32_t v = 0; v < vc; ++v) {
      for (int r = 0; r < 3; ++r) pos.push_back((float)(W[r] * P[v * 3] + W[4 + r] * P[v * 3 + 1] + W[8 + r] * P[v * 3 + 2] + W[12 + r]));
      if (hn == 2) { const float* C = (const float*)(blob + no); for (int r = 0; r < 3; ++r) col.push_back(C[v * 3 + r]); anyColors = true; } else for (int r = 0; r < 3; ++r) col.push_back(1.f);
      if (hn == 1) { double n[3]; for (int r = 0; r < 3; ++r) n[r] = W[r] * N[v * 3] + W[4 + r] * N[v * 3 + 1] + W[8 + r] * N[v * 3 + 2]; const double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); for (int r = 0; r < 3; ++r) nrm.push_back((float)(l > 0 ? n[r] / l : 0)); } else for (int r = 0; r < 3; ++r) nrm.push_back(0);
      if (hu) { uv.push_back(U[v * 2]); uv.push_back(U[v * 2 + 1]); } else { uv.push_back(0); uv.push_back(0); }
    }
    const uint32_t* I = (const uint32_t*)(blob + io); if (ic) for (uint32_t j = 0; j < ic; ++j) idx.push_back((uint32_t)base + I[j]); else for (uint32_t j = 0; j < vc; ++j) idx.push_back((uint32_t)base + j);
    const uint8_t* mat = mats + matStride * rd32(mk + 12); const int32_t img = (int32_t)rd32(mat + 12);
    Raw r; r.first = firstIdx; r.count = (uint32_t)idx.size() - firstIdx; memcpy(r.color, mat + 48, 24); r.iw = r.ih = 0;
    r.icon = !iconMaterial.empty() && iconMaterial == std::string_view((const char*)blob + rd32(mat), rd32(mat + 4));
    if (matStride >= 104) { memcpy(&r.metal, mat + 88, 8); memcpy(&r.rough, mat + 96, 8); }   // version 2: the lit PBR facts
    memcpy(&r.opacity, mat + 72, 8); if (rd32(mat + 8) & 8) transparent = true; if (rd32(mat + 40) == 2) basic = true; if (rd32(mat + 40) == 4) line = true;   /* shading 4: line segments (vertex pairs) */ r.flat = rd32(mat + 16) == 10497;
    if (rd32(mat + 40) == 3) standard = true; if (img >= 0) anyMap = true;   // Material.reserved = shading: 3 = lit PBR (the FACT_STANDARD material)
    if (img >= 0 && (uint32_t)img < nImages) { const uint8_t* im = imgs + 16 * img; const std::string mime((const char*)blob + rd32(im), rd32(im + 4)); int rw = 0, rh = 0;
      if (sscanf(mime.c_str(), "raw/rgba8;%dx%d", &rw, &rh) == 2) { if (rw > 0 && rh > 0 && (size_t)rw * rh * 4 <= rd32(im + 12)) { r.rgba.assign(blob + rd32(im + 8), blob + rd32(im + 8) + (size_t)rw * rh * 4); r.iw = rw; r.ih = rh; r.raw = true; } else logf("image %d of %s: bad raw size", img, glbRel.c_str()); }   // raw RGBA8 rows, the first row at v = 0 (sRGB colour, as the page reads them: core-models.js)
      else if (!decodeEncodedImage(blob + rd32(im + 8), rd32(im + 12), r.rgba, r.iw, r.ih)) logf("image %d of %s did not decode", img, glbRel.c_str()); }
    if (r.icon && !r.rgba.empty()) haveIcon = true;
    if (getenv("QRAZY_NC_DEBUGMODEL")) logf("  part %u: %u indices, colour %.6g %.6g %.6g, normals %d", k, r.count, r.color[0], r.color[1], r.color[2], (int)hn); raws.push_back(std::move(r));
  }
  if (idx.empty() || (!iconMaterial.empty() && !haveIcon)) return -1;
  // fit: centre the bounds, largest side = size
  const size_t nv = pos.size() / 3;
  if (part == 2) for (size_t v = 0; v < nv; ++v) for (int c = 0; c < 3; ++c) pos[v * 3 + c] -= (float)pivot[c];
  if (xf) {
    const double ct = cos(xf[0]), st = sin(xf[0]);
    for (size_t v = 0; v < nv; ++v) { const double x = pos[v * 3], z = pos[v * 3 + 2]; pos[v * 3] = (float)((x * ct + z * st) * xf[1] + xf[2]); pos[v * 3 + 1] = (float)(pos[v * 3 + 1] * xf[1] + xf[3]); pos[v * 3 + 2] = (float)((-x * st + z * ct) * xf[1] + xf[4]);
      if (v * 3 + 2 < nrm.size()) { const double nx = nrm[v * 3], nz = nrm[v * 3 + 2]; nrm[v * 3] = (float)(nx * ct + nz * st); nrm[v * 3 + 2] = (float)(-nx * st + nz * ct); } }
  }
  if (xf && getenv("QRAZY_NC_DEBUGMODEL")) { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; for (size_t v = 0; v < nv; ++v) for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], pos[v * 3 + c]); hi[c] = std::max(hi[c], pos[v * 3 + c]); } logf("  fitted %s: x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, %d colour meshes %d parts", glbRel.c_str(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], (int)anyColors, (int)raws.size()); }
  if (fit) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t v = 0; v < nv; ++v) for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], pos[v * 3 + c]); hi[c] = std::max(hi[c], pos[v * 3 + c]); }
    const float dims = std::max(1e-6f, std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]))); const float sc = (float)size / dims;
    for (size_t v = 0; v < nv; ++v) for (int c = 0; c < 3; ++c) pos[v * 3 + c] = (pos[v * 3 + c] - (lo[c] + hi[c]) * 0.5f) * sc;
  }
  // the facts the page would give this material (variant-facts.js): a colour map only when the record has one (a map program with nothing bound darkens everything), standard for PBR, opaque for the core's own models
  uint32_t facts = 0; if (anyMap || fit) facts |= o.C["FACT_MAP"]; if (standard) facts |= o.C["FACT_STANDARD"]; if (!fit) facts |= o.C["FACT_OPAQUE"];
  if (basic) { facts = (anyColors ? o.C["FACT_COLORS"] : 0u) | (anyMap ? o.C["FACT_MAP"] : 0u) | (transparent ? 0u : o.C["FACT_OPAQUE"]); }   // an unlit material (SHADE_BASIC): the basic program
  m.transparent = transparent;
  if (gun && !line) facts |= o.C["FACT_GLOW"];   // the powerup glow bands on every view weapon material (weapon-powerup-glow.js)
  // rc_viewgun_lights packs one directional light and no hemisphere lights.
  m.prog = line ? objProgram(w, "FAMILY_LINE", anyColors ? o.C["FACT_COLORS"] : 0u, 0, 0, gun ? 0 : w.fogMode) : basic ? objProgram(w, "FAMILY_BASIC", facts, 0, 0, gun ? 0 : w.fogMode) : gun ? objProgram(w, "FAMILY_LIT", facts, 1, 0, 0) : objLitProgram(w, facts);
  // (the view weapon uses its own light counts and no fog)
  const Prog& P = w.progs[m.prog]; const size_t floats = P.stride / 4; std::vector<float> vb(nv * floats);
  for (size_t v = 0; v < nv; ++v) { size_t at = v * floats; for (auto& a : P.inputs) { for (int c = 0; c < a.second; ++c) { float x = 0;
    if (a.first == "position") x = pos[v * 3 + c]; else if (a.first == "normal") x = nrm[v * 3 + c]; else if (a.first == "uv") x = uv[v * 2 + c]; else if (a.first == "color") x = c < 3 ? col[v * 3 + c] : 1.f;
    vb[at + c] = x; } at += (size_t)a.second; } }
  m.vb = makeBuf(w.c, vb.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); memcpy(m.vb.ptr, vb.data(), vb.size() * 4);
  m.ib = makeBuf(w.c, idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT); memcpy(m.ib.ptr, idx.data(), idx.size() * 4);
  for (auto& r : raws) { GeoPart g; g.first = r.first; g.count = r.count; memcpy(g.color, r.color, 24); g.rough = r.rough; g.metal = r.metal; g.opacity = r.opacity; if (tint) for (int c = 0; c < 3; ++c) g.color[c] *= tint[c]; if (!r.rgba.empty()) { g.tex = makeTexture(w.c, r.rgba.data(), r.iw, r.ih, false, r.raw || r.icon); g.hasTex = true; g.flat = r.flat; g.clamp = r.icon; } m.parts.push_back(std::move(g)); }
  // one descriptor set + material block per part (the values never change): the colour is the flattened colour of the glTF material, the map its texture or white
  for (const GeoPart& part : m.parts) {
    const size_t stageBytes = 256; size_t matOff; VkDescriptorSet set; auto& spare = o.freeSlots[m.prog];
    if (!spare.empty()) { set = spare.back().first; matOff = spare.back().second; spare.pop_back(); }   // a released model's set and block
    else { matOff = w.matBump; w.matBump += 512 + stageBytes; if (w.matBump > w.matBuf.size) fatal("material buffer full for object materials");
      VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = w.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &P.set2; NC_CK(vkAllocateDescriptorSets(w.c.dev, &ai, &set)); }
    const size_t stageOff = matOff + 512; m.sets.push_back(set); m.matOffs.push_back(matOff);
    std::vector<VkWriteDescriptorSet> ws; VkDescriptorBufferInfo bi[2] = {{w.matBuf.buf, matOff, 512}, {w.matBuf.buf, stageOff, 256}}; std::vector<VkDescriptorImageInfo> ii; ii.reserve(64);
    for (uint32_t b = 0; b < 2; ++b) { VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = set; wr.dstBinding = b; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr.pBufferInfo = &bi[b]; ws.push_back(wr); }
    const Image& tex = part.hasTex ? part.tex : o.checker;
    for (auto& u : P.slots) { if (u.block != BLK_SAMPLER) continue; const size_t first = ii.size(); for (uint32_t k = 0; k < (u.count ? u.count : 1); ++k) ii.push_back({part.clamp ? w.sampClamp : part.flat ? w.sampFlat : w.samp, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = set; wr.dstBinding = u.binding; wr.descriptorCount = u.count ? u.count : 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii[first]; ws.push_back(wr); }
    vkUpdateDescriptorSets(w.c.dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
    uint8_t* mb = w.matBuf.ptr + matOff; memset(mb, 0, 512); memset(w.matBuf.ptr + stageOff, 0, stageBytes);
    const double one[1] = {part.opacity}, zero3[3] = {0, 0, 0}, rough[1] = {part.rough}, metal[1] = {part.metal}, at0[1] = {0}, mt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (auto& u : P.slots) { if (u.block != BLK_MATERIAL) continue; const std::string n(P.names + u.nameOffset, u.nameLen);
      if (n == "diffuse") { putSlot(mb, u, part.color, 3); if (&part == &m.parts[0]) m.diffusePtr = (float*)(mb + u.offset); } else if (n == "emissive") putSlot(mb, u, zero3, 3); else if (n == "opacity") { putSlot(mb, u, one, 1); if (&part == &m.parts[0]) m.opacityPtr = (float*)(mb + u.offset); }
      else if (n == "weaponPowerups") m.glowPtrs.push_back((float*)(mb + u.offset)); else if (n == "weaponPowerupTime") m.glowTimePtrs.push_back((float*)(mb + u.offset));
      else if (n == "roughness") putSlot(mb, u, rough, 1); else if (n == "metalness") putSlot(mb, u, metal, 1); else if (n == "alphaTest") putSlot(mb, u, at0, 1);
      else if (n == "mapTransform" || n == "emissiveMapTransform") putSlot(mb, u, mt, 9); }
  }
  if (meshIndex < 0) logf("model %s: %zu vertices, %zu indices, %zu parts, %d textured", glbRel.c_str(), nv, idx.size(), m.parts.size(), (int)std::count_if(m.parts.begin(), m.parts.end(), [](const GeoPart& g) { return g.hasTex; }));
  const int handle = (int)o.models.size(); o.models.push_back(std::move(m)); o.modelByFile[id] = handle; return handle;
}

// A model that is no longer drawn (its nodes are gone and the device is idle): buffers and images are freed, its descriptor sets and material blocks go to the spare list. The slot in `models` stays (indices are handles).
static void objReleaseModel(World::Impl& w, int model) {
  World::Impl::Obj& o = *w.obj; if (model < 0 || model >= (int)o.models.size()) return; ObjModel& m = o.models[(size_t)model]; const VkDevice dev = w.c.dev;
  for (Buf* b : {&m.vb, &m.ib}) if (b->buf) { vkDestroyBuffer(dev, b->buf, nullptr); vkFreeMemory(dev, b->mem, nullptr); }
  for (GeoPart& g : m.parts) if (g.hasTex) { vkDestroyImageView(dev, g.tex.view, nullptr); vkDestroyImage(dev, g.tex.img, nullptr); vkFreeMemory(dev, g.tex.mem, nullptr); }
  for (size_t i = 0; i < m.sets.size() && i < m.matOffs.size(); ++i) o.freeSlots[m.prog].push_back({m.sets[i], m.matOffs[i]});
  for (auto it = o.modelByFile.begin(); it != o.modelByFile.end();) { if (it->second == model) it = o.modelByFile.erase(it); else ++it; }
  m = ObjModel();
}

static int objAddPickupFile(World::Impl& w, const std::string& id, const std::string& glbRel, double size, const char* icons) {
  std::vector<uint8_t> bytes; if (!w.fs->read(glbRel, bytes)) { logf("model not found: %s", glbRel.c_str()); return -1; }
  rc_frame_begin(); const uint8_t* cp = putCore(bytes);   // from the map's arena mark: model loads must not pile up
  if (rc_model_parse_glb(cp, (int)bytes.size()) < 0) { logf("rc_model_parse_glb(%s): %s", glbRel.c_str(), rc_model_error()); return -1; }
  return objModelFromRecord(w, id, glbRel, size, true, false, nullptr, 0, nullptr, nullptr, nullptr, nullptr, -1, nullptr, icons);
}

// A contributed weapon file fitted to the built-in shape of `kind` (3 plasma, 4 rocket) as the page does (weapon-fit.js): turned by rc_fit_turn(), its box measured, rc_fit_weapon(kind) -> scale, position, turn.
static int objAddFittedWeapon(World::Impl& w, const std::string& glbRel, int kind) {
  World::Impl::Obj& o = *w.obj; std::vector<uint8_t> bytes; if (!w.fs->read(glbRel, bytes)) return -1;
  rc_frame_begin(); const uint8_t* cp = putCore(bytes);   // from the map's arena mark: model loads must not pile up
  if (rc_model_parse_glb(cp, (int)bytes.size()) < 0) { logf("rc_model_parse_glb(%s): %s", glbRel.c_str(), rc_model_error()); return -1; }
  const double turn = rc_fit_turn(); const double* bb = rc_model_bounds(turn); if (!bb) return -1; double* fb = rc_fit_bounds(); for (int k = 0; k < 6; ++k) fb[k] = bb[k];
  const double* f = rc_fit_weapon(kind); if (!f) return -1; const double xf[5] = {turn, f[0], f[1], f[2], f[3]};   if (getenv("QRAZY_NC_DEBUGMODEL")) logf("  fit %s kind %d: turn %.3f bounds x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f -> scale %.3f pos %.1f %.1f %.1f rot %.3f", glbRel.c_str(), kind, turn, bb[0], bb[3], bb[1], bb[4], bb[2], bb[5], f[0], f[1], f[2], f[3], f[4]);   // f = scale, position xyz (then rotation y, which the page's wrapper does not apply for a file)
  // rc_fit_weapon builds the built-in shape to measure it (the plasma gun's, for one) and leaves THAT in the current model record: read the file again before building from it
  rc_frame_begin(); const uint8_t* cp2 = putCore(bytes); if (rc_model_parse_glb(cp2, (int)bytes.size()) < 0) return -1;
  return objModelFromRecord(w, "gun-file-" + glbRel, glbRel, 1.0, false, true, xf);
}

// one of the core's own models (rc_model_build kind: the runway, weapons ...) in the record's own coordinates
static int objAddCoreModel(World::Impl& w, int kind, bool gun = false, int part = 0, const char* rotorName = nullptr, double* pivotOut = nullptr, const char* meshName = nullptr, const double* tint = nullptr, int uniqueId = 0) {
  World::Impl::Obj& o = *w.obj; const std::string id = std::string(gun ? "gun-model-" : "core-model-") + std::to_string(kind) + (part ? "-part" + std::to_string(part) : std::string()) + (meshName ? std::string("-") + meshName : std::string()) + (uniqueId ? "-u" + std::to_string(uniqueId) : std::string()); auto f = o.modelByFile.find(id); if (f != o.modelByFile.end()) return f->second;
  rc_frame_begin(); if (rc_model_build(kind) < 0) { logf("rc_model_build(%d): %s", kind, rc_model_error()); return -1; }
  if (kind == 16) {
    if (const double* bounds = rc_model_bounds(0)) memcpy(w.mapBounds, bounds, sizeof w.mapBounds);
  }
  const int model = objModelFromRecord(w, id, id, 1.0, false, gun, nullptr, part, rotorName, pivotOut, meshName, tint);
  if (model < 0) o.modelByFile[id] = -1;   // the record has no such mesh: asked once
  return model;
}

static int objAddNode(World::Impl& w, int model, double radius, int materialClass, uint32_t layer) {
  World::Impl::Obj& o = *w.obj; const int node = rc_scene_create(); if (node < 0) { logf("scene store full"); return -1; }
  double* scale = (double*)rc_scene_array(A_SCALE); double* quat = (double*)rc_scene_array(A_QUAT); double* sphere = (double*)rc_scene_array(A_SPHERE);
  int32_t* parent = (int32_t*)rc_scene_array(A_PARENT); int32_t* material = (int32_t*)rc_scene_array(A_MATERIAL); int32_t* objectId = (int32_t*)rc_scene_array(A_OBJECT_ID);
  uint32_t* flags = (uint32_t*)rc_scene_array(A_FLAGS); uint32_t* layers = (uint32_t*)rc_scene_array(A_LAYERS); uint32_t* claim = (uint32_t*)rc_scene_array(A_CLAIM_MASK);
  parent[node] = -1; for (int k = 0; k < 3; ++k) scale[node * 3 + k] = 1; quat[node * 4] = quat[node * 4 + 1] = quat[node * 4 + 2] = 0; quat[node * 4 + 3] = 1;
  sphere[node * 4] = sphere[node * 4 + 1] = sphere[node * 4 + 2] = 0; sphere[node * 4 + 3] = radius; flags[node] |= NODE_MESH | NODE_CLAIMED; layers[node] = layer; claim[node] = layer; material[node] = materialClass;
  const int handle = (int)o.insts.size(); objectId[node] = 100 + handle; o.insts.push_back({node, model}); if ((int)o.modelOfObjectId.size() <= handle) o.modelOfObjectId.resize((size_t)handle + 1, -1); o.modelOfObjectId[handle] = model;
  return handle;
}

// A flat text board: a quad at (0, yc, z) of halfW x halfH facing -z (the player's side at the start), with an RGBA texture (text rasterised by the text engine), drawn unlit (the basic program with a map)
// through the transparent class 2. Vertices are in the sim's frame already, so the node sits at identity.
// A small unlit model from raw arrays (the program is the core's BASIC family: a colour map when rgba is given, opaque unless `transparent`), with its own material block so its colour and opacity can change per instance.
// Used for text boards, impact marks, sparks, beams. Returns the model index; `opacityPtr` / `diffusePtr` of the model point into the material block.
static void objWriteBasicVertices(const Prog& P, float* vb, const float* pos, const float* uv, size_t nv) {
  const size_t floats = P.stride / 4;
  for (size_t v = 0; v < nv; ++v) { size_t at = v * floats; for (auto& a : P.inputs) { for (int c = 0; c < a.second; ++c) { float x = 0;
    if (a.first == "position") x = pos[v * 3 + c]; else if (a.first == "normal") x = c == 2 ? -1.f : 0.f; else if (a.first == "uv" && uv) x = uv[v * 2 + c]; else if (a.first == "color") x = 1.f;
    vb[at + c] = x; } at += (size_t)a.second; } }
}

static int objBasicModel(World::Impl& w, const std::vector<float>& pos, const std::vector<float>& uv, const std::vector<uint32_t>& idx, const uint8_t* rgba, int tw, int th, const double diffuse[3], double opacity, bool transparent, const Image* spriteTexture = nullptr) {
  World::Impl::Obj& o = *w.obj; ObjModel m; m.size = 1; m.transparent = transparent; const size_t nv = pos.size() / 3;
  const uint32_t facts = ((rgba || spriteTexture) ? o.C["FACT_MAP"] : 0u) |
    (transparent ? 0u : o.C["FACT_OPAQUE"]) | (spriteTexture ? o.C["FACT_ATTENUATION"] : 0u);
  m.prog = objProgram(w, spriteTexture ? "FAMILY_SPRITE" : "FAMILY_BASIC", facts, 0, 0, w.fogMode); const Prog& P = w.progs[m.prog];
  m.vb = makeBuf(w.c, nv * P.stride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  objWriteBasicVertices(P, reinterpret_cast<float*>(m.vb.ptr), pos.data(), uv.empty() ? nullptr : uv.data(), nv);
  m.ib = makeBuf(w.c, idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT); memcpy(m.ib.ptr, idx.data(), idx.size() * 4);
  GeoPart g; g.first = 0; g.count = (uint32_t)idx.size(); if (rgba) { g.tex = makeTexture(w.c, rgba, tw, th, false); g.hasTex = true; } m.parts.push_back(std::move(g));
  const GeoPart& part = m.parts[0];
  const size_t matOff = w.matBump; w.matBump += 512; const size_t stageOff = w.matBump; w.matBump += 256; if (w.matBump > w.matBuf.size) fatal("material buffer full");
  VkDescriptorSet set; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = w.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &P.set2; NC_CK(vkAllocateDescriptorSets(w.c.dev, &ai, &set)); m.sets.push_back(set);
  std::vector<VkWriteDescriptorSet> ws; VkDescriptorBufferInfo bi[2] = {{w.matBuf.buf, matOff, 512}, {w.matBuf.buf, stageOff, 256}}; std::vector<VkDescriptorImageInfo> ii; ii.reserve(64);
  for (uint32_t b = 0; b < 2; ++b) { VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = set; wr.dstBinding = b; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr.pBufferInfo = &bi[b]; ws.push_back(wr); }
  for (auto& u : P.slots) { if (u.block != BLK_SAMPLER) continue; const size_t first = ii.size(); const Image& tex = spriteTexture ? *spriteTexture : part.hasTex ? part.tex : o.checker; for (uint32_t k = 0; k < (u.count ? u.count : 1); ++k) ii.push_back({spriteTexture ? w.sampClamp : w.samp, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet = set; wr.dstBinding = u.binding; wr.descriptorCount = u.count ? u.count : 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii[first]; ws.push_back(wr); }
  vkUpdateDescriptorSets(w.c.dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
  uint8_t* mb = w.matBuf.ptr + matOff; memset(mb, 0, 512); memset(w.matBuf.ptr + stageOff, 0, 256);
  const double op[1] = {opacity}, zero3[3] = {0, 0, 0}, at0[1] = {0}, mt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (auto& u : P.slots) { if (u.block != BLK_MATERIAL) continue; const std::string n(P.names + u.nameOffset, u.nameLen);
    if (n == "diffuse") { putSlot(mb, u, diffuse, 3); m.diffusePtr = (float*)(mb + u.offset); } else if (n == "emissive") putSlot(mb, u, zero3, 3); else if (n == "opacity") { putSlot(mb, u, op, 1); m.opacityPtr = (float*)(mb + u.offset); } else if (n == "alphaTest") putSlot(mb, u, at0, 1);
    else if (n == "mapTransform") putSlot(mb, u, mt, 9); }
  const int model = (int)o.models.size(); o.models.push_back(std::move(m)); return model;
}

// The player shadows: one program, one quad and one pipeline (class 7) for all slots; each slot has its own material block (its uniforms) and node. All made here, nothing later.
static void objInitShadows(World::Impl& w) {
  World::Impl::Obj& o = *w.obj;
  const int len = rc_variant_build((int)o.C["FAMILY_EFFECT"], o.C["EFFECT_SHADOW"] | o.C["DIALECT_VULKAN"]); if (len < 0) fatal("rc_variant_build(effect shadow): %s", rc_vulkan_error());
  const std::string rec((const char*)rc_variant_ptr(), (size_t)len); Program p; memcpy(&p, rec.data(), sizeof p);
  const UniformSlot* slots = (const UniformSlot*)(rec.data() + sizeof(Program)); const std::string* blobp = new std::string(rec.data() + sizeof(Program) + p.uniformCount * sizeof(UniformSlot), rec.data() + rec.size());
  const int prog = (int)w.progs.size(); w.progs.emplace_back(); w.buildProgram(w.progs[prog], p, slots, (const uint8_t*)blobp->data(), 1000 + prog, false);
  if (!w.progs[prog].ok) fatal("the shadow program did not compile");
  const Prog& P = w.progs[prog]; const size_t floats = P.stride / 4; std::vector<float> vb(4 * floats, 0.f);
  const float pos[4][3] = {{-.5f, .5f, 0}, {.5f, .5f, 0}, {-.5f, -.5f, 0}, {.5f, -.5f, 0}}, uv[4][2] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}}; const uint32_t idx[6] = {0, 2, 1, 2, 3, 1};   // x, y in -0.5..0.5, uv 0..1, v along +y
  for (size_t v = 0; v < 4; ++v) { size_t at = v * floats; for (auto& a : P.inputs) { for (int c = 0; c < a.second; ++c) vb[at + c] = a.first == "position" ? pos[v][c] : a.first == "uv" ? uv[v][c] : 0.f; at += (size_t)a.second; } }
  ObjModel quad; quad.transparent = true; quad.prog = prog; quad.vb = makeBuf(w.c, vb.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); memcpy(quad.vb.ptr, vb.data(), vb.size() * 4);
  quad.ib = makeBuf(w.c, sizeof idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT); memcpy(quad.ib.ptr, idx, sizeof idx);
  GeoPart g; g.count = 6; quad.parts.push_back(std::move(g));
  for (int s = 0; s < World::SHADOWS; ++s) {
    ObjModel m = quad;   // the buffers are shared: these models are never released
    const size_t matOff = w.matBump; w.matBump += 768; if (w.matBump > w.matBuf.size) fatal("material buffer full (shadows)");
    VkDescriptorSet set; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = w.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &P.set2; NC_CK(vkAllocateDescriptorSets(w.c.dev, &ai, &set)); m.sets.push_back(set);
    VkWriteDescriptorSet ws[2] = {}; VkDescriptorBufferInfo bi[2] = {{w.matBuf.buf, matOff, 512}, {w.matBuf.buf, matOff + 512, 256}};
    for (uint32_t b = 0; b < 2; ++b) { ws[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ws[b].dstSet = set; ws[b].dstBinding = b; ws[b].descriptorCount = 1; ws[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ws[b].pBufferInfo = &bi[b]; }
    vkUpdateDescriptorSets(w.c.dev, 2, ws, 0, nullptr);
    uint8_t* mb = w.matBuf.ptr + matOff; memset(mb, 0, 768);
    World::Impl::Obj::Shadow& sh = o.shadows[s];
    for (auto& u : P.slots) if (u.block == BLK_MATERIAL && u.source == US_EFFECT && u.arg < 6) sh.u[u.arg] = (float*)(mb + u.offset);
    for (int k = 0; k < 6; ++k) if (!sh.u[k]) fatal("the shadow program has no uniform %d", k);
    const int model = (int)o.models.size(); o.models.push_back(std::move(m)); const int h = objAddNode(w, model, 64.0, 7); if (h < 0) fatal("scene store full (shadows)"); sh.node = o.insts[(size_t)h].node;
  }
  ObjectState state; memcpy(&state, rc_scene_material_state(7), sizeof state); w.pipelineFor((uint32_t)prog, state);   // the pipeline exists before the first frame
  logf("player shadows ready: %d slots", (int)World::SHADOWS);
}

// The ghost path's program, pipeline, material blocks and (hidden) nodes, made at load; the line buffers come with a track.
static void objInitGhostPath(World::Impl& w) {
  World::Impl::Obj& o = *w.obj;
  const int len = rc_variant_build((int)o.C["FAMILY_EFFECT"], o.C["EFFECT_GHOST_LINE"] | o.C["DIALECT_VULKAN"]); if (len < 0) fatal("rc_variant_build(effect ghost line): %s", rc_vulkan_error());
  const std::string rec((const char*)rc_variant_ptr(), (size_t)len); Program p; memcpy(&p, rec.data(), sizeof p);
  const UniformSlot* slots = (const UniformSlot*)(rec.data() + sizeof(Program)); const std::string* blobp = new std::string(rec.data() + sizeof(Program) + p.uniformCount * sizeof(UniformSlot), rec.data() + rec.size());
  const int prog = (int)w.progs.size(); w.progs.emplace_back(); w.buildProgram(w.progs[prog], p, slots, (const uint8_t*)blobp->data(), 1000 + prog, false);
  if (!w.progs[prog].ok) { logf("ghost path unavailable: its program did not compile"); return; }
  const Prog& P = w.progs[prog];
  for (int s = 0; s < 2; ++s) {
    World::Impl::Obj::PathLine& L = o.path[s]; ObjModel m; m.transparent = true; m.prog = prog; m.parts.push_back(GeoPart());
    const size_t matOff = w.matBump; w.matBump += 768; if (w.matBump > w.matBuf.size) fatal("material buffer full (ghost path)");
    VkDescriptorSet set; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = w.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &P.set2; NC_CK(vkAllocateDescriptorSets(w.c.dev, &ai, &set)); m.sets.push_back(set);
    VkWriteDescriptorSet ws[2] = {}; VkDescriptorBufferInfo bi[2] = {{w.matBuf.buf, matOff, 512}, {w.matBuf.buf, matOff + 512, 256}};
    for (uint32_t b = 0; b < 2; ++b) { ws[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ws[b].dstSet = set; ws[b].dstBinding = b; ws[b].descriptorCount = 1; ws[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ws[b].pBufferInfo = &bi[b]; }
    vkUpdateDescriptorSets(w.c.dev, 2, ws, 0, nullptr);
    uint8_t* mb = w.matBuf.ptr + matOff; memset(mb, 0, 768);
    for (auto& u : P.slots) { if (u.block != BLK_MATERIAL) continue; const std::string n(P.names + u.nameOffset, u.nameLen); float* at = (float*)(mb + u.offset);
      if (n == "resolution") L.res = at; else if (n == "linewidth") L.width = at; else if (n == "pxLimits") L.px = at; else if (n == "edgeCfg") L.edge = at; else if (n == "diffuse") L.diffuse = at; else if (n == "opacity") L.opacity = at; }
    if (!L.res || !L.width || !L.px || !L.edge || !L.diffuse || !L.opacity) { logf("ghost path unavailable: a uniform is missing"); return; }
    L.px[0] = 1; L.px[1] = 6; L.edge[0] = 1; L.edge[1] = 3;
    L.model = (int)o.models.size(); o.models.push_back(std::move(m)); const int node = objAddNode(w, L.model, 1.0e7, 8, 4); if (node < 0) { L.model = -1; return; }   // layer 4: the player's camera, no mirror
    L.node = o.insts[(size_t)node].node; ((int32_t*)rc_scene_array(A_RENDER_ORDER))[L.node] = 1000; ((uint32_t*)rc_scene_array(A_FLAGS))[L.node] &= ~2u;
  }
  ObjectState state; memcpy(&state, rc_scene_material_state(8), sizeof state); w.pipelineFor((uint32_t)prog, state);
}

// A flat text board: a quad at (0, yc, z) of halfW x halfH turned `yaw` about y (the core's runway rows turn it to face the approach), with an RGBA texture (text rasterised by the text engine), drawn unlit through the transparent class 2.
// Vertices are in the sim's frame already, so the node sits at identity.
static int objAddBoard(World::Impl& w, const uint8_t* rgba, int tw, int th, double z, double yc, double halfW, double halfH, double yaw) {
  const float x = (float)(halfW * cos(yaw)), dz = (float)(halfW * sin(yaw)), top = (float)(yc + halfH), bottom = (float)(yc - halfH), z0 = (float)z + dz, z1 = (float)z - dz;   // u = 0 is the plane's local -x edge
  const std::vector<float> pos = {-x, top, z0, x, top, z1, -x, bottom, z0, x, bottom, z1};
  const std::vector<float> uv = {0, 0, 1, 0, 0, 1, 1, 1}; const std::vector<uint32_t> idx = {0, 1, 2, 2, 1, 3}; const double white[3] = {1, 1, 1};
  const int model = objBasicModel(w, pos, uv, idx, rgba, tw, th, white, 1.0, true);
  return objAddNode(w, model, 20000.0, 2);
}
