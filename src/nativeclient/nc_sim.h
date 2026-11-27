// The game simulation (native-core, the same C++ the website runs as core.wasm). One loaded map, ticked in fixed 8 ms steps.
// It lives in its own translation unit with its namespace renamed, so its arena never shares memory with the render core's.
#pragma once
#include "qrazy_core_api.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nc {

enum { SIM_OUT = 50 };

struct PlayerView {   // what the renderer needs after a tick (world axes of the game: y up)
  double pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}; double yaw = 0, pitch = 0; bool crouched = false;
};

struct HudNumbers;
struct ItemSpot { double pos[3]; std::string classname; };

class Sim {
 public:
  Sim();
  ~Sim();
  // bsp = the map file, fixes = map-fixes.json text (may be empty), name = the map name without extension. vql: 1 = VQL physics, 0 = PQL. Returns false and a reason in error().
  // Weapon set / hook mode for every load from now on (the core's qc_sim_set_mode is one-shot: the loads below call it each time, so no load is ever without).
  void setMode(bool strafe, bool hook);
  bool loadRunway(const std::string& fixes, bool vql, bool stagedMode = false);
  bool loadMap(const std::vector<uint8_t>& bsp, const std::string& fixes, const std::string& name, bool vql, bool stagedMode = false);
  // One 8 ms tick. yaw / pitch in radians (they are rounded to float32 as a logged tick does); masks = the tick's 16 key samples; wantWeapon 0 = none.
  PlayerView player() const;
  const double* out() const;       // the 50-double player / sim output of the last tick
  int moverCount() const;
  // The movers as the core's pose records (fp::Pose, 64 bytes each) indexed by BSP model index; a model no mover owns gets the identity pose. Returns the pose count (0 = no movers).
  int poses(std::vector<uint8_t>& out) const;
  // The map's item spots (where pickups sit, world axes) with the entity's classname. Spot i is the id the sim's item-visibility events carry.
  std::vector<ItemSpot> items() const;
  int weaponHeld() const;
  // A ray traced against the loaded map (the dry railgun query, range 8192): the distance to the first surface and its normal and flags; false = nothing hit within 8192. dir need not be a unit vector.
  bool trace(const double from[3], const double dir[3], double& dist, double normal[3], int& flags) const;
  // The run's state (interface 7): the taint and its rules are the core's (qc_sim_command, new lives); run 0 ready, 1 running, 2 finished.
  bool tainted() const; int runState() const;
  // The page's practice commands (qc_sim_command), passed through unchanged. bits & 1: the sim changed (cues and events were added). state = 31 doubles, valid until the next command.
  struct CommandReply { int bits = 0; const double* state = nullptr; std::string reply, source; };
  CommandReply command(int op, const double in[16], const std::string& text);
  bool frozen() const;   // the core holds the game (a race countdown): bit 256 of the last command's state; a map load clears it
  void restart();   // a respawn on the loaded map (keeps the spawn a command set; ends explore mode unless spectating)
  bool giveItem(const std::string& name, double count, double capSeconds);   // an item by classname ("item_quad"); count 0 = the item's own, capSeconds 0 = no cap
  void setStat(int id, double v);   // 0 health, 1 armor, 2 flight fuel
  void restorePosition(double x, double y, double z, double vx, double vy, double vz, double yaw, double pitch);   // the spectator free fly starts at the followed player (yaw / pitch in radians); taints
  void removePowerups(); void setNoclip(bool on); bool noclip() const;   // setNoclip: the spectator free fly
  double spawnYaw() const;     // the map start's view angles (radians, the sim's convention)
  double spawnPitch() const;
  // The sound cues the core produced since the last drain (every tick and the map load add theirs). The character id picks the footstep family and the voice pack.
  // The weapon and world events of the ticks since the last drain (qrazy_core_api.h QcEvent): shots, impacts, projectiles, explosions, the rail beam, the hook ...
  struct Event { int type = 0, weapon = 0; double pos[3] = {}, dir[3] = {}, to[3] = {}, vel[3] = {}; int id = 0, surface = 0; double radius = 0; };
  void drainEvents(std::vector<Event>& out);
  // The CGaz strafe HUD numbers the last tick took (a 136-byte QcCgaz = TextCgazState); false when it took no snapshot (draw nothing).
  bool cgaz(void* out136) const;
  // The player is on a slick surface (the last tick).
  bool slick() const;
  int liquid() const;   // the eye point's liquid contents bits (32 water, 16 slime, 8 lava) when the eyes are under, else 0
  int trail(float* xz, int maxPoints) const;   // the minimap trail the sim keeps: x, z pairs, oldest first
  // The HUD numbers the sim owns (hud_frame.hpp): health, armor, ammo, the weapon bar, powerups, timer, speed. Call after a tick.
  void hudFrame(struct HudNumbers& out) const;
  static void hudNumbers(const QcHud& frame, struct HudNumbers& out);
  const std::string& error() const;
 private:
  struct Impl;
  Impl* m;
};

}  // namespace nc
