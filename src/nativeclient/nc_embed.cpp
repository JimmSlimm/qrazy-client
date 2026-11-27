// Embedding in the CEF host (src/sdlcef/windows/app_windows.cpp, dev builds): the host starts the native client on its own window, F2 flips between the page (menus, login) and the native view (a match).
// These are the functions the host's QRAZY_TRUE_CLIENT hooks call. The command line is the dev file true-client.cmdline: the same options as the standalone executable.
#include "nc_app.h"
#include "qrazy_core_api.h"
#include "nc_coreload.h"
#include "nc_hub.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if !defined(NC_WINDOWS)
#include <filesystem>
#endif

static std::mutex g_errMu; static std::string g_error, g_errorCode;   // why the native view stopped, for the host to show; the code is the page's refusal code (asset, bsp, core, unsupported)
static void setError(const std::string& e, const char* code = nullptr) { std::lock_guard<std::mutex> g(g_errMu); if (!g_error.empty()) return; g_error = e; g_errorCode = code ? code : e.find("map load failed") == 0 || e.find("sim:") == 0 ? "bsp" : e.find("GPU") != std::string::npos || e.find("Vulkan") != std::string::npos ? "unsupported" : "core"; }
// Pending host setting: -1 means no change, 0 means VSync, positive values are the FPS cap.
static std::atomic<int> g_pendingFrameCap{-1};
static std::thread g_th; static std::atomic<bool> g_visible{false}, g_stop{false};

static std::string moduleDir() {
#if defined(NC_WINDOWS)
  HMODULE m = nullptr; if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&moduleDir, &m)) return {};
  char p[MAX_PATH]; const DWORD n = GetModuleFileNameA(m, p, MAX_PATH); if (!n || n >= MAX_PATH) return {};
  std::string s(p, n); for (auto& c : s) if (c == (char)92) c = '/'; const size_t slash = s.rfind('/'); return slash == std::string::npos ? std::string() : s.substr(0, slash);
#else
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::string() : executable.parent_path().string();
#endif
}

#if defined(NC_WINDOWS)
#include <delayimp.h>
// The game core is a separate library (qrazy_core_api.h). It is chosen and loaded by nc::loadCore before any core call; the delay-load stub then asks for "qrazy_core.dll" by name and gets the module we loaded.
static FARPROC WINAPI coreDelayHook(unsigned event, PDelayLoadInfo info) { if (event == dliNotePreLoadLibrary && info && info->szDll && !_stricmp(info->szDll, "qrazy_core.dll") && nc::coreModule()) return (FARPROC)nc::coreModule(); return nullptr; }
extern "C" const PfnDliHook __pfnDliNotifyHook2 = coreDelayHook;
#endif
static std::string g_coreCache;   // the folder the host keeps downloaded cores in
static bool g_coreSelectionDone;
bool tc_prepare_core() {
  if (g_coreSelectionDone) return nc::coreModule() != nullptr;
  const std::string dir = moduleDir();
  if (dir.empty()) {
    setError("The client runtime directory could not be located.", "core");
    return false;
  }
  std::string used, why;
#if defined(NC_WINDOWS)
  const char* library = "/qrazy_core.dll";
#else
  const char* library = "/libqrazy_core.so";
#endif
  if (!nc::loadCore({g_coreCache, dir + "/core"}, dir + library,
                    QC_INTERFACE_VERSION, used, why)) {
    setError(why, "core");
    return false;
  }
  return true;
}

bool tc_hub_ready() { return tc_prepare_core() && bool(nc::coreNet()); }
void tc_hub_service() { nc::appHub().service(); }
void tc_hub_shutdown() { nc::appHub().shutdown(); }

