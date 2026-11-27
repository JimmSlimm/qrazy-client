// The sim half of the contract between the frozen client and the game core (the library qlrace2 builds and the site serves). The render core (render_core.h) and text engine (text_api.h) are the other halves.
// Plain C, 64-bit safe: no offsets that stand for pointers, no ownership crossing the boundary (every returned pointer is into memory the core owns and stays valid until the next call that says otherwise).
// This header is what the client relies on. The core may change anything behind it; a change to this header or to the byte formats it names bumps QC_INTERFACE_VERSION.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define QC_INTERFACE_VERSION 11   // 11: the hub wire codec qc_wire_* (state / peer frames, stateless) and the peer table qc_peer_* / qc_track_point (peer clock, interpolation, node poses, plate data, the send window; research/PEERS-IN-CORE-DESIGN.md); 10: qc_dry_cues (the sounds of other players' / recorded shots, the browser's soundDryCues); 9: the dry shot queries qc_dry_* (peer hitscan / missile flights / hook fire, mover override); 8: the !showtriggers overlay (render_core.h rc_trigger_overlay, rc_trigger_styles; a model record material may ask for REPEAT wrapping)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QcCue { char name[96]; double pos[3]; double gain; int32_t flags, emitter, group; } QcCue;   // flags: POS 1, LOOP_START 2, LOOP_STOP 4, LOOP_MOVE 8, VOICE 16, SYNTH 32, STOP_ALL 64, SHARE 128
enum {
  QC_SOUND_CHAT=1, QC_SOUND_VOTE=2, QC_SOUND_VOTE_END=3, QC_SOUND_RACE=4,
  QC_SOUND_FINISH=5, QC_SOUND_TAUNT=6, QC_SOUND_SPRAY=7, QC_SOUND_FUNNY=8,
  QC_SOUND_WR_CLIP=9, QC_SOUND_MUTE=10, QC_SOUND_PEERS=11, QC_SOUND_RESET=12,
  QC_SOUND_FOLLOW=13, QC_SOUND_FOLLOW_FLIGHT=14, QC_SOUND_FOLLOW_END=15,
  QC_SOUND_FOLLOW_DRY=16, QC_SOUND_FUNNY_STOP=17
};
enum {
  QC_CUE_UI=256, QC_CUE_WORLD_MUTE=512, QC_CUE_FUNNY=1024, QC_CUE_SPECIAL=2048,
  QC_CUE_WR_CLIP=4096, QC_CUE_EXPLICIT_VOICE=8192,
  QC_CUE_STOP_FUNNY=32768
};
// Binary event payload is one QcCue (144 bytes), copied during the call. Unused fields zero.
// Native hosts send RESET before loading their first map to select this stream for sim and peer cues.
// CHAT: gain=monotonic seconds, flags bit 0 own, bit 1 result/hidden.
// VOTE / VOTE_END: emitter=positive vote id, flags bit 0 passed (end only). Zero id uses the active vote.
// RACE: emitter=count 0..3 or 4 for over, flags=QC_CUE_UI if page-owned in a native match.
// FINISH: emitter=0 finish, 1 PB, 2 WR, name=setting override or empty, flags=QC_CUE_UI for local/preview.
// TAUNT: name=voice id or empty for current voice, flags=POS if remote, pos=source.
// For own taunt gain=monotonic seconds, returns 1 during cooldown (skip relay).
// SPRAY: flags=POS and/or UI, pos=source. FUNNY / WR_CLIP: name=resolved clip, gain=seconds,
// flags=SPECIAL for the special pack. WR_CLIP bit 0 bypasses funnySounds (own WR and preview).
// MUTE: flags bit 0 active, bit 1 chat open, bit 2 multiplayer. PEERS: gain=now after qc_peer_fx_frame.
// RESET: clear chat/vote/funny dedup for a new connection. FOLLOW: relayed normalized QcCue;
// FOLLOW_FLIGHT: flags bit 0 thrust on. FOLLOW_END: clears followed-player loop.
// FOLLOW_DRY: group=dry kind 9 (speaker), 10 (checkpoint) or 11 (effect), emitter=index,
// flags=POS if applicable, pos=source. FUNNY_STOP: cancel pending and playing clips and reset cooldown.
// EXPLICIT_VOICE: name is voice/clip. WORLD_MUTE: gain is 0 or 1. WR_CLIP: pos[0] is max seconds.
int qc_ses_event(int kind, const char* data, int n);
// Caller-owned output, drain up to cap in order. A zero cap queries pending count. -1 invalid arguments.
// -2 reports dropped records and leaves the queued prefix available for the next call.
int qc_ses_cues(QcCue* out, int cap);
// Pure spatial law, audio-thread safe. Delta source minus listener and listener's horizontal right vector.
// Returns distance gain. Writes stereo pan [-1,1] when pan is supplied.
double qc_sound_spatial(double dx, double dy, double dz, double rightX, double rightZ, double* pan);
// Synth recipes: one row per oscillator, seven doubles [wave, Hz, at, duration, glide Hz, peak, attack].
// 0 sine, 1 triangle, 2 square. Query with cap=0. Short buffer returns -1, unknown name returns 0.
int qc_sound_synth(const char* name, int n, double* rows, int cap);
typedef struct QcItem { double pos[3]; char classname[48]; } QcItem;   // an item spot, world axes
typedef struct QcHud { double health, armor, ammo; double barAmmo[16]; double puLeft[8]; int32_t timerMs, held, speed; uint32_t barOwned, puCount, puIndex[8]; int32_t weaponInHand, selected, weaponState; double cooldownMs; } QcHud;   // the numbers one HUD frame needs

int qc_interface_version(void);   // QC_INTERFACE_VERSION of the loaded core

// A map: bsp = the .bsp bytes, fixes = map-fixes.json text (may be empty), name = map name without extension. vql: 1 VQL physics, 0 PQL. Returns 0 or a negative error; qc_sim_error() says why.
int qc_sim_load_map(const uint8_t* bsp, size_t bspLen, const char* fixes, size_t fixesLen, const char* name, size_t nameLen, int vql);
// The built-in runway (no BSP): the sim builds its collision itself. fixes = map-fixes.json text (may be empty). Same return / error convention as qc_sim_load_map. Its drawable geometry is render core model kind 16 (rc_model_build(16): world coordinates, identity placement).
int qc_sim_load_runway(const char* fixes, size_t fixesLen, int vql);
const char* qc_sim_error(void);

// One 8 ms tick. yaw / pitch in radians. masks = the tick's 16 key samples. wantWeapon: 1..9 or 0.
void qc_sim_tick(double yaw, double pitch, const uint8_t* masks16, int wantWeapon, int useItem);

// Host-driven input. All times are seconds on one monotonic host clock. DOM
// physical codes ("KeyW", "Mouse0", "Mouse1", "Mouse2", "WheelUp") are
// the platform-independent vocabulary. Key kind: 0 up, 1 down, 2 repeat,
// 3 wheel pulse. Key and mouse events go in oldest first.
typedef struct QcAction { uint32_t action, code; } QcAction;
typedef struct QcInputConfig {
  double sensitivity, mYaw, mPitch, deltaClamp, fov, zoomFov, zoomSensitivity;
  uint32_t flags, reserved;
} QcInputConfig;
typedef struct QcInputView { double yaw, pitch, fovX, remainder; uint32_t zoomed, flags; } QcInputView;  // flags bit 0: +chat held
enum { QC_INPUT_TIMED=1, QC_INPUT_INVERT_X=2, QC_INPUT_INVERT_Y=4, QC_INPUT_ZOOM_TOGGLE=8,
  QC_INPUT_ZOOM_SCALE=16, QC_INPUT_ZOOM_OUT_ON_DEATH=32 };
uint32_t qc_ses_code(const char* domCode, int n);
int qc_ses_bind(const char* json, int n);  // {binds,cvarBinds}; n=0 installs defaults, -1 rejects without replacing the current table
void qc_ses_config(const QcInputConfig* config);
void qc_ses_reset(double now, double yaw, double pitch);  // load / explicit respawn / camera placement / focus loss
void qc_ses_active(int active, int peerZoom);  // peerZoom -1 local, 0/1 when following. Following holds own ticks.
int qc_ses_key(uint32_t code, int kind, double at);  // -1 when an input or action queue is full; zero on success
void qc_ses_mouse(double dx, double dy, double at);
uint32_t qc_ses_held_mask(void);  // live held state, before tick sampling. Bits 0..7: forward, back, left, right, jump, crouch, walk, attack.
int qc_ses_advance(double now);  // due ticks run, with the browser's 100 ms catch-up clamp
int qc_ses_actions(QcAction* out, int cap);   // -1 leaves queue intact; action name "$cvar" resolves against the stored profile by code
const char* qc_ses_action_name(int action);
void qc_ses_view_input(double now, QcInputView* out);  // after late input pump, preview without consuming pending motion
void qc_ses_zoom_projection(double viewFovY, double baseFovY);  // once view projection is chosen, before the next input pump

// The player's settings. qc_ses_settings takes the page's settings object (UTF-8 JSON by setting key, as the account stores it;
// "binds" / "cvarBinds" in the same object install the bind table; other keys are ignored). Absent or unreadable keys are the
// core's defaults; n = 0 installs all defaults and the default binds. It applies what the sim half owns (look, zoom, timed input,
// autohop, autoswitch, start weapons, voice and sound options, camera / view-gun values, peer options, run-record colour) and
// returns the QC_SET_* groups that changed (everything on the first call), or -1 (rejected, nothing changed). The host reads the
// rest from qc_ses_settings_values(). A settings change only: never per frame. May precede the first map load.
enum { QC_SET_INPUT=1, QC_SET_BINDS=2, QC_SET_SIM=4, QC_SET_SOUND=8, QC_SET_CAMERA=16, QC_SET_PEERS=32, QC_SET_GHOST=64,
  QC_SET_RENDER=128, QC_SET_AUDIO=256, QC_SET_HUD=512, QC_SET_ALL=1023 };
