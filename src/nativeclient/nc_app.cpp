#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string_view>
#define _USE_MATH_DEFINES
// The client application: standalone (main) or embedded in the CEF host window (nc_embed.cpp). Window, Vulkan swapchain, the map drawn from the render core's frame plan, and (with --sim) the game simulation ticked in fixed 8 ms steps from raw input.
// Files:  --pk3 FILE (repeat; the first one named has the highest priority: the map's own pk3, then oa-base and the shared libraries) and/or --root DIR (loose files, searched last)
// Shader archive roles: --oa-pk3 FILE, --pooled-pk3 FILE. The archive supplying the BSP is tier 3.
// World:  --bsp maps/NAME.bsp  [--scripts FILE --files FILE: only to override the script text and file list the archives give] [--fixes map-fixes.json] [--pos x,y,z] [--yaw DEG] [--orbit]
// Play:   --sim (the sim drives the camera: WASD, Space, Ctrl, mouse, 1-9; Esc quits)  --sens N (mouse sensitivity; the core's default without it)  --pql (PQL physics instead of VQL)
// Window: --windowed  --pm immediate|mailbox|fifo  --size WxH  --frames N (exit after N frames)  --cap FPS (0 = uncapped)
#include "nc_vk.h"
#include "nc_shader.h"
#include "nc_world.h"
#include "nc_sim.h"
#include "nc_input.h"
#include "nc_hud.h"
#include "nc_audio.h"
#include "nc_sound.h"
#include "nc_hub.h"
#include "nc_match.h"
#include "nc_match_hud.h"
#include "nc_coreload.h"
#include "render_core.h"
#include "nc_json.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <new>
#include <cstring>
#include <cstdlib>
#include <string>
#include "nc_app.h"
#if defined(NC_WINDOWS)
#define NC_WIN 1
#else
#define NC_WIN 0
#endif
#include <atomic>
#include <stdexcept>
#include <map>
#include <set>
#include <mutex>
#include "text_api.h"
#if defined(NC_WINDOWS)
#include <timeapi.h>
#else
#include <SDL3/SDL.h>
#endif

// The allocation counter is armed only on the recorded render thread.
void* operator new(std::size_t size) {
  if (nc::frameAllocations) ++*nc::frameAllocations;
  while (true) {
    if (void* p = std::malloc(size ? size : 1)) return p;
    if (auto handler = std::get_new_handler()) handler();
    else throw std::bad_alloc();
  }
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static std::string optFonts = "E:/#programming/qlrace2/assets/fonts", optHudAssets = "E:/#programming/qlrace2/render-core/hud-assets";   // dev defaults: the game project's files, until the client carries its own copies
static std::vector<std::pair<std::string, int>> optPk3;
static std::vector<std::string> optFiles2;   // optFiles2: --file virtual/path=file@offset+size
static std::string g_assetsDir;
void appSetAssetsDir(const std::string& dir) { g_assetsDir = dir; }
 static std::string optRoot, optBsp, optScripts, optFiles, optFixes; static double optYaw = 0, optPos[3]; static bool optHasYaw = false, optHasPos = false, optOrbit = false, optSim = false, optPql = false; static bool optWeaponS = false, optHook = false; static std::string optHub, optName = "native", optMapName;   // the account token comes from the environment (QRAZY_TOKEN), never the command line
static std::string optVoice; static std::string optSound; static double optVolume = -1, optSens = 0; static bool optNoHud = false, optNoSound = false;   // --voice / --volume / --sens / --fov: only what was given goes into the settings (capture runs)
struct FakePeer { double x = 0, y = 0, z = 0, yawDeg = 0; int weapon = 0; std::string name, color; };
static std::vector<FakePeer> optFakePeers;   // --fakepeer "x,y,z,yawDeg,weapon,name,#rrggbb": a stand-in for another player, fed to the core's peer table as hub messages (for captures)
static double optLookPitch = -1000;   // --lookpitch DEG: the sim view pitch at the start (for captures)
static bool optFxTest = false;   // --fxtest: spawn one of each weapon effect in front of the start (for captures)
static bool g_cgazOn = false; static double g_cgazY = -1;   // the page's CGaz state ("host.cgaz": {on, ypos})
struct MmCfg { bool have = false; uint32_t zoom = 0, flags = 0, boxPx = 0, trailColor = 0, ghostColor = 0; double range = 0, speedScale = 0; float x = -1, y = -1, scale = 1; };
static MmCfg g_mm; static std::mutex g_mmMu;   /* the page's "host.minimap" layout and options */
static bool optAttack = false;   // --attack: hold the attack key down (for captures)
static int optWeapon = 0;   // --weapon N: preferred starting weapon for standalone captures
static bool optRunway = false;   // --runway: the built-in runway (no BSP; the core builds its collision)
static std::string optOp;   // --op JSON: a sim op the page would send (respawn, mode ...), applied at frame 20 (for captures and checks)
static bool g_showTriggers = false, optShowTriggers = false;   // the page's host.showTriggers (!showtriggers); --show-triggers forces the overlay on (for captures)
static double optFov = 0; static std::vector<std::pair<std::string, std::vector<std::string>>> optBinds;
static std::mutex g_opMu; static std::vector<std::string> g_ops; void appSimOp(const std::string& json) { std::lock_guard<std::mutex> g(g_opMu); if (g_ops.size() < 256) g_ops.push_back(json); }   // the page's ops for the running sim (respawn, mode, cmd ...), any thread; applied before the next tick
static bool g_pagePaused = false;   // the page's real pause (the /pause hold, "paused" in the hud message): the sim stops and HUD element 13 shows; a text box open alone is not this
static bool optBench = false;   // --bench: percentiles of the frame interval, CPU and GPU time at exit
static std::string optShot; static int optShotFrame = 30;
static void (*g_menuRequest)() = nullptr;
// One sim event as the state push lists it, appended to a comma-separated list.
static void appendEventJson(std::string& js, const nc::Sim::Event& e) {
  char b[420]; snprintf(b, sizeof b, "%s{\"type\":%d,\"weapon\":%d,\"id\":%d,\"surface\":%d,\"radius\":%.3f,\"pos\":[%.3f,%.3f,%.3f],\"dir\":[%.6f,%.6f,%.6f],\"to\":[%.3f,%.3f,%.3f],\"vel\":[%.3f,%.3f,%.3f]}", js.empty() ? "" : ",", e.type, e.weapon, e.id, e.surface, e.radius, e.pos[0], e.pos[1], e.pos[2], e.dir[0], e.dir[1], e.dir[2], e.to[0], e.to[1], e.to[2], e.vel[0], e.vel[1], e.vel[2]); js += b;
}
// window.qrazyNativeCommand(...): the core's answer to a page command, as it came.
static std::string commandScript(int cmd, const nc::Sim::CommandReply& r) {
  auto text = [](const std::string& s) { std::string o = "\""; for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += (char)c; } else if (c < 32) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); o += u; } else o += (char)c; } return o + "\""; };
  std::string js = "window.qrazyNativeCommand&&window.qrazyNativeCommand({cmd:" + std::to_string(cmd) + ",bits:" + std::to_string(r.bits) + ",state:[";
  for (int i = 0; i < 31; ++i) { const double v = r.state ? r.state[i] : 0; char b[40]; if (std::isnan(v)) snprintf(b, sizeof b, "NaN"); else if (std::isinf(v)) snprintf(b, sizeof b, v < 0 ? "-Infinity" : "Infinity"); else snprintf(b, sizeof b, "%.17g", v); if (i) js += ','; js += b; }
  return js + "],reply:" + text(r.reply) + ",source:" + text(r.source) + "})";
}
static void (*g_simOut)(const char*) = nullptr; void appSetSimOut(void (*f)(const char*)) { g_simOut = f; }
static void (*g_tabCb)(bool) = nullptr; void appSetTabCallback(void (*f)(bool)) { g_tabCb = f; }
static std::mutex g_boardMu; static std::string g_boardJson; static std::atomic<bool> g_boardDirty{false};
void appSetBoard(const std::string& json) { { std::lock_guard<std::mutex> g(g_boardMu); g_boardJson = json; } g_boardDirty = true; }
struct PageHudMessage { std::string json; int64_t receivedAt; };
static std::mutex g_pageMu; static std::string g_settingsJson; static std::vector<PageHudMessage> g_hudQueue; static std::atomic<bool> g_settingsDirty{false}, g_hudDirty{false};   // HUD messages are processed in arrival order for log and element deltas
static std::mutex g_poseMu; static double g_pose[14]; static bool g_poseValid = false;
static std::atomic<bool> g_poseNow{false}; bool appTakePoseRequest() { return g_poseNow.exchange(false); }
bool appGetPose(double out[14]) { std::lock_guard<std::mutex> g(g_poseMu); if (!g_poseValid) return false; for (int k = 0; k < 14; ++k) out[k] = g_pose[k]; return true; }
void appSetSettings(const std::string& json) { { std::lock_guard<std::mutex> g(g_pageMu); g_settingsJson = json; } g_settingsDirty = true; }
void appSetPageHud(const std::string& json) { const int64_t receivedAt = nc::qpcNow(); { std::lock_guard<std::mutex> g(g_pageMu); if (g_hudQueue.size() < 512) g_hudQueue.push_back({json, receivedAt}); } g_hudDirty = true; }
void appSetMenuRequest(void (*f)()) { g_menuRequest = f; }
void appClearOps() {
  { std::lock_guard<std::mutex> g(g_opMu); g_ops.clear(); }
  { std::lock_guard<std::mutex> g(g_pageMu); g_hudQueue.clear(); }
}
static std::atomic<bool> g_started{false}; bool appTakeStarted() { return g_started.exchange(false); }
static std::atomic<bool> g_hudFeed{false}, g_hudReady{false}, g_audioFeed{false};
bool appAudioFeed() { return g_audioFeed.load(); }
bool appHudFeed() { return g_hudFeed.load(); }
void appHudReady() { g_hudReady = true; }
static void (*g_pageJs)(const char*) = nullptr;
static bool g_deferTextJs = false;
static size_t g_deferredTextCount = 0;
static std::array<std::string, 256> g_deferredTextJs;
// Run records wait for the page across matches.
static nc::RunSender g_runs; static bool g_watchPaused = false;
// The settings live in the core (qc_ses_settings): the page's settings message goes to it whole; the client reads what it draws and plays from g_set. Both outlive a match.
static const QcSettings* g_set = nullptr; static bool g_settingsOk = false;
static double g_ownShadow[2] = {0, 0}, g_speedoMax = 1200;   // page-only values (host.ownShadow: the accessory scale x, z of the own shadow, 0 0 = none; host.hud.speedBarMax)
static bool coreSettings(const std::string& json) {   // render thread, on a change only; n = 0: all defaults and the default binds
  if (json.size() > 0x7fffffff || qc_ses_settings(json.empty() ? nullptr : json.data(), (int)json.size()) < 0) { nc::logf("settings rejected by the core (%zu bytes)", json.size()); return false; }
  return g_settingsOk = true;
}
// qc_peer_set_options overwrites the core's options: the settings' part is copied every time
static void peerOptions(int followed) {
  QcPeerOptions o{}; o.playerSize = g_set->playerSize; o.transparency = g_set->playerTransparency; o.drawPlayers = g_set->drawPlayers != 0; o.plateMode = (int)g_set->playerNames; o.followedNumber = followed > 0 ? followed : 0; qc_peer_set_options(&o);
}
static bool optHidden = false, optFacePickup = false;   // --hidden (Windows): the window is never shown or focused, for captures
static void resetMatchOptions() {   // appRun runs once per match in the same process: every option of the previous match must go (a stale --runway made every later map the runway)
  optPk3.clear(); optFiles2.clear(); optRoot.clear(); optBsp.clear(); optScripts.clear(); optFiles.clear(); optFixes.clear(); optYaw = 0; optPos[0] = optPos[1] = optPos[2] = 0; optHasYaw = optHasPos = optOrbit = optSim = optPql = optWeaponS = optHook = false;
  optHub.clear(); optName = "native"; optMapName.clear(); optVoice.clear(); optSound.clear(); optVolume = -1; optSens = 0; optNoHud = optNoSound = false; optFakePeers.clear(); optLookPitch = -1000; optFxTest = false;
  optAttack = false; optWeapon = 0; optRunway = false; optShowTriggers = false; optOp.clear(); optFov = 0; optBinds.clear(); optHidden = optFacePickup = false; g_pagePaused = false; optBench = false; optShot.clear(); optShotFrame = 30;
}
// After any call into the core's text field: the input side follows its state (typing = the state is non-zero: the mouse is free, every key goes to the field); the game keys held when a box opens do not stay down.
static void textSync(uint32_t before);
static bool g_quit = false; static int g_w = 0, g_h = 0; static bool g_resized = false; static nc::Input g_input;
static void textSync(uint32_t before) {
  const uint32_t now = nc::tf::state(), box = TF_STATE_CHAT | TF_STATE_TEAM | TF_STATE_CONSOLE; g_input.textState = now; g_input.typing = now != 0;
  if (!(before & box) && (now & box)) g_input.releaseAllKeys();
}
void appSetJsOut(void (*f)(const char*)) {
  g_pageJs = f;
  nc::tf::setJsOut([](const char* js) {
    if (g_deferTextJs) {
      if (g_deferredTextCount < g_deferredTextJs.size()) g_deferredTextJs[g_deferredTextCount++] = js;
    } else if (g_pageJs) g_pageJs(js);
  });
}
void appTextUnit(unsigned unit) {   // a WM_CHAR UTF-16 unit from the host window's thread; a surrogate pair is combined here
  static unsigned high = 0;
  if (unit >= 0xD800 && unit < 0xDC00) { high = unit; return; }
  if (unit >= 0xDC00 && unit < 0xE000) { if (high) { nc::tf::queueChar(0x10000u + ((high - 0xD800u) << 10) + (unit - 0xDC00u)); high = 0; } return; }
  high = 0; nc::tf::queueChar(unit);
}
void appTextCommit(const char* utf8) { if (utf8 && *utf8) nc::tf::queueCommit(utf8); }
void appTextPreedit(const char* utf8, unsigned curFrom, unsigned curTo, unsigned ulFrom, unsigned ulTo) { nc::tf::queuePreedit(utf8 ? utf8 : "", curFrom, curTo, ulFrom, ulTo); }
bool appTextComposing() { return nc::tf::composing(); }
bool appCaretRect(float out[4]) { nc::tf::caretRect(out); return out[3] > 0; }
bool appTyping() { return g_input.typing.load(); }
bool appCursor() { return g_input.mouseFree(); }   // the mouse is free: arrow cursor, no capture (typing implies it)
static nc::Hud* g_hudPtr = nullptr;
int appClickElement(int px, int py, int* element, int* index, char* url, int cap) {   // a click on a clickable element (map info 16, race panel 22, race invite 23): 0 nothing, 1 forward to the page (element, index), 2 open url
  if (g_hudPtr && g_hudPtr->hitClippy(px, py)) { *element = 41; *index = 0; return 1; }   // Clippy (HUD_BOX_CLIPPY): the page may dismiss him (qrazyNativeClick element 41)
  nc::Hud::Click c; if (!g_hudPtr || !g_hudPtr->hitClickable(px, py, &c)) return 0;
  *element = c.element; *index = c.index; if (c.url.empty()) return 1; snprintf(url, (size_t)cap, "%s", c.url.c_str()); return 2;
}
void appLogMouse(int kind, int px, int py, bool shift) { if (g_hudPtr) g_hudPtr->logMouse(kind, px, py, shift); }   // the open chat box / console: down 0, move 1, up 2
void appLogWheel(double notches) { if (g_hudPtr) g_hudPtr->logWheel(notches); }

