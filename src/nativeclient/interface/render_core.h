/* C API of the render core (render_core.lib / librender_core.a, or render-core.wasm). The functions a native renderer uses are the "frame packet" ones; the rest exist for the
   parity tests. Plain C, no allocation by the caller: the core keeps its own arena (512 MB static for the native library; define QS_ARENA_DOUBLES at build time to change it).
   Typical use: rc_reset(); load: write a scene input (frame_packet.hpp SceneHeader ...) into rc_alloc(n) memory, rc_build_packet(); copy/consume the LOAD packet; rc_mark();
   every frame: rc_frame_begin(); rc_alloc(n) the frame input (flags REUSE); rc_build_packet(); consume rc_packet_ptr()/rc_packet_len(). The packet is valid until the next
   rc_frame_begin(). Layout: render-core/FRAME-PACKET.md, frame_packet.hpp. */
#ifndef RENDER_CORE_H
#define RENDER_CORE_H
#include <stdint.h>
struct QcMinimapOut;
#ifdef __cplusplus
extern "C" {
#endif
int rc_version(void);
void rc_reset(void);
void* rc_alloc(int bytes);
void rc_mark(void);
void rc_frame_begin(void);
int rc_arena_used(void);
int rc_build_packet(const uint8_t* scene_input, int len);   /* 0 = ok, else a negative error (packet_build.hpp BuildError) */
const uint8_t* rc_packet_ptr(void);
int rc_packet_len(void);
int rc_layout(int what);   /* struct sizes, for a consumer to assert against its own copy of frame_packet.hpp */
/* resident world visibility (cl_wasmrender phase C): load once per map, then one call per frame */
const uint32_t* rc_shader_infos(const char* files, int files_len, int map_fx);   /* after rc_bsp_load + rc_parse_scripts: {flags, cull} per BSP shader (batches.hpp ShaderBatchInfo), decided by the core; null on failure */
int rc_build_batches_auto(const char* files, int files_len, int map_fx, const char* ents, int ents_len);   /* rc_shader_infos + movers / triggers from the entity text + rc_build_batches in one call; 0 or a negative error */
int rc_world_scene_prepare(const char* files, int files_len, int map_fx, const double* sweeps, int n_sweeps);   /* after rc_build_batches_auto + rc_merge_batches: one material per batch + the culling instances; sweeps: n x 7 doubles {model, rest xyz, end xyz} of the sliding movers; call before rc_mark */
int rc_frame_objects(const uint8_t* camera, double time, const uint8_t* poses, int n_poses, int include_static, uint32_t layer_mask, int root);   /* one frame of play: rc_world_frame's packet + the object list, see FRAME-PACKET.md "Frame list" */
int rc_scene_frame(const uint8_t* camera, uint32_t layer_mask, int root);   /* the same list for the objects alone (the view weapon's pass): no world, no packet */
const uint8_t* rc_frame_entries(void);   /* rc_frame_entry_count() fo::Entry records (24 bytes: kind, index, phase, reserved, depthKey), in draw order */
int rc_frame_entry_count(void);
int rc_frame_phase_count(int phase);   /* entries per phase: 0 sky, 1 opaque, 2 transparent */
const uint8_t* rc_frame_draw_blocks(void);   /* the std140 DrawBlock (192 bytes) of each entry */
int rc_pass_block(const uint8_t* camera);   /* fills the camera / fog fields of the std140 PassBlock for a fp::Camera; returns the used bytes (or -1) */
const uint8_t* rc_pass_block_ptr(void);
int rc_mirror_camera(const double* in, uint8_t* out);   /* in = 42 doubles {viewer matrixWorld[16], projection[16], near, far, fovY, plane normal xyz, plane point xyz, doubleSided}, out = a fp::Camera (440 bytes) then the 16-double texture matrix; 0 for a one-sided mirror seen from behind */
int rc_portal_camera(const double* in, uint8_t* out);   /* in = 69 doubles (the viewer, the plane, then the portal in Q3 axes), out as rc_mirror_camera: Q3 R_GetPortalOrientations with spin and roll */
int rc_mirror_aperture(const double* in, const double* pos, int count, double* out);   /* in = 50 doubles, pos = count vertices (3 doubles each), out = 16 doubles: the virtual projection cropped to the mirror's screen rectangle; 1, or 0 for the full view */
int rc_minimap_camera(const double* in, uint8_t* out);   /* in = {cx, cz, S, minY, maxY, clipY} (the map's box and the clip height), out = a fp::Camera (440 bytes): the orthographic top-down camera of the minimap bake, for rc_world_frame; returns the byte count or -1 */
/* Scene axes (+Y up). Writes a 440-byte fp::Camera with GL clip depth and CAM_NO_PVS to an 8-byte-aligned buffer. Returns 440 or -1. */
int rc_minimap_live_camera(const struct QcMinimapOut* view, double playerY, double minY, uint8_t* out);
const float* rc_lights_block(void);   /* the std140 LightBlock (480 bytes) of the last rc_lights_pack */
int rc_vulkan_attribute_count(void);   /* the Vulkan dialect's vertex inputs in AttributeBit order */
int rc_vulkan_attribute(int i, int what);   /* what: 0 the AttributeBit, 1 the float count, 2 the locations taken */
const char* rc_vulkan_attribute_name(int i);
int rc_world_set_fog(const uint8_t* volumes, int len, int count);   /* the map's fog volumes for the next rc_world_scene_prepare (FogVolume[count] + planes, see FRAME-PACKET.md); -1 once a world is prepared */
int rc_model_parse_glb(const uint8_t* bytes, int len);   /* a GLB in; the core-owned model record (NATIVE-CLIENT.md 8.3) at rc_model_ptr(): its byte length, or -1 (rc_model_error()). Load time; copy the record out at once */
int rc_model_box(double width, double height, double depth);   /* THREE.BoxGeometry(w, h, d), one segment per side, as a model record */
int rc_trigger_overlay(const char* fixes, int fixes_len, const char* name, int name_len, int vql, int runway);   /* version 8. The !showtriggers overlay of the map loaded with rc_bsp_load, as a model record at rc_model_ptr(): its byte length, or -1 (rc_model_error()). fixes = map-fixes.json text (may be empty), name = the map name without extension, vql = 1 VQL / 0 PQL (the same values qc_sim_load_map gets); runway != 0 = the built-in runway (no BSP, the other arguments are ignored). Load time; copy the record out at once. The record: node 0, FILL meshes (triangles, uvs; unlit, translucent, no depth write, single sided, one raw RGBA8 512x128 image each with REPEAT wrapping (Material.wrapS / wrapT 10497), linear, no mipmaps) and EDGE meshes named "edge" (line segments, shading 4, opacity .75), in the scene's world coordinates. Draw it after the world, fills before edges, only while the page's settings say showTriggers. */
const char* rc_trigger_styles(void);   /* version 8. The overlay's styles in legend order, NUL terminated: key, 0x1f, rrggbb, 0x1f, legend label, one style per line */
int rc_trigger_order(int line); /* Load time. Apply to each overlay mesh using its material's line shading flag. */
const double* rc_runway_fog(void); /* Seven doubles: sRGB clear RGB [0,1], linear fog RGB, exponential-squared density. */
typedef struct RcRunwayBoard {
  double z, y, width, height, yaw;
  uint32_t rgba; /* 0xAABBGGRR for text_board_bake */
  char text[28]; /* NUL terminated */
} RcRunwayBoard;
int rc_runway_board(int index, RcRunwayBoard* out); /* 1 writes a row, 0 at end or invalid arguments. Load time. */
int rc_runway_surface_flags(double x, double y, double z, int flags); /* Visual surface flags for a confirmed runway hit. */
double rc_hook_model_z(void); /* Grapple model translation along the view rig's local Z. */
double* rc_hook_origin_buffer(void); /* 35 doubles: camera world matrix [0..15], grapple model-to-view matrix [16..31], world output [32..34]. Fixed scratch, no arena allocation. */
void rc_hook_origin(int gunShown); /* Uses the buffer above. Both matrices are column-major affine. Needs only camera matrix when hidden. */
const double* rc_camera_clips(void); /* Four doubles: live world near/far, view weapon near/far. */
int rc_camera_view(const double* eye, const double* quat, double* out); /* Eye XYZ and QcCamera quaternion XYZW to a 16-double column-major view matrix. Returns 0 or -1 on null. */
int rc_model_build(int kind);   /* the game's object content by id (model_gen.hpp ModelKind: 1 machinegun view model) as a model record at rc_model_ptr(); -1 and rc_model_error() on failure */
typedef struct RcPickupVisual {
  char classname[48], model[32], icons[16]; /* manifest key or empty; ammo icon material or empty */
  double stretch;
  uint32_t weapon, size, marker, glow, triangles, materials, radius, reserved;
} RcPickupVisual;
int rc_pickup_visual(int index, RcPickupVisual* out); /* load time: iterate from 0 until 0 is returned; 1 writes a row. weapon is the named mesh in rc_model_build(14); otherwise use model if set. Always fall back to the coloured octahedron (radius, stretch on Y) if no model loads. Colours are sRGB 0xRRGGBB. */
typedef struct RcViewWeaponVisual {
  int32_t weapon, kind;
  char model[32]; /* preferred manifest key, or empty to use the built-in model */
} RcViewWeaponVisual;
int rc_view_weapon_visual(int index, RcViewWeaponVisual* out); /* load time: iterate from 0 until 0 is returned. kind is the built-in rc_model_build id and rc_fit_weapon kind for the optional model file. */
int rc_hook_cable(double fx, double fy, double fz, double tx, double ty, double tz, double time, int attached);   /* the hook cable from f to t at `time`: the number of chain links (0 = too short for any), at most 64; the links' 4x4 matrices (float32, column-major, 16 per link) at rc_hook_cable_links(), the head's position (3 doubles) and orientation xyzw (4 doubles) at rc_hook_cable_head(); allocation-free */
const float* rc_hook_cable_links(void);
const double* rc_hook_cable_head(void);
const double* rc_webhand_pose(double kick);   /* the dev-only web hand for a shot strength: 8 doubles = wrist rotation x, index rotation x, pinky rotation x, index rotation y, pinky rotation y, thumb rotation y, strands visible (0/1), strands scale z */
const double* rc_viewgun_place(double drawGun, double gunX, double gunY, double gunZ, double walkBob, double lower, double bob, double kick);   /* the view weapon rig for the cg_gun settings, the switch drop (lower 0..1), the bob and the recoil (kick 0..1): 7 doubles = visible (0/1), position xyz, rotation xyz (Euler XYZ); allocation-free */
double* rc_fit_bounds(void);   /* 6 doubles the host fills with a contributed weapon file's box (min xyz, max xyz after turning the file by rc_fit_turn() about y) before rc_fit_weapon */
const double* rc_fit_weapon(int kind);   /* fits that file (+x = barrel) to the built-in shape of `kind` (3 plasma gun, 4 rocket launcher) in length and centre: returns 5 doubles = scale, position xyz, rotation y (the turn) */
double rc_fit_turn(void);   /* the turn about y (pi/2) a contributed weapon file gets before its box is measured for rc_fit_weapon */
const double* rc_fit_standard(double scale);   /* the standard placement of a future weapon file: 5 doubles = scale, position xyz, rotation y; cancels the rig's base offset */
const double* rc_splash_pose(double ox, double oy, double oz, double nx, double ny, double nz, int hasNormal, double age, double qx, double qy, double qz, double qw);   /* the plasma splash for an impact at origin o with surface normal n (hasNormal 0: faces the camera quaternion q) at age 0..1: 9 doubles = position xyz, quaternion xyzw, scale, opacity; allocation-free */
const double* rc_orient_unit(double fx, double fy, double fz, double tx, double ty, double tz);   /* the quaternion xyzw (4 doubles) of the shortest rotation taking unit vector f to unit vector t */
double rc_gauntlet_spin(double dt, int attacking);   /* the gauntlet rotor's angle about y (radians, [0, 2pi)) after dt seconds; advances only while attacking; state in the core; the host sets the gauntlet-rotor node's rotation y */
double rc_mg_spin(double dt, int firing);   /* view-model animation: the machinegun barrel's angle (radians, [0, 2pi)) after dt seconds; the spin state is in the core; allocation-free; the host sets the rotor node's rotation z to it */
int rc_beam_build(double fx, double fy, double fz, double tx, double ty, double tz);   /* the railgun beam from f to t as a model record at rc_model_ptr(): meshes core / glow / halo (open tubes, nodes carry the place) and the helix line mesh spiral (shading 4 = line); -2 when shorter than 1 unit; call rc_reset() between beams in a dedicated instance */
int rc_ghost_path(const double* samples, int n);   /* the ghost's route as line segments: n samples of 5 doubles [t, x, y, z, teleportCounter] (0 = none); returns the segment count (0 none, -1 failed); teleports leave a gap */
const float* rc_ghost_path_fwd(void);   /* count x 6 floats (x,y,z,x,y,z; feet + 8 up), track order */
const float* rc_ghost_path_rev(void);   /* the same segments, last first (the Future line) */
const int* rc_ghost_path_before(void);   /* n ints: before[k] = segments that end at or before sample k */
int rc_clips_load(const double* data, int n);   /* a custom weapon's animation clips as a double stream (render-core/clips.hpp): bindings (path, bind pose) then clips (kind idle/fire/fire_empty/spinup/spindown, duration, tracks); returns bit 0 loaded, bit 1 has a fire clip. One player per core instance */
int rc_clips_reset(void);   /* every clip stopped, idle restarted; returns the number of node writes at rc_clips_out() */
void rc_clips_fired(void);   /* one shot fired */
void rc_clips_emptied(void);   /* the trigger pulled with no ammo */
int rc_clips_update(double delta, int attacking);   /* every frame the weapon is held (delta seconds, attacking = trigger held); returns the number of node writes at rc_clips_out() */
const double* rc_clips_out(void);   /* n x 5 doubles [binding, v0, v1, v2, v3]: the binding's translation, rotation (xyzw) or scale */
const double* rc_model_bounds(double turn);   /* the world box of the current model record (rc_model_ptr): 6 doubles (min xyz, max xyz) after turning the whole model `turn` radians about y; null = no vertices. What a host measures a contributed file with before rc_fit_weapon (turn = rc_fit_turn()) or a pickup's fit (turn 0) */
int rc_model_avatar_surface(void);   /* the avatar accessory surface (avatar-dick.js dickSurfaceGeometry) as a model record */
const uint8_t* rc_model_ptr(void);
const char* rc_model_error(void);
int rc_world_set_vulkan(int on);   /* the dialect of the next rc_world_scene_prepare: 0 = GL (default), 1 = Vulkan GLSL 450 (NATIVE-CLIENT.md section 7); -1 = refused while a world is prepared in the other dialect */
void rc_world_scene_reset(void);   /* forgets the prepared world and the dialect (GL again); rc_reset does too */
int rc_world_frame(const uint8_t* camera, double time, const uint8_t* poses, int n_poses, int include_static);   /* a fp::Camera, the clock, fp::Pose[n] by brush model -> the frame packet of the visible batches (rc_packet_ptr / rc_packet_len); include_static: carry the material tables */
int rc_stage_prepare(void);   /* after the load packet: the per-stage float table (stage_values.hpp); float count or a negative error */
int rc_stage_count(void);
const uint32_t* rc_stage_offsets(void);   /* rc_stage_count()+1 float offsets */
const double* rc_stage_frames(double time);   /* every stage at `time` seconds (the backend narrows to float32); null if rc_stage_prepare did not succeed */
int rc_world_load(const uint8_t* world_input, int len);   /* WorldHeader input, frame_packet.hpp; kept in memory */
int rc_world_cull(int cluster, const double* view_proj, uint8_t* pvs_out, uint8_t* vis_out);   /* n bytes each; returns the visible count */
/* The one visibility step for EVERY view (main camera, mirror / portal passes, minimap bake, replay cameras). cluster: where the view's PVS is taken from (a planar mirror: the player's; a portal: its
   camera entity's). flags: 1 = no PVS. skip: an instance never visible in this view (the surface rendered into), -1 = none. spheres: 0, or 4 doubles per instance {centre, radius; radius < 0 = unknown}
   for the moving instances. pvs_out may be 0. Returns the visible count. */
int rc_view_cull(int cluster, const double* view_proj, uint32_t flags, int skip, const double* spheres, uint8_t* pvs_out, uint8_t* vis_out);
/* Which mirror / portal surfaces get their extra scene pass this frame: coverage[n] = the fraction of the screen each covers, drawn[n] in: 1 = had its pass last frame, out: 1 = gets one now
   (at most 2 per frame, the biggest win; below 0.0015 of the screen none; hysteresis 0.001 and x1.25). n <= 64. Returns the passes chosen. */
int rc_view_passes(int n, const double* coverage, uint8_t* drawn);
/* The frame plan (frame_plan.hpp, NATIVE-CLIENT.md 8.9): the core decides the passes of a frame. Once per map after the world is loaded, before rc_mark: rc_plan_begin, rc_plan_surface per mirror / portal
   surface (fr::SurfaceIn 160 bytes + its vertices, 3 doubles each; returns the surface id). Per frame: rc_plan_live()[id] = 1 for a surface that shows a live view, the fr::PlanIn (1040 bytes) at rc_plan_in(),
   rc_plan_run(main_vis or 0, spheres or 0) = the number of passes at rc_plan_passes() (fr::Pass, 728 bytes each, in draw order); rc_plan_drawn()[id] = the surface has its pass this frame;
   rc_plan_vis() + Pass.visOffset = the visible world instances of a pass; rc_plan_pass_frame(pass, ...) = that pass's draw list (rc_frame_entries / blocks / states, rc_packet_ptr when it draws the world).
   rc_plan_sizeof: 0 SurfaceIn, 1 Pass, 2 PlanIn, 3 surface capacity, 4 pass capacity. */
int rc_plan_begin(void);
int rc_plan_surface(const uint8_t* in, const double* pos, int count);
uint8_t* rc_plan_live(void);
uint8_t* rc_plan_drawn(void);
uint8_t* rc_plan_in(void);
int rc_plan_run(const uint8_t* main_vis, const double* spheres);
const uint8_t* rc_plan_passes(void);
const uint8_t* rc_plan_vis(void);
int rc_plan_sizeof(int what);
int rc_plan_pass_frame(int pass, double time, const uint8_t* poses, int n_poses, int include_static, int root);
/* A prepared world registers its own mirror / portal surfaces (rc_world_scene_prepare): a host calls rc_plan_begin / rc_plan_surface only for surfaces that are not world batches.
   rc_plan_surface_count; rc_plan_surface_batch(id) = the world batch that IS the surface (-1: host registered); rc_plan_surface_in(id) = its fr::SurfaceIn (target size, plane, portal camera).
   rc_plan_poses(poses, n): the movers' fp::Pose records of the frame about to be planned (a mover's mirror follows its model's pose).
   rc_world_set_sorted(on): sorted translucency: a translucent batch is drawn as its surfaces (DrawItem.objectId = batches + surface, bufferId = the batch, its own index range). */
int rc_plan_surface_count(void);
int rc_plan_surface_batch(int id);
const uint8_t* rc_plan_surface_in(int id);
void rc_plan_poses(const uint8_t* poses, int n);
void rc_world_set_sorted(int on);
int rc_world_split_instance(int split);   /* the culling instance of a translucent surface (rc_batch_splits index; -1: never culled) */
int rc_world_instance_count(void);        /* the culling instances of the loaded world: a visibility answer (rc_world_cull, rc_plan_vis) has one byte each */
int rc_world_batch_instance(int batch);   /* the culling instance of a prepared world batch (-1: never culled): SurfaceIn.instance of a mirror surface that is a world batch */
/* BSP surfaces (cl_wasmrender phase C2a): rc_bsp_load first. See surfaces.hpp for the record / vertex layout. */
int rc_tessellate(void);
int rc_surf_count(void);
const int32_t* rc_surf_records(void);
int rc_surf_vertex_count(void);
const double* rc_surf_vertices(void);
int rc_surf_index_count(void);
const int32_t* rc_surf_indices(void);
/* World batches (phase C2b): rc_bsp_load + rc_tessellate first; the input is a BatchHeader (batches.hpp). Batch records are sizeof(Batch)/4 int32 each. */
int rc_build_batches(const uint8_t* input, int len);
int rc_batch_count(void);
const int32_t* rc_batch_records(void);
const char* rc_batch_keys(void);
const double* rc_batch_vertices(void);
const int32_t* rc_batch_indices(void);
const int32_t* rc_batch_clusters(void);
int rc_batch_split_count(void);
const int32_t* rc_batch_splits(void);
int rc_batch_sizes(int what);   /* 0 = record width in int32, 1 = vertices, 2 = indices, 3 = cluster ids */
/* Lightmap atlas + batch merging (phase C2c): after rc_build_batches. Replaces the batches; pages are RGBA8. */
int rc_merge_batches(void);
int rc_page_count(void);
int rc_page_width(int page);
int rc_page_height(int page);
const uint8_t* rc_page_data(int page);
int rc_face_cluster_count(int face);   /* PVS clusters of a BSP face, after rc_build_batches */
const int32_t* rc_face_clusters(int face);
/* parity-test entry points */
int rc_parse_scripts(const char* text, int len);
/* Map-load only. Start with rc_reset and submit one whole script file per call. The higher tier wins:
   0 pooled library, 1 OpenArena, 2 loose/shared scripts, 3 selected PK3. Within a tier the later
   definition wins, except tier 1 uses the first file containing a name and its last definition.
   Keep input bytes alive through use of the scripts. Returns 0 or an error. rc_serialize_shaders
   exposes the resolved definitions for host image discovery. */
int rc_map_shader_source(const char* text, int len, int tier);
/* After sources, before image discovery: apply map-fixes.json "shaders" for the actual BSP basename
   without extension, exact key preferred then ASCII case-insensitive. Keep JSON bytes alive.
   Empty JSON is allowed. Returns 0 or an error. */
int rc_map_shader_fixes(const char* json, int len, const char* name, int name_len);
/* After rc_bsp_load and script resolution: write worldspawn fog brushes to rc_out_ptr/len in the
   rc_world_set_fog format. Returns volume count or -1. cap -1 keeps all, 0 keeps none, positive
   keeps that many in BSP order. Consume the bytes before the next rc_out producer/reset. */
int rc_map_fog(int cap);
/* Map texture decode/refresh only. In-place RGBA8 box filter, up to 10 levels. Drops odd trailing
   rows/columns per step and stops when either dimension reaches 1. size is [width,height] in/out.
   Returns 0 or -1 with inputs untouched on invalid size/buffer length. */
int rc_picmip_rgba(uint8_t* pixels, int len, int32_t* size, int levels);
int rc_shader_count(void);
int rc_serialize_shaders(void);
int rc_serialize_decisions(const char* files, int files_len);
int rc_split_blocks(const char* text, int len);
const uint8_t* rc_out_ptr(void);
int rc_out_len(void);
int rc_sort(const double* keys, int n, int transparent, int* order);
int rc_sort_cap(void);   /* the allocation-free per-frame sort: static buffers (capacity rc_sort_cap() keys) */
double* rc_sort_keys(void);   /* n x 5 doubles, as rc_sort */
int* rc_sort_order(void);   /* n int32 out */
/* the resident scene (scene_store.hpp): nodes as flat arrays the host writes into (rc_scene_array ids = scene::Array), then rc_scene_update (world matrices + inherited visibility), write the 16 doubles A_VIEW_PROJ,
   rc_scene_select(layer_mask) (the visible drawable nodes, split sky / opaque / transparent and sorted; read with rc_scene_list / rc_scene_list_count). No per-frame allocation. */
int rc_scene_cap(void);
int rc_scene_material_cap(void);
void rc_scene_reset(void);
int rc_scene_create(void);   /* a node index or -1 */
void rc_scene_destroy(int node);
int rc_scene_high_water(void);
void* rc_scene_array(int what);
void rc_scene_update(void);
int rc_scene_select(uint32_t layer_mask, int root);   /* root >= 0: only the nodes below that node */
const int32_t* rc_scene_list(int which);
int rc_scene_list_count(int which);
/* program variants (glsl_variants.hpp): the basic / lit / sprite / line / effect programs, built on request. rc_variant_build(family, key) returns the byte length of one record {Program, UniformSlot[uniformCount], blob}
   (offsets inside the blob) at rc_variant_ptr(), valid until the next build / rc_frame_begin, or -1; the constants name the keys and uniform sources the core defines. */
/* DIALECT_VULKAN in the key (and SceneHeader.flags & 8 for the packet's programs) selects the Vulkan GLSL 450 dialect (NATIVE-CLIENT.md section 7); rc_vulkan_error() is the reason of the last failed lowering */
const char* rc_vulkan_error(void);
int rc_variant_build(int family, uint32_t key);
int rc_variant_key(int family, uint32_t facts, int num_dir, int num_hemi, int fog_mode);   /* the facts of a draw (rc_variant_const FACT_*), the scene's light counts and fog mode (0 none, 1 exp, 2 linear) -> the variant key (OR the dialect bit); -1 invalid */
const uint8_t* rc_variant_ptr(void);
int rc_variant_const_count(void);
const char* rc_variant_const_name(int i);
uint32_t rc_variant_const_value(int i);
/* the scene's lights (lights.hpp): a host writes records into rc_lights_in and the view into rc_lights_view, packs once per pass with rc_lights_pack(n), reads the arrays with rc_lights_out(which) and the sizes with rc_lights_info(what) */
double* rc_lights_in(void);
double* rc_lights_view(void);
int rc_lights_pack(int n);
/* Write the main camera's column-major view matrix to rc_lights_view() before sampling at its eye. use_grid=0 selects the fixed view-space lights. Returns the 480-byte LightBlock. */
const float* rc_viewgun_lights(double eye_x, double eye_y, double eye_z, int use_grid);
const double* rc_lights_out(int which);   /* doubles: the host rounds to its upload type */
int rc_lights_info(int what);
/* the object pipeline state (object_state.hpp, FRAME-PACKET.md): the host writes the facts of a material into rc_object_state_facts (doubles in rc_object_state_fact_name order), rc_object_state_derive returns the ObjectState record
   for them (a pure function, one static record), rc_scene_material_set stores it as the state of a scene-store material class, rc_scene_material_state reads it back. */
const uint8_t* rc_frame_states(void);   /* the ObjectState (184 bytes, rc_layout(29)) of each frame-list entry, same order as rc_frame_entries() */
int rc_object_state_size(void);
int rc_object_state_fact_count(void);
const char* rc_object_state_fact_name(int i);
double* rc_object_state_facts(void);
const uint8_t* rc_object_state_derive(void);
int rc_scene_material_set(int material_class);
const uint8_t* rc_scene_material_state(int material_class);
int rc_sort_run(int n, int transparent);   /* 0 = ok, -1 = n out of range */
void rc_predict_run(const double* steps, int n, double* out);
double rc_fog_ray(const double* planes, int n, const double* start, const double* end);
int rc_bsp_load(const uint8_t* bytes, int len);
int rc_bsp_leafs(void);
int rc_vis_clusters(void);
int rc_point_cluster(double x, double y, double z);
int rc_clusters_in_box(const double* mn, const double* mx, uint8_t* found);
int rc_cluster_visible(int from, int to);
#ifdef __cplusplus
}
#endif
#endif