// X(group, field, settings key). An empty key: filled by a rule of its own. Booleans are 0 / 1, colours 0xRRGGBB (ghostColor -1 =
// the recorded samples' own), crouchTime seconds, transparencies 0..1, playerNames 1 aimed only / 2 always.
#define QC_SETTINGS_FIELDS(X) \
  X(INPUT,sensitivity,"sensitivity") X(INPUT,mYaw,"m_yaw") X(INPUT,mPitch,"m_pitch") X(INPUT,invertMouseX,"invertMouseX") \
  X(INPUT,invertMouseY,"invertMouseY") X(INPUT,deltaClamp,"") X(INPUT,tickInput,"tickInput") X(INPUT,fov,"fov") \
  X(INPUT,zoomFov,"zoomFov") X(INPUT,zoomSensitivity,"zoomSensitivity") X(INPUT,zoomToggle,"zoomToggle") \
  X(INPUT,zoomScaling,"zoomScaling") X(INPUT,zoomOutOnDeath,"zoomOutOnDeath") \
  X(SIM,autohop,"autohop") X(SIM,autoSwitch,"autoSwitch") X(SIM,startWeaponWeapons,"") X(SIM,startWeaponStrafe,"") X(SIM,startWeaponHook,"") \
  X(SOUND,lowAmmoSound,"lowAmmoSound") X(SOUND,lowAmmoPercentile,"lowAmmoPercentile") X(SOUND,checkpointSound,"") \
  X(CAMERA,crouchTime,"crouchTime") X(CAMERA,landBob,"landBob") X(CAMERA,walkBob,"walkBob") X(CAMERA,drawGun,"drawGun") \
  X(CAMERA,gunX,"gunX") X(CAMERA,gunY,"gunY") X(CAMERA,gunZ,"gunZ") \
  X(PEERS,playerSize,"playerSize") X(PEERS,playerTransparency,"playerTransparency") X(PEERS,drawPlayers,"drawPlayers") X(PEERS,playerNames,"playerNames") \
  X(GHOST,showGhost,"showGhost") X(GHOST,ghostColor,"") X(GHOST,ghostSize,"ghostSize") X(GHOST,ghostTransparency,"ghostTransparency") \
  X(GHOST,ghostPath,"ghostPath") X(GHOST,ghostPathOpacity,"ghostPathOpacity") X(GHOST,ghostPathWidth,"ghostPathWidth") X(GHOST,playerColor,"") \
  X(RENDER,gamma,"gamma") X(RENDER,contrast,"contrast") X(RENDER,picmip,"picmip") X(RENDER,renderScale,"renderScale") X(RENDER,antialias,"antialias") \
  X(RENDER,sortTranslucent,"sortTranslucent") X(RENDER,mirrors,"mirrors") X(RENDER,mapFx,"mapFx") X(RENDER,volumeFog,"volumeFog") \
  X(RENDER,fastSky,"fastSky") X(RENDER,marks,"marks") X(RENDER,markTime,"markTime") X(RENDER,spraysOn,"spraysOn") X(RENDER,showDickModels,"showDickModels") \
  X(AUDIO,volume,"volume") X(AUDIO,footstepVolume,"footstepVolume") X(AUDIO,ambient,"ambient") X(AUDIO,chatBeep,"chatBeep") \
  X(AUDIO,muteBackground,"muteBackground") X(AUDIO,funnySounds,"funnySounds") X(AUDIO,raceAnnouncements,"raceAnnouncements") \
  X(HUD,speedometer,"speedometer") X(HUD,cgazFull,"") X(HUD,conHeight,"conHeight") X(HUD,conOpacity,"conOpacity") X(HUD,conScale,"conScale") \
  X(HUD,minimap,"minimap") X(HUD,showFps,"showFps") X(HUD,jumpSpeed,"jumpSpeed") X(HUD,drawInputCmds,"drawInputCmds") X(HUD,confirmLeave,"confirmLeave")
#define QC_SETTINGS_MEMBER(group,name,key) double name;
typedef struct QcSettings {
  QC_SETTINGS_FIELDS(QC_SETTINGS_MEMBER)
  char voice[32], finishSound[32], wrSound[32];  // zero-terminated ids
} QcSettings;
#undef QC_SETTINGS_MEMBER
int qc_ses_settings(const char* json, int n);
const QcSettings* qc_ses_settings_values(void);  // valid for the library's life; the defaults before the first qc_ses_settings

// Camera inputs are in render/world axes. previous/current are feet. overridePos is feet for
// mode 1 (follow), eye for mode 2 (explicit camera). mode 0 previews pending local mouse.
// All times are seconds except weaponTimeMs. Set interpolation=1 when snapping to the current tick.
// Negative interpolation sources local feet, crouch, bob and weapon state from the native session.
// Once qc_ses_settings has run, crouchTime, walkBob, landBob and gunX/Y/Z come from the settings (the input's are ignored).
// drawGun stays the host's: the settings' drawGun, or 0 where the view shows no gun.
// mode: 0 local, 1 follow, 2 explicit view, 3 explicit view (gun hidden).
// weaponState: 0 ready, 1 firing, 2 dropping, 3 raising. fallSpeed: negative on a landing, zero otherwise.
typedef struct QcCameraInput {
  double previous[3], current[3], overridePos[3], overrideYaw, overridePitch;
  double interpolation, delta, crouchTime, walkBob, landBob, fallSpeed;
  double gunX, gunY, gunZ, weaponTimeMs, kick, lastFire;
  double mode, crouched, bobCycle, landedCrouched, landedWater, reset, drawGun;
  double weaponState, vql, customFire;
} QcCameraInput;
typedef struct QcCamera {
  double eye[3], yaw, pitch, quaternion[4], fovX, fovY, bob, kick;
  double gun[7];  // visible, camera-space position xyz, rotation xyz (Euler XYZ)
  uint32_t zoomed, inputFlags; // inputFlags bit 0: +chat held
} QcCamera;
int qc_ses_camera(double now, const QcCameraInput* in, QcCamera* out);  // after the second input pump; -1 for a missing buffer

enum QcMatchOp {
  QC_MATCH_QUERY=0, QC_MATCH_START=1, QC_MATCH_STOP=2, QC_MATCH_MODE=3,
  QC_MATCH_RELOAD=4, QC_MATCH_RESET=5, QC_MATCH_PAUSE=6, QC_MATCH_FREEZE=7,
  QC_MATCH_FOLLOW=8, QC_MATCH_LOADING=9, QC_MATCH_CHAT_APPEND=10,
  QC_MATCH_CHAT_VIEW=11, QC_MATCH_CLOCK=12, QC_MATCH_CHAT_RESET=13
};
enum QcMatchFlags {
  QC_MATCH_ACTIVE=1, QC_MATCH_PAUSED=2, QC_MATCH_FROZEN=4,
  QC_MATCH_FOLLOWING=8, QC_MATCH_LOADING_FLAG=16
};
enum QcMatchPause { QC_MATCH_MENU=1, QC_MATCH_HOLD=2, QC_MATCH_BOX=4 };
enum QcHudRunText {
  QC_HUD_READY=0, QC_HUD_RUNNING=1, QC_HUD_CHECKPOINT=2, QC_HUD_FINISHED=3,
  QC_HUD_REJECTED=4, QC_HUD_MISSED_CHECKPOINT=5, QC_HUD_DEATH=6
};
enum QcHudNotice { QC_HUD_PICKUP=1, QC_HUD_TARGET_PICKUP=2, QC_HUD_MESSAGE=3, QC_HUD_MEDKIT=4 };
enum QcHudChatView { QC_HUD_CHAT_FEED=0, QC_HUD_CHAT_HISTORY=1, QC_HUD_CHAT_OPEN=2, QC_HUD_CHAT_HIDDEN=3 };

// Host calls on the session thread. Times are monotonic seconds in the same domain as qc_ses_advance.
// in points to four doubles when supplied. START/RELOAD after a successful host map load: [now].
// STOP clears lifecycle holds. MODE [vql,strafe,hook] stages the next host load (returns 1 if reload needed).
// RESET [now] returns 1 if respawned, 0 during a race freeze. PAUSE [reason mask], FREEZE [on], FOLLOW [peer: -1 own, 0 free, >0 follow].
// LOADING [on,fraction] takes a UTF-8 label (<=255 bytes), -1 fraction is indeterminate.
// CHAT_APPEND [receivedAt] appends one host log line. CHAT_RESET clears the log and rewinds its indices to 0.
// CHAT_VIEW [QcHudChatView]. CLOCK [now] updates presentation time without ticks.
// QUERY returns QcMatchFlags. Other successful operations return 0, invalid arguments return -1.
int qc_ses_match(int op, const double* in, const char* text, int len);

// Caller-owned snapshot. Compare individual revisions for changed recipes and speed-board reports.
// Chat range is [chatFirst,chatEnd), zero-based absolute line indices (uint32 wrap); host retains corresponding rich text.
// Notice: BSP entity index or first target-pickup index in noticeId (noticeCount is the number granted).
// Speed-board fields are [start,top,end]; revisions advance on reports, not on every tick.
typedef struct QcHudFrame {
  uint32_t matchFlags, pauseReasons, mode, runText;
  int32_t follow, deathCause, checkpoint, noticeId;
  uint32_t runRevision, speedRevision, splitRevision, jumpRevision, noticeRevision, loadingRevision;
  uint32_t chatFirst, chatEnd, chatView, noticeKind, noticeCount, jumpCount;
  uint32_t boardRevision[3];
  int32_t startOrEnd; // 0 none, 1 start, 2 finish
  double now, elapsed, speed, split, speedAt, splitAt, noticeAt, loadFraction;
  double boardSpeed[3], jumps[8];
  char loadingText[256];
  uint32_t boardCarried; // bit per boardSpeed entry already carried by the finish message
  int32_t noticeEntities[39]; // target-pickup entity ids in event order, up to 38 and then -1
  QcHud numbers; // local-player live HUD values, in the existing qc_sim_hud layout
} QcHudFrame;
int qc_ses_hud(QcHudFrame* out);  // 0 on success, -1 for null. Raw values for the existing HUD recipes.

const double* qc_sim_out(void);          // the 50 doubles the last tick produced (position, velocity, crouched at 13, ...)
int qc_sim_weapon_held(void);
double qc_sim_spawn_yaw(void);
double qc_sim_spawn_pitch(void);

// Practice commands (version 6), called between ticks. Each leaves its sound cues and events in the lists, like a tick. The player's position and velocity need no call of their own: qc_sim_out() already
// gives them (0-2 position x y z, 3-5 velocity, 6 on ground, 13 crouched; refreshed by every call below); the view angles are the client's own.
int qc_sim_give_weapon(int weapon);   // !give / !hook: a practice weapon with unlimited ammo (weapon ids as in qc_sim_hud; 10 = the practice grappling hook). Taints the run. Nonzero when granted.
int qc_sim_give_item(const char* name, int len, double count, double capSeconds);   // an item by its classname ("item_quad", ...). count: seconds / quantity, 0 = the item's own. capSeconds: powerup timers end at most this long from now, 0 = no cap. Nonzero when given.
void qc_sim_set_stat(int id, double v);   // id 0 health, 1 armor, 2 flight fuel
void qc_sim_restore_position(double x, double y, double z, double vx, double vy, double vz, double yaw, double pitch);   // !loadpos: places the player (feet position, scene axes as in qc_sim_out; yaw / pitch in radians: the client takes them as its view angles), clears every trigger cooldown, taints, drops a running timer
void qc_sim_set_spawn(double x, double y, double z, double yaw, double pitch, int ground);   // !setspawn: where qc_sim_restart (and a death) puts the player from now on; a map load brings the map's own spawn back. qc_sim_spawn_yaw / pitch return the new angles.
void qc_sim_reset_spawn(void);   // !setspawn reset: the loaded map's own spawn again (from the next qc_sim_restart or death; the player is not moved now)
void qc_sim_remove_powerups(void);   // ends every running powerup (quad, haste, battle suit, regeneration, invisibility)
void qc_sim_restart(void);   // a respawn (kill, the restart key) on the loaded map without loading it again: keeps the spawn set by qc_sim_set_spawn, ends noclip. Take the view angles from qc_sim_spawn_yaw / pitch afterwards.
void qc_sim_noclip(int on);   // explore mode (!nc). While on, qc_sim_tick flies the player (keys of masks16[15]: forward / back / strafe along the view, jump up, crouch down, walk = slow) instead of running the sim: no collision, no triggers, no clock. Turning it on taints the run and drops a running timer; a map load or qc_sim_restart turns it off.
int qc_sim_noclip_on(void);

