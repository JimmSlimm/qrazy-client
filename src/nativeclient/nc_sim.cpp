// The game simulation as the client sees it: a thin wrapper over the core library's sim calls (qrazy_core_api.h). One loaded map, ticked in fixed 8 ms steps.
#include "qrazy_core_api.h"
#include "nc_hud.h"
#include "nc_sim.h"
#ifndef NC_CORE_LOADER
#include "nc_coreload.h"
#endif
#include <cstring>
#include <cmath>
#include <algorithm>

namespace nc {

struct Sim::Impl { std::string error; double yaw = 0, pitch = 0; int strafe = 0, hook = 0; bool frozen = false; };
Sim::Sim() : m(new Impl()) {}
Sim::~Sim() { delete m; }
const std::string& Sim::error() const { return m->error; }
const double* Sim::out() const { return qc_sim_out(); }
int Sim::moverCount() const { return qc_sim_pose_count(); }
double Sim::spawnYaw() const { return qc_sim_spawn_yaw(); }
double Sim::spawnPitch() const { return qc_sim_spawn_pitch(); }
int Sim::weaponHeld() const { return qc_sim_weapon_held(); }

void Sim::setMode(bool strafe, bool hook) { m->strafe = strafe; m->hook = hook; }
bool Sim::tainted() const { return qc_sim_tainted() != 0; }
bool Sim::frozen() const { return m->frozen; }
Sim::CommandReply Sim::command(int op, const double in[16], const std::string& text) {
  CommandReply r; r.bits = qc_sim_command(op, in, text.data(), (int)text.size()); r.state = qc_sim_command_state(); m->frozen = r.state && ((int)r.state[0] & 256) != 0;
  int len = 0; const char* p = qc_sim_command_reply(&len); if (p && len > 0) r.reply.assign(p, (size_t)len); const char* src = qc_sim_command_source(); if (src) r.source = src; return r;
}
int Sim::runState() const { return qc_sim_run_state(); }
void Sim::restart() { qc_sim_restart(); }
bool Sim::giveItem(const std::string& name, double count, double capSeconds) { return qc_sim_give_item(name.c_str(), (int)name.size(), count, capSeconds) != 0; }
void Sim::setStat(int id, double v) { qc_sim_set_stat(id, v); }
void Sim::restorePosition(double x, double y, double z, double vx, double vy, double vz, double yaw, double pitch) { qc_sim_restore_position(x, y, z, vx, vy, vz, yaw, pitch); }
void Sim::removePowerups() { qc_sim_remove_powerups(); }
void Sim::setNoclip(bool on) { qc_sim_noclip(on ? 1 : 0); }
bool Sim::noclip() const { return qc_sim_noclip_on() != 0; }

bool Sim::loadRunway(const std::string& fixes, bool vql, bool stagedMode) {
  m->frozen = false;
  if (!stagedMode) qc_sim_set_mode(m->strafe, m->hook);
  const int rc = qc_sim_load_runway(fixes.data(), fixes.size(), vql ? 1 : 0);
  if (rc != 0) { m->error = qc_sim_error(); return false; }
  return true;
}

bool Sim::loadMap(const std::vector<uint8_t>& bsp, const std::string& fixes, const std::string& name, bool vql, bool stagedMode) {
  m->frozen = false;
  if (!stagedMode) qc_sim_set_mode(m->strafe, m->hook);
  const int rc = qc_sim_load_map(bsp.data(), bsp.size(), fixes.data(), fixes.size(), name.data(), name.size(), vql ? 1 : 0);
  if (rc != 0) { m->error = qc_sim_error(); return false; }
  return true;
}


int Sim::poses(std::vector<uint8_t>& out) const {
  const int count = qc_sim_pose_count(); out.assign((size_t)count * 64, 0);
  return count > 0 ? qc_sim_poses(out.data(), (int)out.size()) : 0;
}

bool Sim::slick() const { const double* o = qc_sim_out(); return o && o[18] != 0; }
int Sim::liquid() const { const double* o = qc_sim_out(); return o && o[14] >= 3 ? (int)o[15] : 0; }
int Sim::trail(float* xz, int maxPoints) const { return qc_sim_trail(xz, maxPoints); }

bool Sim::cgaz(void* out136) const { static_assert(sizeof(QcCgaz) == 136, "QcCgaz"); QcCgaz c; memset(&c, 0, sizeof c); const int ok = qc_sim_cgaz(&c); memcpy(out136, &c, sizeof c); return ok != 0; }

void Sim::drainEvents(std::vector<Event>& out) {
  const int n = qc_sim_event_count();
  for (int i = 0; i < n; ++i) { QcEvent q; if (!qc_sim_event(i, &q)) continue; Event e; e.type = q.type; e.weapon = q.weapon; for (int k = 0; k < 3; ++k) { e.pos[k] = q.pos[k]; e.dir[k] = q.dir[k]; e.to[k] = q.to[k]; e.vel[k] = q.vel[k]; } e.id = q.id; e.surface = q.surface; e.radius = q.radius; out.push_back(e); }
  qc_sim_events_clear();
}

void Sim::hudFrame(HudNumbers& o) const {
  QcHud f; qc_sim_hud(&f);
  hudNumbers(f, o);
}
void Sim::hudNumbers(const QcHud& f, HudNumbers& o) {
  o.health = f.health; o.armor = f.armor; o.ammo = f.ammo; for (int i = 0; i < 16; ++i) o.barAmmo[i] = f.barAmmo[i]; for (int i = 0; i < 8; ++i) { o.puLeft[i] = f.puLeft[i]; o.puIndex[i] = f.puIndex[i]; }
  o.weaponInHand = f.weaponInHand; o.selected = f.selected; o.weaponState = f.weaponState; o.cooldownMs = f.cooldownMs;
  o.timerMs = f.timerMs; o.held = f.held; o.speed = f.speed; o.barOwned = f.barOwned; o.puCount = f.puCount;
}

bool Sim::trace(const double from[3], const double dir[3], double& dist, double normal[3], int& flags) const {
  const double l = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); if (l < 1e-9) return false;
  const double yaw = atan2(dir[0], dir[2]), pitch = asin(std::max(-1.0, std::min(1.0, dir[1] / l)));   // the sim's view axes: forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)
  QcShot sh; if (qc_dry_hitscan(7, from, yaw, pitch, &sh, 1) < 1 || sh.fraction >= 1) return false;
  dist = sh.fraction * 8192.0; for (int k = 0; k < 3; ++k) normal[k] = sh.normal[k]; flags = sh.flags; return true;
}

