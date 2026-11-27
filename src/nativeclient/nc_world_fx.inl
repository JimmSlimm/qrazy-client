// Included by nc_world.cpp after the object side: the weapon effects the sim reports as events. Impact marks, sparks, explosions, projectiles and the rail beam are nodes of the object scene with their own material
// blocks (so each can fade); nothing here decides what happens, the sim does. Looks and numbers are the page's (marks.js, main.js blastStyle / showProjectile).

namespace {
struct Rgba { float r, g, b, a; };
struct Stop { float at; Rgba c; };

// the radial canvas gradients of marks.js, 128 x 128 straight-alpha RGBA
static std::vector<uint8_t> radialTexture(const std::vector<Stop>& stops) {
  std::vector<uint8_t> px(128 * 128 * 4);
  for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
    const float d = std::min(1.f, std::sqrt((x + .5f - 64) * (x + .5f - 64) + (y + .5f - 64) * (y + .5f - 64)) / 64.f); Rgba c = stops.back().c;
    for (size_t k = 0; k + 1 < stops.size(); ++k) if (d <= stops[k + 1].at) { const float t = stops[k + 1].at > stops[k].at ? (d - stops[k].at) / (stops[k + 1].at - stops[k].at) : 0.f; const Rgba& a = stops[k].c; const Rgba& b = stops[k + 1].c; c = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; break; }
    uint8_t* p = &px[((size_t)y * 128 + x) * 4]; p[0] = (uint8_t)(c.r + .5f); p[1] = (uint8_t)(c.g + .5f); p[2] = (uint8_t)(c.b + .5f); p[3] = (uint8_t)(std::min(1.f, c.a) * 255.f + .5f);
  }
  return px;
}

static void quatFromUnitVectors(double fx, double fy, double fz, double tx, double ty, double tz, double q[4]) {   // scene-builder.js setQuatFromUnitVectors
  double x, y, z, w = fx * tx + fy * ty + fz * tz + 1;
  if (w < 2.2e-16) { w = 0; if (fabs(fx) > fabs(fz)) { x = -fy; y = fx; z = 0; } else { x = 0; y = -fz; z = fy; } } else { x = fy * tz - fz * ty; y = fz * tx - fx * tz; z = fx * ty - fy * tx; }
  const double l = sqrt(x * x + y * y + z * z + w * w); if (l == 0) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; } q[0] = x / l; q[1] = y / l; q[2] = z / l; q[3] = w / l;
}
static void quatMul(const double a[4], const double b[4], double o[4]) {   // a * b
  o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1]; o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0]; o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3]; o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void quatFromRows(const double R[3][3], double q[4]) {   // a rotation matrix (row r, column c) as a unit quaternion
  const double tr = R[0][0] + R[1][1] + R[2][2];
  if (tr > 0) { const double s = sqrt(tr + 1.0) * 2; q[3] = 0.25 * s; q[0] = (R[2][1] - R[1][2]) / s; q[1] = (R[0][2] - R[2][0]) / s; q[2] = (R[1][0] - R[0][1]) / s; }
  else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { const double s = sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2; q[3] = (R[2][1] - R[1][2]) / s; q[0] = 0.25 * s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = (R[0][2] + R[2][0]) / s; }
  else if (R[1][1] > R[2][2]) { const double s = sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2; q[3] = (R[0][2] - R[2][0]) / s; q[0] = (R[0][1] + R[1][0]) / s; q[1] = 0.25 * s; q[2] = (R[1][2] + R[2][1]) / s; }
  else { const double s = sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2; q[3] = (R[1][0] - R[0][1]) / s; q[0] = (R[0][2] + R[2][0]) / s; q[1] = (R[1][2] + R[2][1]) / s; q[2] = 0.25 * s; }
}
static double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
}  // namespace