// Mode and run state (version 7).
void qc_sim_set_mode(int strafe, int hook);   // the weapon mode / hook mode of the NEXT qc_sim_load_map / qc_sim_load_runway (strafe 1 = !s weapon set, 0 = !w; hook 1 = hook-strafe: strafe + hook). Cleared by that load: call it before every load, also a reload for a mode change. Physics (vql) stays the load's own argument.
// The commands that act on the game, with their reset rules (!nc, !give, !savepos / !loadpos, last checkpoint, !setspawn, !goto, !cgaz, practice taint, new life, map change). The page decides op and arguments
// (native-core/practice.hpp PR_*); the host passes them through unchanged: in16 = 16 doubles, text = up to 383 bytes (fields split by 0x1f). Returns PRR_* bits (1 the sim changed: cues and events were added; 2 the saved position / spawn changed).
// Then hand back to the page: the 31 doubles of qc_sim_command_state(), the reply lines (qc_sim_command_reply: 'T' chat / 'S' status ... separated by '\n', not zero terminated) and qc_sim_command_source() (zero terminated).
// Op 16 (PR_FREEZE, additive): in16[0] 1 = the player is frozen (a race countdown): qc_sim_tick and qc_sim_restart do nothing until op 16 with 0, or a map load. Bit 256 of qc_sim_command_state()[0] says it is on. Returns 0, no reply lines.
int qc_sim_command(int op,const double* in16,const char* text,int len);
const double* qc_sim_command_state(void);
const char* qc_sim_command_reply(int* len);
const char* qc_sim_command_source(void);
int qc_sim_tainted(void);   // 1 when the run is tainted (practice weapon, noclip, loadpos, qc_sim_set_tainted(1), ...). The core's flag is the one truth: a death or qc_sim_restart does NOT clear it, the host does with qc_sim_set_tainted.
void qc_sim_set_tainted(int on);   // the host's taint (the page tells the client: the CGaz HUD, a new life that starts clean or tainted)
int qc_sim_run_state(void);   // 0 ready (idle), 1 running, 2 finished
void qc_sim_drop_run(void);   // version 8. A practice aid went on mid-run (!showtriggers): a RUNNING timer is dropped at once (run state back to ready, checkpoints cleared), the player is not moved; nothing happens when no run is running. The taint is separate (qc_sim_set_tainted).

// The run recorder (additive within version 11; research/NATIVE-RUN-RESULT-DESIGN.md). The core records every run itself: ONE capture runs from a reset (map load, restart, death) to the next, holding the packed input log (v4 of
// input-log-pack.js: 1 version byte, per tick yaw f32, pitch f32, a flags byte = selected weapon | key-event count (max 3, 3 = a count byte follows) << 4 | use-item 0x40 | autohop-off 0x80, then the key events slice << 3 | key). The log
// is what a leaderboard submission carries; the server replays it. Nothing is allocated per tick; the buffers are the core's (valid until the next tick / reset / load).
void qc_sim_set_autohop(int on);   // the tick's autohop (cg_autohop), default 1. Recorded per tick in the log (flags bit 0x80 = off), so a run played with autohop off replays as played.
void qc_run_set_appearance(int style, int color);   // the player's look in the ghost samples (rocket style code, colour 0xRRGGBB). Unset: style 2, colour 0x80e8d1.
int qc_run_ticks(void);   // rows recorded in this capture (it stops at 300000 = 40 minutes: the capture is then cut)
const uint8_t* qc_run_log(int* len);   // the capture so far as a v4 packed log (the version byte first; len = its bytes, 1 for an empty capture)
// The record of a finished run, frozen when an UNTAINTED run finishes (never for a tainted finish, a practice weapon, or fewer than 2 rows). A newer finish replaces it; a reset or qc_run_result_release clears it.
typedef struct QcRunResult {
  int32_t serial;                  // +1 per recorded finish (a retriggerable start can finish twice in one capture)
  int32_t ticks;                   // rows up to and including the finish tick (the log prefix to submit)
  int32_t cut;                     // 1 when the capture hit the 40 minute cap before this finish
  int32_t checkpointCount, ghostCount, ghostStride, logBytes;   // ghostStride = 14
  int32_t vql, strafe, hook, spawnGround, hasStats;
  double elapsed;                  // seconds, the core's run clock at the finish (the server's replay time is the authority)
  double stats[4];                 // start, top, average, end speed (horizontal, units per second)
  double spawn[5];                 // x y z yaw pitch of the attempt's spawn (scene axes, radians)
  double moverTime;                // the rotator / bobber / train clock at the restart, < 0 = the map has none
  char map[64];                    // the loaded map's name ("runway" for the built-in one)
} QcRunResult;   // 200 bytes
int qc_run_result(QcRunResult* out);   // 1 and fills out when a record is held, else 0
int qc_run_capture(QcRunResult* out);   // /record: the CURRENT capture (reset to now) in the same struct (serial 0, checkpointCount / ghostCount 0, elapsed 0, logBytes = qc_run_log's length); 1 when it holds at least 2 rows, else 0. Send it like a record (qrazyNativeRun, capture: true).
int qc_run_result_log(const uint8_t** p);   // the record's log prefix: sets *p (when not NULL) and returns logBytes, 0 when no record is held
int qc_run_result_checkpoints(double* out, int capacity);   // the checkpoint times in run seconds, in order: copies them and returns the count, -1 when capacity is too small, 0 when none / no record
int qc_run_result_ghost(const float** p);   // sets *p (when not NULL) to ghostCount * 14 float32 samples and returns ghostCount: t x y z yaw pitch crouched vx vy vz weapon style color teleportCounter+1 (ghost-track-codec.js live-frame layout), every 4th run tick plus the last frame
void qc_run_result_release(int serial);   // drops the record when its serial matches

// Movers as the render core's pose records (64 bytes each, indexed by BSP model index; identity for a model no mover owns).
int qc_sim_pose_count(void);
int qc_sim_poses(uint8_t* out, int capacityBytes);   // fills out and returns the pose count, or -1 when capacityBytes is too small

int qc_sim_item_count(void);
int qc_sim_item(int index, QcItem* out);   // item spot `index` (the id the sim's item-visibility events carry); 0 = no such spot

void qc_sim_hud(QcHud* out);   // call after a tick. Version 3 adds the weapon state: weaponInHand = the weapon id drawn now, selected = the weapon the player wants (a switch is under way when they differ), weaponState 0 ready / 1 firing / 2 dropping / 3 raising, cooldownMs = milliseconds until the weapon can fire or the switch finishes. Weapon ids are the game's (game-rules.js WP_*: 1 gauntlet, 2 machinegun, 3 shotgun, 4 grenade launcher, 5 rocket, 7 railgun, 8 plasma, 9 BFG, 10 grappling hook). barAmmo / barOwned are indexed by the HUD weapon bar slot, as before.

// Weapon and world events of the ticks since the last qc_sim_events_clear. The sim fires, flies and detonates the weapons inside qc_sim_tick: the attack key is bit 128 of masks16[15], the wanted weapon is wantWeapon.
// Axes are the sim's. dir is a unit vector (SHOT: the aim; IMPACT / EXPLOSION: the surface normal; 0 when there is none). vel is units per second. surface is the Q3 surface flag word of what was hit (SURF_NOIMPACT 0x10, SURF_NOMARKS 0x20,
// SURF_NODRAW 0x80, SURF_METALSTEPS 0x1000, SURF_FLESH 0x40000 ...): the mark / spark choice is the host's. Unused fields are 0.
enum { QC_EV_SHOT = 1, QC_EV_IMPACT = 2, QC_EV_PROJECTILE_SPAWN = 3, QC_EV_PROJECTILE_UPDATE = 4, QC_EV_PROJECTILE_BOUNCE = 5, QC_EV_PROJECTILE_END = 6, QC_EV_EXPLOSION = 7, QC_EV_BEAM = 8,
       QC_EV_SWITCH = 9, QC_EV_HOOK_ATTACH = 10, QC_EV_HOOK_UPDATE = 11, QC_EV_HOOK_END = 12, QC_EV_PICKUP = 13, QC_EV_WEAPON_PICKUP = 14, QC_EV_DEATH = 15, QC_EV_PAIN = 16,
       QC_EV_RUN_START = 17, QC_EV_CHECKPOINT = 18, QC_EV_RUN_FINISH = 19, QC_EV_JUMP = 20,
       QC_EV_RUN_RESULT = 21, QC_EV_RESET = 22, QC_EV_RUN_ABORT = 23, QC_EV_FINISH_REJECTED = 24,
       QC_EV_FIRE = 25, QC_EV_MAP_SHOOTER = 26 };