void tc_start(NativeWindow hwnd, const char* cmdline) {
  nc::g_throwOnFatal = true; appClearOps(); appTakeStarted(); { std::lock_guard<std::mutex> g(g_errMu); g_error.clear(); g_errorCode.clear(); }
  if (!tc_prepare_core()) return;
  { const std::string dir = moduleDir() + "/native-assets"; FILE* f = fopen((dir + "/hud/hud-quads.table.bin").c_str(), "rb"); if (f) { fclose(f); appSetAssetsDir(dir); } }   // shipped next to the program; absent in a dev tree: the dev paths apply
  std::vector<std::string> args; args.push_back("qrazy"); std::string cur; bool inq = false, any = false;
  for (const char* p = cmdline;; ++p) {
    const char ch = *p;
    if (ch == '"') { inq = !inq; any = true; continue; }
    if ((ch == ' ' || ch == 0) && !inq) { if (any) args.push_back(cur); cur.clear(); any = false; if (!ch) break; continue; }
    cur += ch; any = true;
  }
  g_pendingFrameCap = -1;
  g_stop = false;
  g_th = std::thread([hwnd, args]() mutable {
    std::vector<char*> argv; for (auto& a : args) argv.push_back(&a[0]);
    try { const int rc = appRun((int)argv.size(), argv.data(), hwnd, &g_visible, &g_stop, &g_pendingFrameCap); if (rc != 0) setError("The graphics self-check failed.", "unsupported"); }
    catch (const std::exception& e) { nc::logf("native client stopped: %s", e.what()); setError(e.what()); }
  });
}
// Once at launch: where downloaded cores are kept, and the site's manifest. The check and download run in the background; a newer core is used from the next start.
void tc_core_prepare(const char* cacheDir, bool fetch, bool staging) {
  { std::string log = cacheDir ? cacheDir : ""; for (auto& c : log) if (c == (char)92) c = (char)47; const size_t s = log.rfind((char)47); if (s != std::string::npos) nc::setLogFile(log.substr(0, s) + "/native.log"); }   // <profile>/native.log: what the native view does, for diagnosis
  g_coreCache = cacheDir ? cacheDir : ""; for (auto& c : g_coreCache) if (c == (char)92) c = '/';
  tc_prepare_core();
  g_coreSelectionDone = true;
  if (fetch) nc::startCoreUpdate(g_coreCache, staging, QC_INTERFACE_VERSION);
}
void tc_core_wait() { nc::waitCoreUpdate(); }
void tc_set_menu_request(void (*f)()) { appSetMenuRequest(f); }
void tc_set_tab(void (*f)(bool)) { appSetTabCallback(f); }
void tc_board(const char* json) { appSetBoard(json ? json : ""); }
bool tc_get_pose(double* out14) { return appGetPose(out14); }
bool tc_take_started() { return appTakeStarted(); }
bool tc_audio_feed() { return appAudioFeed(); }
bool tc_hud_feed() { return appHudFeed(); }
void tc_hud_ready() { appHudReady(); }
std::string tc_error_code() { std::lock_guard<std::mutex> g(g_errMu); return g_errorCode; }   // of the last error taken
bool tc_take_pose_request() { return appTakePoseRequest(); }
void tc_sim_op(const char* json) { appSimOp(json ? json : ""); }
bool tc_typing() { return appTyping(); }
bool tc_cursor() { return appCursor(); }
int tc_click(int x, int y, int* element, int* index, char* url, int cap) { return appClickElement(x, y, element, index, url, cap); }
void tc_log_mouse(int kind, int x, int y, bool shift) { appLogMouse(kind, x, y, shift); }
void tc_log_wheel(double notches) { appLogWheel(notches); }
void tc_set_sim_out(void (*f)(const char*)) { appSetSimOut(f); }
void tc_set_js_out(void (*f)(const char*)) { appSetJsOut(f); }
void tc_text_unit(unsigned unit) { appTextUnit(unit); }
void tc_text_commit(const char* utf8) { appTextCommit(utf8); }
void tc_text_preedit(const char* utf8, unsigned a, unsigned b, unsigned c, unsigned d) { appTextPreedit(utf8, a, b, c, d); }
bool tc_text_composing() { return appTextComposing(); }
bool tc_caret_rect(float* out4) { return appCaretRect(out4); }
void tc_log(const char* message) { nc::logf("host: %s", message ? message : ""); }   // the host's own notes go to the same log
void tc_settings(const char* json) { appSetSettings(json ? json : ""); }
void tc_hud(const char* json) { appSetPageHud(json ? json : ""); }
// The host polls this: true once, with the reason, after the native view stopped on its own (graphics could not start, the device was lost, the map would not load).
bool tc_take_error(char* buf, size_t n) { std::lock_guard<std::mutex> g(g_errMu); if (g_error.empty() || !buf || n == 0) return false; std::snprintf(buf, n, "%s", g_error.c_str()); g_error.clear(); return true; }
void tc_set_frame_cap(int fps) { g_pendingFrameCap = fps; }
void tc_set_visible(bool visible) { g_visible = visible; }
void tc_stop() { g_stop = true; if (g_th.joinable()) g_th.join(); }
void tc_push_state(const uint8_t*, size_t) {}
#if !defined(NC_WINDOWS)
// Linux host hooks: forward every SDL event (any thread) and apply the capture wish on the host's main thread each frame.
void tc_event(const SDL_Event* e) { appOnSdlEvent(*e); }
bool tc_wants_capture() { return g_visible.load() && appCaptureWanted(); }
#endif   // the page's game state is not used: the native client runs its own sim