struct FxState {
  struct MarkStyle { double radius; bool additive; std::vector<Stop> stops; int cap; };
  struct Slot { int model = -1, node = -1; double age = 0, life = 0; bool on = false; };
  std::vector<MarkStyle> styles; std::vector<std::vector<Slot>> marks; std::vector<size_t> next;   // marks[style]: a ring of slots
  struct Spark { int node = -1; double age = 0, life = 0, v[3] = {}, p[3] = {}, size = 1; bool on = false; };
  std::vector<Spark> sparks; size_t sparkNext = 0; int sparkModel = -1;
  struct Blast { int model = -1, node = -1, smodel = -1, snode = -1; double age = 0, life = 0, radius = 1, at[3] = {}, normal[3] = {}; bool on = false, splash = false, hasNormal = false, ghost = false; };   // sphere (model/node) or the plasma splash (smodel/snode, the core's rc_splash_pose)
  std::vector<Blast> blasts; size_t blastNext = 0;
  struct Proj { int node = -1; int weapon = 0; };
  std::map<int, Proj> projs; std::map<int, std::vector<int>> freeProj;   // live projectiles by id; nodes to reuse per weapon
  struct Beam { std::vector<int> model, node; std::vector<float> base; std::vector<bool> spiral; double age = 0, life = 0; bool on = false; };   // one model + node per mesh of the core's beam record
  std::vector<Beam> beams; size_t beamNext = 0;
  struct HookSet { std::vector<int> links; int head = -1; bool on = false; }; std::vector<HookSet> hooks;   // the hook cables (slot 0 = the player's, one more for each other player): a node per chain link and the head
  double markTime = 5.0; unsigned rng = 12345;
  double rnd() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }
  FxState() {
    auto S = [](float at, float r, float g, float b, float a) { return Stop{at, {r, g, b, a * 1.f}}; };
    styles = {   // 0 bullet, 1 railBurn, 2 railGlow, 3 scorch (the radius is the blast's), 4 plasma, 5 plasmaBurn
      {2.5, false, {S(0, 0, 0, 0, 1), S(.35f, 10, 10, 10, .95f), S(.5f, 120, 110, 95, .6f), S(.75f, 40, 38, 35, .35f), S(1, 0, 0, 0, 0)}, 96},
      {7, false, {S(0, 0, 0, 0, .95f), S(.45f, 0, 15, 20, .7f), S(1, 0, 0, 0, 0)}, 16},
      {10, true, {S(0, 0, 0, 0, 0), S(.35f, 40, 200, 255, .2f), S(.55f, 120, 240, 255, .9f), S(.7f, 40, 160, 255, .4f), S(1, 0, 0, 0, 0)}, 16},
      {10, false, {S(0, 0, 0, 0, .85f), S(.35f, 8, 6, 4, .65f), S(.7f, 15, 12, 10, .3f), S(1, 0, 0, 0, 0)}, 48},
      {9, true, {S(0, 170, 220, 255, 1), S(.3f, 60, 130, 255, .85f), S(.7f, 20, 50, 200, .35f), S(1, 0, 0, 0, 0)}, 16},
      {7.5, false, {S(0, 0, 5, 25, .8f), S(.6f, 0, 10, 40, .4f), S(1, 0, 0, 0, 0)}, 16}};
    marks.resize(styles.size()); next.assign(styles.size(), 0); for (size_t i = 0; i < styles.size(); ++i) marks[i].resize((size_t)styles[i].cap);
    sparks.resize(96); blasts.resize(8); beams.resize(4);
  }
};

static FxState& fxOf(World::Impl& w) { if (!w.fxp) w.fxp = std::make_shared<FxState>(); return *static_cast<FxState*>(w.fxp.get()); }
static void fxNode(World::Impl& w, int handle, const double* pos, const double* quat, double sx, double sy, double sz, bool visible) {
  if (handle < 0) return;
  const int node = w.obj->insts[handle].node; double* p = (double*)rc_scene_array(A_POS); double* q = (double*)rc_scene_array(A_QUAT); double* s = (double*)rc_scene_array(A_SCALE); uint32_t* fl = (uint32_t*)rc_scene_array(A_FLAGS);
  if (pos) for (int k = 0; k < 3; ++k) p[node * 3 + k] = pos[k]; if (quat) for (int k = 0; k < 4; ++k) q[node * 4 + k] = quat[k]; s[node * 3] = sx; s[node * 3 + 1] = sy; s[node * 3 + 2] = sz;
  if (visible) fl[node] |= 2u; else fl[node] &= ~2u;
}

static void fxNodeHide(World::Impl& w, int handle) { const double away[3] = {0, -1e5, 0}; fxNode(w, handle, away, nullptr, 1, 1, 1, false); }   // a new node starts visible at the origin: hidden until it is posed

