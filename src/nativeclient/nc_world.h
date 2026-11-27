// The map: loads it into the render core, compiles the core's programs, owns the GPU side of the world (vertex/index buffers, textures, materials, pipelines, mirror/portal targets)
// and draws the frame plan's passes exactly as the core lists them. Nothing here decides what is visible, in what order, or which GLSL runs.
#pragma once
#include "nc_vk.h"
#include "nc_assets.h"
#include <functional>
#include <memory>
#include <string>

struct QcMinimapOut;

namespace nc {

struct CameraIn { double view[16], proj[16], pos[3]; double fovY = 100, nearZ = 1, farZ = 20000; };   // the live camera takes its clips from the core (rc_camera_clips); the defaults are the diagnostic camera's

struct WorldStats {
  int batches = 0, vertices = 0, indices = 0, programs = 0, materials = 0, surfaces = 0;
  int passes = 0, worldDraws = 0, objectDraws = 0, objectEntriesSkipped = 0, pipelines = 0, texturesLoaded = 0, texturesMissing = 0;
  double loadSeconds = 0, planMs = 0, recordMs = 0;   // planMs / recordMs: CPU time of the last frame
};

// r_renderScale / r_gamma / r_contrast (nc_post.inl): resize() only when the wanted size differs (the GPU is idle then), record() after the scene is in `fb`.
struct PostPass {
  Image color, depth; VkFramebuffer fb = VK_NULL_HANDLE; int w = 0, h = 0; VkPipeline pipe = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
  void resize(const Context& c, VkRenderPass screenPass, int w, int h);
  void record(VkCommandBuffer cb, VkRenderPass screenPass, VkFramebuffer target, int sw, int sh, double gamma, double contrast,
              const std::function<void(VkCommandBuffer)>& overlay = nullptr);
};

class World {
 public:
  std::function<void(const char* stage, double fraction)> onProgress;   // called during load() between its stages and per program / material: the host can draw a loading screen (0..1)
  World();
  ~World();
  // Loads one map from the files in `fs`. bsp = path of the .bsp inside fs; scriptsFile / filesList = the merged shader script text and the newline list of archive files (host-prepared for now).
  bool load(const Context& c, const FileSource& fs, const std::string& bsp, const std::string& scriptsFile, const std::string& filesList, VkRenderPass screenPass, const std::string& fixesJson = std::string());
  // No map: only objects (the runway, a view weapon). Sets up the GPU side without a BSP; frames are planned from the core's resident scene (rc_scene_frame).
  bool loadObjectsOnly(const Context& c, const FileSource& fs, VkRenderPass screenPass);
  // One of the core's own models (rc_model_build kind, e.g. 16 the runway) in the record's own coordinates; returns a handle for setObject, -1 on failure. radius: a sphere around the origin that holds it (culling).
  int addCoreModel(int kind, double radius);
  // The view weapon: one node per weapon model (rc_model_build kind, e.g. 1 the machinegun) on the gun layer, drawn in a second pass after the world with the depth cleared.
  // The players' ground shadows: SHADOWS fixed quads with the core's EFFECT_SHADOW program, made with the objects at load. The host writes each shown one every frame from the core's QcShadow
  // (u = fade, ext, R, y0, y1, rw) and hides the rest.
  static constexpr int SHADOWS = 18;
  void setShadow(int slot, const double pos[3], const double quat[4], double size, const double u[6]);
  void hideShadow(int slot);
  // Avatars (other players, the ghost): the host makes one node per part and writes the core's pose records into them.
  int addAvatarBody(const float rgb[3], int uniqueId);   // the core's ghost body (model 13) in this linear colour
  int addAvatarGun(int weapon);   // a held-gun node (model 14, the mesh named by the weapon number); -1 when there is no mesh
  int addAvatarAccessory(const float rgb[3], int uniqueId);   // rc_model_avatar_surface in this linear colour
  int addPlate(const uint8_t* rgba, int w, int h);   // a unit quad facing +z with this straight-alpha image
  void setNodePose(int handle, const double pos[3], const double quat[4], const double scale[3], bool visible);
  void setOpacity(int handle, double opacity);   // of the handle's model
  int addViewWeaponGlb(const std::vector<uint8_t>& glb, double scale);   // /custommodel: fitted by rc_fit_standard(scale)
  int addObjectGlb(const std::vector<uint8_t>& glb, double size);   // /custompickup
  void setLoadOptions(int picmip, bool mapFx);   // r_picmip, r_mapFx: read by load()
  void setPicmip(int level);
  void refreshPicmip();   // render thread, after the previous frame's fence and before recording
  void setRender(bool sortTranslucent, bool volumeFog, bool fastSky);   // r_sortTranslucent, r_volumeFog, r_fastSky: any time after load
  // cg_ghostPath: the ghost's route as two lines (ghost-path.js). Build once per track (rows = stride 14: t x y z ... teleport counter in 13); draw every frame (no allocation). k = the sample the ghost is at; resW / resH in layout pixels.
  void buildGhostPath(const double* rows, int n);
  void drawGhostPath(bool on, int k, double width, double opacity, const double linearRgb[3], double resW, double resH);
  void setMirrors(bool on);   // r_mirrors: off = no mirror / portal passes (the surfaces draw their ordinary material)
  // Weapon effects (the sim reports them as events): impact marks (style 0 bullet, 1 rail burn, 2 rail glow, 3 scorch, 4 plasma, 5 plasma burn; radius <= 0 = the style's own), bullet sparks, an explosion's expanding sphere,
  // projectiles (weapon 5 rocket, 4 grenade, 8 plasma, 9 BFG) and the rail beam. Call fxUpdate once per frame with the frame's time step and camera.
  void fxMark(int style, const double pos[3], const double normal[3], double radius);
  void fxSparks(const double pos[3], const double normal[3]);
  void fxBlast(int weapon, const double pos[3], double radius, const double* normal = nullptr, bool ghost = false);   // normal: the surface (the plasma splash faces it; null = the camera)
  void fxProjectile(int id, int weapon, const double pos[3], const double vel[3], bool spawn);
  void fxProjectileEnd(int id);
  void fxBeam(const double from[3], const double to[3]);
  void fxHook(const double from[3], const double to[3], double time, bool attached, int slot = 0);   // the hook cable from the muzzle to the hook head; slot 0 = the player's, 1.. = other players' (one cable each)
  void fxHookEnd(int slot = 0);
  // Where the first-person cable leaves the gun, in world space (the core's rc_hook_origin): the rendered camera and the rig's placement (QcCamera::gun). No allocation.
  void hookOrigin(const CameraIn& cam, const double* gunPlace, double out[3]);
  void fxUpdate(double dt, const CameraIn& cam);
  void fxSetMarkTime(double seconds);
  int addViewWeapon(int kind);
  // Each frame: the machinegun barrel and the gauntlet rotor turn by the core's own spin (rc_mg_spin / rc_gauntlet_spin; they advance only while firing / attacking).
  void spinViewRotors(double dt, bool machinegunFiring, bool gauntletAttacking);
  // The same from a contributed weapon file in the file source (+x = barrel), fitted to the built-in shape of fitKind (3 plasma, 4 rocket) like the page does; -1 when the file is not there.
  int addViewWeaponFile(const std::string& glbRel, int fitKind);
  // Each frame: which weapon node is held (-1 none) and the rig numbers for rc_viewgun_place (cg_drawGun, cg_gunX/Y/Z, the walk bob scale, the switch drop 0..1, the bob 0..1, the recoil 0..1).
  void placeViewGun(int heldHandle, const double* p);
  // A flat text board facing -z at (0, yCentre, z), 2 halfW x 2 halfH, textured with an RGBA bitmap, unlit and transparent (the runway's START / FINISH / checkpoint boards). Needs the objects-only scene.
  int addBoard(const uint8_t* rgba, int w, int h, double z, double yCentre, double halfW, double halfH, double yaw);
  // The scene fog of an objects-only scene, set BEFORE adding models (the program variants depend on it): mode 0 none / 1 exponential (FogExp2), colour in linear rgb, density, and the clear colour (sRGB).
  void setFog(int mode, const float linearRgb[3], float density, const float clearSrgb[3]);
  // Where to stand: the first info_player_start / deathmatch (three.js axes). false = the map has none.
  bool spawn(double eye[3], double& yawDeg) const;
  std::string entityClass(int index) const;   // classname of BSP entity `index` (lump order), "" when none
  // Plans the frame, records every pass (mirror/portal targets first, then the world into `mainFb`). The command buffer is in the recording state. time in seconds.
  // poses: fp::Pose records (64 bytes) indexed by BSP model index, nPoses of them; null/0 = every mover at rest.
  void record(VkCommandBuffer cb, VkFramebuffer mainFb, int w, int h, const CameraIn& cam, double time, const uint8_t* poses = nullptr, int nPoses = 0,
              const std::function<void(VkCommandBuffer)>& overlay = nullptr);   // overlay: recorded inside the screen pass after the world and objects (the HUD)
  // Records inside the screen render pass, before HUD overlays.
  bool recordMinimap(VkCommandBuffer cb, int w, int h, const float box[4],
                     const QcMinimapOut& view, double playerY, double minY,
                     double time, const uint8_t* poses, int nPoses, VkQueryPool timestamps);
  const WorldStats& stats() const;
  const double* mapBounds() const;   // min xyz and max xyz in scene axes
  void warmPipelines();