enum { QC_FIRE_CROUCHED = 1, QC_FIRE_QUAD = 2, QC_FIRE_CHEAT_HOOK = 4, QC_MAP_SHOOTER_AIMED = 1 };
// FIRE: one per successful local weapon fire, immediately after the legacy SHOT (which retains its eye
// and aim vector). weapon = weapon ID, pos = feet, dir[0/1] = exact yaw/pitch in radians, surface =
// QC_FIRE_* flags sampled at fire time. CHEAT_HOOK applies only to a grapple fired in cheat-hook mode.
// MAP_SHOOTER: one per actual hostile missile launch, after its PROJECTILE_SPAWN. id = zero-based BSP
// entity index, weapon = missile weapon ID, surface = QC_MAP_SHOOTER_AIMED for a _targetplayer shooter.
// With AIMED set, to is the exact target in sim axes (activator's feet plus 24 on Y). Otherwise to is zero.
// RUN_RESULT (the run recorder, additive within version 11): a finished-run record is ready (qc_run_result): id = its serial, radius = the run clock in seconds, weapon = the rows (ticks) of the log prefix. Comes after the RUN_FINISH of the same tick.
// RESET: a new capture began (the page clears its per-run state): id 0 = map load, 1 = restart (qc_sim_restart / the restart key), 2 = death, 3 = out of the world (a fall). dir[0] / dir[1] = the view angles of the new life.
// RUN_ABORT: a started untainted run ended without a finish (a restart, a death, !loadpos, noclip): pos = (start, top, average) speed in units per second, vel[0] = end speed, radius = the run clock in seconds when it ended.
// FINISH_REJECTED: the run reached the finish but was tainted or used a practice weapon, so nothing finished: id 0 = the finish line / flag, 1 = a stop-timer target. The page says "<taint source> WAS USED THIS RUN".
// RUN_START / CHECKPOINT / RUN_FINISH / JUMP (version 4.1): pos and vel = the player's position and velocity at that moment, radius = the run clock in SECONDS (JUMP: 0). dir[0] / dir[1] = the player's yaw / pitch in radians at that moment (version 7: what !loadpos from the last checkpoint needs). CHECKPOINT: id = 1 for a flag, 0 for a target. JUMP: one per tick that performed a jump (the take-off speed is hypot(vel[0], vel[2])).
typedef struct QcEvent {
  int32_t type, weapon;
  double pos[3];    // SHOT: the eye; IMPACT / EXPLOSION: the point; PROJECTILE_*: the position; BEAM: where it starts (the eye); HOOK_*: the hook head; PICKUP: the item entity's position when known
  double dir[3];    // see above
  double to[3];     // BEAM: where it ends (the rail's hit point or the end of its range); HOOK_UPDATE: the eye
  double vel[3];    // PROJECTILE_SPAWN / UPDATE: velocity
  int32_t id, surface;   // PROJECTILE_*: the projectile number (restarts at a reset); IMPACT: surface = the flag word, id = the hit mover (0 when none); SWITCH: id = the weapon switched from; HOOK_UPDATE: id = 1 when attached; PICKUP: id = the item entity index; DEATH: id = 0 died 1 killed 2 fell 3 drowned; PAIN: id = health after
  double radius;    // PROJECTILE_SPAWN / EXPLOSION: the splash radius
} QcEvent;   // 120 bytes: type 0, weapon 4, pos 8, dir 32, to 56, vel 80, id 104, surface 108, radius 112
// The CGaz strafe HUD's numbers for the last tick: the same 136 bytes as text_api.h TextCgazState (memcpy into it), with the fields the host owns left 0 (yaw, fovX, ypos, fbWidth, fbHeight, flags; the rest is the sim's): set yaw (degrees, the view's yaw
// of THIS frame), fovX (degrees), ypos (the HUD placement, -1 default), fbWidth / fbHeight and flags (1 hidden, 2 full style) before text_hud_cgaz. Returns 1 when the last tick produced a snapshot, 0 when not (draw nothing).
typedef struct QcCgaz { double yaw, vx, vz, vPrev, movementDir, fovX, accelWidth, ypos, wish, accel, opposingAccel, friction, speed, preSpeed; uint32_t fbWidth, fbHeight, flags, keysHeld, supported, pad0; } QcCgaz;   // 136 bytes
int qc_sim_cgaz(QcCgaz* out);

// The minimap trail: the player's path since the last reset, kept by the sim (a ring of 4096 points: one every 32 units of travel, a NaN pair where the pen lifts after a teleport / jump over 600 units; cleared by a reset or a map load).
// Copies the newest min(count, maxPoints) points, OLDEST FIRST, as x, z float pairs (scene axes, like qc_sim_out) into out_xz (2 * maxPoints floats) and returns that count. Pass it to text_hud_minimap as trail_xz with trailCount.
int qc_sim_trail(float* out_xz, int maxPoints);

int qc_sim_event_count(void);
int qc_sim_event(int index, QcEvent* out);   // 1 and fills out, 0 for no such event
void qc_sim_events_clear(void);

int qc_sim_cue_count(void);   // the sound cues since the last qc_sim_cues_clear (every tick and the map load add theirs)
int qc_sim_cue(int index, QcCue* out);
void qc_sim_cues_clear(void);
void qc_sim_set_voice(const char* character, int len);
// Player settings the sim applies itself (additive within version 11). Each is kept across map loads; call them when the settings arrive.
// Sound: cg_lowAmmoWarningSound (0 off, 1 the reload cue, 2 the click), cg_lowAmmoPercentile (0..1 of 200 ammo), cg_checkpointSound (0 synth, 1 s_health, 2 n_health, 3 menu3, 4 off: no cue at the start / checkpoints). Defaults 0, 0.2, 0. Other players' shot cues use them too.
void qc_sim_set_sound_options(int lowAmmoMode, double lowAmmoPercentile, int checkpointChoice);
// cg_autoswitch (default 0): a picked-up weapon (not the machinegun) is selected on the next qc_sim_tick whose wantWeapon is 0. The host selects nothing itself on QC_EV_WEAPON_PICKUP.
void qc_sim_set_autoswitch(int on);
// cg_preferredStartingWeapons: the weapon id to hold at every spawn (map load, restart, death) in the weapon, strafe and hook modes (0 = none; ignored when the spawn inventory lacks it). Takes effect at the next spawn.
void qc_sim_set_start_weapons(int weapons, int strafe, int hook);

// Dry shot queries (version 9): what the game shows of OTHER players' shots (multiplayer.js peerFired) and of recordings, traced against the loaded map. None of it simulates a player or changes the world (a shot can hit a door, never open it).
// Axes = the sim's scene axes (as qc_sim_out: x y z, Y UP), yaw / pitch in radians as qc_sim_tick takes them, units = game units, times in seconds on the caller's own clock (a flight's sample k is at start + k * QC_DRY_DT). mode: 0 = PQL, 1 = VQL (the shooter's physics).
// No heap crosses the boundary: the caller passes the buffers and their capacities. A call returns -1 for "no map loaded" and -2 for "a buffer is too small" (the header is still filled, so the caller can size and call again), else its result count.
// MOVERS: the shots are traced against the live sim's mover state (doors, platforms, rotators) by default. A host that shows a DIFFERENT world (a spectated player's doors, a demo's recorded movers) passes its state with qc_dry_set_movers; count 0 goes back to the live sim.
#define QC_DRY_DT 0.008            // the flight's step (SV_FRAME)
#define QC_DRY_MAX_SHOTS 20        // hitscan: the shotgun's pellets are the most (1 for machinegun / railgun)
#define QC_DRY_MAX_SAMPLES 4096    // a flight's samples: a buffer of 3 * QC_DRY_MAX_SAMPLES doubles always suffices (the longest life is 20 s = 2500 steps)
#define QC_DRY_MAX_BOUNCES 256     // a flight's bounce records (grenades): an int32 buffer of this size always suffices
#define QC_DRY_MOVER_VALUES 5      // per mover: offset x y z, angle, health
// One traced shot. fraction 1 = it reached the end of its range (nothing hit); end = the end / hit point, normal = the surface normal. flags = the hit surface's raw BSP surface flags (SURF_NOIMPACT 0x10, SURF_NOMARKS 0x20, SURF_NODRAW 0x80, SURF_METALSTEPS 0x1000, SURF_FLESH 0x40000 ...). mover = the index of the hit mover (-1 none). startsolid / allsolid: the shot started inside a solid.
typedef struct QcShot { double fraction; double end[3]; double normal[3]; int32_t flags, mover, startsolid, allsolid; } QcShot;   // 72 bytes: fraction 0, end 8, normal 32, flags 56, mover 60, startsolid 64, allsolid 68
// A missile's flight (rocket / grenade / plasma); its samples are returned separately, 3 doubles (x y z) each. endType 0 = still flying when its life ended, 1 = vanished (no explosion), 2 = explodes at endOrigin. endTime = when it ends (the caller's clock); endNormal is valid when hasNormal.
// base / delta = the missile as it was launched (its start position, its velocity in units per second): where to play the launch sound / which way the model points. The bounce indices (into the samples) are the samples at which a grenade bounced.
typedef struct QcFlight { int32_t sampleCount, endType, bounceCount, hasNormal; double endTime, endOrigin[3], endNormal[3], base[3], delta[3]; } QcFlight;   // 120 bytes: counts 0, endTime 16, endOrigin 24, endNormal 48, base 72, delta 96
typedef struct QcGrapple { double pos[3], velocity[3], born, lastTime; } QcGrapple;   // another player's hook as fired: pos = its muzzle, velocity in units per second (a straight line), born = now, lastTime = now - 0.05
int qc_dry_mover_count(void);   // the loaded map's mover count (0 = no map)
int qc_dry_set_movers(const double* movers, int count);   // movers = count * QC_DRY_MOVER_VALUES doubles (the sim's mover order); the dry queries after it trace against them. count must equal qc_dry_mover_count, else nothing changes (returns 0); count 0 = the live sim's again (returns 1).
// Another player's machinegun (2) / shotgun (3) / railgun (7) from eye (feet y + 50, or + 36 crouched). Fills out with up to capacity shots (QC_DRY_MAX_SHOTS always suffices) and returns the count: 20 for the shotgun, 1 for machinegun / railgun, 0 for any other weapon.
int qc_dry_hitscan(int weapon, const double eye[3], double yaw, double pitch, QcShot* out, int capacity);
// A missile fired by another player: weapon 4 grenade launcher, 5 rocket, 8 plasma. feet = the shooter's feet, crouched 0/1. fireNow = when it was fired and start = when the first trace happens, both on the caller's clock (the browser uses fireNow = now - 0.008, start = now).
// samples: sampleCapacity * 3 doubles; bounces: bounceCapacity int32. Returns the sample count (= head->sampleCount), -1 no map, -2 a buffer is too small, -3 not a missile weapon (qc_dry_flight_state too).
int qc_dry_flight_fire(int weapon, const double feet[3], int crouched, double yaw, double pitch, double fireNow, double start, int mode, QcFlight* head, double* samples, int sampleCapacity, int32_t* bounces, int bounceCapacity);
// A map shooter (shooter_rocket / _grenade / _plasma; entity = its BSP entity index) a watched player triggered. hasTarget 1 = aimed at target (a _targetplayer shooter: the activator's origin). random1 / random2 = the two [0,1) draws its spread takes. Same result form; the weapon follows the entity's classname (the client knows it). -3: no such entity.
int qc_dry_flight_shooter(int entity, int hasTarget, const double target[3], double fireNow, double start, int mode, double random1, double random2, QcFlight* head, double* samples, int sampleCapacity, int32_t* bounces, int bounceCapacity);
// A missile already flying when a recording began: weapon, base / pos / delta (units, units per second), time, born (on the flight's own clock), gravity / stopped / hostile 0/1, radius (splash), start. Same result form.
int qc_dry_flight_state(int weapon, const double base[3], const double pos[3], const double delta[3], double time, double born, int gravity, int stopped, int hostile, double radius, double start, QcFlight* head, double* samples, int sampleCapacity, int32_t* bounces, int bounceCapacity);
// Another player's grappling hook (weapon 10) as fired: it flies in a straight line at the returned velocity until the peer's own hook message says where it stuck (the sim's attach rule is not run here). scale = 1 (or the hitscan hook's speed scale of a cheat-hook peer). Returns 1, or -1 no map.
int qc_dry_grapple_fire(const double feet[3], int crouched, double yaw, double pitch, double now, double scale, QcGrapple* out);
// PLAYER SHADOWS (additive within version 11): the ground shadow of one player (the own player, every other player, the ghost), as the browser draws it. Call once per shown player per frame; allocation-free.
// feet = the player's feet position (scene axes, as qc_sim_out); yaw = where it looks (radians, only used with an accessory); accScaleX / accScaleZ = the accessory part's scale, 0 0 = plain circle.
// The ground is box-traced against the dry world (movers as above). visible 0 = draw nothing. Otherwise draw a 1 x 1 plane (x, y in -0.5..0.5, uv 0..1) at position, rotated by quat (xyzw), scaled (size, size, 1), with the render core's
// EFFECT_SHADOW program (rc_variant_build, effect family): uniforms fade, ext, R, y0, y1, rw from this record; transparent, depth test on, depth write off, double sided. Returns visible, -1 no map, -2 null argument.
typedef struct QcShadow { int32_t visible, pad; double position[3], quat[4], size, fade, ext, R, y0, y1, rw; } QcShadow;   // 120 bytes: visible 0, position 8, quat 32, size 64, fade 72, ext 80, R 88, y0 96, y1 104, rw 112
int qc_dry_player_shadow(const double feet[3], double yaw, double accScaleX, double accScaleZ, QcShadow* out);
// The SOUNDS of what the host shows of other players / recordings (stateless; the browser's soundDryCues). Writes the cues of `kind` for `args` (n doubles, at most 16 read, missing ones 0) into out; returns their count,
// -1 cap too small (QC_DRY_MAX_CUES always suffices), -2 bad arguments, -3 unknown kind. QcCue.flags POS 1 = pos is a world position (without it: heard at the listener); LOOP_START 2 / LOOP_MOVE 8 / LOOP_STOP 4 with emitter = the loop's id.
// kind 0 SHOT: args = weapon, quad 0/1, x, y, z (muzzle), atListener 1 (heard at the listener, no pos). weapon 1 gauntlet 2 MG 3 SG 4 GL 5 RL 6 LG 7 RG 8 PG 9 BFG 10 grapple; the fire sound, plus the quad sound when quad (not for the grapple).
// kind 1 EXPLOSION: args = weapon, x, y, z (a missile's endOrigin; weapons 4 / 5 / 8, others give 0 cues).
// kind 2 GRENADE BOUNCE: args = x, y, z (a bounce sample of a flight).
// kind 3 HITSCAN IMPACT: args = weapon, fraction, flags, x, y, z (a QcShot's fraction, flags, end); 0 cues when it has no impact sound.
// kind 5 MISSILE HUM (loop): args = weapon, x, y, z, emitter (an id unique to the missile; the same id moves / stops it); 0 cues for a weapon that does not hum.
// Others (soundDry in native-core/sound.hpp): 4 weapon switch (no args), 7 held-weapon loops (weapon, dropping, attackHeld), 8 flight swoosh (thrust), 9 target_speaker (entity index), 10 checkpoint cue (setting), 11 named effect (fx, hasPos, x, y, z), 12 hook pull loop (attached).
#define QC_DRY_MAX_CUES 768
int qc_dry_cues(int kind, const double* args, int n, QcCue* out, int cap);