static void fxMarkImpl(World::Impl& w, int style, const double pos[3], const double n[3], double radius) {
  FxState& fx = fxOf(w); if (style < 0 || style >= (int)fx.styles.size()) return; const FxState::MarkStyle& st = fx.styles[(size_t)style];
  FxState::Slot& s = fx.marks[(size_t)style][fx.next[(size_t)style]]; fx.next[(size_t)style] = (fx.next[(size_t)style] + 1) % fx.marks[(size_t)style].size();
  if (s.model < 0) {   // made on first use: a unit quad with the style's gradient and its own material block
    const std::vector<uint8_t> tex = radialTexture(st.stops); const std::vector<float> pos4 = {-1, 1, 0, 1, 1, 0, -1, -1, 0, 1, -1, 0}, uv4 = {0, 0, 1, 0, 0, 1, 1, 1}; const std::vector<uint32_t> idx = {0, 1, 2, 2, 1, 3}; const double white[3] = {1, 1, 1};
    s.model = objBasicModel(w, pos4, uv4, idx, tex.data(), 128, 128, white, 1.0, true); s.node = objAddNode(w, s.model, 2.0, st.additive ? 4 : 3); fxNodeHide(w, s.node);
  }
  const double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (len < 1e-9) return; const double nn[3] = {n[0] / len, n[1] / len, n[2] / len};
  double q0[4], qz[4], q[4]; quatFromUnitVectors(0, 0, 1, nn[0], nn[1], nn[2], q0); const double roll = fx.rnd() * 6.283185307; qz[0] = qz[1] = 0; qz[2] = sin(roll / 2); qz[3] = cos(roll / 2); quatMul(q0, qz, q);   // each mark turned about its normal at random
  const double r = radius > 0 ? radius : st.radius, at[3] = {pos[0] + nn[0] * 0.1, pos[1] + nn[1] * 0.1, pos[2] + nn[2] * 0.1};
  fxNode(w, s.node, at, q, r, r, r, true); if (w.obj->models[(size_t)s.model].opacityPtr) *w.obj->models[(size_t)s.model].opacityPtr = 1.f; s.age = 0; s.life = fx.markTime; s.on = true;
}

static void fxSparksImpl(World::Impl& w, const double pos[3], const double n[3]) {
  FxState& fx = fxOf(w);
  if (fx.sparkModel < 0) {
    const std::vector<uint8_t> tex = radialTexture({{0, {255, 255, 220, 1}}, {.3f, {255, 200, 80, .9f}}, {.7f, {255, 110, 20, .3f}}, {1, {0, 0, 0, 0}}});
    const std::vector<float> pos4 = {-1, 1, 0, 1, 1, 0, -1, -1, 0, 1, -1, 0}, uv4 = {0, 0, 1, 0, 0, 1, 1, 1}; const std::vector<uint32_t> idx = {0, 1, 2, 2, 1, 3}; const double white[3] = {1, 1, 1};
    fx.sparkModel = objBasicModel(w, pos4, uv4, idx, tex.data(), 128, 128, white, 1.0, true);
    for (auto& sp : fx.sparks) { sp.node = objAddNode(w, fx.sparkModel, 2.0, 5); fxNodeHide(w, sp.node); }   // one shared model and material: sparks do not fade by opacity, they shrink
  }
  const double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); const double nn[3] = {len > 0 ? n[0] / len : 0, len > 0 ? n[1] / len : 1, len > 0 ? n[2] / len : 0};
  for (int k = 0; k < 3; ++k) {   // SPARKS_PER_HIT 3, marks.js addSparks
    FxState::Spark& p = fx.sparks[fx.sparkNext]; fx.sparkNext = (fx.sparkNext + 1) % fx.sparks.size(); const double sp = 60 + fx.rnd() * 120;
    p.v[0] = nn[0] * sp + (fx.rnd() - .5) * 140; p.v[1] = nn[1] * sp + (fx.rnd() - .5) * 140 + 30; p.v[2] = nn[2] * sp + (fx.rnd() - .5) * 140;
    p.age = 0; p.life = .18 + fx.rnd() * .2; p.size = 1.3 + fx.rnd() * .9; p.on = true; for (int c = 0; c < 3; ++c) p.p[c] = pos[c] + nn[c] * .5;
  }
}