// ---- the platform: window, event pump, size. Windows: Win32 window or the CEF host's HWND with a message-only window for raw input. Linux: an SDL3 (Wayland) window, or the host's SDL_Window with its events forwarded.
#if defined(NC_WINDOWS)
static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {   // the message-only window of an embedded client: raw input only
  if (g_input.handle(m, w, l)) return 0;
  return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_input.typing.load()) {   // the pointer on the open chat box / console (the log view): selection, wheel
    const int x = (short)LOWORD(l), y = (short)HIWORD(l); const bool shift = (w & MK_SHIFT) != 0;
    if (m == WM_LBUTTONDOWN) { SetCapture(h); appLogMouse(0, x, y, shift); return 0; }
    if (m == WM_MOUSEMOVE && GetCapture() == h) { appLogMouse(1, x, y, shift); return 0; }
    if (m == WM_LBUTTONUP && GetCapture() == h) { ReleaseCapture(); appLogMouse(2, x, y, shift); return 0; }
    if (m == WM_MOUSEWHEEL) { appLogWheel(GET_WHEEL_DELTA_WPARAM(w) / 120.0); return 0; }
  }
  if (m == WM_CHAR) { appTextUnit((unsigned)w); return 0; }   // standalone: the window's own text input (the embedded host's native view does the same, and the IME)
  if (g_input.handle(m, w, l)) { if (g_input.quitRequested) g_quit = true; return 0; }
  switch (m) {
    case WM_SIZE: g_w = LOWORD(l); g_h = HIWORD(l); g_resized = true; return 0;
    case WM_SETFOCUS: if (optSim) (!optHidden && (g_input.setCapture(!g_input.mouseFree()), true)); return 0;
    case WM_CLOSE: case WM_DESTROY: g_quit = true; PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

struct Plat { NativeWindow win = nullptr; void* aux = nullptr; void* instance = nullptr; };
static void platProcess() { SetProcessDPIAware(); timeBeginPeriod(1); }
static Plat platCreate(NativeWindow host, bool windowed, int w, int h) {
  Plat p; HINSTANCE hi = GetModuleHandleW(nullptr); p.instance = hi;
  WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"QrazyNative"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); if (!host) RegisterClassW(&wc);
  if (host) {   // embedded: draw into the host's window; raw input comes to a message-only window of this thread
    WNDCLASSW ic = {}; ic.lpfnWndProc = InputProc; ic.hInstance = hi; ic.lpszClassName = L"QrazyNativeInput"; RegisterClassW(&ic);
    p.aux = CreateWindowExW(0, L"QrazyNativeInput", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr); p.win = host;
  } else p.win = windowed ? CreateWindowW(L"QrazyNative", L"Qrazy native", WS_OVERLAPPEDWINDOW | (optHidden ? 0 : WS_VISIBLE), 100, 100, w, h, nullptr, nullptr, hi, nullptr)
                          : CreateWindowExW(0, L"QrazyNative", L"Qrazy native", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, hi, nullptr);
  return p;
}
static void platSize(NativeWindow win, int& w, int& h) { RECT r; GetClientRect(win, &r); w = r.right; h = r.bottom; }
static void platAttachInput(const Plat& p, NativeWindow host, const std::atomic<bool>* hostVisible) {
  if (host) g_input.attachRaw((HWND)p.aux, host, [host, hostVisible] { return hostVisible->load() && GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(host, GA_ROOT); });
  else g_input.attach(p.win);
}
static void platPump(NativeWindow) { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
static void platShutdown(const Plat& p) { if (p.aux) DestroyWindow((HWND)p.aux); timeEndPeriod(1); }
#else
// Events the host forwards (embedded): queued here, processed on the render thread so Input is only ever touched by one thread.
static std::mutex g_evMu; static std::vector<SDL_Event> g_evQueue;
void appOnSdlEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_TEXT_INPUT: if (e.text.text) nc::tf::queueCommit(e.text.text); break;   // the toolkit's text input (needs SDL_StartTextInput while a box is open: appTextInputWanted)
    case SDL_EVENT_TEXT_EDITING: if (e.edit.text) { const std::string t = e.edit.text; uint32_t from = 0, to = 0; if (e.edit.start >= 0) { size_t i = 0; int32_t cp = 0; while (i < t.size() && cp < e.edit.start) { ++i; while (i < t.size() && ((unsigned char)t[i] & 0xC0) == 0x80) ++i; ++cp; } from = to = (uint32_t)i; } nc::tf::queuePreedit(t, from, to, 0, 0); } break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_WINDOW_FOCUS_LOST: {
      std::lock_guard<std::mutex> g(g_evMu); if (g_evQueue.size() < 4096) g_evQueue.push_back(e); break; }
    default: break;
  }
}
bool appCaptureWanted() { return g_input.captureWanted(); }
bool appTextInputWanted() { return nc::tf::state() != 0; }   // the host calls SDL_StartTextInput / SDL_StopTextInput on its main thread from this (and SDL_SetTextInputArea from appCaretRect)

struct Plat { NativeWindow win = nullptr; void* aux = nullptr; void* instance = nullptr; };
static void platProcess() {}
static NativeWindow g_platWin = nullptr;
static void platPointer(const SDL_Event& e) {   // the pointer on the open chat box / console (the log view): selection, wheel
  if (!g_input.typing.load() || !g_platWin) return; const float d = SDL_GetWindowPixelDensity(g_platWin); const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
  switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: if (e.button.button == SDL_BUTTON_LEFT) appLogMouse(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : 2, (int)(e.button.x * d), (int)(e.button.y * d), shift); break;
    case SDL_EVENT_MOUSE_MOTION: if (e.motion.state & SDL_BUTTON_LMASK) appLogMouse(1, (int)(e.motion.x * d), (int)(e.motion.y * d), shift); break;
    case SDL_EVENT_MOUSE_WHEEL: appLogWheel(e.wheel.y); break;
    default: break;
  }
}
static Plat platCreate(NativeWindow host, bool windowed, int w, int h) {
  Plat p; if (host) { p.win = g_platWin = host; return p; }
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland"); SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0"); SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1");   // native Wayland, raw relative motion (as the CEF host sets them)
  if (!SDL_Init(SDL_INIT_VIDEO)) nc::fatal("SDL video init failed: %s", SDL_GetError());
  p.win = SDL_CreateWindow("Qrazy native", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | (windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN));
  if (!p.win) nc::fatal("SDL_CreateWindow failed: %s", SDL_GetError());
  g_platWin = p.win; return p;
}
static void platSize(NativeWindow win, int& w, int& h) { SDL_GetWindowSizeInPixels(win, &w, &h); }
static void platAttachInput(const Plat& p, NativeWindow host, const std::atomic<bool>* hostVisible) {
  if (host) g_input.attachSdl(p.win, [host, hostVisible] { return hostVisible->load() && (SDL_GetWindowFlags(host) & SDL_WINDOW_INPUT_FOCUS) != 0; }, false);
  else g_input.attachSdl(p.win, [window = p.win] { return (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0; }, true);
}
static void platPump(NativeWindow host) {
  if (host) { static thread_local std::vector<SDL_Event> q; q.clear(); { std::lock_guard<std::mutex> g(g_evMu); q.swap(g_evQueue); } for (const SDL_Event& e : q) { platPointer(e); g_input.onSdlEvent(e); } return; }
  SDL_Event e; while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_quit = true;
    else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED && optSim) (!optHidden && (g_input.setCapture(!g_input.mouseFree()), true));
    else { platPointer(e); g_input.onSdlEvent(e); }
  }
  if (g_input.quitRequested) g_quit = true;
}
static void platShutdown(const Plat& p) { if (p.win && !p.aux) { /* the standalone window dies with the process; the host owns its own */ } }
#endif

// ---- --shot FILE [--shotframe N]: the presented frame as a PNG (stored deflate) and its mean colour, to look at what the executor draws without a screenshot tool
static uint32_t crcTab[256]; static bool crcInit = false;
static uint32_t crc32(uint32_t c, const unsigned char* p, size_t n) { if (!crcInit) { for (uint32_t i = 0; i < 256; ++i) { uint32_t x = i; for (int k = 0; k < 8; ++k) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crcTab[i] = x; } crcInit = true; } c = ~c; while (n--) c = crcTab[(c ^ *p++) & 255] ^ (c >> 8); return ~c; }
static void be32(std::vector<unsigned char>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((unsigned char)(x >> (i * 8))); }
static void pngChunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& d) { be32(out, (uint32_t)d.size()); std::vector<unsigned char> t(type, type + 4); t.insert(t.end(), d.begin(), d.end()); out.insert(out.end(), t.begin(), t.end()); be32(out, crc32(0, t.data(), t.size())); }
static void writePngBgra(const std::string& path, const unsigned char* bgra, int w, int h) {
  std::vector<unsigned char> raw; raw.reserve((size_t)(w * 3 + 1) * h); double sum[3] = {0, 0, 0};
  for (int y = 0; y < h; ++y) { raw.push_back(0); for (int x = 0; x < w; ++x) { const unsigned char* p = bgra + ((size_t)y * w + x) * 4; raw.push_back(p[2]); raw.push_back(p[1]); raw.push_back(p[0]); sum[0] += p[2]; sum[1] += p[1]; sum[2] += p[0]; } }
  std::vector<unsigned char> z = {0x78, 0x01}; size_t pos = 0; uint32_t a = 1, b = 0; for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  while (pos < raw.size()) { size_t n = std::min<size_t>(65535, raw.size() - pos); const bool last = pos + n >= raw.size(); z.push_back(last ? 1 : 0); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255); z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n); pos += n; }
  be32(z, (b << 16) | a);
  std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, ihdr; be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h); ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  pngChunk(out, "IHDR", ihdr); pngChunk(out, "IDAT", z); pngChunk(out, "IEND", {});
  FILE* f = fopen(path.c_str(), "wb"); if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
  const double n = (double)w * h; nc::logf("shot %s: %dx%d, mean colour r %.1f g %.1f b %.1f (of 255)", path.c_str(), w, h, sum[0] / n, sum[1] / n, sum[2] / n);
}
static std::string readText(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb"); if (!f) return {}; fseek(f, 0, SEEK_END); std::string s((size_t)ftell(f), '\0'); fseek(f, 0, SEEK_SET); if (fread(&s[0], 1, s.size(), f) != s.size()) s.clear(); fclose(f); return s;
}