// The hub wire (version 11): the position messages between the client and the hub; one implementation for the hub, the page and the client (native-core/wire.hpp). Stateless.
// state (client -> hub, 26 B; 25 B accepted): 1, x y z (float32), yaw (int16, radians*10000, wrapped to +-PI), pitch (int16, radians*10000), vx vy vz (int16 ups), crouch (byte, low 2 bits), weapon (byte), teleport (byte).
// peer (hub -> client, 17 B; 16 B accepted): 2, player number, x y z, yaw, teleport. peer full (27 B; 26 B accepted): 3, player number, x y z, yaw, pitch, vx vy vz, crouch, weapon, teleport.
// teleport byte: the sender's teleport counter as 1-255 (wrapping), 0 = unknown (also for the shorter legacy lengths).
// kind 1 state / 2 peer / 3 peer full (decode result); number 0 for kind 1; full 1 for kind 3. Angles in radians, velocities in units per second. crouchBits / weapon / teleport are bytes (encode takes the low 2 / 8 / 8 bits). reserved: 0.
typedef struct QcWireState { int32_t kind, number, full, teleport; double x, y, z, yaw, pitch, vx, vy, vz; int32_t crouchBits, weapon; int32_t reserved[2]; } QcWireState;   // 96 bytes: kind 0, number 4, full 8, teleport 12, x 16, y 24, z 32, yaw 40, pitch 48, vx 56, vy 64, vz 72, crouchBits 80, weapon 84, reserved 88
// 1 ok / 0 invalid (out is zeroed): a type or length the format never had, or a coordinate that is NaN / infinite. Fields a frame does not carry read 0.
int qc_wire_decode(const uint8_t* p, size_t n, QcWireState* out);
// A state frame (26 bytes) from s (kind / number / full ignored). yaw is wrapped to +-PI, angles and velocities rounded and clamped to int16. Returns 26, or 0 when a number of s is not finite.
int qc_wire_encode_state(const QcWireState* s, uint8_t out26[26]);
// The hub's peer frame for player `number` (1-255) from s: 17 bytes, or 27 bytes with full (look, velocity, crouch, weapon). Returns the length, 0 when cap is too small.
int qc_wire_encode_peer(const QcWireState* s, int number, int full, uint8_t* out, size_t cap);

// The peer table (version 11, native-core/peers.hpp; research/PEERS-IN-CORE-DESIGN.md): other players' avatars computed by the core. ONE table per library instance (the sim half's single instance). The host feeds the hub's messages and frames with ITS
// monotonic clock `now` (seconds; the core never reads a clock) and applies the node records qc_peer_frame writes. No allocation after the static table (16 slots, a 32-sample ring each); every record below is flat, 64-bit safe, 8-byte aligned.
// Hub messages are the unchanged wire (the JSON of mp-server.js, the bytes above): a client on an older library keeps its own copies until it adopts these calls.
#define QC_PEER_MAX 16   // the hub's MAX_PLAYERS; a join beyond it is ignored
// qc_peer_text event bits (the low 8 bits of its result; bits 8..15 = slot + 1, 0 = no single slot)
#define QC_PE_JOINED 1
#define QC_PE_LEFT 2
#define QC_PE_LOADED 4
#define QC_PE_RENAMED 8
#define QC_PE_PLATE_CHANGED 16   // what the plate shows (name / second line) changed: bake it again (QcPeerInfo.plateKey is the new key)
#define QC_PE_SPEC 32
#define QC_PE_WELCOME 64
#define QC_PE_CHANGED 128        // another field of QcPeerInfo changed (colour, lag, phys, guest, paused, number)
#define QC_PE_WATCHED 0x10000    // the hub said whether someone spectates us ("watched", no slot); QC_PE_WATCHED_ON: they do. The core already started / stopped collecting for qc_watch_out
#define QC_PE_WATCHED_ON 0x20000
// QcPeerInfo.flags
#define QC_PF_LOADING 1
#define QC_PF_SPEC 2
#define QC_PF_LAG 4
#define QC_PF_GUEST 8
#define QC_PF_PAUSED 16
// QcPeerNode.part
#define QC_PART_BODY 0       // variant 0; the model record kind 13, tinted
#define QC_PART_GUN 1        // variant = the weapon number (1..10): show that gun's mesh (model kind 14, meshes named by weapon number), hide the others
#define QC_PART_ACCESSORY 2  // variant = tier + 1 (0 = none): rc_model_avatar_surface, one material per tier; flags VISIBLE only when the tier has a length
#define QC_PART_PLATE 3      // variant = key = the plate image's key (QcPeerInfo.plateKey): a camera-facing sprite, quat = the camera's, scale = the world size in x / y (z 1)
// QcPeerNode.flags
#define QC_NF_VISIBLE 1
#define QC_NF_TELEPORTED 2   // body: the segment being drawn contains a teleport (the avatar jumps instead of sliding)
#define QC_NF_BILLBOARD 4    // plate: quat already faces the camera
#define QC_NF_TRANSPARENT 8  // the avatar transparency setting is above 0 (tint alpha carries it)
// QcPartSpec.flags
#define QC_PS_TRANSPARENT 1
#define QC_PS_TWO_SIDED 2
#define QC_PS_NO_DEPTH_WRITE 4
#define QC_PS_SINGLE_PASS 8
#define QC_PS_MATERIAL_FROM_RECORD 16   // the material is the model record's own
#define QC_PS_SPRITE 32

// One drawable part of one peer, in WORLD space (Y up, feet position): the final transform, nothing to compose. 128 bytes.
//   pos / quat (x y z w, includes the model's +pi turn, the gun's pitch, the plate's camera facing) / scale (world, x y z); tint = linear rgb (body: the player colour; others 1) + alpha (1 - the transparency setting; plates 1); key = player number (plates: the plate key).
typedef struct QcPeerNode { int32_t slot, part, variant, flags; double pos[3], quat[4], scale[3]; float tint[4]; uint32_t key; int32_t reserved[3]; } QcPeerNode;   // 128 bytes: slot 0, part 4, variant 8, flags 12, pos 16, quat 40, scale 72, tint 96, key 112, reserved 116
// The drawn pose of one peer (what the spectator camera, the HUD and the minimap read): yaw the short way round, the same moment as the nodes. flags: 1 visible, 2 teleport in the segment, 4 loading, 8 spectator.
typedef struct QcPeerPose { int32_t number, slot, flags, crouched, weapon, zoomed; double pos[3]; double yaw, pitch; double vel[3]; double delay, stamp; double reserved[2]; } QcPeerPose;   // 120 bytes: pos 24, yaw 48, pitch 56, vel 64, delay 88 (the interpolation delay now, seconds), stamp 96 (the time the newest sample was filed under), reserved 104: reserved[0] = the host-clock time (qc_peer_fx_json's `now`) of this player's last shot, -1e9 = none; reserved[1] = that shot's weapon mode (0 pql, 1 vql), 0 when none (the gun kick / firing state of a followed player)
// A roster entry: what the plate and the player list need. flags QC_PF_*; tier = accessory tier + 1 (0 none); phys 0 none / 1 pql / 2 vql; wm = 0 or the character 's' / 'w' / 'h'; id = the hub's player id; name = the shown name (UTF-8, colour codes and clan separators intact);
// color '#rrggbb'; subline = the plate's second line (physics + weapon mode, the leaderboard profile label, or the lag warning), UTF-8.
typedef struct QcPeerInfo { int32_t number, slot, flags, tier; uint32_t plateKey; int32_t phys, wm, reserved; char id[40]; char name[96]; char color[8]; char subline[64]; } QcPeerInfo;   // 240 bytes: id 32, name 72, color 168, subline 176
// From the settings, cheap enough for every frame. playerSize (cg_playerSize), transparency 0..1 (the avatar transparency setting), drawPlayers 0/1, routeOnly 0/1 (a route viewer on another map: nobody is drawn), followedNumber (the spectated player's number, 0 none: not drawn),
// plateMode 1 (a name shows only while aimed at) or 2 (always). Defaults after a reset: 1, 0.6, 1, 0, 0, 2.
typedef struct QcPeerOptions { double playerSize, transparency; int32_t drawPlayers, routeOnly, followedNumber, plateMode; double reserved[4]; } QcPeerOptions;   // 64 bytes
// The view the plates are sized and oriented for: eye, the unit forward vector, the camera's quaternion (x y z w).
typedef struct QcPeerCamera { double eye[3], forward[3], quat[4]; } QcPeerCamera;   // 80 bytes
// What a host creates each part from; localPos / localScale = the part's place inside the avatar's root (informational: the node records are final). modelKind: 13 the avatar body, 14 the guns (rc_model_build kinds), -1 the accessory surface (rc_model_avatar_surface), 0 none (the plate image).
typedef struct QcPartSpec { int32_t part, variant, modelKind, renderOrder, flags, reserved0; double localPos[3], localScale[3]; int32_t reserved[4]; } QcPartSpec;   // 88 bytes; accessory with variant >= 1: reserved[0] = the tier's colour 0xRRGGBB
// qc_track_point result: sample k and the mix 0..1 toward k + 1 (0 across a teleport; tele 1 then).
typedef struct QcTrackPoint { int32_t k, tele; double mix; } QcTrackPoint;   // 16 bytes