static void fxBlastImpl(World::Impl& w, int weapon, const double pos[3], double radius, const double* normal, bool ghost) {   // ghost: another player's blast, grey and fainter (demo-viewer.js makeBlastMesh)
  FxState& fx = fxOf(w); FxState::Blast& b = fx.blasts[fx.blastNext]; const size_t slot = fx.blastNext; fx.blastNext = (fx.blastNext + 1) % fx.blasts.size(); const double away[3] = {0, -1e5, 0};
  if (b.snode >= 0) fxNode(w, b.snode, away, nullptr, 1, 1, 1, false); if (b.node >= 0) fxNode(w, b.node, away, nullptr, 1, 1, 1, false);
  const bool plasma = weapon == 8, bfg = weapon == 9; const uint32_t hex = plasma ? 0x66c8ff : ghost ? 0xb8bcc4 : bfg ? 0x7dff4d : 0xffa040; b.ghost = ghost;
  if (plasma) {   // main.js spawnBlast: the plasma splash (plasma-splash.js), the core's plane and pose; it faces the surface normal, else the camera
    if (b.smodel < 0) { b.smodel = objAddCoreModel(w, 15, false, 0, nullptr, nullptr, "splash", nullptr, (int)slot + 101); if (b.smodel >= 0) { b.snode = objAddNode(w, b.smodel, 2.0, 5); fxNodeHide(w, b.snode); } }
    if (b.smodel >= 0) { b.splash = true; b.life = .12; b.age = 0; b.on = true; b.hasNormal = normal != nullptr; for (int c = 0; c < 3; ++c) { b.at[c] = pos[c]; b.normal[c] = normal ? normal[c] : 0; } return; }
  }
  b.splash = false;
  if (b.model < 0) { b.model = objAddCoreModel(w, 12, false, 0, nullptr, nullptr, "blast", nullptr, (int)slot + 1); if (b.model < 0) return; b.node = objAddNode(w, b.model, 2.0, 2); }
  if (w.obj->models[(size_t)b.model].diffusePtr) for (int c = 0; c < 3; ++c) w.obj->models[(size_t)b.model].diffusePtr[c] = (float)srgbToLinear(((hex >> (16 - 8 * c)) & 255) / 255.0);
  b.life = .25; b.radius = radius * .6; b.age = 0; b.on = true; const double s = b.radius * .3; const double q[4] = {0, 0, 0, 1};
  fxNode(w, b.node, pos, q, s, s, s, true); if (w.obj->models[(size_t)b.model].opacityPtr) *w.obj->models[(size_t)b.model].opacityPtr = ghost ? .35f : .8f;
}

static void fxProjectileImpl(World::Impl& w, int id, int weapon, const double pos[3], const double vel[3], bool spawn) {
  FxState& fx = fxOf(w); auto it = fx.projs.find(id);
  if (it == fx.projs.end()) {
    if (!spawn && weapon == 0) return; FxState::Proj p; p.weapon = weapon; auto& fl = fx.freeProj[weapon];
    if (!fl.empty()) { p.node = fl.back(); fl.pop_back(); }
    else { static const char* names[16] = {}; const char* nm = weapon == 5 ? "rocket" : weapon == 4 ? "grenade" : weapon == 8 ? "plasma" : weapon == 9 ? "bfg" : nullptr; (void)names; if (!nm) return;
      const int model = objAddCoreModel(w, 12, false, 0, nullptr, nullptr, nm, nullptr, 0); if (model < 0) return; p.node = objAddNode(w, model, 30.0, w.obj->models[(size_t)model].transparent ? 2 : 1); }
    it = fx.projs.emplace(id, p).first;
  }
  double q[4] = {0, 0, 0, 1}; if (weapon == 5 || it->second.weapon == 5) { const double l = sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]); if (l > 1e-6) quatFromUnitVectors(0, 1, 0, vel[0] / l, vel[1] / l, vel[2] / l, q); }   // the rocket's body (a cylinder along +y) points along its flight
  fxNode(w, it->second.node, pos, q, 1, 1, 1, true);
}
static void fxProjectileEndImpl(World::Impl& w, int id) {
  FxState& fx = fxOf(w); auto it = fx.projs.find(id); if (it == fx.projs.end()) return; const double away[3] = {0, -1e5, 0}; fxNode(w, it->second.node, away, nullptr, 1, 1, 1, false); fx.freeProj[it->second.weapon].push_back(it->second.node); fx.projs.erase(it);
}