int appRun(int argc, char** argv, NativeWindow host, const std::atomic<bool>* hostVisible, const std::atomic<bool>* hostStop, std::atomic<int>* hostFrameCap) {
  resetMatchOptions(); g_started = false; g_hudFeed = false; g_hudReady = false; g_audioFeed = false;
  optBench = getenv("QRAZY_NC_BENCH") != nullptr;   // also available to the embedded client
  g_deferTextJs = false; g_deferredTextCount = 0; g_watchPaused = false;
  bool pageHudFeed = false, pageAudioFeed = false;
  std::vector<std::pair<std::string, uint64_t>> funnyPacks;
  bool netFeed = false; std::string profileId;
  bool windowed = false; int w = 1280, h = 720, maxFrames = 0; double capFps = 0; nc::PresentMode pm = nc::PresentMode::Immediate;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* n) { return !strcmp(argv[i], n) && i + 1 < argc; };
    if (!strcmp(argv[i], "--windowed")) windowed = true;
    else if (!strcmp(argv[i], "--hud-feed")) pageHudFeed = true;
    else if (!strcmp(argv[i], "--net-feed")) netFeed = true;
    else if (is("--profile-id")) profileId = argv[++i];
    else if (!strcmp(argv[i], "--audio-feed")) pageAudioFeed = true;
    else if (is("--pm")) { std::string v = argv[++i]; pm = v == "mailbox" ? nc::PresentMode::Mailbox : v == "fifo" ? nc::PresentMode::Fifo : nc::PresentMode::Immediate; }
    else if (is("--size")) sscanf(argv[++i], "%dx%d", &w, &h);
    else if (is("--frames")) maxFrames = atoi(argv[++i]);
    else if (is("--cap")) capFps = atof(argv[++i]);
    else if (is("--sound")) optSound = argv[++i]; else if (is("--hub")) optHub = argv[++i]; else if (is("--name")) optName = argv[++i]; else if (is("--voice")) optVoice = argv[++i]; else if (is("--volume")) optVolume = atof(argv[++i]); else if (!strcmp(argv[i], "--nosound")) optNoSound = true;
    else if (is("--pk3") || is("--oa-pk3") || is("--pooled-pk3")) {
      const int tier = is("--oa-pk3") ? 1 : is("--pooled-pk3") ? 0 : 2;
      optPk3.emplace_back(argv[++i], tier);
    }
    else if (is("--funny-pk3")) {
      const std::string spec = argv[++i];
      const auto at = spec.rfind('@');
      if (at != std::string::npos) funnyPacks.emplace_back(spec.substr(0, at), strtoull(spec.c_str() + at + 1, nullptr, 10));
    }
    else if (is("--file")) optFiles2.push_back(argv[++i]);
    else if (is("--fov")) optFov = atof(argv[++i]); else if (!strcmp(argv[i], "--runway")) optRunway = true; else if (!strcmp(argv[i], "--show-triggers")) optShowTriggers = true; else if (is("--op")) optOp = argv[++i]; else if (is("--weapon")) optWeapon = atoi(argv[++i]); else if (!strcmp(argv[i], "--attack")) optAttack = true; else if (!strcmp(argv[i], "--fxtest")) optFxTest = true; else if (is("--lookpitch")) optLookPitch = atof(argv[++i]);
    else if (is("--fakepeer")) { FakePeer fp; char nm[64] = "", cl[16] = "#ffffff"; if (sscanf(argv[++i], "%lf,%lf,%lf,%lf,%d,%63[^,],%15s", &fp.x, &fp.y, &fp.z, &fp.yawDeg, &fp.weapon, nm, cl) >= 5) { fp.name = nm; fp.color = cl; optFakePeers.push_back(fp); } }
    else if (is("--bind")) { const std::string spec = argv[++i]; const size_t eq = spec.find('='); if (eq != std::string::npos) { std::vector<std::string> codes; size_t p = eq + 1; while (p <= spec.size()) { size_t e = spec.find(',', p); if (e == std::string::npos) e = spec.size(); if (e > p) codes.push_back(spec.substr(p, e - p)); p = e + 1; } optBinds.push_back({spec.substr(0, eq), codes}); } }   // --bind jump=Space,KeyV
    else if (is("--fonts")) optFonts = argv[++i]; else if (is("--hudassets")) optHudAssets = argv[++i]; else if (!strcmp(argv[i], "--nohud")) optNoHud = true;
    else if (is("--root")) optRoot = argv[++i]; else if (is("--bsp")) optBsp = argv[++i]; else if (is("--scripts")) optScripts = argv[++i]; else if (is("--files")) optFiles = argv[++i]; else if (is("--fixes")) optFixes = argv[++i];
    else if (is("--yaw")) { optYaw = atof(argv[++i]); optHasYaw = true; }
    else if (is("--pos")) optHasPos = sscanf(argv[++i], "%lf,%lf,%lf", &optPos[0], &optPos[1], &optPos[2]) == 3;
    else if (is("--sens")) optSens = atof(argv[++i]);
    else if ((!strcmp(argv[i], "--settings") || !strcmp(argv[i], "--pagehud")) && i + 1 < argc) { const bool isSettings = !strcmp(argv[i], "--settings"); FILE* bf = fopen(argv[++i], "rb"); if (bf) { std::string j; char bb[4096]; size_t bn; while ((bn = fread(bb, 1, sizeof bb, bf)) > 0) j.append(bb, bn); fclose(bf); if (isSettings) appSetSettings(j); else appSetPageHud(j); } }   // --settings FILE / --pagehud FILE: what the page would send (for captures)
    else if (!strcmp(argv[i], "--board") && i + 1 < argc) { FILE* bf = fopen(argv[++i], "rb"); if (bf) { std::string j; char bb[4096]; size_t bn; while ((bn = fread(bb, 1, sizeof bb, bf)) > 0) j.append(bb, bn); fclose(bf); appSetBoard(j); } }   // --board FILE: a board message to draw (for captures)
    else if (!strcmp(argv[i], "--hidden")) { optHidden = true; windowed = true; }   // never a fullscreen popup: a hidden run is a window that is created without WS_VISIBLE and never shown
    else if (!strcmp(argv[i], "--facepickup")) optFacePickup = true; else if (!strcmp(argv[i], "--bench")) optBench = true;
    else if (is("--shot")) optShot = argv[++i]; else if (is("--shotframe")) optShotFrame = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--orbit")) optOrbit = true; else if (!strcmp(argv[i], "--sim")) optSim = true; else if (!strcmp(argv[i], "--pql")) optPql = true; else if (!strcmp(argv[i], "--wmode") && i + 1 < argc) optWeaponS = argv[++i][0] == 's'; else if (!strcmp(argv[i], "--hook")) optHook = true;
  }
  const bool networkMatch = optSim && nc::coreNet() && (host ? netFeed : !optHub.empty());
  const bool sharedMatch = optSim && (pageHudFeed || (!host && networkMatch)) && nc::coreHasMatchHud();
  auto& hub = nc::appHub();
  struct EndNetworkMap {
    nc::Hub& hub;
    bool standalone;
    ~EndNetworkMap() { hub.mapLoading(); if (standalone) hub.shutdown(); }
  } endNetworkMap{hub, !host};
  // Interface 11 retains its lifecycle gate after STOP. Re-arm it for a later legacy match in this process.
  static bool startedSharedMatch = false;
  const bool legacyAfterShared = optSim && !sharedMatch && startedSharedMatch;
  struct StopMatch {
    bool managed;
    ~StopMatch() { if (managed) nc::matchOp(QC_MATCH_STOP); }
  };
  QcHudFrame sessionHud{};
  nc::MatchHudFeed matchFeed;
  nc::MatchChat matchChat;
  const auto setLoading = [](bool on, double fraction, std::string_view text = {}) {
    if (text.size() > 255) {
      size_t end = 255;
      while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
      text = text.substr(0, end);
    }
    nc::matchOp(QC_MATCH_LOADING, on, fraction, 0, text);
  };
  nc::shaderInit();
  { std::vector<uint32_t> sp; std::string lg;   // self-check: a Vulkan-dialect vertex shader in the core's style must compile in process
    const bool ok = nc::compileGlsl(nc::ShaderStage::Vertex, "#version 450\nlayout(location=0) in vec3 position;\nvoid main(){gl_Position=vec4(position,1.0);}\n", sp, lg);
    nc::logf("glslang self-check: %s (%zu words)", ok ? "ok" : lg.c_str(), sp.size()); if (!ok) return 1; }
  platProcess();
  const Plat plat = platCreate(host, windowed, w, h); NativeWindow hwnd = plat.win;
  platSize(hwnd, g_w, g_h);
  platAttachInput(plat, host, hostVisible);
  g_input.cacheCodes();
  g_input.typing = false; g_input.cursor = false; g_input.paused = false; g_input.followingPov = false; g_input.releaseAllKeys(); g_input.menuRequested = false; g_input.quitRequested = false; { std::lock_guard<std::mutex> pg(g_poseMu); g_poseValid = false; }   // process-wide input state does not carry over from the previous match
  {   // before the sim loads (the spawn takes the start weapon): the page's latest settings message, else the defaults plus the command line's options (captures)
    std::string j; { std::lock_guard<std::mutex> g(g_pageMu); j = g_settingsJson; }
    if (!j.empty()) { g_settingsDirty = true; if (!coreSettings(j) && !g_settingsOk) coreSettings(std::string()); }   // dirty: the first frame applies the host part to this match's objects
    else {
      coreSettings(std::string());
      std::ostringstream o; const char* sep = "{";
      const auto key = [&](const char* k) -> std::ostringstream& { o << sep << '"' << k << "\":"; sep = ","; return o; };
      if (optSens > 0) key("sensitivity") << optSens;
      if (optFov > 0) key("fov") << optFov;
      if (optVolume >= 0) key("volume") << optVolume;
      if (!optVoice.empty()) key("voice") << std::quoted(optVoice);
      if (!host && optWeapon) key("startWeapons") << "{\"w\":" << optWeapon << ",\"s\":" << optWeapon << ",\"hook\":" << optWeapon << '}';
      if (!optBinds.empty()) {
        key("binds") << '{';
        for (size_t b = 0; b < optBinds.size(); ++b) { o << (b ? "," : "") << std::quoted(optBinds[b].first) << ":["; for (size_t i = 0; i < optBinds[b].second.size(); ++i) o << (i ? "," : "") << std::quoted(optBinds[b].second[i]); o << ']'; }
        o << '}';
      }
      if (!o.str().empty()) coreSettings(o.str() + "}");
    }
  }
  g_set = qc_ses_settings_values();   // after the first install: reading the values first would make the core count the defaults as already applied
  const auto syncCapture = [&] { g_input.setCapture(optSim && !optHidden && !g_input.mouseFree() && (!host || hostVisible->load())); };   // the one rule for grabbing the mouse: playing, shown, and no text box / clickable element wants it free
  if (!nc::loadLoader()) nc::fatal("the Vulkan loader was not found");
  nc::Context c; if (!nc::createContext(c, hwnd, plat.instance)) nc::fatal("no GPU can present to this window");
  VkRenderPass rp = nc::makeScreenRenderPass(c, VK_FORMAT_B8G8R8A8_UNORM, true);
  nc::Swapchain sc; nc::buildSwapchain(c, sc, g_w, g_h, pm, rp);
  struct GpuGuard { nc::Context& c; nc::Swapchain& sc; bool done = false; ~GpuGuard() { if (done) return; try { if (c.dev) vkDeviceWaitIdle(c.dev); nc::destroySwapchain(c, sc); nc::destroyContext(c); } catch (...) {} } } gpuGuard{c, sc};   // a fatal error unwinds through here: the window must still be released for the next start
  StopMatch stopMatch{sharedMatch || legacyAfterShared};
  nc::logf("swapchain %dx%d, %zu images, present mode requested %d got %d", sc.w, sc.h, sc.images.size(), (int)sc.requested, (int)sc.actual);

  // ---- the map (optional: without --root the loop just clears)
  nc::PostPass post; uint32_t pathRevision = 0;
  nc::FileSource fs; fs.root = optRoot;
  nc::World world; bool haveWorld = false; double eye[3] = {0, 100, 0}, yaw0 = 0, pitch = 0, fov = g_set->fov;
  for (const auto& [pk, tier] : optPk3) { std::string p = pk; uint64_t off = 0; const size_t at = pk.rfind('@'); if (at != std::string::npos && at + 1 < pk.size() && pk.find_first_not_of("0123456789", at + 1) == std::string::npos) { p = pk.substr(0, at); off = strtoull(pk.c_str() + at + 1, nullptr, 10); }   // "file@offset": an archive inside a larger file
    std::string err; if (!fs.mountZip(p, &err, off, tier)) nc::fatal("%s", err.c_str()); nc::logf("mounted %s", p.c_str()); }
  for (const std::string& spec : optFiles2) {   // "assets/models/x.glb=C:/path/file@4096+12345"
    const size_t eq = spec.find('='), at = spec.rfind('@'), plus = spec.rfind('+'); if (eq == std::string::npos || at == std::string::npos || plus == std::string::npos || !(eq < at && at < plus)) { nc::logf("bad --file %s", spec.c_str()); continue; }
    fs.mountFile(spec.substr(0, eq), spec.substr(eq + 1, at - eq - 1), strtoull(spec.c_str() + at + 1, nullptr, 10), strtoull(spec.c_str() + plus + 1, nullptr, 10));
  }
  if (!g_assetsDir.empty()) { optFonts = g_assetsDir + "/fonts"; optHudAssets = g_assetsDir + "/hud"; }   // the client's own copies, next to the program
  nc::Hud hud; bool haveHud = false;
  if (optSim && !optNoHud) { haveHud = hud.init(c, rp, optFonts, optHudAssets); if (haveHud) hud.clearAllElements(); }   // the core's element table outlives a match (one process, one core): a new match starts empty (the page's new recorder resends every live element, but never clears the stale ones, e.g. the vote line)
  if (haveHud) g_hudPtr = &hud;
  struct HudPtrReset { ~HudPtrReset() { g_hudPtr = nullptr; } } hudPtrReset;   // declared after hud: gone before it
  if (haveHud) nc::tf::reset(); g_input.textState = 0;
  if (haveHud) g_input.textKey = [](uint32_t k, uint32_t mods, bool down, uint32_t id) {   // every key while the core's text field is open, and the console key always (Input::keyScan)
    const uint32_t before = nc::tf::state(); const uint32_t r = nc::tf::key(k, mods, down, id); textSync(before); return r; };
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = c.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VkCommandBuffer cb; NC_CK(vkAllocateCommandBuffers(c.dev, &cai, &cb));
  VkQueryPool qpool; { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 4; NC_CK(vkCreateQueryPool(c.dev, &qi, nullptr, &qpool)); }
  bool qValid = false, mmQueryValid = false;
  std::vector<double> benchInterval, benchCpu, benchGpu, benchMinimapCpu, benchMinimapGpu;
  uint64_t benchMinimapAllocs = 0;
  if (optBench) { benchMinimapCpu.reserve(262144); benchMinimapGpu.reserve(262144); }
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; VkFence fence; NC_CK(vkCreateFence(c.dev, &fi, nullptr, &fence));
  VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acq; NC_CK(vkCreateSemaphore(c.dev, &semi, nullptr, &acq));
  // ---- the loading screen: the core's loading element 21 (text + bar, what the page shows as #mp-loading), drawn between the load stages so the native view is on screen and alive while the map builds
  std::string loadName = optBsp.substr(optBsp.find_last_of('/') == std::string::npos ? 0 : optBsp.find_last_of('/') + 1); loadName = loadName.substr(0, loadName.find_last_of('.')); int64_t loadShownAt = 0;
  const auto showLoading = [&](const std::string& text, double fraction, bool force) {
    if (!haveHud) return; const int64_t now = nc::qpcNow(); if (!force && loadShownAt && (now - loadShownAt) * 1000 < 40 * nc::qpcFrequency()) return; loadShownAt = now;   // at most ~25 frames a second: each one waits for the GPU
    NC_CK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    uint32_t idx; const VkResult ar = vkAcquireNextImageKHR(c.dev, sc.sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx); if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return;
    NC_CK(vkResetFences(c.dev, 1, &fence));
    vkResetCommandBuffer(cb, 0); VkCommandBufferBeginInfo bi2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb, &bi2);
    if (sharedMatch) {
      setLoading(true, fraction, text);
      nc::matchHud(sessionHud);
      hud.prepareLoading(cb, sc.w, sc.h, sessionHud.loadingText, sessionHud.loadFraction);
    } else hud.prepareLoading(cb, sc.w, sc.h, text, fraction);
    VkClearValue cv[2] = {}; cv[0].color = {{0.04f, 0.07f, 0.09f, 1}}; cv[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = rp; rb.framebuffer = sc.fbs[idx]; rb.renderArea = {{0, 0}, {(uint32_t)sc.w, (uint32_t)sc.h}}; rb.clearValueCount = 2; rb.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); hud.record(cb, sc.w, sc.h); vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &acq; sub.pWaitDstStageMask = &wait;
    sub.commandBufferCount = 1; sub.pCommandBuffers = &cb; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &sc.done[idx]; NC_CK(vkQueueSubmit(c.queue, 1, &sub, fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sc.done[idx]; pi.swapchainCount = 1; pi.pSwapchains = &sc.sc; pi.pImageIndices = &idx; vkQueuePresentKHR(c.queue, &pi);
  };
  if (haveHud) {   // the page's pixel ratio is in its settings message that may already be waiting (the main loop applies it later): the loading text must have the right size
    std::string j; { std::lock_guard<std::mutex> g(g_pageMu); j = g_settingsJson; } nc::J r; if (!j.empty() && nc::parseJson(j, r) && r.t == nc::J::Obj) { const nc::J* ho = r.get("host"); const double dpr = ho ? ho->num("dpr", 0) : 0; if (dpr > 0) hud.setUiScale((float)dpr); } }
  world.onProgress = [&](const char* stage, double f) { if (host) hub.service(); showLoading("Loading " + loadName + ": " + stage, f, false); };
  if (!optRunway && !optRoot.empty()) showLoading("Loading " + loadName + "â¦", -1, true);
  if (optRunway) {   // the built-in runway: no map, the core builds its model (rc_model_build kind 16, the sim's own frame, identity placement)
    haveWorld = world.loadObjectsOnly(c, fs, rp); if (!haveWorld) nc::fatal("objects-only scene failed");
    { const double* fog = rc_runway_fog(); const float clearSrgb[3] = {(float)fog[0], (float)fog[1], (float)fog[2]}, fogLinear[3] = {(float)fog[3], (float)fog[4], (float)fog[5]}; world.setFog(1, fogLinear, (float)fog[6], clearSrgb); }   // the core's FogExp2 and clear colour
    const int runway = world.addCoreModel(16, 20000.0); if (runway < 0) nc::fatal("the runway model could not be built"); const double origin[3] = {0, 0, 0}; world.setObject(runway, origin, 0, true);
  } else if (!optRunway && (!optRoot.empty() || !optPk3.empty())) {
    { std::string j; { std::lock_guard<std::mutex> g(g_pageMu); j = g_settingsJson; } if (!j.empty()) coreSettings(j); }   // the message that arrived since the start: picmip and mapFx are read by the load
    world.setLoadOptions((int)g_set->picmip, g_set->mapFx != 0);
    haveWorld = world.load(c, fs, optBsp, optScripts, optFiles, rp, readText(optFixes));
    if (!haveWorld) nc::fatal("map load failed");
    if (!world.spawn(eye, yaw0)) nc::logf("no spawn entity; camera at (0,100,0)");
    if (optHasPos) memcpy(eye, optPos, sizeof eye); if (optHasYaw) yaw0 = optYaw;
  }
  // ---- the simulation
  struct Pickup { int handle, index; double pos[3]; }; std::vector<Pickup> pickups;
  std::map<std::string, std::vector<int>> pickupHandles;
  static_assert(sizeof(QcCameraInput) == 264 && offsetof(QcCameraInput, mode) == 184);
  static_assert(sizeof(QcCamera) == 168 && offsetof(QcCamera, gun) == 104 &&
                offsetof(QcCamera, zoomed) == 160 && offsetof(QcCamera, inputFlags) == 164);
  QcCamera camera{};
  bool cameraReset = true, haveCamera = false;
  bool inputHistory = false;
  struct { bool on = false, attached = false; double pos[3] = {0, 0, 0}, time = 0; } ownHook;   // the own cable as of the last tick that moved it
  nc::PeerFxView peerFx; nc::AvatarView avatars; nc::Ghost ghost; ghost.clear();   // other players, their shots and the leaderboard ghost: the core's records
  const double* bounds = world.mapBounds();
  const double mmCx = (bounds[0] + bounds[3]) / 2, mmCz = (bounds[2] + bounds[5]) / 2;
  const double mmSide = std::max(bounds[3] - bounds[0], bounds[5] - bounds[2]) * 1.08 + 100;
  const double mmMinY = bounds[1];
  if (haveWorld && haveHud) world.warmPipelines();
  nc::FollowClock ghostClock; QcComposeOut compose{}; QcMinimapOut mmView{}; bool mmReset = true; static float mmGhost[QC_MINIMAP_TRACK_POINTS * 2];   // the world composition: the core decides, the client draws
  std::map<int, int> ownGun; struct CustomItem { int handle; double pos[3]; }; std::vector<CustomItem> customItems;   // /custommodel (ownGun: the built-in handle a custom one replaced), /custompickup
  int followN = -1; QcPeerPose followPose; bool haveFollow = false;   // op follow: -1 playing, 0 free fly, > 0 the followed player's number
  std::string watchedKeysSender;
  uint32_t watchedKeys = 0;
  unsigned watchedKeysGeneration = 0;
  double slickCheckAt = 0;
  bool slickBelow = false;
  std::map<int, int> gunHandle;
  std::map<std::string, RcPickupVisual> pickupVisuals;
  std::array<bool, 11> customFire{};
  double gunKick = 0;
  std::vector<uint8_t> simBsp; std::string simFixes, simName;   // what the sim was loaded from: a respawn loads it again (the core has no reset call)
  nc::Sim sim; nc::PlayerView cur; std::vector<uint8_t> poseBytes; int nPoses = 0; bool poseDirty = true; bool poseNowWanted = false; std::string heldEvents;   /* restart / page command: run events (JSON objects) waiting for the next state push */
  double simPushAt = -1e9; int pushTainted = -1, pushRun = -1;
  const auto loadSim = [&]() {
    if (!sharedMatch) sim.setMode(optWeaponS, optHook);
    return optRunway ? sim.loadRunway(simFixes, !optPql, sharedMatch)
                     : sim.loadMap(simBsp, simFixes, simName, !optPql, sharedMatch);
  };
  nc::J modelManifest;
  const auto modelFile = [&](const char* key) -> std::string {
    const nc::J* models = modelManifest.get("models");
    if (!*key) return {};
    const std::string mounted = std::string("assets/models/by-name/") + key + ".glb";
    if (fs.exists(mounted)) return mounted;
    const nc::J* entry = models ? models->get(key) : nullptr;
    const std::string file = entry ? entry->str("file") : "";
    if (file.size() <= 4 || file.substr(file.size() - 4) != ".glb" ||
        file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) return {};
    return "assets/models/" + file;
  };
  const auto syncPickups = [&] {
    if (!haveWorld) return;
    for (const Pickup& pickup : pickups) world.setObject(pickup.handle, pickup.pos, 0, false);
    pickups.clear();
    std::map<std::string, size_t> used;
    for (const auto& spot : sim.items()) {
      const auto visual = pickupVisuals.find(spot.classname);
      if (visual == pickupVisuals.end()) continue;
      const auto& row = visual->second;
      auto& handles = pickupHandles[spot.classname];
      size_t& next = used[spot.classname];
      if (next == handles.size()) {
        const int handle = world.addPickup(row.weapon, modelFile(row.model), row.icons, row.size,
                                           row.marker, row.glow, row.radius, row.stretch);
        if (handle < 0) continue;
        handles.push_back(handle);
      }
      const int handle = handles[next++];
      pickups.push_back({handle, spot.index, {spot.pos[0], spot.pos[1], spot.pos[2]}});
      world.setObject(handle, spot.pos, 0, sim.itemVisible(spot.index));
    }
    nc::logf("pickups: %zu placed", pickups.size());
  };
  const bool sessionAudio = optSim && nc::soundEvent(QC_SOUND_RESET) == 0;
  if (optSim) {
    if (sharedMatch) nc::matchOp(QC_MATCH_MODE, !optPql, optWeaponS, optHook);
    simFixes = readText(optFixes);
    if (optRunway) { optMapName = "runway"; if (!loadSim()) nc::fatal("sim: %s", sim.error().c_str()); }
    else {
    std::vector<uint8_t>& bsp = simBsp; if (!haveWorld || !fs.read(optBsp, bsp)) nc::fatal("--sim needs --root and --bsp");
    std::string name = optBsp.substr(optBsp.find_last_of('/') == std::string::npos ? 0 : optBsp.find_last_of('/') + 1); name = name.substr(0, name.find_last_of('.')); optMapName = name;
    simName = name; if (!loadSim()) nc::fatal("sim: %s", sim.error().c_str());
    }
    cur = sim.player(); g_input.yaw = optHasYaw ? optYaw * M_PI / 180.0 : sim.spawnYaw(); g_input.pitch = sim.spawnPitch(); if (optLookPitch > -999) g_input.pitch = optLookPitch * M_PI / 180.0;   // the map start looks where the map says (--yaw DEG overrides)
    nc::logf("sim ready: %d movers, start (%.1f %.1f %.1f)", sim.moverCount(), cur.pos[0], cur.pos[1], cur.pos[2]);
    if (haveWorld) {
      const char* manifestPath = "assets/models/manifest.json";
      std::vector<uint8_t> bytes;
      std::string manifestText;
      if (fs.read(manifestPath, bytes)) manifestText.assign(bytes.begin(), bytes.end());
      else if (!g_assetsDir.empty()) manifestText = readText(g_assetsDir + "/" + manifestPath);
      if (!nc::parseJson(manifestText, modelManifest)) modelManifest = {};
      for (int i = 0; ; ++i) {
        RcViewWeaponVisual row{};
        if (!rc_view_weapon_visual(i, &row)) break;
        const std::string file = modelFile(row.model);
        int handle = file.empty() ? -1 : world.addViewWeaponFile(file, row.kind);
        const bool preferred = handle >= 0;
        if (!preferred) handle = world.addViewWeapon(row.kind);
        if (handle >= 0) gunHandle[row.weapon] = handle;
        nc::logf("view weapon %d: %s (kind %d)", row.weapon,
                 preferred ? file.c_str() : handle >= 0 ? "built-in" : "unavailable", row.kind);
      }
      for (int i = 0; ; ++i) {
        RcPickupVisual row{};
        if (!rc_pickup_visual(i, &row)) break;
        pickupVisuals.emplace(row.classname, row);
      }
      syncPickups();
    }
    syncCapture();
    if (optFacePickup && !pickups.empty()) {   // --facepickup: turn toward the nearest pickup (for captures)
      size_t best = 0; double bd = 1e30; for (size_t k = 0; k < pickups.size(); ++k) { const double dx = pickups[k].pos[0] - cur.pos[0], dz = pickups[k].pos[2] - cur.pos[2], d = dx * dx + dz * dz; if (d < bd) { bd = d; best = k; } }
      const double dx = pickups[best].pos[0] - cur.pos[0], dy = pickups[best].pos[1] - (cur.pos[1] + 50), dz = pickups[best].pos[2] - cur.pos[2];
      g_input.yaw = atan2(dx, dz); g_input.pitch = atan2(dy, sqrt(dx * dx + dz * dz)); nc::logf("facing the nearest pickup, %.0f units away", sqrt(bd));
    }
  }

  // the pickups' motion records (qc_ses_items): the map's pickups first, then the custom ones; room for every one made now
  constexpr size_t CUSTOM_ITEMS = 24; std::vector<QcItemSpin> spins; double itemsAt = -1;
  const auto addSpin = [&](const double p[3], bool visible = true) { QcItemSpin it{}; it.baseY = it.y = p[1]; it.phase = fmod(p[0] * 0.013 + p[2] * 0.017, 6.283); it.visible = visible; spins.push_back(it); };
  const auto resetPickupSpins = [&] {
    spins.clear();
    spins.reserve(pickups.size() + CUSTOM_ITEMS);
    for (const Pickup& pk : pickups) addSpin(pk.pos, sim.itemVisible(pk.index));
    for (const CustomItem& item : customItems) addSpin(item.pos);
  };
  customItems.reserve(CUSTOM_ITEMS);
  resetPickupSpins();
  // the !showtriggers overlay for the held map and physics: built hidden at every load and again after a mode op (the physics decides which entities exist); its label images are uploaded here
  const auto buildTriggers = [&] { if (!haveWorld) return; world.buildTriggerOverlay(optSim ? simFixes : readText(optFixes), loadName, !optPql, optRunway); world.showTriggerOverlay(optShowTriggers || g_showTriggers); };
  buildTriggers();
  if (haveHud && (haveWorld && !optRunway)) { world.onProgress = nullptr; showLoading("Loading " + loadName + ": starting", 0.98, true); hud.endLoading(); }
  nc::Audio audio; bool haveAudio = false;
  if (sessionAudio && !optNoSound) { haveAudio = audio.init(&fs); }
  const bool pageSoundFeed = pageAudioFeed && sessionAudio && haveAudio;
  // what the client itself does with the settings values: once at the start and after each change
  const auto applyValues = [&]() {
    if (haveAudio) { audio.setVolume((float)g_set->volume, (float)g_set->footstepVolume, (float)g_set->ambient); audio.setVoice(g_set->voice); }
    if (haveWorld) { world.fxSetMarkTime(g_set->markTime); world.setMirrors(g_set->mirrors != 0); world.setRender(g_set->sortTranslucent != 0, g_set->volumeFog != 0, g_set->fastSky != 0); }
    if (haveHud) { hud.setConsoleLook(g_set->conHeight, g_set->conOpacity, g_set->conScale); hud.setSpeedometer((uint32_t)g_set->speedometer, g_speedoMax); }
  };
  applyValues();
  nc::Scoreboard board;
  struct ChatLine { int64_t at; std::string text; }; std::vector<ChatLine> chatLog; std::string chatShown; bool chatHistory = false; bool pageChat = false, chatOpen = false, consoleOpen = false, typingOn = false; std::string typingText, typingLabel, typingHint; uint32_t typingCaret = 0, typingFrom = 0, typingTo = 0;   // pageChat: the page sends the chat lines (then the native hub chat is not shown twice)   // the feed: the last 6 lines of the last 8 seconds
  bool haveNet = false;
  if (networkMatch && !host) {
    const char* token = getenv("QRAZY_TOKEN");
    hub.openStandalone(optHub, "{\"type\":\"hello\",\"name\":" + nc::jsonQuote(optName) +
                               ",\"token\":" + (token ? nc::jsonQuote(token) : "null") + "}");
  }
  int64_t nextTestSound = 0;
  if (optRunway && haveHud) {   // the runway's text boards: the core's rows (text, colour, place, size, turn)
    RcRunwayBoard row;
    for (int i = 0; rc_runway_board(i, &row); ++i) {
      if (text_board_bake((const uint8_t*)row.text, (uint32_t)strnlen(row.text, sizeof row.text), row.rgba) != 1) { nc::logf("board \"%s\": bake failed", row.text); continue; }
      const int h = world.addBoard(text_board_pixels(), TEXT_BOARD_W, TEXT_BOARD_H, row.z, row.y, row.width / 2, row.height / 2, row.yaw);   // addBoard copies the pixels (the next bake overwrites them)
      nc::logf("board \"%s\" at z %.0f: handle %d", row.text, row.z, h);
    }
  }
  int shownFps = -1; double p_listener[3] = {0, 0, 0}; std::array<QcCue, 128> cueBuf;
  const auto drainAudio = [&] {
    if (!sessionAudio) return;
    for (;;) {
      const int count = qc_ses_cues(cueBuf.data(), int(cueBuf.size()));
      if (count == -2) { nc::logf("session audio overflow; draining retained cues"); continue; }
      if (count == -1) nc::logf("session audio drain rejected");
      if (count <= 0) break;
      if (haveAudio) for (int i = 0; i < count; ++i) {
        const QcCue& cue = cueBuf[i];
        audio.cue(cue.name, cue.pos, cue.gain, cue.flags, cue.emitter,
          cue.group == 1 ? nc::SoundGroup::Footstep : cue.group == 2 ? nc::SoundGroup::Ambient : nc::SoundGroup::Effects);
      }
    }
  };
  const auto soundMute = [&](bool active) {
    if (haveAudio) audio.setMuted(g_set->muteBackground != 0 && !g_input.focused());
    if (!sessionAudio) return;
    QcCue mute{};
    const bool chat = (nc::tf::state() & (TF_STATE_CHAT | TF_STATE_TEAM)) != 0;
    mute.flags = int(active) | (int(chat) << 1) | (int(haveNet) << 2);
    nc::soundEvent(QC_SOUND_MUTE, mute);
  };
  const auto resetPeerSound = [&] {
    if (!sessionAudio) return;
    nc::soundEvent(QC_SOUND_FOLLOW_END);
    nc::soundEvent(QC_SOUND_RESET);
  };

  if (!networkMatch) qc_peer_reset();
  peerOptions(0);
  int64_t fakeAt = 0;
  if (!networkMatch && nc::coreNet() && !optFakePeers.empty()) { std::string w = "{\"type\":\"welcome\",\"id\":\"me\",\"n\":1,\"peers\":["; for (size_t i = 0; i < optFakePeers.size(); ++i) { const FakePeer& f = optFakePeers[i]; if (i) w += ','; w += "{\"id\":\"fake" + std::to_string(i + 2) + "\",\"n\":" + std::to_string(i + 2) + ",\"name\":\"" + f.name + "\",\"color\":\"" + f.color + "\",\"x\":0,\"y\":0,\"z\":0,\"yaw\":0,\"spec\":false,\"loading\":false,\"phys\":\"pql\",\"wm\":\"s\",\"profileId\":\"\",\"paused\":false,\"lag\":false,\"g\":false}"; } w += "]}"; qc_peer_reset(); peerOptions(0); nc::coreNet().in(0, reinterpret_cast<const uint8_t*>(w.data()), int(w.size()), nc::inputSeconds(nc::qpcNow())); }
  uint64_t chatRevision = 0;
  nc::LatencyLog latency;
  int64_t pumpEnd = 0;
  const auto pump = [&] {
    const bool before = g_input.focused();
    g_input.pollFocus();
    if (!before && g_input.focused()) syncCapture();
    platPump(host);
    if (latency.active()) pumpEnd = nc::qpcNow();
  };
  unsigned netGeneration = hub.generation();
  uint32_t speedRevision[3]{};
  const auto advertiseMode = [&] {
    if (networkMatch) hub.fact(QC_NET_PHYS, optPql ? 1 : 2,
                               optHook ? 'h' : optWeaponS ? 's' : 'w', 0, profileId);
  };
  const auto netService = [&](bool suspended = false) {
    const double now = nc::inputSeconds(nc::qpcNow());
    if (!networkMatch && nc::coreNet() && !optFakePeers.empty() && nc::qpcNow() >= fakeAt) { fakeAt = nc::qpcNow() + nc::qpcFrequency() * 66 / 1000; for (size_t i = 0; i < optFakePeers.size(); ++i) { const FakePeer& f = optFakePeers[i]; QcWireState ws; memset(&ws, 0, sizeof ws); ws.full = 1; ws.number = (int)i + 2; ws.x = f.x; ws.y = f.y; ws.z = f.z; ws.yaw = f.yawDeg * M_PI / 180.0; ws.weapon = f.weapon; uint8_t b[32]; const int n = qc_wire_encode_peer(&ws, ws.number, 1, b, sizeof b); if (n > 0) nc::coreNet().in(1, b, n, now); } }
    if (networkMatch) {
      hub.fact(QC_NET_WATCH_VIEW, followN < 0, g_watchPaused || suspended || g_input.typing.load());
      hub.service([&](int bits, const std::string& text) {
        if (!(bits & QC_NET_FOLLOW_STATE) &&
            (!(bits & QC_NET_FOLLOW_SOUND) || !sessionAudio || followN <= 0)) return;
        nc::J message;
        if (!nc::parseJson(text, message)) return;
        if (bits & QC_NET_FOLLOW_SOUND) {
          nc::watchedSound(message, followN);
        } else if (message.str("type") == "keys") {
          const double mask = message.num("k", -1);
          const std::string sender = message.str("id");
          if (!sender.empty() && mask >= 0 && mask <= 63 && std::floor(mask) == mask) {
            watchedKeysSender = sender;
            watchedKeys = static_cast<uint32_t>(mask);
            watchedKeysGeneration = hub.generation();
          }
        }
      });
      haveNet = hub.connected();
      if (netGeneration != hub.generation()) {
        netGeneration = hub.generation();
        resetPeerSound();
        haveFollow = false;
        slickBelow = false;
        slickCheckAt = 0;
        peerOptions(followN);
        if (haveNet && !host) hub.fact(QC_NET_LOADED, int(optMapName.size()), 0, 0, optMapName);
        advertiseMode();
      }
      if (followN > 0 && haveFollow) {
        QcPeerPose pose{};
        if (!qc_peer_pose(followN, &pose)) {
          if (sessionAudio) nc::soundEvent(QC_SOUND_FOLLOW_END);
          haveFollow = false;
        }
      }
      if (!host) hub.fact(QC_NET_PAUSE, g_pagePaused || suspended);
    } else if (host) {
      hub.service();
    }
    if (!sharedMatch && g_pageJs) g_runs.service(now, g_pageJs);
  };
  // ops that must work behind the menu too (the rest wait for the match to show again)
  const auto reportOp = [&](const nc::J& j, const std::string& op) {
    if (op == "runack") g_runs.ack((int)j.num("serial", 0));
    else if (op == "capture") g_runs.takeCapture((int)j.num("serial", 0));
    else if (op == "watchsound") { const std::string t = j.str("text"); qc_watch_sound((int)j.num("kind", 0), (int)j.num("value", 0), t.data(), (int)t.size()); }
    else if (op == "soundfact") {
      if (pageSoundFeed) {
        const int kind = nc::soundInt(j.num("kind"));
        if (kind == QC_SOUND_FUNNY || kind == QC_SOUND_WR_CLIP) {
          // Funny packs may finish downloading after the match starts.
          for (auto pack = funnyPacks.begin(); pack != funnyPacks.end();) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(pack->first, ec) && fs.mountZip(pack->first, nullptr, pack->second))
              pack = funnyPacks.erase(pack);
            else ++pack;
          }
        }
        nc::pageSound(j);
      }
    }
    else return false;
    return true;
  };

  const uint32_t captureAttack = !host && optAttack ? qc_ses_code("Mouse0", 6) : 0;
  const int64_t freq = nc::qpcFrequency(), t0 = nc::qpcNow();
  int64_t last = t0, ticksRun = 0;
  bool wasHidden = false;
  nc::Buf shotBuf; int shotW = 0, shotH = 0; bool shotPending = false;
  if (sharedMatch || legacyAfterShared) {
    if (nc::matchOp(QC_MATCH_START, nc::inputSeconds(t0)) < 0) nc::fatal("sim: match start refused");
    if (sharedMatch) {
      startedSharedMatch = true;
      nc::matchOp(QC_MATCH_CHAT_RESET);
      nc::matchOp(QC_MATCH_CHAT_VIEW, QC_HUD_CHAT_FEED);
      setLoading(false, -1);
      nc::matchHud(matchFeed.sent);
      nc::logf("match HUD: Stage 3b");
    }
  }
  if (!sharedMatch || !host) g_input.reset(nc::inputSeconds(t0));
  g_hudFeed = sharedMatch;
  g_audioFeed = pageSoundFeed;
  if (networkMatch) {
    hub.mapReady(optMapName);
    advertiseMode();
  }
  if (optSim) g_started = true;
  qc_ses_active(optSim, -1);
  if (optSim) latency.start(t0, freq);
  std::vector<nc::Sim::Event> evs;
  std::vector<std::string> ops;
  std::string joined, simPush, pageActions;
  pageActions.reserve(256 * 96);
  uint64_t shownRevision = UINT64_MAX;
  size_t lastShownFirst = 0;
  uint32_t shownChatFirst = UINT32_MAX, shownChatEnd = UINT32_MAX;
  g_input.recordMotion = latency.active();
  g_input.motionReceipt = {};
  long frames = 0, windowFrames = 0; double windowStart = 0, worst = 0;
  int64_t resizeStarted = 0;
  while (!g_quit && (!maxFrames || frames < maxFrames) && !(hostStop && hostStop->load())) {
    if (latency.active()) latency.begin(ticksRun);
    if (host && g_input.tabEdge) { const bool down = g_input.tabEdge == 1; g_input.tabEdge = 0; nc::logf("Tab %s (page paused %d)", down ? "down" : "up", (int)g_pagePaused); if (!down) board.held = false; if (g_tabCb) g_tabCb(down); }   // embedded: the page owns the information data, ask it
    if (g_boardDirty.exchange(false)) { std::string j; { std::lock_guard<std::mutex> g(g_boardMu); j = g_boardJson; } nc::parseScoreboard(j, board); nc::logf("board message: held %d, %zu cards, %zu groups", (int)board.held, board.cards.size(), board.groups.size()); }
    if (g_settingsDirty.exchange(false)) {   // the player's settings from the page (qrazyNativeSettings): look, view, audio, binds
      std::string j; { std::lock_guard<std::mutex> g(g_pageMu); j = g_settingsJson; } nc::J r;
      const int previousPicmip = (int)g_set->picmip;
      const bool accepted = coreSettings(j);
      if (accepted && haveWorld && (int)g_set->picmip != previousPicmip)
        world.setPicmip((int)g_set->picmip);
      if (accepted && nc::parseJson(j, r) && r.t == nc::J::Obj) {
        auto flag = [](const nc::J* o, const char* k) { const nc::J* v = o ? o->get(k) : nullptr; return v && v->t == nc::J::Bool && v->b; };
        const nc::J* ho = r.get("host");   // what only the page can work out
        const auto part = [&](const char* k) { const nc::J* v = ho ? ho->get(k) : nullptr; return v && v->t == nc::J::Obj ? v : nullptr; };
        if (const nc::J* hl = part("hud")) {
          g_speedoMax = hl->num("speedBarMax", 1200);
          if (haveHud) { std::vector<std::pair<std::string, double>> f; for (const auto& kv : hl->o) if (kv.second.t == nc::J::Num) f.push_back({kv.first, kv.second.n}); hud.setLayout(f); }
        }
        if (haveHud) { const double dpr = ho ? ho->num("dpr", 0) : 0; nc::logf("settings: page dpr %.3f", dpr); if (dpr > 0) hud.setUiScale((float)dpr); }
        if (const nc::J* mj = part("minimap")) {
          MmCfg c; c.have = true; c.zoom = (uint32_t)mj->num("zoom", 0); c.flags = (uint32_t)mj->num("flags", 0); c.boxPx = (uint32_t)mj->num("boxPx", 0); c.trailColor = (uint32_t)mj->num("trailColor", 0); c.ghostColor = (uint32_t)mj->num("ghostColor", 0);
          c.range = mj->num("range", 0); c.speedScale = mj->num("speedScale", 0); c.x = (float)mj->num("x", -1); c.y = (float)mj->num("y", -1); c.scale = (float)mj->num("scale", 1);
          { std::lock_guard<std::mutex> lk(g_mmMu); g_mm = c; }
        }
        if (const nc::J* os = part("ownShadow")) { g_ownShadow[0] = os->num("scaleX", 0); g_ownShadow[1] = os->num("scaleZ", 0); }
        if (const nc::J* cg = part("cgaz")) { g_cgazOn = flag(cg, "on"); g_cgazY = cg->num("ypos", -1); }
        if (ho) if (const nc::J* tr = ho->get("showTriggers")) if (tr->t == nc::J::Bool) { if (tr->b != g_showTriggers) nc::logf("settings from the page: trigger overlay %s", tr->b ? "on" : "off"); g_showTriggers = tr->b; if (haveWorld) world.showTriggerOverlay(optShowTriggers || g_showTriggers); }
        if (haveHud) if (const nc::J* b = r.get("binds")) if (b->t == nc::J::Obj) {   // the page's "Unpause with "P" key" line (main.js updatePausedNotice, bindings.js keyName)
          std::string hint = "Unpause with \"/pause\"";
          for (const auto& bd : b->o) if (bd.first == "softPause" && bd.second.t == nc::J::Arr && !bd.second.a.empty() && bd.second.a[0].t == nc::J::Str) {
            std::string k = bd.second.a[0].s; auto rep = [&](const char* from, const char* to) { size_t p = k.find(from); if (p != std::string::npos) k.replace(p, strlen(from), to); };
            if (k.rfind("Key", 0) == 0) k.erase(0, 3); else if (k.rfind("Digit", 0) == 0) k.erase(0, 5);
            if (k == "Mouse0") k = "Mouse 1"; else if (k == "Mouse2") k = "Mouse 2"; else if (k == "Mouse1") k = "Mouse 3";
            rep("Control", "Ctrl "); rep("Arrow", "Arrow "); rep("WheelUp", "Wheel \xE2\x86\x91"); rep("WheelDown", "Wheel \xE2\x86\x93");
            hint = "Unpause with \"" + k + "\" key"; break; }
          hud.setPausedHint(hint);
        }
        peerOptions(followN); applyValues();
        nc::logf("settings from the page applied");
      } else nc::logf("settings from the page unreadable");
    }
    if (g_hudDirty.exchange(false)) {   // chat lines and the typing line from the page (qrazyNativeHud)
      std::vector<PageHudMessage> msgs; { std::lock_guard<std::mutex> g(g_pageMu); msgs.swap(g_hudQueue); }
      for (const PageHudMessage& msg : msgs) {   // in arrival order
      nc::J r; const nc::J* chat = nullptr;
      if (!nc::parseJson(msg.json, r) || r.t != nc::J::Obj || (r.num("feed") == 1) != sharedMatch) continue;
      if (const nc::J* cu = r.get("cursor"); cu && cu->t == nc::J::Bool) g_input.cursor = cu->b;
      if (const nc::J* pu = r.get("paused"); pu && pu->t == nc::J::Bool) g_pagePaused = pu->b;
      if (sharedMatch) {
        if (const nc::J* reset = r.get("chatReset"); reset && reset->t == nc::J::Bool && reset->b) {
          nc::matchOp(QC_MATCH_CHAT_RESET);
          matchChat.reset();
          shownChatEnd = UINT32_MAX;
        }
        if (const nc::J* append = r.get("chatAppend"); append && append->t == nc::J::Arr) {
          const double receive = nc::inputSeconds(msg.receivedAt);
          for (const nc::J& entry : append->a) {
            const nc::J* line = entry.get("text");
            if (line && line->t == nc::J::Str &&
                nc::matchOp(QC_MATCH_CHAT_APPEND, receive - std::max(0.0, entry.num("ageMs")) / 1000.0) == 0)
              matchChat.append(line->s);
          }
        }
        if (const nc::J* view = r.get("chatView"); view && view->t == nc::J::Num)
          nc::matchOp(QC_MATCH_CHAT_VIEW, view->n);
        if (const nc::J* loading = r.get("loading")) {
          const bool on = loading->t == nc::J::Obj;
          setLoading(on, on ? std::clamp(loading->num("fraction", -1), -1.0, 1.0) : -1, loading->str("text"));
          if (haveHud) {
            nc::matchHud(sessionHud);
            if (sessionHud.matchFlags & QC_MATCH_LOADING_FLAG) hud.setLoading(sessionHud.loadingText, sessionHud.loadFraction);
            else hud.endLoading();
          }
        }
      } else {
        chat = r.get("chat");
        const nc::J* hi = r.get("history");
        chatHistory = hi && hi->t == nc::J::Bool && hi->b;
      }
      if (chat && chat->t == nc::J::Obj) {
        pageChat = true; ++chatRevision; chatLog.clear();   /* chat.open / console.open no longer matter here: the core's text field says whether a box is open */ const int64_t nowQ = nc::qpcNow(), fq = nc::qpcFrequency();
        if (const nc::J* lines = chat->get("lines")) if (lines->t == nc::J::Arr) for (const nc::J& l : lines->a) {
          if (l.t != nc::J::Obj) continue; const nc::J* sys = l.get("system"); const nc::J* team = l.get("team"); const bool isSys = sys && sys->t == nc::J::Bool && sys->b, isTeam = team && team->t == nc::J::Bool && team->b;
          const std::string body = l.str("text"), name = l.str("name"); const char one = 1;   // the game's chatLineText: 0x01 + RRGGBB switches the colour
          std::string line = isSys ? std::string(1, one) + "8ea0a8" + body : std::string(1, one) + "ffffff" + (isTeam ? "(" : "") + name + (isTeam ? "): " : ": ") + std::string(1, one) + (isTeam ? "33ffff" : "33ff33") + body;
          chatLog.push_back({nowQ - (int64_t)(l.num("ageMs", 0) * 0.001 * (double)fq), line});
        }
      }
      if (const nc::J* lg = r.get("log")) if (lg->t == nc::J::Arr && haveHud) for (const nc::J& op : lg->a) {   // the log views' operations since the last message: [view, op, ...args], applied in order
        if (op.t != nc::J::Arr || op.a.size() < 2 || op.a[1].t != nc::J::Str || op.a[1].s.empty()) continue; const auto num = [&](size_t i) { return i < op.a.size() && op.a[i].t == nc::J::Num ? op.a[i].n : 0.0; };
        const char kind = op.a[1].s[0]; const int view = (int)num(0);
        if (kind == 'a') hud.logOp(view, kind, op.a.size() > 2 && op.a[2].t == nc::J::Str ? op.a[2].s : std::string(), (uint32_t)(int64_t)num(3), (uint32_t)num(4), (uint32_t)num(5), 0);
        else if (kind == 'g') hud.logOp(view, kind, std::string(), 0, 0, (uint32_t)num(2), (uint32_t)num(3));
        else hud.logOp(view, kind, std::string(), 0, 0, 0, (uint32_t)num(2)); }
      if (const nc::J* stj = r.get("status"); stj && haveHud) {
        if (stj->t == nc::J::Str) hud.setTimerStatus(stj->s);
        else if (sharedMatch && stj->t == nc::J::Null) hud.setTimerStatus({});
      }
      if (const nc::J* rows = r.get("rows")) if (rows->t == nc::J::Obj && haveHud) {   // the page's elements: a delta of "set" and "clear" (the engine keeps the state); `until` is on the page's clock, `now` its clock when sent
        const double hostNow = nc::Hud::nowMs() - (sharedMatch ? nc::inputSeconds(nc::qpcNow() - msg.receivedAt) * 1000.0 : 0);
        const double pageNow = rows->num("now", -1);
        if (const nc::J* cl = rows->get("clear")) if (cl->t == nc::J::Arr) for (const nc::J& q : cl->a) if (q.t == nc::J::Num) hud.clearElement((int)q.n);
        if (const nc::J* st = rows->get("set")) if (st->t == nc::J::Arr) for (const nc::J& q : st->a) { if (q.t != nc::J::Obj) continue; nc::Hud::Element e;
          e.element = (int)q.num("element", 0); e.kind = (uint32_t)q.num("kind", 0); e.color = (uint32_t)q.num("color", 0); e.color2 = (uint32_t)q.num("color2", 0); e.flags = (uint32_t)q.num("flags", 0); e.aux = (uint32_t)q.num("aux", 0);
          e.num = q.num("num", 0); e.d0 = q.num("d0", 0); e.d1 = q.num("d1", 0); e.d2 = q.num("d2", 0); e.text = q.str("text");
          if (const nc::J* el = q.get("el")) if (el->t == nc::J::Obj) { e.elX = el->num("x", -1); e.elY = el->num("y", -1); e.elScale = el->num("scale", 0); }
          const double u = q.num("until", 0); e.until = u <= 0 ? 0 : pageNow >= 0 ? hostNow + (u - pageNow) : hostNow + u;
          if (u > 0 && e.until <= nc::Hud::nowMs()) { hud.clearElement(e.element); continue; }
          if (const nc::J* lk = q.get("links")) if (lk->t == nc::J::Arr) for (const nc::J& u : lk->a) e.links.push_back(u.t == nc::J::Str ? u.s : std::string());
          hud.setElement(e); }
      }
      if (r.t == nc::J::Obj) { if (const nc::J* wp = r.get("watchPaused")) if (wp->t == nc::J::Bool) g_watchPaused = wp->b;
        if (const nc::J* gc = r.get("ghostClock"); gc || !sharedMatch) ghostClock.set(gc, nc::inputSeconds(msg.receivedAt));
        if (haveHud) {
          const nc::J* ck = r.get("clippy");
          if (ck && ck->t == nc::J::Obj)
            hud.setClippy(ck->num("ms", -1) + (sharedMatch ? nc::inputSeconds(nc::qpcNow() - msg.receivedAt) * 1000.0 : 0), ck->str("text"));
          else if (ck || !sharedMatch) hud.setClippy(-1, std::string());
        }
      }
      }
    }
    { static int64_t tfLast = 0; const int64_t tn = nc::qpcNow(); const double tdt = tfLast ? (double)(tn - tfLast) * 1000.0 / (double)nc::qpcFrequency() : 0.0; tfLast = tn;
      const uint32_t tb = nc::tf::state(); nc::tf::frame(tdt); textSync(tb); chatOpen = (nc::tf::state() & (TF_STATE_CHAT | TF_STATE_TEAM)) != 0; consoleOpen = (nc::tf::state() & TF_STATE_CONSOLE) != 0; }   // queued characters / IME text, the drag tick, the field's events
    g_input.paused = optSim && g_pagePaused;
    { const bool tp = g_input.typing.load(), free = g_input.mouseFree(); static bool prevFree = false; if (free != prevFree) { prevFree = free; syncCapture(); nc::logf("mouse %s (typing %d [chat %d, console %d], page cursor flag %d, captured %d)", free ? "free" : "taken", (int)tp, (int)chatOpen, (int)consoleOpen, (int)g_input.cursor.load(), (int)g_input.captured()); }   // a text box open: the mouse is free; closed: the view takes it back
      if (tp) g_input.typingRepeat(nc::qpcNow() * 1000000 / nc::qpcFrequency()); }
    if (host && g_input.menuRequested) { g_input.menuRequested = false; g_input.quitRequested = false; if (g_menuRequest) g_menuRequest(); }   // embedded: Esc asks the host for the menu
    if (g_quit) break;
    if (host) {
      if (sharedMatch && !g_hudReady) {
        nc::matchOp(QC_MATCH_CLOCK, nc::inputSeconds(nc::qpcNow()));
        netService(true);
        nc::sleepMs(4);
        continue;
      }
      if (!hostVisible->load()) {
        pump();
        qc_ses_active(0, -1);
        if (sharedMatch) {
          nc::matchOp(QC_MATCH_PAUSE, QC_MATCH_MENU | (g_pagePaused ? QC_MATCH_HOLD : 0));
          qc_ses_advance(nc::inputSeconds(nc::qpcNow()));
        }
        netService(true);
        soundMute(false);
        { std::vector<std::string> ops, keep; { std::lock_guard<std::mutex> g(g_opMu); ops.swap(g_ops); } for (std::string& o : ops) { nc::J j; if (!(nc::parseJson(o, j) && j.t == nc::J::Obj && reportOp(j, j.str("op")))) keep.push_back(std::move(o)); } std::lock_guard<std::mutex> g(g_opMu); g_ops.insert(g_ops.begin(), std::make_move_iterator(keep.begin()), std::make_move_iterator(keep.end())); }
        drainAudio();
        if (sharedMatch && g_pageJs) g_runs.service(nc::inputSeconds(nc::qpcNow()), g_pageJs);
        if (!wasHidden) { wasHidden = true; const uint32_t tb = nc::tf::state(); nc::tf::close(6); textSync(tb); g_input.setCapture(false); } nc::sleepMs(4); continue; }
      if (wasHidden) { wasHidden = false; last = nc::qpcNow(); if (!sharedMatch) g_input.reset(nc::inputSeconds(last)); syncCapture(); }
    }
    if (host || !NC_WIN) { int pw = 0, ph = 0; platSize(hwnd, pw, ph); if (pw != g_w || ph != g_h) { g_w = pw; g_h = ph; g_resized = true; } }
    if (g_w < 2 || g_h < 2) { pump(); if (sharedMatch) nc::matchOp(QC_MATCH_CLOCK, nc::inputSeconds(nc::qpcNow())); nc::sleepMs(10); continue; }
    if (hostFrameCap) {
      const int fps = hostFrameCap->exchange(-1);
      if (fps >= 0) {
        pm = fps == 0 ? nc::PresentMode::Fifo : nc::PresentMode::Immediate;
        capFps = fps;
      }
    }
    const bool resized = g_resized;
    g_resized = false;
    if (resized || pm != sc.requested) {
      const int64_t buildStarted = resized && host ? nc::qpcNow() : 0;
      if (buildStarted) {
        if (!resizeStarted) resizeStarted = buildStarted;
        nc::logf("resize: native view %ux%u -> %dx%d", sc.w, sc.h, g_w, g_h);
      }
      nc::buildSwapchain(c, sc, g_w, g_h, pm, rp);
      if (buildStarted) nc::logf("resize: swapchain rebuilt in %.1f ms", 1000.0 * (nc::qpcNow() - buildStarted) / freq);
      if (resized && g_input.captured()) { g_input.setCapture(false); syncCapture(); }
    }
    if (capFps > 0) {   // spin the last 2 ms: Sleep granularity is not enough at 240 fps
      const int64_t target = (int64_t)((double)freq / capFps);
      for (;;) { const int64_t dt = nc::qpcNow() - last; if (dt >= target || g_quit) break; if ((target - dt) * 1000 > 2 * freq) nc::sleepMs(1); }
    }
    if (g_quit) break;
    NC_CK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    if (hostStop && hostStop->load()) break;
    if (haveWorld && !g_settingsDirty.load()) world.refreshPicmip();
    if (shotPending) { shotPending = false; writePngBgra(optShot, shotBuf.ptr, shotW, shotH); }
    if (optBench && qValid && frames >= 30) {
      uint64_t ts[4] = {};
      const uint32_t count = mmQueryValid ? 4 : 2;
      if (vkGetQueryPoolResults(c.dev, qpool, 0, count, count * sizeof(uint64_t), ts, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        benchGpu.push_back((double)(ts[1] - ts[0]) * c.timestampPeriod / 1e6);
        if (mmQueryValid && benchMinimapGpu.size() < benchMinimapGpu.capacity())
          benchMinimapGpu.push_back((double)(ts[3] - ts[2]) * c.timestampPeriod / 1e6);
      }
    }
    mmQueryValid = false;
    uint32_t idx; VkResult ar = vkAcquireNextImageKHR(c.dev, sc.sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { nc::buildSwapchain(c, sc, g_w, g_h, pm, rp); continue; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) nc::fatal("acquire: %d", (int)ar);
    const int64_t acquiredAt = nc::qpcNow();
    const double ft = double(acquiredAt - last) / freq;
    last = acquiredAt;
    // While paused, collect the unpause bind without advancing the simulation.
    const auto peerZoom = [&] { return followN > 0 ? int(haveFollow && followPose.zoomed) : -1; };
    qc_ses_active(optSim, peerZoom());
    pump();
    QcAction actions[256];
    const int numActions = optSim ? qc_ses_actions(actions, int(std::size(actions))) : 0;
    if (numActions < 0 || g_input.takeOverflow()) {
      nc::logf("session input queue overflow");
      g_input.releaseAllKeys();
    }
    netService();
    if (!optOp.empty() && frames == 20) { appSimOp(optOp); optOp.clear(); }
    if (optSim) {   // the page's ops for the sim (respawn, mode, cmd ...) and the restart key, applied before the tick
      ops.clear(); { std::lock_guard<std::mutex> g(g_opMu); ops.swap(g_ops); }
      auto keepRunEvents = [&]() {   // before a restart's events are dropped: the page still has to hear of the new life (RESET id 1..3; 0 is a load), a death and an aborted run
        evs.clear(); sim.drainEvents(evs);
        for (const nc::Sim::Event& e : evs) if ((e.type == QC_EV_RESET && e.id >= 1 && e.id <= 3) || e.type == QC_EV_DEATH || e.type == QC_EV_RUN_ABORT) appendEventJson(heldEvents, e);
      };
      auto afterRespawn = [&](const char* why, int64_t r0) {
        // after a respawn or a mode reload: view angles from the spawn, the client's own caches start over (the trail is the core's; pending effect events are dropped)
        cur = sim.player(); g_input.yaw = optHasYaw ? optYaw * M_PI / 180.0 : sim.spawnYaw(); g_input.pitch = sim.spawnPitch(); poseDirty = true;
        cameraReset = true; gunKick = 0;
        ownHook.on = false; world.fxHookEnd();
        world.fxClearOwnProjectiles();
        keepRunEvents(); pushRun = -1;   // the state push goes out again at once
        if (!sharedMatch) g_input.reset(nc::inputSeconds(nc::qpcNow()));
        nc::logf("%s: %.1f ms, at (%.1f %.1f %.1f)", why, double(nc::qpcNow() - r0) * 1000.0 / (double)freq, cur.pos[0], cur.pos[1], cur.pos[2]);
      };
      auto restartSim = [&](const char* why) {
        const int64_t r0 = nc::qpcNow();
        if (sharedMatch) {
          if (nc::matchOp(QC_MATCH_RESET, nc::inputSeconds(r0)) != 1) return;
        } else {
          if (sim.frozen()) return;
          sim.restart();
        }
        if (followN == 0) sim.setNoclip(true);
        afterRespawn(why, r0);
      };
      for (const std::string& opJson : ops) {
        nc::J j; if (!nc::parseJson(opJson, j) || j.t != nc::J::Obj) continue; const std::string op = j.str("op"); if (reportOp(j, op)) continue; nc::logf("op %s: %.80s", op.c_str(), opJson.c_str()); 
        if (op == "respawn") restartSim("respawn (page)");
        else if (op == "cmd") {   // a command that acts on the game: the core decides everything, the answer goes back to the page
          double in[16] = {}; if (const nc::J* a = j.get("in")) if (a->t == nc::J::Arr) for (size_t i = 0; i < 16 && i < a->a.size(); ++i) in[i] = a->a[i].n;
          const int cmd = (int)j.num("cmd", -1);
          if (sharedMatch && cmd == 16) { nc::matchOp(QC_MATCH_FREEZE, in[0] != 0); continue; }
          const double serial = sim.out()[33]; const nc::Sim::CommandReply r = sim.command(cmd, in, j.str("text"));
          if (r.bits & 1) {   // the sim changed; its cues and events are drained with this frame's, as after a tick
            cur = sim.player(); poseDirty = true; pushRun = -1;
            if (sim.out()[33] != serial && r.state) {   // the player was placed (!loadpos, !goto): the view angles are the placed ones
              g_input.yaw = r.state[29]; g_input.pitch = r.state[30]; g_input.resetForPlacement(nc::inputSeconds(nc::qpcNow())); cameraReset = true; }
          }
          nc::logf("cmd %d -> bits %d", cmd, r.bits); if (g_pageJs) g_pageJs(commandScript(cmd, r).c_str());
        }
        else if (op == "giveitem") { const std::string nm = j.str("name"); if (!sim.giveItem(nm, j.num("count", 0), j.num("capSeconds", 0))) nc::logf("giveitem %s: refused", nm.c_str()); }
        else if (op == "stat") sim.setStat((int)j.num("id", 0), j.num("v", 0));   // not sent by the page yet: health 0, armor 1, flight fuel 2
        else if (op == "removepowerups") sim.removePowerups();
        else if (op == "openText") {   // the page's chat / team chat bind (the key went to the page, which decided): {mode: "chat" | "team" | "console" (or 1 / 2 / 3)}; the key still held is the opener (its character must not type)
          const nc::J* mj = j.get("mode"); uint32_t mode = TF_CHAT; if (mj && mj->t == nc::J::Str) mode = mj->s == "team" ? TF_TEAM : mj->s == "console" ? TF_CONSOLE : TF_CHAT; else if (mj && mj->t == nc::J::Num) mode = (uint32_t)mj->n;
          const uint32_t tb = nc::tf::state(); nc::tf::open(mode, 0); textSync(tb); }
        else if (op == "closeText") { const uint32_t tb = nc::tf::state(); nc::tf::close(6); textSync(tb); }   // the page closes the box (menu opened, a map load)
        else if (op == "ghost") ghost.set(j);
        else if (op == "follow") { const int n = (int)j.num("n", -1);
          if (sharedMatch) {
            if (nc::matchOp(QC_MATCH_FOLLOW, n) < 0) continue;
          } else {
            if (n == 0) { if (followN > 0 && haveFollow) { sim.restorePosition(followPose.pos[0], followPose.pos[1], followPose.pos[2], 0, 0, 0, followPose.yaw, followPose.pitch); g_input.yaw = followPose.yaw; g_input.pitch = followPose.pitch; } sim.setNoclip(true); }
            else if (followN == 0) sim.setNoclip(false);
          }
          if (followN != n) { if (sessionAudio) nc::soundEvent(QC_SOUND_FOLLOW_END); followN = n; haveFollow = false; slickBelow = false; slickCheckAt = 0; cameraReset = true; cur = sim.player(); poseDirty = true; pushRun = -1; if (!sharedMatch) g_input.reset(nc::inputSeconds(nc::qpcNow())); }
          const bool pov = n > 0;
          if (g_input.followingPov.exchange(pov) != pov) syncCapture();
          peerOptions(n); }
        else if (op == "custommodel") { const int w = (int)j.num("weapon", 0); const nc::J* cl = j.get("clear");
          if (cl && cl->t == nc::J::Bool && cl->b) { if (w >= 1 && w <= 10) customFire[w] = false; auto f = ownGun.find(w); if (f != ownGun.end()) { if (f->second < 0) gunHandle.erase(w); else gunHandle[w] = f->second; ownGun.erase(f); } }
          else if (haveWorld && w >= 1 && w <= 10) { const int h = world.addViewWeaponGlb(nc::unbase64(j.str("glb")), j.num("scale", 1)); if (h >= 0) { if (!ownGun.count(w)) ownGun[w] = gunHandle.count(w) ? gunHandle[w] : -1; gunHandle[w] = h; const nc::J* fire = j.get("customFire"); customFire[w] = fire && fire->t == nc::J::Bool && fire->b; } nc::logf("custommodel weapon %d: %s", w, h >= 0 ? "loaded" : "refused"); } }
        else if (op == "custompickup") { const nc::J* cl = j.get("clear");
          if (cl && cl->t == nc::J::Bool && cl->b) { const double away[3] = {0, -1e5, 0}; for (const CustomItem& c2 : customItems) world.setObject(c2.handle, away, 0, false); customItems.clear(); spins.resize(pickups.size()); }
          else if (haveWorld && customItems.size() < CUSTOM_ITEMS) { double size = j.num("size", 0); const auto visual = pickupVisuals.find(j.str("classname")); if (!(size > 0)) size = visual == pickupVisuals.end() ? 26 : visual->second.size;
            const uint32_t glow = visual == pickupVisuals.end() ? 0xffffffu : visual->second.glow;
            const int h = world.addObjectGlb(nc::unbase64(j.str("glb")), size, glow); if (h >= 0) customItems.push_back({h, {cur.pos[0] + 40 * sin(g_input.yaw), cur.pos[1] + 15, cur.pos[2] + 40 * cos(g_input.yaw)}}); if (h >= 0) addSpin(customItems.back().pos); nc::logf("custompickup: %s", h >= 0 ? "placed" : "refused"); } }
        else if (op == "pose") poseNowWanted = true;   // answered after this frame's pose is published
        else if (op == "mode") {   // the held map again with the new weapon set / hook / physics: no asset or world work
          const bool oldPql = optPql, oldStrafe = optWeaponS, oldHook = optHook;
          const nc::J* v = j.get("vql"); const nc::J* h = j.get("hook"); if (v && v->t == nc::J::Bool) optPql = !v->b; if (h && h->t == nc::J::Bool) optHook = h->b; if (j.get("weaponMode")) optWeaponS = j.str("weaponMode") == "s";
          profileId = j.str("profileId");
          if (sharedMatch && nc::matchOp(QC_MATCH_MODE, !optPql, optWeaponS, optHook) != 1) {
            advertiseMode();
            continue;
          }
          const int64_t r0 = nc::qpcNow();
          if (!loadSim()) {
            if (sharedMatch) {
              optPql = oldPql; optWeaponS = oldStrafe; optHook = oldHook;
              nc::matchOp(QC_MATCH_MODE, !optPql, optWeaponS, optHook);
              nc::fatal("sim: mode reload refused: %s", sim.error().c_str());
            }
            nc::logf("mode: the sim could not be loaded again: %s", sim.error().c_str());
            continue;
          }
          if (sharedMatch && nc::matchOp(QC_MATCH_RELOAD, nc::inputSeconds(nc::qpcNow())) < 0)
            nc::fatal("sim: match reload refused");
          advertiseMode();
          syncPickups();
          resetPickupSpins();
          // A physics mode change keeps the same map and peer roster.
          if (!networkMatch) qc_peer_clear_samples();
          slickBelow = false;
          slickCheckAt = 0;
          buildTriggers();
          afterRespawn("mode reload", r0);
        }
      }
      pageActions.clear();
      for (int i = 0; i < numActions; ++i) {
        const QcAction& action = actions[i];
        const std::string_view name = qc_ses_action_name(int(action.action));
        if (name.empty() || (g_pagePaused && name != "softPause" && name != "menu" && !(sharedMatch && name == "restart"))) continue;
        if (name == "restart") {
          if (followN <= 0) restartSim("respawn (key)");
        } else if (name == "menu") {
          g_input.menuRequested = true;
          g_input.quitRequested = true;
        } else if (name == "taunt" && (pageSoundFeed || (!host && haveAudio))) {
          if (followN >= 0) continue;
          QcCue cue{};
          cue.gain = nc::inputSeconds(nc::qpcNow());
          if (nc::soundEvent(QC_SOUND_TAUNT, cue) == 0 && haveNet)
            hub.sendText("{\"type\":\"taunt\",\"voice\":" + nc::jsonQuote(g_set->voice) + "}");
        } else if (name == "chat" || name == "teamChat") {
          if (!haveHud) continue;
          int scan = 0;
          g_input.domCode(action.code, &scan);
          const uint32_t tb = nc::tf::state();
          nc::tf::open(name == "chat" ? TF_CHAT : TF_TEAM,
            g_input.keyStillDown(scan) ? uint32_t(scan) + 1u : 0u);
          textSync(tb);
        } else if (g_simOut) {
          if (pageActions.empty()) pageActions = "{\"inputActions\":[";
          else pageActions += ',';
          pageActions += "{\"action\":\"";
          pageActions += name;
          pageActions += "\",\"code\":\"";
          pageActions += g_input.domCode(action.code);
          pageActions += "\"}";
        }
      }
      if (!pageActions.empty()) {
        pageActions += "]}";
        g_simOut(pageActions.c_str());
      }
    }
    if ((g_quit || g_input.menuRequested) && !host) break;
    if (optSim) {
      if (sharedMatch) {
        nc::matchOp(QC_MATCH_PAUSE, (g_input.menuRequested ? QC_MATCH_MENU : 0) |
                    (g_pagePaused ? QC_MATCH_HOLD : 0) |
                    (g_input.typing.load() ? QC_MATCH_BOX : 0));
      }
      const bool active = g_input.focused() && !g_input.menuRequested &&
        (sharedMatch || (!g_pagePaused && (haveNet || !g_input.typing.load())));
      qc_ses_active(active, peerZoom());
      soundMute(!g_input.menuRequested && !g_pagePaused);
      const double now = nc::inputSeconds(nc::qpcNow());
      if (active && captureAttack) qc_ses_key(captureAttack, 1, now);
      const int ran = qc_ses_advance(now);
      if (ran > 0) {
        ticksRun += ran;
        g_runs.takeResult();
        cur = sim.player();
        poseDirty = true;
      }
    }
    if (haveNet && optSim && followN < 0) {   // our own state frame: the core owns the send window; none while spectating
      QcWireState ws; memset(&ws, 0, sizeof ws); ws.full = 1; ws.x = cur.pos[0]; ws.y = cur.pos[1]; ws.z = cur.pos[2]; ws.yaw = g_input.yaw; ws.pitch = g_input.pitch; ws.vx = cur.vel[0]; ws.vy = cur.vel[1]; ws.vz = cur.vel[2];
      ws.crouchBits = (cur.crouched ? 1 : 0) | (camera.zoomed ? 2 : 0); ws.weapon = sim.weaponHeld(); const double* sample = sim.out(); ws.teleport = sample ? int(sample[33]) : -1;
      hub.state(ws);
    }
    if (followN > 0 && qc_peer_pose(followN, &followPose)) haveFollow = true;
    if (followN > 0 && haveFollow) { for (int k = 0; k < 3; ++k) { cur.pos[k] = followPose.pos[k]; cur.vel[k] = followPose.vel[k]; } cur.crouched = followPose.crouched != 0; g_input.yaw = followPose.yaw; g_input.pitch = followPose.pitch; }   // the camera, speedometer and minimap are the followed player's
    if (sharedMatch) nc::matchHud(sessionHud);
    if (haveNet && sharedMatch) {
      const int board = (optPql ? 0 : 3) + (optHook ? 2 : optWeaponS ? 0 : 1);
      for (int kind = 0; kind < 3; ++kind) {
        if (speedRevision[kind] == sessionHud.boardRevision[kind]) continue;
        speedRevision[kind] = sessionHud.boardRevision[kind];
        hub.fact(QC_NET_SPEED, board, kind | ((sessionHud.boardCarried & (1u << kind)) ? 4 : 0),
                 sessionHud.boardSpeed[kind]);
      }
    }
    const auto liveNumbers = [&](nc::HudNumbers& n) {
      if (sharedMatch) nc::Sim::hudNumbers(sessionHud.numbers, n);
      else sim.hudFrame(n);
    };
    if (optSim && haveWorld) {
      evs.clear();
      sim.drainEvents(evs);
      for (const nc::Sim::Event& e : evs) {
        if (e.type == QC_EV_SHOT) gunKick = 1;
        if (haveNet && followN < 0) {
          if (e.type == QC_EV_FIRE) {
            hub.effect(QC_NET_FIRE, e.surface, e.weapon, optPql ? 1 : 2,
                       e.pos, e.dir[0], e.dir[1]);
          } else if (e.type == QC_EV_MAP_SHOOTER) {
            const bool aimed = (e.surface & QC_MAP_SHOOTER_AIMED) != 0;
            hub.effect(QC_NET_SHOT, aimed, e.id, optPql ? 1 : 2, aimed ? e.to : nullptr);
          } else if (e.type == QC_EV_HOOK_UPDATE)
            hub.effect(QC_NET_HOOK, e.id ? 2 : 1, 0, 0, e.id ? e.pos : nullptr);
          else if (e.type == QC_EV_HOOK_END)
            hub.effect(QC_NET_HOOK, 0, 0, 0, nullptr);
        }
        if (e.type == QC_EV_RESET) cameraReset = true;
      }
      if (g_simOut) {   // the page's state push: events (all but the per-tick weapon flight updates: pickups, start speed, checkpoints, jump speeds ...) plus the core's taint and run state, which follow the core even without events. Sent with events, when taint or run change, and 4 times a second while the clock runs
        std::string& js = heldEvents;
        for (const nc::Sim::Event& e : evs) {
          if (e.type == QC_EV_FIRE || e.type == QC_EV_MAP_SHOOTER) continue;
          if (sharedMatch) {
            if (e.type != QC_EV_RUN_START && e.type != QC_EV_CHECKPOINT && e.type != QC_EV_RUN_FINISH &&
                e.type != QC_EV_RUN_ABORT && e.type != QC_EV_RESET && e.type != QC_EV_DEATH) continue;
          } else if (e.type <= QC_EV_BEAM || e.type == QC_EV_HOOK_UPDATE) continue;
          appendEventJson(js, e);
        }
        const int tnt = sim.tainted() ? 1 : 0, run = sim.runState(); const double nowS = (double)nc::qpcNow() / (double)freq;
        if (!js.empty() || tnt != pushTainted || run != pushRun || (!sharedMatch && run == 1 && nowS - simPushAt >= 0.25)) {
          pushTainted = tnt; pushRun = run; simPushAt = nowS; nc::HudNumbers tn; liveNumbers(tn); char head[160]; snprintf(head, sizeof head, "{\"t\":%d,\"speed\":%d,\"tainted\":%s,\"run\":%d,\"events\":[", tn.timerMs, tn.speed, tnt ? "true" : "false", run); simPush.assign(head); simPush += js; simPush += "]";
          if (!sharedMatch) { simPush += '}'; g_simOut(simPush.c_str()); simPush.clear(); }
        }
        js.clear();
      }
    }
    if (optSim && haveWorld) {   // the leaderboard ghost this frame (the core's): whether it is drawn, its clock, route line and minimap track; camera-independent
      QcComposeIn in{}; in.now = nc::inputSeconds(nc::qpcNow()); in.duration = ghost.time; in.followT = ghostClock.t; in.followAt = ghostClock.at; in.ownRun = -1; in.plateKey = (int32_t)ghost.plateKey;
      in.flags = (ghost.on ? QC_COMPOSE_GHOST : 0) | (ghost.show.value_or(g_set->showGhost != 0) ? QC_COMPOSE_SHOW : 0) | (ghost.path.value_or(g_set->ghostPath != 0) ? QC_COMPOSE_PATH : 0)
                 | (followN > 0 ? QC_COMPOSE_FOLLOWING : 0) | (ghostClock.running ? QC_COMPOSE_FOLLOW_RUNNING : 0) | (ghostClock.paused ? QC_COMPOSE_FOLLOW_PAUSED : 0);
      qc_ses_compose(&in, &compose);
    }
    MmCfg mc;
    if (optSim && haveHud && haveWorld) {
      { std::lock_guard<std::mutex> lk(g_mmMu); mc = g_mm; }
      mmView = QcMinimapOut{};
      if (mc.have && g_set->minimap != 0) {
        if (!(mc.flags & 1u)) {   // the view and the ghost track window (the core's), on the whole map's bounds
          QcMinimapIn in{}; in.cx = mmCx; in.cz = mmCz; in.S = mmSide; in.px = cur.pos[0]; in.pz = cur.pos[2]; in.yaw = g_input.yaw; in.speed = hypot(cur.vel[0], cur.vel[2]); in.dt = std::min(0.1, ft); in.range = mc.range; in.speedScale = mc.speedScale;
          in.zoom = (int32_t)mc.zoom; in.ghostSample = compose.track ? compose.pathSample : -1;
          in.flags = (mc.flags & 2u ? 0 : QC_MINIMAP_ROTATE) | (mmReset ? QC_MINIMAP_RESET : 0);
          if (qc_ses_minimap(&in, &mmView, mmGhost, QC_MINIMAP_TRACK_POINTS) != 0) mmView = QcMinimapOut{};
          else mmReset = false;
        }
      }
    }
    const double composeTime = nc::inputSeconds(nc::qpcNow() - t0);
    if (haveWorld) {
      for (size_t i = 0; i < pickups.size(); ++i)
        spins[i].visible = sim.itemVisible(pickups[i].index);
      qc_ses_items(composeTime, itemsAt < 0 ? 0 : std::min(0.1, composeTime - itemsAt), spins.data(), (int)spins.size()); itemsAt = composeTime;
      const auto placeItem = [&](int handle, const double pos[3], const QcItemSpin& it) { const double p[3] = {pos[0], it.y, pos[2]}; world.setObject(handle, p, it.yaw, it.visible != 0, it.baseY - it.y); };
      for (size_t i = 0; i < pickups.size(); ++i) placeItem(pickups[i].handle, pickups[i].pos, spins[i]);
      for (size_t i = 0; i < customItems.size() && pickups.size() + i < spins.size(); ++i) placeItem(customItems[i].handle, customItems[i].pos, spins[pickups.size() + i]);
      if (optSim && poseDirty) { nPoses = sim.poses(poseBytes); poseDirty = false; }
    }
    const bool following = followN > 0 && haveFollow;
    if (haveHud) hud.setPlayerHudView(followN < 0, following);
    nc::HudNumbers gunState{};
    if (optSim && !gunHandle.empty()) liveNumbers(gunState);
    if (sharedMatch && haveHud) {
      if (sessionHud.chatFirst != shownChatFirst || sessionHud.chatEnd != shownChatEnd) {
        shownChatFirst = sessionHud.chatFirst;
        shownChatEnd = sessionHud.chatEnd;
        matchChat.join(sessionHud, joined);
        hud.setChat(joined);
      }
      hud.setChatHistory(sessionHud.chatView == QC_HUD_CHAT_HISTORY);
    }
    const int held = !optSim ? -1 : following ? followPose.weapon :
      gunState.weaponInHand > 0 ? gunState.weaponInHand : sim.weaponHeld();
    const double cameraDelta = std::clamp(ft, 0.0, 0.1);
    if (optSim && !sharedMatch && g_pagePaused) qc_ses_active(1, peerZoom());
    g_deferTextJs = true;
    pump();
    g_deferTextJs = false;
    if (optSim && (!g_input.focused() || g_input.menuRequested ||
                  (!sharedMatch && (g_pagePaused || (!haveNet && g_input.typing.load()))))) qc_ses_active(0, peerZoom());
    const int64_t nowQ = nc::qpcNow();
    const double ta = nc::inputSeconds(nowQ), t = nc::inputSeconds(nowQ - t0);
    uint32_t heldMask = 0;
    if (optSim) {
      QcCameraInput in{};
      in.interpolation = -1;
      in.delta = cameraDelta;
      in.drawGun = followN == 0 ? 0 : g_set->drawGun;   // the other camera settings are the core's own
      in.reset = cameraReset;
      in.kick = gunKick;
      in.vql = !optPql;
      in.customFire = held > 0 && held < int(customFire.size()) && customFire[held];
      if (following) {
        in.mode = 1;
        memcpy(in.overridePos, followPose.pos, sizeof in.overridePos);
        in.overrideYaw = followPose.yaw;
        in.overridePitch = followPose.pitch;
        in.crouched = followPose.crouched != 0;
        in.lastFire = followPose.reserved[0];
      }
      QcCamera next{};
      if (qc_ses_camera(ta, &in, &next) == 0) {
        camera = next;
        haveCamera = true;
        cameraReset = false;
        if (!following) gunKick = camera.kick;
        g_input.yaw = camera.yaw;
        g_input.pitch = camera.pitch;
      }
      heldMask = qc_ses_held_mask();
      if (haveWorld && latency.active()) {
        latency.camera(pumpEnd, following ? nc::MotionReceipt{} : g_input.motionReceipt);
        g_input.motionReceipt = {};
      }
    }
    if (frames > 0 && ft > worst) worst = ft;
    if (optBench && frames >= 30) { benchInterval.push_back(ft * 1000); benchCpu.push_back(world.stats().planMs + world.stats().recordMs); }
    vkResetCommandBuffer(cb, 0); VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb, &bi);
    vkCmdResetQueryPool(cb, qpool, 0, 4); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
    bool poseReady = false;
    // r_renderScale / r_gamma / r_contrast: only away from the defaults is the scene drawn offscreen
    const bool usePost = haveWorld && (g_set->renderScale != 1 || g_set->gamma != 1 || g_set->contrast != 1);
    int rw = sc.w, rh = sc.h;
    if (usePost) {
      const double s = std::clamp(g_set->renderScale, 0.1, 2.0); const int cap = (int)c.props.limits.maxImageDimension2D;
      rw = std::clamp((int)lround(sc.w * s), 1, cap); rh = std::clamp((int)lround(sc.h * s), 1, cap);
      if (rw != post.w || rh != post.h) post.resize(c, rp, rw, rh);
    }
    const VkFramebuffer sceneFb = usePost ? post.fb : sc.fbs[idx];
    std::function<void(VkCommandBuffer)> overlay;
    const auto drawHud = [&](VkCommandBuffer b) {
      float box[4];
      if (mmView.w > 0 && hud.minimapBox(sc.w, sc.h, box)) {
        const int64_t start = optBench ? nc::qpcNow() : 0;
        uint64_t allocs = 0;
        struct AllocationScope {
          uint64_t* previous = nc::frameAllocations;
          uint64_t& count;
          ~AllocationScope() { nc::frameAllocations = previous; if (previous) *previous += count; }
        } counters{nc::frameAllocations, allocs};
        if (optBench) nc::frameAllocations = &allocs;
        // The browser clips relative to the local body even while spectating a peer.
        const double playerY = followN > 0 && haveFollow ? sim.out()[1] : cur.pos[1];
        mmQueryValid = world.recordMinimap(b, sc.w, sc.h, box, mmView, playerY, mmMinY,
                                           t, poseBytes.data(), nPoses, optBench ? qpool : VK_NULL_HANDLE);
        if (optBench) benchMinimapAllocs += allocs;
        if (optBench && mmQueryValid && frames >= 30 && benchMinimapCpu.size() < benchMinimapCpu.capacity())
          benchMinimapCpu.push_back(1000 * nc::inputSeconds(nc::qpcNow() - start));
      }
      hud.record(b, sc.w, sc.h);
    };
    if (haveWorld) {
      nc::CameraIn cam;
      if (optSim && haveCamera) {
        memcpy(p_listener, camera.eye, sizeof camera.eye);
        fov = camera.fovY;
        const double unzoomedY = atan(tan(g_set->fov * M_PI / 360) / (double(sc.w) / sc.h)) * 360 / M_PI;
        qc_ses_zoom_projection(camera.fovY, unzoomedY);
        cam = nc::makeCameraView(camera.eye, camera.quaternion, (double)sc.w / sc.h, camera.fovY, world.farPlane());
        poseReady = true;
      } else cam = nc::makeCamera(eye, yaw0 + (optOrbit ? t * 20.0 : 0.0), pitch, (double)sc.w / sc.h, fov, world.farPlane());
      if (optSim) {
        if (haveHud && !sharedMatch) {
          const bool showHistory = chatHistory || (camera.inputFlags & 1u);
          const size_t shownMax = showHistory && !chatOpen ? 14 : 6;
          size_t first = chatLog.size() > shownMax ? chatLog.size() - shownMax : 0;
          if (!showHistory) {
            const int64_t cutoff = nowQ - 8 * freq;
            while (first < chatLog.size() && chatLog[first].at < cutoff) ++first;
          }
          if (shownRevision != chatRevision || first != lastShownFirst) {
            shownRevision = chatRevision;
            lastShownFirst = first;
            joined.clear();
            for (size_t k = first; k < chatLog.size(); ++k) {
              if (!joined.empty()) joined += '\n';
              joined += chatLog[k].text;
            }
            if (joined != chatShown) { chatShown = joined; hud.setChat(joined); }
          } hud.setChatHistory(showHistory && !chatOpen);
        }
        if (optSim && haveHud) {
          uint32_t hudKeys = heldMask;
          if (followN >= 0) {
            hudKeys = following && watchedKeysGeneration == hub.generation() &&
                      nc::isFollowedSender(followN, watchedKeysSender) ? watchedKeys : 0;
          }
          const double* feet = followN < 0 ? cur.pos : following && !(followPose.flags & (4 | 8)) ? followPose.pos : nullptr;
          if (!feet) {
            slickBelow = false;
            slickCheckAt = 0;
          } else if (ta >= slickCheckAt) {
            // The native view displays the local sim's movers even in followed POV.
            slickBelow = qc_dry_slick_below(feet) == 1;
            slickCheckAt = ta + 0.1;
          }
          hud.setLive(g_input.pitch * 180.0 / M_PI, slickBelow, hudKeys, g_pagePaused, sim.liquid());
        }
        if (haveHud) hud.setBoard(&board);
        if (haveWorld) {   // the other players and the leaderboard ghost: the core's node records for this frame's camera
          QcPeerCamera pc; for (int k = 0; k < 3; ++k) { pc.eye[k] = cam.pos[k]; pc.forward[k] = -cam.view[k * 4 + 2]; }
          memcpy(pc.quat, camera.quaternion, sizeof pc.quat);
          static QcPeerNode nodes[QC_PEER_MAX * 4]; avatars.begin(); avatars.accessories = g_set->showDickModels != 0;
          const int pn = qc_peer_frame(ta, &pc, nodes, QC_PEER_MAX * 4);
          avatars.apply(world, haveHud ? &hud : nullptr, nodes, pn, 0, [](int slot, std::string& name, std::string& sub) { QcPeerInfo pi; if (!qc_peer_info(slot, &pi)) return false; name = pi.name; sub = pi.subline; return true; });
          {   // the ghost's records for the final camera, then every ground shadow of the frame (the core picks who casts one)
            QcPeerNode gn[4]; const int n4 = qc_ses_view(&pc, gn, 4);
            if (n4 > 0) avatars.apply(world, haveHud ? &hud : nullptr, gn, n4, nc::AvatarView::GHOST_SLOT, [&](int, std::string& name, std::string& sub) { name = ghost.name; sub = ghost.sub; return true; });
            static_assert(QC_SHADOW_MAX == nc::World::SHADOWS, "shadow slots");
            QcShadowIn in{}; in.flags = QC_SHADOW_WORLD; in.followed = followN > 0 ? followN : 0;
            if (followN < 0) { in.flags |= QC_SHADOW_SELF; in.self[0] = haveCamera ? camera.eye[0] : cur.pos[0]; in.self[1] = cur.pos[1]; in.self[2] = haveCamera ? camera.eye[2] : cur.pos[2]; in.selfYaw = g_input.yaw; in.selfAccX = g_ownShadow[0]; in.selfAccZ = g_ownShadow[1]; }
            static QcShadow quads[QC_SHADOW_MAX]; const int count = std::max(0, qc_ses_shadows(&in, quads, QC_SHADOW_MAX));
            for (int s = 0; s < nc::World::SHADOWS; ++s) { if (s >= count) { world.hideShadow(s); continue; } const QcShadow& q = quads[s]; const double u[6] = {q.fade, q.ext, q.R, q.y0, q.y1, q.rw}; world.setShadow(s, q.position, q.quat, q.size, u); }
          }
          avatars.end(world);
          {   // cg_ghostPath (main.js drawGhostPath): the ghost's route, passed and ahead
            const bool path = compose.path != 0 && g_set->ghostPathOpacity > 0;
            if (path && pathRevision != ghost.revision) { pathRevision = ghost.revision; world.buildGhostPath(ghost.rows.data(), ghost.count); }
            double rgb[3]; for (int k = 0; k < 3; ++k) rgb[k] = nc::srgbToLinear(((compose.pathColor >> (16 - 8 * k)) & 255) / 255.0);
            const double dpr = haveHud ? std::max(0.5, (double)hud.uiScale()) : 1.0;
            world.drawGhostPath(path, compose.pathSample, g_set->ghostPathWidth, g_set->ghostPathOpacity, rgb, sc.w / dpr, sc.h / dpr);
          }
        }
        if (optSim && haveHud && haveWorld) {
          TextMinimapState st{};
          st.px = cur.pos[0]; st.pz = cur.pos[2];
          st.flags = mc.flags; st.boxPx = mc.boxPx; st.trailColor = mc.trailColor; st.ghostColor = mc.ghostColor;
          st.fovTenths = (uint32_t)lround(g_set->fov * 10); st.x = mc.x; st.y = mc.y; st.scale = mc.scale;
          static float trailBuf[2000];
          if (mmView.w > 0 && (mc.flags & 4u)) st.trailCount = (uint32_t)sim.trail(trailBuf, 1000);
          hud.setMinimap(st, mmView, trailBuf, mmGhost);
        }
        if (optSim && haveHud) { uint8_t cg[136]; const bool have = sim.cgaz(cg); hud.setCgaz(g_cgazOn && have, cg, g_input.yaw * 180.0 / M_PI, 2 * atan(tan(fov * M_PI / 360) * ((double)sc.w / sc.h)) * 180 / M_PI, g_cgazY, g_set->cgazFull != 0 ? 2u : 0u); }   // the strafe HUD: this frame's view angle and horizontal fov
        if (haveHud) { nc::HudNumbers hn; liveNumbers(hn); hn.fps = g_set->showFps != 0 ? shownFps : -1; if (followN > 0 && haveFollow) { hn.speed = (int)hypot(followPose.vel[0], followPose.vel[2]); hn.weaponInHand = followPose.weapon; } hud.prepare(cb, sc.w, sc.h, hn); overlay = [&drawHud](VkCommandBuffer b) { drawHud(b); }; }
        if (optSim && haveWorld) {   // the sim's weapon events -> the effects: shots kick the gun, hits leave marks and sparks, projectiles fly, explosions flash, the rail draws its beam
          double hookFrom[3]; world.hookOrigin(cam, camera.gun, hookFrom);   // the first-person cable and its sparks: this frame's camera and gun
          const int shotMode = optPql ? 0 : 1; const double eyeNow[3] = {cur.pos[0], cur.pos[1] + (cur.crouched ? 36 : 50), cur.pos[2]};
          for (size_t ei = 0; ei < evs.size(); ++ei) { const nc::Sim::Event& e = evs[ei]; switch (e.type) {
            case 2: nc::fxShot(world, QC_SESFX_HITSCAN, e.weapon, shotMode, optRunway, eyeNow, e.pos, e.dir, e.surface, 0.5); break;   // IMPACT: it exists only below fraction 1, which is all the core reads of it
            case 3: world.fxProjectile(e.id, e.weapon, e.pos, e.vel, true); break;   // PROJECTILE_SPAWN
            case 4: case 5: world.fxProjectile(e.id, e.weapon, e.pos, e.vel, false); break;   // UPDATE / BOUNCE
            case 6: world.fxProjectileEnd(e.id); break;
            case 7: { const double* n = e.dir[0] * e.dir[0] + e.dir[1] * e.dir[1] + e.dir[2] * e.dir[2] > 0.5 ? e.dir : nullptr; world.fxBlast(e.weapon, e.pos, e.radius, n); nc::fxShot(world, QC_SESFX_EXPLOSION, e.weapon, shotMode, optRunway, nullptr, e.pos, n); break; }   // EXPLOSION: the blast is ours, the scorches the core's
            case 8: {   // BEAM: the rail. Its IMPACT (when it hit) follows: one call for the shot, so one beam
              const nc::Sim::Event* hit = ei + 1 < evs.size() && evs[ei + 1].type == 2 && evs[ei + 1].weapon == e.weapon && !memcmp(evs[ei + 1].pos, e.to, sizeof e.to) ? &evs[ei + 1] : nullptr;
              nc::fxShot(world, QC_SESFX_HITSCAN, e.weapon, shotMode, optRunway, e.pos, e.to, hit ? hit->dir : nullptr, hit ? hit->surface : 0, hit ? 0.5 : 1.0); if (hit) ++ei; break; }
            case 10: nc::fxShot(world, QC_SESFX_HOOK_LAND, e.weapon, shotMode, optRunway, hookFrom, e.pos, nullptr); break;   // HOOK_ATTACH
            case 11: ownHook.on = true; ownHook.attached = e.id == 1; ownHook.time = ta; memcpy(ownHook.pos, e.pos, sizeof ownHook.pos); break;   // HOOK_UPDATE: the head; the cable is drawn below, every frame
            case 12: ownHook.on = false; world.fxHookEnd(); break;
            case QC_EV_RESET: ownHook.on = false; world.fxHookEnd(); world.fxClearOwnProjectiles(); break;
          } }
          if (ownHook.on) { if (followN < 0) world.fxHook(hookFrom, ownHook.pos, ownHook.time, ownHook.attached); else world.fxHookEnd(); }
          if (optFxTest && frames == 5) { const double o[3] = {0, 50, 300}; static const char* nm[6] = {"+x", "-x", "+y", "-y", "+z", "-z"}; static const double ax[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (int k = 0; k < 6; ++k) { double d = -1, n[3] = {}; int fl = 0; const bool h = sim.trace(o, ax[k], d, n, fl); nc::logf("trace %s: %s dist %.1f normal %.2f %.2f %.2f", nm[k], h ? "hit" : "none", d, n[0], n[1], n[2]); } }
          if (optFxTest && frames == (optShot.empty() ? 40 : optShotFrame - 2)) {   // one of each, in front of the start: a rocket and its blast, a plasma ball, a BFG ball, a grenade, a rail beam, a plasma scorch
            const double up[3] = {0, 1, 0}, p1[3] = {-40, 30, 300}, v1[3] = {0, 0, 1}, p2[3] = {0, 30, 300}, p3[3] = {40, 30, 300}, p4[3] = {80, 30, 300}, e1[3] = {-30, 1, 400}, e2[3] = {30, 1, 400}, b0[3] = {20, 40, 20}, b1[3] = {20, 40, 700};
            world.fxProjectile(1, 5, p1, v1, true); world.fxProjectile(2, 8, p2, v1, true); world.fxProjectile(3, 9, p3, v1, true); world.fxProjectile(4, 4, p4, v1, true); world.fxBlast(5, e1, 120); world.fxMark(3, e1, up, 100, optRunway); world.fxBlast(8, e2, 60); world.fxMark(5, e2, up, 0, optRunway); world.fxMark(4, e2, up, 0, optRunway); world.fxBeam(b0, b1); const double h0[3] = {-15, 40, 10}, h1[3] = {-60, 60, 380}; world.fxHook(h0, h1, 0.5, true); }
          peerFx.update(world, ta, optRunway, following ? followN : 0, hookFrom);
          if (sessionAudio) {
            QcCue peerClock{};
            peerClock.gain = ta;
            nc::soundEvent(QC_SOUND_PEERS, peerClock);
          }
          world.fxUpdate(std::min(0.1, ft), cam);
        }
        if (!gunHandle.empty()) {
          const bool firing = following ? ta - followPose.reserved[0] < 0.15 : gunState.weaponState == 1;
          const bool attack = following ? firing : followN < 0 && (heldMask & (1u << 7));
          const auto it = gunHandle.find(held);
          world.placeViewGun(it == gunHandle.end() ? -1 : it->second, camera.gun);
          uint32_t glow = 0;   // own powerups only (HudNumbers.puIndex: 0 quad, 1 haste, 3 battle suit)
          if (followN < 0) for (uint32_t i = 0; i < gunState.puCount && i < 8; ++i) glow |= gunState.puIndex[i] == 0 ? 1u : gunState.puIndex[i] == 3 ? 2u : gunState.puIndex[i] == 1 ? 4u : 0u;
          world.setGunGlow(glow, ta);
          world.spinViewRotors(cameraDelta, held == 2 && firing, held == 1 && attack);
        }
        world.record(cb, sceneFb, rw, rh, cam, t, poseBytes.data(), nPoses, usePost ? nullptr : overlay);
      }
      else world.record(cb, sceneFb, rw, rh, cam, t);
      if (usePost) post.record(cb, rp, sc.fbs[idx], sc.w, sc.h, g_set->gamma, g_set->contrast, overlay);
    } else {
      VkClearValue cv[2]; const float k = optRunway ? 0.12f : 0.15f + 0.1f * (float)((frames / 30) & 1); cv[0].color = optRunway ? VkClearColorValue{{0.12f, 0.14f, 0.18f, 1}} : VkClearColorValue{{k, 0.05f, 0.1f, 1}};   // no world: a steady dark colour for the runway (not drawn yet), the diagnostic flash otherwise cv[1].depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = rp; rb.framebuffer = sc.fbs[idx]; rb.renderArea = {{0, 0}, {(uint32_t)sc.w, (uint32_t)sc.h}}; rb.clearValueCount = 2; rb.pClearValues = cv;
      vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb);
    }
    if (!optShot.empty() && frames == optShotFrame) {   // copy the finished image out of the swapchain (it is in PRESENT_SRC after the screen pass)
      shotW = sc.w; shotH = sc.h; shotBuf = nc::makeBuf(c, (size_t)shotW * shotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      nc::imageBarrier(cb, sc.images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy bc = {}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)shotW, (uint32_t)shotH, 1};
      vkCmdCopyImageToBuffer(cb, sc.images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuf.buf, 1, &bc);
      nc::imageBarrier(cb, sc.images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      shotPending = true;
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1); qValid = true;
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &acq; sub.pWaitDstStageMask = &wait;
    sub.commandBufferCount = 1; sub.pCommandBuffers = &cb; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &sc.done[idx]; 
    NC_CK(vkResetFences(c.dev, 1, &fence));
    if (latency.active()) latency.submit();
    NC_CK(vkQueueSubmit(c.queue, 1, &sub, fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sc.done[idx]; pi.swapchainCount = 1; pi.pSwapchains = &sc.sc; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(c.queue, &pi);
    if (resizeStarted && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR)) {
      nc::logf("resize: first Vulkan present accepted after %.1f ms", 1000.0 * (nc::qpcNow() - resizeStarted) / freq);
      resizeStarted = 0;
    }
    const int64_t presentReturned = latency.active() ? nc::qpcNow() : 0;
    if (g_pageJs) {
      for (size_t i = 0; i < g_deferredTextCount; ++i) g_pageJs(g_deferredTextJs[i].c_str());
      if (sharedMatch) {
        const std::string delta = matchFeed.delta(sessionHud, optMapName);
        if (!delta.empty() || !simPush.empty()) {
          if (simPush.empty()) simPush = "{";
          else if (!delta.empty()) simPush += ',';
          if (!delta.empty()) { simPush += "\"hud\":"; simPush += delta; }
          simPush += '}';
          const std::string script = "window.qrazyNativeSim&&window.qrazyNativeSim(" + simPush + ")";
          g_pageJs(script.c_str());
          simPush.clear();
        }
        // Use the same ordered bridge queue for the finish HUD and the result.
        g_runs.service(nc::inputSeconds(nc::qpcNow()), g_pageJs);
      }
    }
    g_deferredTextCount = 0;
    if (haveAudio) {
      audio.setListener(p_listener, g_input.yaw);
      if (!optSound.empty() && nowQ >= nextTestSound) {
        nextTestSound = nowQ + 2 * freq;
        audio.play(optSound, nullptr);
      }
    }
    drainAudio();
    if (presentReturned) latency.present(ticksRun, presentReturned);
    if (optSim && bool(camera.inputFlags & 1u) != inputHistory) {
      inputHistory = bool(camera.inputFlags & 1u);
      if (g_simOut) g_simOut(inputHistory ? "{\"inputHistory\":true}" : "{\"inputHistory\":false}");
    }
    if (poseReady) {
      const double* p = p_listener;
      { std::lock_guard<std::mutex> pg(g_poseMu); g_pose[0] = p[0]; g_pose[1] = p[1]; g_pose[2] = p[2]; g_pose[3] = g_input.yaw; g_pose[4] = g_input.pitch; for (int k = 0; k < 3; ++k) { g_pose[5 + k] = cur.pos[k]; g_pose[8 + k] = cur.vel[k]; } g_pose[11] = sim.out()[6] != 0; g_pose[12] = cur.crouched; g_pose[13] = followN > 0 && haveFollow ? followPose.weapon : sim.weaponHeld(); g_poseValid = true; }
      if (poseNowWanted) { poseNowWanted = false; g_poseNow = true; }   // where the player is: [0..2] the eye, [3] yaw, [4] pitch (radians), [5..7] the feet, [8..10] the velocity, [11] on ground, [12] crouched; the page's menu background and its /viewpos
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) continue; if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) nc::fatal("present: %d", (int)pr);
    ++frames; ++windowFrames;
    if (t - windowStart >= 2.0) {
      const nc::WorldStats& st = world.stats();
      nc::logf("%.1f fps, worst frame %.2f ms | plan %.2f ms record %.2f ms | passes %d draws %d | ticks %lld", windowFrames / (t - windowStart), worst * 1000, st.planMs, st.recordMs, st.passes, st.worldDraws, (long long)ticksRun);
      if (haveAudio) { const nc::AudioStats as = audio.stats(); nc::logf("audio: %d voices, played %llu, missing %llu, underruns %llu", as.voices, (unsigned long long)as.played, (unsigned long long)as.missing, (unsigned long long)as.underruns); }
      shownFps = (int)(windowFrames / (t - windowStart) + 0.5); windowStart = t; windowFrames = 0; worst = 0;
    }
  }
  g_input.recordMotion = false;
  latency.finish();
  if (stopMatch.managed) {
    nc::matchOp(QC_MATCH_STOP);
    stopMatch.managed = false;
  }
  audio.shutdown(); g_input.setCapture(false);
  if (optBench) {
    auto pct = [](std::vector<double> v, double p) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))]; };
    auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / (double)v.size(); };
    nc::logf("bench %zu frames | interval ms: mean %.3f p50 %.3f p95 %.3f p99 %.3f max %.3f (%.1f fps)", benchInterval.size(), mean(benchInterval), pct(benchInterval, .5), pct(benchInterval, .95), pct(benchInterval, .99), pct(benchInterval, 1.0), 1000.0 / std::max(1e-9, mean(benchInterval)));
    nc::logf("bench cpu plan+record ms: mean %.3f p99 %.3f max %.3f | gpu ms: mean %.3f p50 %.3f p99 %.3f max %.3f", mean(benchCpu), pct(benchCpu, .99), pct(benchCpu, 1.0), mean(benchGpu), pct(benchGpu, .5), pct(benchGpu, .99), pct(benchGpu, 1.0));
    if (!benchMinimapCpu.empty() && !benchMinimapGpu.empty())
      nc::logf("bench minimap ms: CPU mean %.3f p99 %.3f | GPU mean %.3f p99 %.3f (%zu samples), %llu C++ allocations",
               mean(benchMinimapCpu), pct(benchMinimapCpu, .99), mean(benchMinimapGpu),
               pct(benchMinimapGpu, .99), benchMinimapGpu.size(), (unsigned long long)benchMinimapAllocs);
  }
  vkDeviceWaitIdle(c.dev); if (shotPending) writePngBgra(optShot, shotBuf.ptr, shotW, shotH);
  nc::destroySwapchain(c, sc); nc::destroyContext(c); gpuGuard.done = true;   // release the window: the next match may start on the same one
  platShutdown(plat);
  nc::logf("exit after %ld frames", frames);
  return 0;
}
