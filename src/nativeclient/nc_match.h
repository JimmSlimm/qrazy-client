// What a native match shows and reports from the core's own tables: other players and the leaderboard ghost (the core's node records), other players' shots and hooks, and finished-run records for the page.
// Included by nc_app.cpp only. All calls from the render thread (the core has one peer table per library instance).
#pragma once
#include "nc_shotfx.h"
#include "nc_hud.h"
#include "nc_json.h"
#include "qrazy_core_api.h"
#include "text_api.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nc {

inline std::string base64(const uint8_t* p, size_t n) {
  static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string r; r.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) { const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0); r += a[v >> 18]; r += a[(v >> 12) & 63]; r += i + 1 < n ? a[(v >> 6) & 63] : '='; r += i + 2 < n ? a[v & 63] : '='; }
  return r;
}
inline std::vector<uint8_t> unbase64(const std::string& s) {
  std::vector<uint8_t> r; r.reserve(s.size() / 4 * 3); uint32_t v = 0; int bits = 0;
  for (unsigned char c : s) { int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; if (d < 0) continue; v = (v << 6) | (uint32_t)d; bits += 6; if (bits >= 8) { bits -= 8; r.push_back((uint8_t)(v >> bits)); } }
  return r;
}
inline float srgbToLinear(double s) { return (float)(s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4)); }