// A new connection (or the host dropped it): every peer, our own number, the early-loaded ids and the send window are cleared. The options stay.
void qc_peer_reset(void);
// A map change: every peer is `loading` with no samples (nothing of the old map is drawn) until a fresh frame and `loaded`.
void qc_peer_clear_samples(void);
// Our own player number (from the welcome, which qc_peer_text also reads). Frames of that number are never drawn.
void qc_peer_set_self(int number);
// A hub text message: welcome, join, leave, loaded, appearance, rename, lag, spec, phys, guest, paused (the rest is ignored). Returns QC_PE_* bits | (slot + 1) << 8, or 0 for an ignored / unknown / invalid message. n <= 65536.
int qc_peer_text(const char* json, size_t n, double now);
// Session multiplayer. One connection per core instance, calls serialized on the session owner.
// now is monotonic seconds in both calls. Drop buffers from a previous connection before reset.
enum { QC_NET_PAGE=1, QC_NET_WATCHED=2, QC_NET_WATCHED_ON=4, QC_NET_PUMP=8,
       QC_NET_FOLLOW_SOUND=16, QC_NET_FOLLOW_STATE=32, QC_NET_OUTPUT_CAP=81940 };
// One UTF-8 text (binary=0) or binary (binary=1) WebSocket payload. Over 65536 bytes,
// text goes only to PAGE (e.g. sprays), binary is ignored.
// Returns PAGE for page/executor delivery of the unchanged text, and WATCHED/ON on watch changes.
// Binary acceptance returns (slot+1)<<8. PUMP requests an immediate drain for a ping reply.
// FOLLOW_SOUND and FOLLOW_STATE label text for the followed-player presentation executor.
// Ignored/invalid returns 0. -2: probe reply queue full, drain and retry.
int qc_ses_net_in(int binary,const uint8_t* p,int n,double now);
// Whole frames: uint32 little-endian payload length with the high bit set for binary, then payload
// without padding or a terminator. Returns bytes written, 0 idle, -1 invalid args/clock, or minus
// the next frame's total size if none fits. Positive partial drains keep the unread suffix.
// Host owns all returned bytes and retries transport with those bytes. Drain only when sendable.
int qc_ses_net_out(double now,uint8_t* out,int cap);
// Sample a pose (teleport is the unwrapped sim counter, -1 unknown). No encoding until net_out.
int qc_ses_net_state(const QcWireState* state);
enum { QC_NET_RESET, QC_NET_LOADED, QC_NET_PAUSE, QC_NET_PHYS, QC_NET_SPEC, QC_NET_FOLLOW,
       QC_NET_SPEED, QC_NET_WATCH_VIEW, QC_NET_FORGET_MODE, QC_NET_FIRE, QC_NET_HOOK, QC_NET_SHOT, QC_NET_DROP_POSE };
// RESET clears pending output, clock, state window and watch feed. Also reset the peer table
// and FX with the existing peer reset calls on a new connection. LOADED resets map-scoped send
// state. LOADED text = a map bytes + optional guest PB JSON object, no delimiter.
// PAUSE/SPEC take boolean a. PHYS takes a=1 PQL/2 VQL, b='s'/'w'/'h', text=profile ID.
// FOLLOW takes the target ID (empty clears). SPEED takes a=(phys-1)*3+modeIndex(s=0,w=1,h=2),
// b=0 start/1 top/2 end plus bit 4 if carried by finish, value=ups. WATCH_VIEW takes a=eligible,
// b=watch pause. FORGET_MODE forces the next mode advertisement. DROP_POSE stops pose heartbeats
// and discards queued pose and watch payloads the host has yet to drain.
// Unused args zero. Text is copied (max 1024 bytes). 1 queued, 0 no frame, -1 bad fact,
// -2 full (64 facts): drain and retry. Null text is allowed when n=0.
int qc_ses_net_fact(int kind,int a,int b,double value,const char* text,int n);
// FIRE: flags crouch=1, quad=2, cheatHook=4, id=weapon, mode=1 PQL/2 VQL, aim in radians.
// HOOK: flags 0 absent/1 flying/2 attached; position only for attached.
// SHOT: flags 0 fixed/1 aimed, id=entity index, mode as FIRE, xyz=target for aimed.
// Same return codes as net_fact. Only FIRE uses now; unused fields zero.
int qc_ses_net_effect(int kind,int flags,int id,int mode,double x,double y,double z,double yaw,double pitch,double now);
// Being watched (version 11, additive; native-core/watch.hpp; research/NATIVE-RUN-RESULT-DESIGN.md section 8). Call once per host frame with watched = "the hub says someone spectates us and we are not spectating", paused = menu / chat open (the host owns it).
// Writes the hub lines to send NOW into out, newline separated, none trailing, and returns their length: run (state / paused / restart), keys (every tick), snd (cues flagged CUE_SHARE, checkpoint splits {cp,t}, host sounds), inv, world (on change only). 0 = nothing to send (always 0 while not watched); -1 = capacity too small (the next call sends everything again; 81920 always suffices).
int qc_watch_out(int watched, int paused, double now, char* out, int capacity);
// A new connection / map: the next qc_watch_out sends everything (qc_peer_text's "watched" message does this itself).
void qc_watch_reset(void);
// A sound the host decided joins the next snd line. kind 1: a pickup / target message (text, UTF-8, <= 1024 bytes); 2: a jump take-off speed (value, ups); 3: a named cue (text "kind" or "kind sound", names of letters digits _ / . -, 1..64). Core-decided ones (cues, checkpoint splits) it adds itself.
void qc_watch_sound(int kind, int value, const char* text, int len);
// A peer / peer-full frame of the hub, stamped with `now`. Returns the slot, or -1 (not a peer frame, or a player the table does not know).
int qc_peer_binary(const uint8_t* p, size_t n, double now);
void qc_peer_set_options(const QcPeerOptions* o);
// Advances every peer to `now` (shown PEER_DELAY late: the adaptive delay of the arrival clock) and writes 4 records per known peer, in slot order and part order (visible or not; the host applies them all). Returns the record count, or -1 when capacity is too
// small (nothing is written; capacity QC_PEER_MAX * 4 always suffices). camera may be NULL (plates then stay hidden). The aimed-name test (plateMode 1) traces the loaded map, when there is one.
int qc_peer_frame(double now, const QcPeerCamera* camera, QcPeerNode* out, int capacity);
// The pose of player `number` as of the last qc_peer_frame. 1, or 0 when the player has no drawn pose yet.
int qc_peer_pose(int number, QcPeerPose* out);
// The roster entry of a slot (0 .. QC_PEER_MAX - 1). 1, or 0 for a free slot.
int qc_peer_info(int slot, QcPeerInfo* out);
// The model / material class of a part (see QcPartSpec). 1, or 0 for an unknown part / variant (guns 1..10, accessory 0..24).
int qc_peer_part_spec(int part, int variant, QcPartSpec* out);
// Our own state frame, with the send window of the page and the client in ONE place: s = the player's state (teleport = the sim's teleport COUNTER, any value >= 0; the frame carries it as a byte 1..255, 0 when negative). Writes the 26-byte frame to out26 and returns 26
// when it must go out now: the state changed and 66 ms passed since the last frame, or 3 s passed since the last frame (heartbeat), or force (the host's own timer fired) and a state was ever given. Otherwise 0. A state with a number that is not finite sends nothing.
int qc_peer_state_out(const QcWireState* s, double now, int force, uint8_t out26[26]);
// Where time t falls on a track the host holds (a recorded ghost, a demo, a route): samples of `stride` doubles starting t x y z; `counterSlot` = the index of the sample's teleport counter (-1 none; the counter, when both samples carry one > 0, decides a teleport, else the speed guess);
// rule 0 = a recorded run (25000 ups over the samples' spacing), 1 = a live peer (8000 ups over one 66 ms send interval); cursor = a sample to start searching from. Returns 1, or 0 for bad arguments.
int qc_track_point(const double* samples, int count, int stride, int counterSlot, int rule, double t, int cursor, QcTrackPoint* out);