static void fxBeamEnd(World::Impl& w, FxState::Beam& b) {   // hide the beam's nodes; its models go back to the spare list (a new beam builds fresh ones: the points are baked)
  const double away[3] = {0, -1e5, 0}; for (size_t k = 0; k < b.node.size(); ++k) fxNode(w, b.node[k], away, nullptr, 1, 1, 1, false); b.on = false;
}
static void fxBeamImpl(World::Impl& w, const double from[3], const double to[3]) {
  // the page (marks.js railBeam): the core's beam record, rc_beam_build(from, to): core / glow / halo tubes and the helix line, each its own material, faded by age (core and glow k^2, spiral k)
  FxState& fx = fxOf(w); FxState::Beam& b = fx.beams[fx.beamNext]; fx.beamNext = (fx.beamNext + 1) % fx.beams.size();
  if (!b.node.empty()) { fxBeamEnd(w, b); for (size_t k = 0; k < b.node.size(); ++k) { rc_scene_destroy(w.obj->insts[(size_t)b.node[k]].node); w.obj->modelOfObjectId[(size_t)b.node[k]] = -1; objReleaseModel(w, b.model[k]); } b.node.clear(); b.model.clear(); b.base.clear(); b.spiral.clear(); }
  const int len = rc_beam_build(from[0], from[1], from[2], to[0], to[1], to[2]); if (len <= 0) return;
  const uint8_t* rp = rc_model_ptr(); const std::vector<uint8_t> rec(rp, rp + rd32(rp + 32)); const uint32_t nMeshes = rd32(rec.data() + 16), nNodes = rd32(rec.data() + 12), nMaterials = rd32(rec.data() + 20); const uint8_t* mshs = rec.data() + 40 + 280 * nNodes; const uint8_t* mats = mshs + 48 * nMeshes; const uint32_t matStride = rd32(rec.data() + 4) >= 2 ? 104 : 88; (void)nMaterials;
  const double zero[3] = {0, 0, 0};
  for (uint32_t k = 0; k < nMeshes; ++k) {
    const int model = objModelFromRecord(w, "beam-" + std::to_string(fx.beamNext) + "-" + std::to_string(k) + "-" + std::to_string(w.obj->models.size()), "beam", 1.0, false, false, nullptr, 0, nullptr, nullptr, nullptr, nullptr, (int)k, rec.data()); if (model < 0) continue;
    const int h = objAddNode(w, model, 1e5, 5); b.model.push_back(model); b.node.push_back(h); b.spiral.push_back(rd32(mats + matStride * rd32(mshs + 48 * k + 12) + 40) == 4);
    const float* op = w.obj->models[(size_t)model].opacityPtr; b.base.push_back(op ? *op : 1.f); fxNode(w, h, zero, nullptr, 1, 1, 1, true);
  }
  b.age = 0; b.life = .6; b.on = true;
}

// The hook cable (hook-model.js): rc_hook_cable gives the chain links' matrices and the head's pose from the muzzle to the hook; each link is a node of the "link" shape, the head a node of the "head" shape.
static void fxHookImpl(World::Impl& w, const double from[3], const double to[3], double time, bool attached, int slot) {
  FxState& fx = fxOf(w); if (slot < 0 || slot > 63) return; if ((int)fx.hooks.size() <= slot) fx.hooks.resize((size_t)slot + 1); FxState::HookSet& hs = fx.hooks[(size_t)slot];
  if (hs.head < 0) {
    const int lm = objAddCoreModel(w, 8, false, 0, nullptr, nullptr, "link", nullptr, 0), hm = objAddCoreModel(w, 8, false, 0, nullptr, nullptr, "head", nullptr, 0); if (lm < 0 || hm < 0) return;
    hs.head = objAddNode(w, hm, 40.0, 1); for (int k = 0; k < 64; ++k) hs.links.push_back(objAddNode(w, lm, 40.0, 1));
  }
  const int count = rc_hook_cable(from[0], from[1], from[2], to[0], to[1], to[2], time, attached ? 1 : 0); const double* h = rc_hook_cable_head(); const float* L = rc_hook_cable_links(); const double away[3] = {0, -1e5, 0};
  fxNode(w, hs.head, h, h + 3, 1, 1, 1, true);
  for (int k = 0; k < 64; ++k) {
    if (k >= count) { fxNode(w, hs.links[(size_t)k], away, nullptr, 1, 1, 1, false); continue; }
    const float* m = L + k * 16; const double p[3] = {m[12], m[13], m[14]}; double sx = sqrt((double)m[0] * m[0] + (double)m[1] * m[1] + (double)m[2] * m[2]), sy = sqrt((double)m[4] * m[4] + (double)m[5] * m[5] + (double)m[6] * m[6]), sz = sqrt((double)m[8] * m[8] + (double)m[9] * m[9] + (double)m[10] * m[10]);
    if (sx < 1e-9) sx = 1; if (sy < 1e-9) sy = 1; if (sz < 1e-9) sz = 1;
    const double R[3][3] = {{m[0] / sx, m[4] / sy, m[8] / sz}, {m[1] / sx, m[5] / sy, m[9] / sz}, {m[2] / sx, m[6] / sy, m[10] / sz}}; double q[4]; quatFromRows(R, q);
    fxNode(w, hs.links[(size_t)k], p, q, sx, sy, sz, true);
  }
  hs.on = true;
}
static void fxHookEndImpl(World::Impl& w, int slot) {
  FxState& fx = fxOf(w); if (slot < 0 || slot >= (int)fx.hooks.size() || !fx.hooks[(size_t)slot].on) return; FxState::HookSet& hs = fx.hooks[(size_t)slot]; const double away[3] = {0, -1e5, 0}; fxNode(w, hs.head, away, nullptr, 1, 1, 1, false); for (int n : hs.links) fxNode(w, n, away, nullptr, 1, 1, 1, false); hs.on = false;
}