// Finished runs (and /record captures) on their way to the page: window.qrazyNativeRun once per part, the whole record again every 2 s until the page's {"op":"runack"}. Process-wide: a record outlives its match.
class RunSender {
 public:
  typedef void (*Emit)(const char* js);
  // A record the core holds now (event 21): copied at once.
  void takeResult() {
    QcRunResult r; if (!qc_run_result(&r)) return;
    const uint8_t* log = nullptr; const int logBytes = qc_run_result_log(&log); const float* ghost = nullptr; const int ghostCount = qc_run_result_ghost(&ghost);
    std::vector<double> cps((size_t)std::max(0, std::min(4096, r.checkpointCount))); const int nc_ = cps.empty() ? 0 : qc_run_result_checkpoints(cps.data(), (int)cps.size()); cps.resize((size_t)std::max(0, nc_));
    build(r.serial, r, log, logBytes, (const uint8_t*)ghost, (size_t)std::max(0, ghostCount) * 14 * 4, ghostCount, cps, false);
    qc_run_result_release(r.serial);
  }
  // /record: the current capture, answered under the page's serial; nothing when it holds fewer than 2 rows.
  void takeCapture(int serial) {
    QcRunResult r; if (!qc_run_capture(&r)) { logf("capture %d: fewer than 2 rows, nothing sent", serial); return; }
    int logBytes = 0; const uint8_t* log = qc_run_log(&logBytes); build(serial, r, log, logBytes, nullptr, 0, 0, {}, true);
  }
  void ack(int serial) { for (size_t i = pending_.size(); i-- > 0;) if (pending_[i].serial == serial) { logf("run %d acknowledged", serial); pending_.erase(pending_.begin() + (long)i); } }
  void service(double now, Emit emit) { if (!emit) return; for (Rec& r : pending_) if (now - r.sentAt >= 2.0) { r.sentAt = now; for (const std::string& s : r.parts) emit(s.c_str()); } }
 private:
  struct Rec { int serial; double sentAt; std::vector<std::string> parts; };
  std::vector<Rec> pending_;
  static std::string num(double v) { if (!std::isfinite(v)) return "0"; char b[40]; snprintf(b, sizeof b, "%.17g", v); return b; }
  void build(int serial, const QcRunResult& r, const uint8_t* log, int logBytes, const uint8_t* ghost, size_t ghostBytes, int ghostCount, const std::vector<double>& cps, bool capture) {
    const size_t lb = log && logBytes > 0 ? (size_t)logBytes : 0, gb = ghost ? ghostBytes : 0, longest = std::max(lb, gb);
    size_t slice = 192 * 1024; if (longest > slice * 64) slice = (longest + 63) / 64; const int parts = (int)std::max<size_t>(1, (longest + slice - 1) / slice);
    std::string map; for (size_t i = 0; i < sizeof r.map && r.map[i]; ++i) { const unsigned char c = (unsigned char)r.map[i]; if (c == '"' || c == '\\') map += '\\'; if (c >= 32) map += (char)c; }
    Rec rec; rec.serial = serial; rec.sentAt = -1e9;
    for (int i = 0; i < parts; ++i) {
      const size_t at = (size_t)i * slice; auto cut = [&](const uint8_t* p, size_t n) { return at < n ? base64(p + at, std::min(slice, n - at)) : std::string(); };
      std::string j = "window.qrazyNativeRun&&window.qrazyNativeRun({serial:" + std::to_string(serial) + ",part:" + std::to_string(i) + ",parts:" + std::to_string(parts) + ",log:\"" + cut(log, lb) + "\",ghost:\"" + cut(ghost, gb) + "\"";
      if (capture) j += ",capture:true";
      if (i == 0) {
        auto flag = [](int v) { return std::string(v ? "true" : "false"); };
        j += ",map:\"" + map + "\",vql:" + flag(r.vql) + ",strafe:" + flag(r.strafe) + ",hook:" + flag(r.hook) + ",elapsed:" + num(capture ? 0 : r.elapsed) + ",ticks:" + std::to_string(r.ticks) + ",cut:" + flag(r.cut) + ",checkpoints:[";
        for (size_t k = 0; k < cps.size(); ++k) { if (k) j += ','; j += num(cps[k]); }
        j += "],stats:"; if (!capture && r.hasStats) j += "[" + num(r.stats[0]) + "," + num(r.stats[1]) + "," + num(r.stats[2]) + "," + num(r.stats[3]) + "]"; else j += "null";
        j += ",spawn:{x:" + num(r.spawn[0]) + ",y:" + num(r.spawn[1]) + ",z:" + num(r.spawn[2]) + ",yaw:" + num(r.spawn[3]) + ",pitch:" + num(r.spawn[4]) + ",ground:" + flag(r.spawnGround);
        if (r.moverTime >= 0) j += ",moverTime:" + num(r.moverTime);
        j += "},logBytes:" + std::to_string(lb) + ",ghostCount:" + std::to_string(ghostCount) + ",ghostStride:14,coreInterface:" + std::to_string(QC_INTERFACE_VERSION) + ",coreHash:\"\"";
      }
      rec.parts.push_back(j + "})");
    }
    logf("run %d%s: %zu log bytes, %d ghost rows, %d part(s)", serial, capture ? " (capture)" : "", lb, ghostCount, parts);
    for (size_t i = pending_.size(); i-- > 0;) if (pending_[i].serial == serial) pending_.erase(pending_.begin() + (long)i);
    if (pending_.size() >= 8) pending_.erase(pending_.begin());
    pending_.push_back(std::move(rec));
  }
};