// Other players' shots and hooks (native-core/peerfx.hpp). The host passes the hub's `ev` (fire / hook) and `shot` JSON in as received and, once per frame, reads (1) the records to draw, (2) the one-shot events that are due, (3) the due sound cues.
// Everything is shown 0.1 s after it arrived (`now` = the host's clock in seconds, the same clock as qc_peer_frame). The shots are traced against the loaded map with the mover state set by qc_dry_set_movers when the host shows another world's doors.
// A shot that does not fit (64 flights, 128 pending items) is not drawn. Map change / new connection: qc_peer_clear_samples / qc_peer_reset also drop every flight, hook and pending item.
#define QC_FX_MAX 160            // capacity that always suffices for qc_peer_fx_frame
#define QC_FX_MISSILE 1          // a flying missile: pos, dir = its unit flight direction (the rocket body points along it), weapon, mode (0 pql 1 vql), id = the flight's id (stable while it flies)
#define QC_FX_BLAST 2            // an explosion showing: pos = the last flight sample (the blast's centre), from = the impact origin, dir = the surface normal (flags HAS_NORMAL), age 0..1 of the blast's life (0.25 s, plasma 0.12 s)
#define QC_FX_HOOK 3             // a grappling hook: pos = the head, from = where the cable leaves the shooter's hand, id = the player's number, slot = the peer's slot, t = the drawn time, flags ATTACHED once it stuck, CROUCHED
#define QC_FXF_HAS_NORMAL 1
#define QC_FXF_ATTACHED 2
#define QC_FXF_CROUCHED 4
typedef struct QcPeerFx { int32_t kind, id, weapon, mode, slot, flags, reserved0, reserved1; double pos[3], dir[3], from[3], age, t, aux; } QcPeerFx;   // 128 bytes
#define QC_FXE_HITSCAN 1         // a beam hit (machinegun / shotgun pellet / railgun): a = the eye, b = the end, c = the surface normal, fraction, aux0 = the surface's BSP flags, mover = the hit mover's index or -1, flags STARTSOLID 1 / ALLSOLID 2
#define QC_FXE_EXPLOSION 2       // a missile exploded (at its drawn time): a = the origin, c = the normal (flags HAS_NORMAL 4), weapon, mode
#define QC_FXE_HOOK_LAND 3       // a hook stuck: a = where the cable started, b = where it landed
typedef struct QcPeerFxEvent { int32_t kind, weapon, mode, slot, flags, mover, reserved0, reserved1; double a[3], b[3], c[3], fraction, aux0, aux1; } QcPeerFxEvent;   // 128 bytes
// One hub text message: {"type":"ev"...} (fire / hook) or {"type":"shot"...} (a map shooter fired for the watched player). Returns 1 fired | 2 hook | 4 shooter missile, | (slot + 1) << 8 for a known shooter; 0 = ignored (an unknown player, a malformed message).
int qc_peer_fx_json(const char* json, size_t n, double now);
// The records to draw at `now` (missiles, blasts, hooks); also schedules what is due (an explosion's event and cue) and drops what is over. Returns the count, or -1 when capacity < QC_FX_MAX would not hold them all (nothing written).
int qc_peer_fx_frame(double now, QcPeerFx* out, int capacity);
// The one-shot events due at `now` (impact marks, explosion marks, hook sparks), oldest first; the rest stays queued. Returns the count taken.
int qc_peer_fx_events(double now, QcPeerFxEvent* out, int capacity);
// The sound cues due at `now` (the shot, the wall impact, the explosion), QcCue as qc_dry_cues writes them, with positions in the world. Returns the count taken (at most 64).
int qc_peer_cues(double now, QcCue* out, int capacity);

// Static-pose avatars (native-core/avatars.hpp): avatars a host places by hand with no track and no peer (the appearance preview's two models, the player's own reflection in a mirror), through the same node records as every other avatar.
#define QC_AVATAR_MAX 8
#define QC_AF_VISIBLE 1       // draw the body and the held gun
#define QC_AF_PLATE 2         // the plate record is shown (plateKey = the hash of name + sub: bake the image for it)
#define QC_AF_PLATE_FIXED 4   // the plate has the aimed (constant screen) size
#define QC_AF_CROUCHED 8      // the held gun rides at the crouched height
// One avatar: pos = the feet (scene axes, Y up), yaw = the facing (the model's own +pi turn is added by the core), pitch = the look pitch the gun follows, weapon 1..10 (0 = machinegun), size (<= 0 / NaN -> 1), transparency 0..1 (NaN -> 0.6), rgb = the body colour 0xRRGGBB (sRGB),
// name = the shown name (UTF-8, colour codes and clan separators intact: an official !dick clan tag gives the accessory), sub = the plate's second line.
typedef struct QcAvatarIn { int32_t slot, flags, weapon, reserved0; double pos[3], yaw, pitch, size, transparency; uint32_t rgb; int32_t reserved1; char name[96]; char sub[64]; } QcAvatarIn;   // 240 bytes
// 4 QcPeerNode records per entry (body, held gun, accessory, plate; node.slot = the entry's slot, key = slot, plate key = the roster's hash of name + sub), written in input order. Returns the node count (4 * n), or -1 when n > QC_AVATAR_MAX, nodeCapacity < 4 * n or the arguments are invalid. camera may be NULL (plates stay hidden).
int qc_avatar_frame(const QcAvatarIn* in, int n, const QcPeerCamera* camera, QcPeerNode* nodes, int nodeCapacity);
// The accessory tier + 1 of a shown name (0 = none; an official !dick clan tag has one): what qc_peer_info reports for a peer of that name.
int qc_avatar_tier(const char* name, int len);

// The node factory (version 11, native-core/assembly.hpp): the core describes the scene nodes of an assembly and writes the poses; a host makes what the template says and copies the pose rows (it decides no part, place, draw order or material flag).
#define QC_ASM_AVATAR 1       // variant = the accessory tier (0 none, 1..24): only the accessory row's colour depends on it
#define QC_ASM_MISSILE 2      // variant = weapon * 2 + ghost (rocket 5, grenade 4, plasma 8, BFG 9; ghost 1 = another player's / a recording's shot): one mesh
#define QC_ASM_BLAST 3        // the same variants: one mesh, its own material (param = the blast's life in seconds, colour = the explosion's); a plasma blast is the splash (model 15)
#define QC_AN_GROUP 0         // QcAsmNode.kind: an empty node
#define QC_AN_MESH 1          // a mesh: geometry = the render core's model `model` (rc_model_build kind; -1 = rc_model_avatar_surface), mesh `mesh` of it, material `material` of its record (or derived from matFlags QC_PS_* + color)
#define QC_AN_SPRITE 2        // a camera-facing sprite; flags QC_RF_TEXTURED: the host supplies the image (the plate's, keyed by the pose row's variant)
#define QC_AN_INSTANCED 3     // instanced meshes (the hook's chain links)
#define QC_AN_LINE 4          // a line strip (the web hook's line)
#define QC_RF_VISIBLE 1       // QcAsmNode.flags
#define QC_RF_TELEPORTED 2    // the avatar jumps instead of sliding (the body record's QC_NF_TELEPORTED)
#define QC_RF_TEXTURED 4
#define QC_RF_STATIC 8        // a template row that no pose row ever changes
#define QC_RF_OPACITY 16      // the mesh has a material of its own; color[3] of a pose row is its opacity
#define QC_ASM_HOOK 4         // variant 0: the grappling hook's cable (QC_HK_* indices; the chain row's param = its most links, the links' matrices and the head's pose are the render core's rc_hook_cable)
#define QC_HK_ROOT 0
#define QC_HK_CHAIN 1         // instanced links: model 8 (the cable), mesh "link", material "steel"
#define QC_HK_HEAD 2          // mesh "head", material "steel"
#define QC_HK_WEB_LINE 3      // the dev-only web hook's line strip (the host writes its two points); white
#define QC_HK_WEB_HEAD 4      // mesh "web-head", material "white"
#define QC_HK_COUNT 5
// Avatar template indices (QcAsmNode.index): the avatar's root (the body record's pose), the model root (the model's own turn), body, accessory, the held-gun group, one mesh per gun, the plate. Pose rows carry every index but QC_AV_MODEL.
#define QC_AV_ROOT 0
#define QC_AV_MODEL 1
#define QC_AV_BODY 2
#define QC_AV_ACCESSORY 3
#define QC_AV_GUN_GROUP 4
#define QC_AV_GUN0 5          // the first of 9 gun meshes (variant = the weapon number 1 2 3 4 5 7 8 9 10, mesh name = that number)
#define QC_AV_PLATE 14
#define QC_AV_COUNT 15
// One node of an assembly, a template row and a pose row alike (192 bytes). Template: what to make (kind, parent, draw order, material flags QC_PS_*, model / mesh / material names, rest pose local to the parent). Pose: index, flags, variant (the accessory tier, the
// plate key), the local pose and the colour (linear rgb; the avatar body's tint and the accessory tier's colour) to write each frame. slot = the record slot (QcPeerNode.slot) a pose row belongs to.
typedef struct QcAsmNode { int32_t slot, index, parent, kind, renderOrder, flags, matFlags, model, variant, reserved0; char mesh[16], material[16]; double pos[3], quat[4], scale[3], color[4], param; } QcAsmNode;   // 192 bytes: index 4, parent 8, kind 12, flags 20, model 28, variant 32, mesh 40, material 56, pos 72, quat 96, scale 128, color 152, param 184
// How many rows the template of an assembly has (0 = unknown assembly or variant), and row `index` of it (1, or 0 for an unknown index / variant).
int qc_asm_count(int assembly, int variant);
int qc_asm_node(int assembly, int variant, int index, QcAsmNode* out);
// The pose rows of `avatars` avatars from their QcPeerNode records (4 each: body, gun, accessory, plate; from qc_peer_frame, qc_track_frame or qc_avatar_frame): QC_AV_COUNT - 1 rows per avatar in input order (no model-root row). Returns the row count, or -1 when capacity is too small.
int qc_asm_avatar_pose(const QcPeerNode* records, int avatars, QcAsmNode* out, int capacity);
// One shot at one moment (a missile at pos flying along dir, or a blast centred at pos at age 0..1 of its life: a plasma splash is placed by origin / normal when flagged, else by pos and camQuat): its pose row. Returns 1, 0 (a weapon or kind with no node) or -1 (bad arguments).
#define QC_FXI_GHOST 1          // another player's / a recording's shot
#define QC_FXI_HAS_NORMAL 2     // normal is the surface the blast hit
#define QC_FXI_HAS_ORIGIN 4     // origin is the impact point (a plasma splash sits there; else at pos)
typedef struct QcAsmFxIn { int32_t kind, weapon, mode, flags, reserved0, reserved1; double age, pos[3], dir[3], origin[3], normal[3], camQuat[4]; } QcAsmFxIn;   // 160 bytes; kind = QcPeerFx.kind (1 missile, 2 blast), mode 0 PQL / 1 VQL
int qc_asm_fx_pose(const QcAsmFxIn* in, QcAsmNode* out, int capacity);
// The same for a record of qc_peer_fx_frame (always another player's shot) at the camera's rotation camQuat (x y z w); NULL = the identity.
int qc_asm_fx_pose_peer(const QcPeerFx* record, const double* camQuat, QcAsmNode* out, int capacity);