static void fxUpdateImpl(World::Impl& w, double dt, const CameraIn& cam) {
  if (!w.fxp || !w.obj) return; FxState& fx = fxOf(w); const double away[3] = {0, -1e5, 0};
  for (size_t si = 0; si < fx.marks.size(); ++si) for (auto& s : fx.marks[si]) { if (!s.on) continue; s.age += dt;
    if (s.age >= s.life) { s.on = false; fxNode(w, s.node, away, nullptr, 1, 1, 1, false); continue; }
    if (float* op = w.obj->models[(size_t)s.model].opacityPtr) *op = (float)std::min(1.0, (s.life - s.age) / std::min(1.5, s.life / 2)); }   // marks.js: fades out over the last 1.5 s
  double R[3][3]; for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) R[r][c] = cam.view[r * 4 + c];   // the camera's world rotation: the transpose of the view's 3 x 3
  double cq[4]; quatFromRows(R, cq);
  for (auto& p : fx.sparks) { if (!p.on) continue; p.age += dt; if (p.age >= p.life) { p.on = false; fxNode(w, p.node, away, nullptr, 1, 1, 1, false); continue; }
    p.v[1] -= 700.0 * dt; for (int c = 0; c < 3; ++c) p.p[c] += p.v[c] * dt; const double k = p.size * (1 - p.age / p.life); fxNode(w, p.node, p.p, cq, k, k, k, true); }   // SPARK_GRAVITY 700, shrinking sprites facing the camera
  for (auto& b : fx.blasts) { if (!b.on) continue; b.age += dt; const double t = b.age / b.life;
    if (t >= 1) { b.on = false; fxNode(w, b.splash ? b.snode : b.node, away, nullptr, 1, 1, 1, false); continue; }
    if (b.splash) { const double* p = rc_splash_pose(b.at[0], b.at[1], b.at[2], b.normal[0], b.normal[1], b.normal[2], b.hasNormal ? 1 : 0, t, cq[0], cq[1], cq[2], cq[3]); const double pp[3] = {p[0], p[1], p[2]}, qq[4] = {p[3], p[4], p[5], p[6]};
      fxNode(w, b.snode, pp, qq, p[7], p[7], p[7], true); if (float* op = w.obj->models[(size_t)b.smodel].opacityPtr) *op = (float)p[8]; continue; }
    const double s = b.radius * (.3 + .7 * t); const int node = w.obj->insts[b.node].node; double* sc = (double*)rc_scene_array(A_SCALE); sc[node * 3] = sc[node * 3 + 1] = sc[node * 3 + 2] = s; if (float* op = w.obj->models[(size_t)b.model].opacityPtr) *op = (float)((b.ghost ? .35 : .8) * (1 - t)); }
  for (auto& b : fx.beams) { if (!b.on) continue; b.age += dt; const double k = 1 - b.age / b.life; if (k <= 0) { fxBeamEnd(w, b); continue; }
    for (size_t c = 0; c < b.model.size(); ++c) if (float* op = w.obj->models[(size_t)b.model[c]].opacityPtr) *op = (float)(b.base[c] * (b.spiral[c] ? k : k * k)); }
}