std::vector<ItemSpot> Sim::items() const {
  std::vector<ItemSpot> out; const int n = qc_sim_item_count();
  for (int k = 0; k < n; ++k) { QcItem it; if (!qc_sim_item(k, &it)) continue; ItemSpot s; s.pos[0] = it.pos[0]; s.pos[1] = it.pos[1]; s.pos[2] = it.pos[2]; s.classname = it.classname; out.push_back(std::move(s)); }
  return out;
}

PlayerView Sim::player() const {
  PlayerView v; const double* o = qc_sim_out(); v.pos[0] = o[0]; v.pos[1] = o[1]; v.pos[2] = o[2]; v.vel[0] = o[3]; v.vel[1] = o[4]; v.vel[2] = o[5]; v.crouched = o[13] != 0; v.yaw = m->yaw; v.pitch = m->pitch; return v;
}

#ifndef NC_CORE_LOADER   // standalone Windows build: the core is linked by import library, there is no loader (nc_coreload.cpp defines these elsewhere)
bool coreHasMatchHud() { return true; }
int matchOp(QcMatchOp op, double a, double b, double c, std::string_view text) {
  const double in[4] = {a, b, c, 0};
  return text.size() <= 255 ? qc_ses_match(op, in, text.data(), int(text.size())) : -1;
}
int matchHud(QcHudFrame& out) { return qc_ses_hud(&out); }
#endif

}  // namespace nc