// The track players (native-core/tracks.hpp): recorded tracks drawn as avatars by the core (race ghost, a spectated player's WR ghost, the route viewer's markers) and queried (demo viewer, HQ appearance pass). The host copies a track into a SLOT only when the
// track changes (never per frame): the pool holds 2^21 doubles in all, shared by the slots. A sample = `stride` (4..64) doubles [t,x,y,z,yaw,pitch,crouched,vx,vy,vz,weapon,style,color,teleport counter...] (the ghost format; shorter rows read 0 / machinegun for the missing columns).
#define QC_TRACK_MAX 16
#define QC_TRACK_RULE_RUN 0     // a recorded run: the sample's teleport counter (column counterSlot, > 0 on both samples) decides, else the 25000 ups speed guess
#define QC_TRACK_RULE_PEER 1    // a live peer's step (8000 ups over one send interval)
#define QC_TRACK_RULE_QLDEMO 2  // a QL demo: column counterSlot holds the entity flags, a change of bit 4 is a teleport
// QcTrackIn.flags
#define QC_TF_VISIBLE 1         // draw the body, gun (and plate when asked)
#define QC_TF_PLATE 2           // the plate record is shown (plateKey != 0 as well); the host bakes the image for plateKey
#define QC_TF_TINT 4            // the body tint is `tint` (linear rgb); else white
#define QC_TF_PLATE_FIXED 8     // the plate has the aimed (constant screen) size; else it is never bigger than that
// One avatar to draw this frame: slot, t = the track time (seconds, the track's own clock), size = the player size scale (cg_ghostSize; <= 0 / NaN -> 1), transparency 0..1 (NaN -> 0.6).
typedef struct QcTrackIn { int32_t slot, flags; double t, size, transparency; float tint[4]; uint32_t plateKey; int32_t reserved0; double reserved1; } QcTrackIn;   // 64 bytes: t 8, size 16, transparency 24, tint 32, plateKey 48
// Where the track is at t: k / mix / tele as qc_track_point; pos / yaw (the short way round) / pitch lerped; weapon = the held weapon, crouched; posed 0 when the slot holds no track.
typedef struct QcTrackState { int32_t slot, k, tele, weapon; double mix, pos[3], yaw, pitch; int32_t crouched, posed; } QcTrackState;   // 72 bytes: mix 16, pos 24, yaw 48, pitch 56, crouched 64
// Copies a track into a slot (0 .. QC_TRACK_MAX - 1), replacing what it held: count >= 1 samples of `stride` doubles. counterSlot = the column of the teleport counter / flags (-1 none). Returns 1, or 0 when the arguments are invalid or the pool is full (the slot is then empty).
int qc_track_slot_set(int slot, int count, int stride, int counterSlot, int rule, const double* samples);
void qc_track_slot_clear(int slot);
// Where time t falls on the slot's track (as qc_track_point, with the slot's own rule); cursor = the sample to search from. 1, or 0 for an empty slot.
int qc_track_slot_point(int slot, double t, int cursor, QcTrackPoint* out);
// 1 when the step from sample i - 1 to sample i is a teleport under the slot's rule (route lines break there), else 0 (also for i out of range or an empty slot).
int qc_track_slot_teleport(int slot, int i);
// The avatars of `n` (<= QC_TRACK_MAX) entries: 4 QcPeerNode records each (body, gun, accessory (always hidden: a track has no player name), plate; node.slot = the entry's slot, key = slot, plate key = plateKey), written in input order, and one QcTrackState each.
// The core keeps each slot's search cursor. Returns the node count (4 * n), or -1 when nodeCapacity < 4 * n or the arguments are invalid. camera may be NULL (plates stay hidden). An entry with an empty slot gets hidden records.
int qc_track_frame(const QcTrackIn* in, int n, const QcPeerCamera* camera, QcPeerNode* nodes, int nodeCapacity, QcTrackState* states);

// World composition, part A: the leaderboard ghost (the track in slot 0, uploaded by the host when the run changes). Once per
// frame after the due ticks, qc_ses_compose decides (camera-independent) whether the ghost is drawn, its clock and sample, its
// colour, the route line and the minimap track; then qc_ses_view(final camera) writes the ghost's 4 node records. Size, colour
// and transparency come from the settings (qc_ses_settings). Neither call allocates or traces.
enum { QC_COMPOSE_GHOST=1,                      // slot 0 holds the run to show
  QC_COMPOSE_SHOW=2, QC_COMPOSE_PATH=4,         // the ghost model / route line toggles
  QC_COMPOSE_HIDDEN=8,                          // the host hides ghosts (a race)
  QC_COMPOSE_NO_PATH=16,                        // a view without route line or minimap track (a demo, the route camera)
  QC_COMPOSE_FOLLOWING=32,                      // spectating a player: the clock is theirs
  QC_COMPOSE_FOLLOW_RUNNING=64, QC_COMPOSE_FOLLOW_PAUSED=128 };
// now / followAt: host monotonic seconds. followT: the followed run time reported at followAt. duration: the ghost run's time.
// ownRun / ownElapsed: ownRun -1 = the loaded sim's own run (what a host passes); else 0 ready, 1 running, 2 finished.
typedef struct QcComposeIn { double now, duration, followT, followAt, ownElapsed; int32_t flags, ownRun, plateKey, reserved; } QcComposeIn;   // 56 bytes: flags 40
// track: draw the minimap's ghost track up to pathSample; path: draw the route line up to pathSample. color / pathColor: 0xRRGGBB.
typedef struct QcComposeOut { double time, tint[3]; int32_t visible, sample, track, path; uint32_t color, pathColor; int32_t pathSample, reserved; } QcComposeOut;   // 64 bytes: tint 8, visible 32, color 48, pathSample 56
int qc_ses_compose(const QcComposeIn* in, QcComposeOut* out);                  // 0, or -1 for a missing buffer
int qc_ses_view(const QcPeerCamera* camera, QcPeerNode* out, int capacity);    // records written (0 = hidden), -1 when capacity < 4

// World composition, part B: the minimap, once per frame while it is shown. in: the map's square (centre cx, cz, side S), the
// followed pose (px, pz, yaw rad, horizontal speed), seconds since the last frame, the player's options (range, speedScale
// percent, zoom 0 full / 1 flat / 2 speed). ghostSample: QcComposeOut.pathSample while its track != 0, else -1.
// out: the view of the top-down camera pass; ghost: the ghost track's x, z float pairs round that sample (NaN pair = pen up),
// ghostCount points, ghostK the ghost's point in them. No allocation (the thinned track is built when slot 0 changes).
#define QC_MINIMAP_TRACK_POINTS 1000
enum { QC_MINIMAP_ROTATE=1, QC_MINIMAP_PREVIEW=2,   // the HUD setup box: a made-up pose in the middle of the map
  QC_MINIMAP_RESET=4 };                             // new map bounds: the speed zoom jumps instead of easing
typedef struct QcMinimapIn { double cx, cz, S, px, pz, yaw, speed, dt, range, speedScale; int32_t zoom, flags, ghostSample, reserved; } QcMinimapIn;   // 96 bytes: zoom 80
typedef struct QcMinimapOut { double cx, cz, w, yaw; int32_t rotate, ghostCount, ghostK, reserved; } QcMinimapOut;   // 48 bytes: rotate 32
int qc_ses_minimap(const QcMinimapIn* in, QcMinimapOut* out, float* ghost, int capPoints);   // 0, or -1 (missing buffer, capPoints < QC_MINIMAP_TRACK_POINTS)

// World composition, part B: every ground shadow of the frame, once per frame after qc_peer_frame and qc_ses_view. The core
// picks who casts one: the host's own pose (QC_SHADOW_SELF: the own player, or a replay's), and with QC_SHADOW_WORLD the
// composed ghost, every drawn player and the followed player (their number; hidden in first person). Accessory footprints of
// the others follow their tier and the showDickModels setting. Each record draws as qc_dry_player_shadow describes.
enum { QC_SHADOW_SELF=1, QC_SHADOW_WORLD=2 };
#define QC_SHADOW_MAX (QC_PEER_MAX+2)
typedef struct QcShadowIn { double self[3], selfYaw, selfAccX, selfAccZ; int32_t flags, followed; } QcShadowIn;   // 56 bytes: flags 48
int qc_ses_shadows(const QcShadowIn* in, QcShadow* out, int capacity);   // visible records written; -1 missing buffer or capacity < QC_SHADOW_MAX, -2 no map

// World composition, part B: the pickups' idle motion (map pickups, custom pickups), once per frame. The host owns the records:
// baseY and phase = (x * 0.013 + z * 0.017) mod 6.283 per pickup, yaw = its angle so far (radians), visible = it is shown.
// A visible record turns by 2 radians a second (dt = seconds since the last frame) and gets y = baseY + a gentle bob at the host
// time `now` (seconds); an invisible one is left as it is. Stateless, allocation-free.
typedef struct QcItemSpin { double baseY, phase, yaw, y; int32_t visible, reserved; } QcItemSpin;   // 40 bytes: visible 32
int qc_ses_items(double now, double dt, QcItemSpin* items, int n);   // 0, or -1 (missing buffer, n < 0)

// World composition, part C: what one shot event leaves, once per event, for the own shots, other players' (qc_peer_fx_events) and
// replays. in: kind QC_SESFX_*; weapon; mode 0 PQL / 1 VQL; flags HAS_NORMAL, RUNWAY (the built-in runway: its invisible walls take
// no marks). HITSCAN: from = the eye, pos = the end, normal, surface = the BSP flags, fraction (nothing for the gauntlet).
// EXPLOSION: pos, normal (without HAS_NORMAL the floor is probed 48 down). HOOK_LAND: from = where the cable started, pos = where
// it landed. Traced against the dry world (movers as qc_dry_set_movers says). The marks setting is the core's (qc_ses_settings):
// off = only the rail beam. out: draw the commands in order. BEAM: pos = from, normal = to. MARK: style QC_MARK_*, radius 0 = the
// style's own size. SPARKS: a burst off the surface. No allocation.
enum { QC_SESFX_HITSCAN=1, QC_SESFX_EXPLOSION=2, QC_SESFX_HOOK_LAND=3 };
enum { QC_SESFX_HAS_NORMAL=1, QC_SESFX_RUNWAY=2 };
enum { QC_FXCMD_BEAM=1, QC_FXCMD_MARK=2, QC_FXCMD_SPARKS=3 };
enum { QC_MARK_BULLET=0, QC_MARK_RAIL_BURN=1, QC_MARK_RAIL_GLOW=2, QC_MARK_SCORCH=3, QC_MARK_PLASMA=4, QC_MARK_PLASMA_BURN=5 };
#define QC_FXCMD_MAX 8
typedef struct QcFxIn { int32_t kind, weapon, mode, flags, surface, reserved; double fraction, from[3], pos[3], normal[3]; } QcFxIn;   // 104 bytes: fraction 24, from 32
typedef struct QcFxCmd { int32_t kind, style; double pos[3], normal[3], radius; } QcFxCmd;   // 64 bytes: pos 8, radius 56
int qc_ses_fx(const QcFxIn* in, QcFxCmd* out, int capacity);   // commands written; -1 missing buffer or capacity < QC_FXCMD_MAX, -2 no map

#ifdef __cplusplus
}
#endif