  // Fit the pickup to size, or create its marker. setObject places the resulting handle each frame.
  int addPickup(uint32_t weapon, const std::string& glbRel, const char* icons, double size,
                uint32_t marker, double radius, double stretch);
  // The !showtriggers overlay (rc_trigger_overlay): built hidden for the loaded map (the same fixes text, map name and physics the sim is loaded with) or the runway; call again after a physics mode change
  // (the old one is freed). Label images are uploaded here. showTriggerOverlay shows or hides it (the page's settings.showTriggers).
  bool buildTriggerOverlay(const std::string& fixes, const std::string& name, bool vql, bool runway);
  void showTriggerOverlay(bool on);
  void setObject(int handle, const double pos[3], double yaw, bool visible);
  struct Impl;   // the executor's state; public only so the translation unit's helpers can name it
 private:
  std::unique_ptr<Impl> m;
};

// A look-from camera in the game's three.js axes (yaw 0 looks along +x, 90 along -z). Perspective, vertical fov in degrees.
CameraIn makeCamera(const double eye[3], double yawDeg, double pitchDeg, double aspect, double fovYDeg);
CameraIn makeCameraView(const double eye[3], const double quat[4], double aspect, double fovYDeg);   // the live camera: QcCamera eye and quaternion, view matrix and clips from the core
CameraIn makeCameraFwd(const double eye[3], const double forward[3], double aspect, double fovYDeg);

}  // namespace nc