// Applies the core's avatar node records (4 per avatar: body, gun, accessory, plate) to scene nodes. Slots 0..15 = the peer table's, 16 = the track ghost. Nodes are created on first use and kept.
inline void insert(std::vector<int>& sorted, int value) {
  auto at = std::lower_bound(sorted.begin(), sorted.end(), value);
  if (at == sorted.end() || *at != value) sorted.insert(at, value);
}
inline bool contains(const std::vector<int>& sorted, int value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

class AvatarView {
 public:
  static constexpr int SLOTS = 17, GHOST_SLOT = 16;
  bool accessories = false;   // render.showDick: off = no accessory is drawn
  void begin() { hidden_.swap(shown_); shown_.clear(); }
  // name / sub of the plate image a slot asks for (called once per new plate key).
  template <class Text> void apply(World& w, Hud* hud, const QcPeerNode* n, int count, int slotBase, Text text) {
    for (int i = 0; i < count; ++i) {
      const QcPeerNode& r = n[i]; if (!(r.flags & QC_NF_VISIBLE)) continue; const int slot = slotBase + r.slot; if (slot < 0 || slot >= SLOTS) continue; int h = -1;
      if (r.part == QC_PART_BODY) {
        auto lin = [](float v) { return (uint32_t)std::max(0.f, std::min(255.f, v * 255.f + .5f)); }; const uint64_t key = (uint64_t)slot << 32 | lin(r.tint[0]) << 16 | lin(r.tint[1]) << 8 | lin(r.tint[2]);
        auto f = body_.find(key); if (f == body_.end()) { if (body_.size() >= 96) continue; f = body_.emplace(key, w.addAvatarBody(r.tint, (int)body_.size() + 1)).first; } h = f->second; w.setOpacity(h, r.tint[3]);
      } else if (r.part == QC_PART_GUN) {
        const int key = slot * 32 + r.variant; auto f = gun_.find(key); if (f == gun_.end()) f = gun_.emplace(key, w.addAvatarGun(r.variant)).first; h = f->second;
      } else if (r.part == QC_PART_ACCESSORY) {
        if (!accessories || r.variant <= 0) continue; const int key = slot * 64 + r.variant; auto f = acc_.find(key);
        if (f == acc_.end()) { QcPartSpec ps; if (!qc_peer_part_spec(QC_PART_ACCESSORY, r.variant, &ps)) continue; const uint32_t c = (uint32_t)ps.reserved[0]; const float rgb[3] = {srgbToLinear(((c >> 16) & 255) / 255.0), srgbToLinear(((c >> 8) & 255) / 255.0), srgbToLinear((c & 255) / 255.0)}; f = acc_.emplace(key, w.addAvatarAccessory(rgb, key)).first; }
        h = f->second; w.setOpacity(h, r.tint[3]);
      } else if (r.part == QC_PART_PLATE) {
        if (!hud || !r.key) continue; const uint64_t pk = (uint64_t)slot << 32 | r.key; auto f = plate_.find(pk);
        if (f == plate_.end()) { if (plate_.size() >= 96) continue; std::string name, sub; if (!text(r.slot, name, sub)) continue; if (text_plate_bake((const uint8_t*)name.data(), (uint32_t)name.size(), (const uint8_t*)sub.data(), (uint32_t)sub.size()) != 1) continue;
          f = plate_.emplace(pk, w.addPlate(text_plate_pixels(), TEXT_PLATE_W, TEXT_PLATE_H)).first; logf("plate baked for key %u", r.key); }
        h = f->second;
      }
      if (h < 0) continue; w.setNodePose(h, r.pos, r.quat, r.scale, true); insert(shown_, h); auto old = std::lower_bound(hidden_.begin(), hidden_.end(), h); if (old != hidden_.end() && *old == h) hidden_.erase(old);
    }
  }
  void end(World& w) { static const double away[3] = {0, -1e5, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1}; for (int h : hidden_) w.setNodePose(h, away, q, s, false); hidden_.clear(); }
 private:
  std::map<uint64_t, int> body_; std::map<int, int> gun_, acc_; std::map<uint64_t, int> plate_; std::vector<int> shown_, hidden_;
};

// Other players' shots and hooks, as the core's records say (qc_peer_fx_*): missiles and cables to draw, one-shot marks, and the due sounds.
class PeerFxView {
 public:
  // followed: the player watched in first person (0 none); their cable starts at followedFrom, the first-person hook origin
  void update(World& w, double now, bool runway, int followed, const double* followedFrom) {
    nextMissiles_.clear(); nextHooks_.clear(); const int n = qc_peer_fx_frame(now, fx_, QC_FX_MAX);
    for (int i = 0; i < n; ++i) { const QcPeerFx& r = fx_[i];
      if (r.kind == QC_FX_MISSILE) { const int id = 1000000 + r.id; insert(nextMissiles_, id); const bool isNew = !contains(missiles_, id); w.fxProjectile(id, r.weapon, r.pos, r.dir, isNew); }
      else if (r.kind == QC_FX_HOOK) { const int s = r.slot + 1; insert(nextHooks_, s); w.fxHook(followed > 0 && r.id == followed ? followedFrom : r.from, r.pos, r.t, (r.flags & QC_FXF_ATTACHED) != 0, s); }
    }
    for (int id : missiles_) if (!contains(nextMissiles_, id)) w.fxProjectileEnd(id);
    for (int s : hooks_) if (!contains(nextHooks_, s)) w.fxHookEnd(s);
    missiles_.swap(nextMissiles_); hooks_.swap(nextHooks_);
    const int ne = qc_peer_fx_events(now, ev_, 64);
    for (int i = 0; i < ne; ++i) { const QcPeerFxEvent& e = ev_[i];
      if (e.kind == QC_FXE_HITSCAN) fxShot(w, QC_SESFX_HITSCAN, e.weapon, e.mode, runway, e.a, e.b, e.c, (int)e.aux0, e.fraction);   // the rail's beam is among the core's commands
      else if (e.kind == QC_FXE_EXPLOSION) { const double* n = (e.flags & 4) ? e.c : nullptr; w.fxBlast(e.weapon, e.a, blastSplash(e.weapon, e.mode), n, true); fxShot(w, QC_SESFX_EXPLOSION, e.weapon, e.mode, runway, nullptr, e.a, n); }
      else if (e.kind == QC_FXE_HOOK_LAND) fxShot(w, QC_SESFX_HOOK_LAND, 0, e.mode, runway, e.a, e.b, nullptr);
    }
  }
 private:
  QcPeerFx fx_[QC_FX_MAX];
  QcPeerFxEvent ev_[64];
  std::vector<int> missiles_, hooks_, nextMissiles_, nextHooks_;
};

// The followed player's run clock, as of the last HUD message (qc_ses_compose reads it).
struct FollowClock {
  bool running = false, paused = false;
  double t = 0, at = 0;   // the followed run time, valid at `at` (the qc_ses_advance clock)

  void set(const J* j, double received) {
    if (!j || j->t != J::Obj) { *this = {}; return; }
    const J* r = j->get("running");
    const J* p = j->get("paused");
    running = r && r->t == J::Bool && r->b;
    paused = p && p->t == J::Bool && p->b;
    t = j->num("t") + (running && !paused ? j->num("ageMs") / 1000.0 : 0);
    at = received;
  }
};

// The leaderboard ghost the page sends (op "ghost"): track slot 0 of the core. What is shown of it is the core's (qc_ses_compose / qc_ses_view).
struct Ghost {
  bool on = false; double time = 0; int count = 0; uint32_t plateKey = 0; std::string name, sub; std::vector<double> rows;
  std::optional<bool> show, path;   // older pages leave visibility to the settings
  uint32_t revision = 0;   // a new or dropped track (the ghost path is rebuilt from it)
  void clear() { on = false; ++revision; qc_track_slot_clear(0); }
  void set(const J& j) {
    const J* o = j.get("on"); if (!o || o->t != J::Bool || !o->b) { clear(); return; }
    clear(); const int n = (int)j.num("count", 0); const std::vector<uint8_t> b = unbase64(j.str("rows"));
    if (n < 1 || (int)j.num("stride", 0) != 14 || b.size() != (size_t)n * 14 * 4) { logf("ghost op refused: %d rows, %zu bytes", n, b.size()); return; }
    rows.resize((size_t)n * 14); for (size_t i = 0; i < rows.size(); ++i) { float f; memcpy(&f, b.data() + i * 4, 4); rows[i] = f; }
    const J* sh = j.get("show"); const J* pa = j.get("path");
    show = sh && sh->t == J::Bool ? std::optional<bool>(sh->b) : std::nullopt;
    path = pa && pa->t == J::Bool ? std::optional<bool>(pa->b) : std::nullopt;
    count = n; time = j.num("time"); name = j.str("name"); sub = j.str("sub");
    uint32_t key = 2166136261u; for (unsigned char ch : name) key = (key ^ ch) * 16777619u; key = (key ^ 10u) * 16777619u; for (unsigned char ch : sub) key = (key ^ ch) * 16777619u;
    plateKey = name.empty() ? 0 : key | 1;
    on = qc_track_slot_set(0, n, 14, 13, QC_TRACK_RULE_RUN, rows.data()) == 1; logf("ghost: %d rows, %s", n, on ? "loaded" : "refused by the core");
  }
};

}  // namespace nc
