#define _USE_MATH_DEFINES
#include "nc_hud.h"
#include "nc_assets.h"
#include "nc_shader.h"
#include "nc_platform.h"
#include "text_api.h"
#include "qrazy_core_api.h"
#include "hud_list.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <array>
#include <mutex>
#include <set>
#include <vector>
#include <ctime>
#if defined(NC_WINDOWS)
#include <windows.h>
#endif

namespace nc {

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
struct HudVertex { float x, y, u, v; uint32_t c; float cx, cy, cr; };   // cx, cy, cr: the circle clip in pixels (cr 0 = none)
static_assert(sizeof(HudVertex) == 32, "HudVertex");
struct HudDraw { uint32_t tex, blend, first, count; int32_t sx, sy; uint32_t sw, sh; };

static const uint32_t PAGE_TEX = 0x10000;   // texture id of an atlas page: PAGE_TEX + page id
// the constant tables of the game's HUD (hud-status.js BAR_WEAPONS / POWERUP_ICONS via render-core/text/hud-state.mjs HUD_TABLES)
static const uint32_t BAR_COLORS[9] = {4294943565u, 4285454325u, 4284134655u, 4285717615u, 4284115967u, 4289789757u, 4294937778u, 4283301757u, 4286624964u};
static const int BAR_LOWS[9] = {0, 20, 3, 3, 3, 3, 20, 3, 0};
static const uint32_t PU_COLORS[6] = {4294933581u, 4281584895u, 4283256319u, 4278235391u, 4294937783u, 4292663503u};
static const uint32_t PU_ICONS[6] = {12, 13, 14, 15, 16, 17};
static const char* PU_LABELS[6] = {"QUAD DAMAGE", "HASTE", "REGENERATION", "BATTLE SUIT", "FLIGHT", "INVISIBILITY"};

struct Hud::Impl {
  Context c; VkRenderPass rp = VK_NULL_HANDLE; bool ok = false;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; VkPipeline pipes[3] = {}; VkDescriptorPool pool = VK_NULL_HANDLE; VkSampler sampLinear = VK_NULL_HANDLE, sampNearest = VK_NULL_HANDLE;
  Buf vb, stage; std::map<uint32_t, VkDescriptorSet> texSets; std::vector<Image> images;
  std::map<uint32_t, hud::GlyphDef> defs; struct Page { Image img; bool ready = false; }; std::map<uint32_t, Page> pages;
  std::vector<std::vector<uint8_t>> fonts; float uiScale = 1.f; std::string chat; bool chatHistory = false; Scoreboard board; bool boardOn = false; bool cgazOn = false; uint8_t cgazState[136] = {};
  struct ElHit { std::vector<std::array<float, 4>> rects; std::vector<std::string> links; };
  float clippyRect[4] = {}; bool clippyOn = false; double clippyMs = -1; std::string clippyText; bool clippyTextNew = false;   // Clippy: the page's state; the rect (layout px) the core drew him in, under hitMu
  std::mutex hitMu; std::map<int, ElHit> hits;   // the clickable elements: the flagged runs (layout px) and the page's link addresses (element 16)
  // the log views: the open box (-1 none) and where its log area is, the pointer focus, a running drag, and the queued mouse events
  struct LogEv { int kind; float x, y; uint32_t mods, clicks; double wheel; };
  bool logDrag = false; double logTickMs = 0; float lastX = 0, lastY = 0; bool btnDown = false; double tfCfgH = -1, tfCfgO = -1, tfCfgS = -1;
  std::mutex logMu; std::vector<LogEv> logEvs; double lastDownMs = 0; float lastDownX = 0, lastDownY = 0; uint32_t downClicks = 0;
  void pumpLog(); void pushLayout();
  uint32_t speedoMode = 0; double speedoMax = 1200;
  bool playerHudVisible = true, playerHudFollowing = false;
  float rasterScale = 1.f;
  std::map<int, Element> liveTpl; double livePitch = 0; bool liveSlick = false, livePaused = false; uint32_t liveKeys = 0; int liveLiquid = 0, liquidSent = -1;   // templates of the live elements and the state they show
  const QcMinimapOut* mmView = nullptr; TextMinimapState mm = {}; const float* mmTrail = nullptr; const float* mmGhost = nullptr;
  std::string pausedHint;
  std::string timerStatus; double conHeight = 0.5, conOpacity = 0.9, conScale = 1.0;
  TextHudLayout hudLayout;
  void drawBoard(int W, int H);
  std::vector<HudDraw> draws; const uint8_t* cur = nullptr; uint32_t curBytes = 0; uint32_t serial = 0; bool haveSerial = false; uint32_t nVerts = 0;

  void registerTexture(uint32_t id, const Image& im, bool nearest);
  void apply(VkCommandBuffer cb, const uint8_t* wire, size_t len);
  void buildVertices(const uint8_t* wire);
};

Hud::Hud() : m(new Impl()) {}
Hud::~Hud() { delete m; }
void Hud::setChat(const std::string& joined) { m->chat = joined; }
void Hud::setChatHistory(bool on) { m->chatHistory = on; }
void Hud::setLayout(const std::vector<std::pair<std::string, double>>& fields) {
  Impl& h = *m; if (!h.ok) return; TextHudLayout& L = h.hudLayout;
  struct F { const char* name; void* at; bool isFloat; };
#define U(n) {#n, &L.n, false}
#define FL(n) {#n, &L.n, true}
  const F table[] = {U(crosshair), U(crosshairSize), U(crosshairThickness), U(crosshairColor), U(crosshairHidden), FL(timerX), FL(timerY), FL(timerScale), U(timerHidden), FL(vitalsX), FL(vitalsY), FL(vitalsScale), U(vitalsFlags),
    U(speedBarMax), U(speedBarWidth), U(speedBarHeight), U(speedFlags), FL(speedX), FL(speedY), FL(speedScale), FL(fpsX), FL(fpsY), FL(fpsScale), U(fpsHidden),
    U(wbLayout), U(wbFlags), U(wbOnlyOwned), U(wbOnlyWithAmmo), FL(wbX), FL(wbY), FL(wbScale), U(puLayout), U(puFlags), FL(puX), FL(puY), FL(puScale)};
#undef U
#undef FL
  for (const auto& kv : fields) for (const F& f : table) if (kv.first == f.name) { if (f.isFloat) *(float*)f.at = (float)kv.second; else *(uint32_t*)f.at = kv.second < 0 ? 0u : (uint32_t)(kv.second + 0.5); }
  uint32_t oledRows = 0, oledWidgets = 0;
  for (const auto& [name, value] : fields) {
    // The page builds masks with JavaScript bitwise operators, which return signed 32-bit values.
    if (!(value >= INT32_MIN && value <= UINT32_MAX) || value != std::trunc(value)) continue;
    if (name == "oledRows") oledRows = static_cast<uint32_t>(static_cast<int64_t>(value));
    if (name == "oledWidgets") oledWidgets = static_cast<uint32_t>(static_cast<int64_t>(value));
  }
  h.pushLayout();
  text_hud_set_oled(oledRows, oledWidgets);
}
void Hud::Impl::pushLayout() {   // the player's layout into the engine; with a QL speedometer style (1-3) the Qrazy speedometer (speedFlags bit 0 = hidden) is not drawn
  TextHudLayout L = hudLayout; if (!playerHudVisible || (speedoMode >= 1 && speedoMode <= 3)) L.speedFlags |= 1u;
  memcpy(text_hud_layout_addr(), &L, sizeof L);
}
void Hud::setUiScale(float scale) {
  Impl& h = *m; if (!h.ok || scale < 0.5f || scale > 8.f) return; const float s = std::floor(scale * 100.f + .5f) / 100.f; if (s == h.uiScale) return;
  h.uiScale = s; logf("HUD ui scale %.2f", s);
}
double Hud::nowMs() { return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0; }
static bool isLiveEl(int e) { return e == 5 || e == 6 || e == 12 || e == 13; }
static void elSet(const Hud::Element& e, uint32_t flags, const std::string& text) {
  TextRecipeIn in; memset(&in, 0, sizeof in); in.elX = e.elX; in.elY = e.elY; in.elScale = e.elScale; in.num = e.num; in.d0 = e.d0; in.d1 = e.d1; in.d2 = e.d2; in.color = e.color; in.color2 = e.color2; in.flags = flags; in.aux = e.aux; in.textLen = (uint32_t)text.size();
  text_hud_el_set((uint32_t)e.element, e.kind, &in, (const uint8_t*)text.data(), e.until);
}
void Hud::setElement(const Element& e) { if (isClickable(e.element)) { std::lock_guard<std::mutex> lk(m->hitMu); m->hits[e.element].links = e.links; } if (isLiveEl(e.element)) m->liveTpl[e.element] = e; else elSet(e, e.flags, e.text); }
void Hud::clearElement(int element) { if (isClickable(element)) { std::lock_guard<std::mutex> lk(m->hitMu); m->hits.erase(element); } m->liveTpl.erase(element); text_hud_el_clear((uint32_t)element); }
void Hud::clearAllElements() { { std::lock_guard<std::mutex> lk(m->hitMu); m->hits.clear(); } m->liveTpl.clear(); text_hud_el_clear_all(); }
bool Hud::hitClickable(double px, double py, Click* out) {
  Impl& h = *m; std::lock_guard<std::mutex> lk(h.hitMu); const float x = (float)(px / h.uiScale), y = (float)(py / h.uiScale);
  for (const auto& kv : h.hits) for (size_t i = 0; i < kv.second.rects.size(); ++i) { const auto& r = kv.second.rects[i]; if (x < r[0] || y < r[1] || x >= r[0] + r[2] || y >= r[1] + r[3]) continue;
    Click c; c.element = kv.first; c.index = (int)i;
    if (kv.first == 16) { if (i + 1 == kv.second.rects.size()) c.index = -1; else if (i < kv.second.links.size()) c.url = kv.second.links[i]; else return false; }   // the last flagged run is Close
    if (out) *out = c; return true; }
  return false;
}
void Hud::setPausedHint(const std::string& t) { m->pausedHint = t; }
void Hud::setLive(double pitchDeg, bool slickOn, uint32_t keyMask, bool paused, int liquid) { Impl& h = *m; h.liveLiquid = liquid; h.livePitch = pitchDeg; h.liveSlick = slickOn; h.liveKeys = keyMask & 63u; h.livePaused = paused; }
float Hud::uiScale() const { return m->uiScale; }
bool Hud::minimapBox(int width, int height, float out[4]) const {
  if (!m->mmView || !m->cur || !text_hud_box(HUD_BOX_MINIMAP, out)) return false;
  const auto* header = reinterpret_cast<const hud::Header*>(m->cur);
  const float sx = (float)width / header->fbWidth, sy = (float)height / header->fbHeight;
  out[0] *= sx; out[1] *= sy; out[2] *= sx; out[3] *= sy;
  return true;
}
void Hud::setMinimap(const TextMinimapState& state, const QcMinimapOut& view, const float* trail, const float* ghost) {
  Impl& h = *m;
  h.mmView = view.w > 0 ? &view : nullptr;
  h.mm = state;
  h.mmTrail = trail; h.mmGhost = ghost;
}
void Hud::setSpeedometer(uint32_t mode, double maxSpeed) { m->speedoMode = mode; m->speedoMax = maxSpeed > 0 ? maxSpeed : 1200; if (m->ok) m->pushLayout(); }
void Hud::setPlayerHudView(bool playing, bool following) {
  Impl& h = *m;
  const bool visible = playing || following;
  if (h.playerHudVisible == visible && h.playerHudFollowing == following) return;
  h.playerHudVisible = visible;
  h.playerHudFollowing = following;
  text_hud_el_clear(5);
  text_hud_el_clear(12);
  h.pushLayout();
}
void Hud::setCgaz(bool on, const void* state136, double yawDeg, double fovX, double ypos, uint32_t flags) {
  Impl& h = *m; h.cgazOn = on && state136; if (!h.cgazOn) return; static_assert(sizeof(TextCgazState) == 136, "TextCgazState"); memcpy(h.cgazState, state136, 136);
  TextCgazState* st = (TextCgazState*)h.cgazState; st->yaw = yawDeg; st->fovX = fovX; st->ypos = ypos; st->flags = flags;
}
void Hud::setClippy(double ms, const std::string& text) { std::lock_guard<std::mutex> lk(m->hitMu); m->clippyOn = ms >= 0; m->clippyMs = ms >= 0 ? nowMs() - ms : -1; if (!text.empty()) { m->clippyText = text.size() > 512 ? text.substr(0, 512) : text; m->clippyTextNew = true; } }
bool Hud::hitClippy(double px, double py) { Impl& h = *m; const float x = (float)(px / h.uiScale), y = (float)(py / h.uiScale); std::lock_guard<std::mutex> lk(h.hitMu); const float* r = h.clippyRect; return r[2] > 0 && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]; }
void Hud::setTimerStatus(const std::string& text) { m->timerStatus = text; }
void Hud::setConsoleLook(double height, double opacity, double scale) { m->conHeight = std::min(1.0, std::max(0.1, height)); m->conOpacity = std::min(1.0, std::max(0.0, opacity)); m->conScale = std::min(3.0, std::max(0.5, scale)); }
void Hud::setBoard(const Scoreboard* b) { if (b && b->held) { m->board = *b; m->boardOn = true; } else m->boardOn = false; }

// ---- the core text field (text_tf_*): the chat box, team chat and the console. Every core call is on the render thread; the host window's thread only queues characters / IME text.
namespace tf {
static std::atomic<uint32_t> g_state{0}; static void (*g_js)(const char*) = nullptr;
struct In { int kind; uint32_t cp; std::string s; uint32_t a, b, c, d; };   // kind 0 char, 1 commit, 2 preedit
static std::mutex g_mu; static std::vector<In> g_in; static std::mutex g_caretMu; static float g_caret[4] = {};
void setJsOut(void (*f)(const char*)) { g_js = f; }
static const char* modeName(uint32_t m) { return m == TF_CHAT ? "chat" : m == TF_TEAM ? "team" : m == TF_CONSOLE ? "console" : ""; }
static std::string jsString(const std::string& t) {   // a JS / JSON string literal
  std::string r = "\""; char b[8];
  for (const unsigned char c : t) { if (c == '"' || c == 92) { r += (char)92; r += (char)c; } else if (c < 0x20) { snprintf(b, sizeof b, "%cu%04x", 92, c); r += b; } else r += (char)c; }
  return r + "\"";
}
static void emit(const std::string& js) { if (g_js) g_js(js.c_str()); }
void queueChar(uint32_t cp) { std::lock_guard<std::mutex> lk(g_mu); if (g_in.size() < 1024) g_in.push_back({0, cp, std::string(), 0, 0, 0, 0}); }
void queueCommit(const std::string& u) { std::lock_guard<std::mutex> lk(g_mu); if (g_in.size() < 1024) g_in.push_back({1, 0, u, 0, 0, 0, 0}); }
void queuePreedit(const std::string& u, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { std::lock_guard<std::mutex> lk(g_mu); if (g_in.size() < 1024) g_in.push_back({2, 0, u, a, b, c, d}); }
void setCaret(float x, float y, float w, float h) { std::lock_guard<std::mutex> lk(g_caretMu); g_caret[0] = x; g_caret[1] = y; g_caret[2] = w; g_caret[3] = h; }
void caretRect(float out[4]) { std::lock_guard<std::mutex> lk(g_caretMu); for (int i = 0; i < 4; ++i) out[i] = g_caret[i]; }
uint32_t state() { return g_state.load(); }
bool composing() { return (g_state.load() & TF_STATE_COMPOSING) != 0; }
void flushEvents() {   // poll to empty after every input call: the page learns open / closed / submitted lines, the clipboard works
  TextFieldEvent ev;
  while (text_tf_poll(&ev)) {
    switch (ev.type) {
      case TF_EV_OPENED: logf("text field: opened (%s)", modeName(ev.a)); emit(std::string("window.qrazyNativeTextState&&window.qrazyNativeTextState({open:true,mode:\"") + modeName(ev.a) + "\",reason:0})"); break;
      case TF_EV_CLOSED: logf("text field: closed (%s, reason %u)", modeName(ev.a), ev.b); emit(std::string("window.qrazyNativeTextState&&window.qrazyNativeTextState({open:false,mode:\"") + modeName(ev.a) + "\",reason:" + std::to_string(ev.b) + "})"); break;
      case TF_EV_SUBMIT: { const std::string t((const char*)text_tf_event_bytes(), ev.len); emit(std::string("window.qrazyNativeTextSubmit&&window.qrazyNativeTextSubmit({mode:\"") + modeName(ev.a) + "\",text:" + jsString(t) + "})"); break; }
      case TF_EV_CLIPBOARD_SET: setClipboardText(std::string((const char*)text_tf_event_bytes(), ev.len)); break;
      case TF_EV_PASTE_REQUEST: { const std::string c = getClipboardText(); text_tf_paste((const uint8_t*)c.data(), (uint32_t)c.size()); break; }
      case TF_EV_ESCAPE_RELEASED: emit("window.qrazyNativeTextEscapeReleased&&window.qrazyNativeTextEscapeReleased()"); break;
      default: break;   // TF_EV_FONT_WANTED: the console's monospace face is loaded with the other fonts at start
    }
  }
  g_state = text_tf_state();
}
uint32_t key(uint32_t tfKey, uint32_t mods, bool down, uint32_t hostId) { const uint32_t r = text_tf_key((hostId << 8) | (tfKey & 0xFF), mods, down ? 1u : 0u); flushEvents(); return r; }
bool open(uint32_t mode, uint32_t hostId) { text_tf_open(mode, hostId); flushEvents(); return (g_state.load() & (TF_STATE_CHAT | TF_STATE_TEAM | TF_STATE_CONSOLE)) != 0; }
void close(uint32_t reason) { text_tf_close(reason); flushEvents(); }
void reset() { { std::lock_guard<std::mutex> lk(g_mu); g_in.clear(); } if (text_tf_state()) text_tf_close(6); flushEvents(); }
void frame(double dtMs) {
  std::vector<In> in; { std::lock_guard<std::mutex> lk(g_mu); in.swap(g_in); }
  for (const In& e : in) {
    if (e.kind == 0) text_tf_char(e.cp); else if (e.kind == 1) text_tf_commit((const uint8_t*)e.s.data(), (uint32_t)e.s.size()); else text_tf_preedit((const uint8_t*)e.s.data(), (uint32_t)e.s.size(), e.a, e.b, e.c, e.d);
    flushEvents();
  }
  if (g_state.load()) { text_tf_tick(dtMs); flushEvents(); }
  g_state = text_tf_state();
}
}  // namespace tf

// ---- the log views (text_api.h text_lv_*): the page's operations in order, the mouse / wheel queued from the host's thread, the keys from the render thread
static std::string clampUtf8(const std::string& t, size_t max) { if (t.size() <= max) return t; size_t n = max; while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) --n; return t.substr(0, n); }   // the engine keeps 384 bytes an entry
void Hud::logOp(int view, char op, const std::string& text, uint32_t rgba, uint32_t flags, uint32_t group, uint32_t n) {
  if (!m->ok || view < 0 || view > 1) return; const uint32_t v = (uint32_t)view;
  switch (op) {
    case 'c': text_lv_clear(v); break;
    case 'a': { const std::string t = clampUtf8(text, 384); text_lv_append(v, (const uint8_t*)t.data(), (uint32_t)t.size(), rgba, flags, group); break; }
    case 'r': text_lv_set_repeat(v, n); break;
    case 'p': text_lv_pop(v); break;
    case 'g': text_lv_group_collapse(v, group, n); break;
    default: break;
  }
}
void Hud::logMouse(int kind, double px, double py, bool shift) {
  Impl& h = *m; const float x = (float)(px / h.uiScale), y = (float)(py / h.uiScale); std::lock_guard<std::mutex> lk(h.logMu); const double now = nowMs(); uint32_t clicks = 1;
  if (kind == LOG_DOWN) { clicks = (now - h.lastDownMs < 450 && std::fabs(x - h.lastDownX) < 5 && std::fabs(y - h.lastDownY) < 5) ? h.downClicks % 3 + 1 : 1; h.downClicks = clicks; h.lastDownMs = now; h.lastDownX = x; h.lastDownY = y; }   // the 2nd / 3rd press in a row: double / triple click
  if (h.logEvs.size() < 512) h.logEvs.push_back({kind, x, y, shift ? 1u : 0u, clicks, 0});
}
void Hud::logWheel(double notches) { std::lock_guard<std::mutex> lk(m->logMu); if (m->logEvs.size() < 512) m->logEvs.push_back({3, 0, 0, 0, 0, notches}); }
void Hud::Impl::pumpLog() {   // the mouse on the open text field: text_tf_pointer in layout px; the box draws and handles the log view, the close buttons and the field
  std::vector<LogEv> evs; { std::lock_guard<std::mutex> lk(logMu); evs.swap(logEvs); }
  if (!tf::state()) { logDrag = false; return; }   // no box open: the events are dropped
  const double now = nowMs();
  for (const LogEv& e : evs) {
    if (e.kind != 3) { lastX = e.x; lastY = e.y; }
    if (e.kind == 3) { text_tf_wheel(lastX, lastY, (float)(e.wheel * 3.0 * 20.0)); tf::flushEvents(); continue; }   // three lines a notch (a line is about 20 px)
    if (e.kind == 0) btnDown = true; else if (e.kind == 2) btnDown = false; const uint32_t buttons = btnDown ? 1u : 0u;
    uint32_t r = text_tf_pointer((uint32_t)e.kind, e.x, e.y, e.mods, buttons);
    if (e.kind == 0) { if (e.clicks == 2) r |= text_tf_pointer(3, e.x, e.y, e.mods, buttons); else if (e.clicks == 3) r |= text_tf_pointer(4, e.x, e.y, e.mods, buttons); }
    logDrag = e.kind != 2 && (r & TF_P_TICK);
    tf::flushEvents();
  }
}

// The information board (information-overlay.js layout 3; the page's DOM version is bridge-production.js renderInformation): a centred panel with the map line, the six record cards, the player table by category and the spectators.
static uint32_t col(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }
void Hud::Impl::drawBoard(int W, int H) {
  const float s = std::max(0.8f, std::min(2.f, (float)H / 1080.f)); const Scoreboard& b = board;
  const uint32_t szTitle = text_size_for_px((uint32_t)(26 * s + .5f)), szRow = text_size_for_px((uint32_t)(16 * s + .5f)), szSmall = text_size_for_px((uint32_t)(14 * s + .5f));
  const float pw = std::min((float)W - 48 * s, 1200 * s), pad = 24 * s, iw = pw - 2 * pad, rowH = 40 * s, groupH = 34 * s;
  const float cardsH = b.cards.empty() ? 0.f : 84 * s, headH = 76 * s, tailH = 76 * s;
  const float maxH = (float)H - 16 * s;
  float need = 2 * pad + headH + cardsH + tailH + (b.notice.empty() ? 0.f : 40 * s);
  size_t shown = 0; for (const ScoreGroup& g : b.groups) { if (need + groupH + rowH > maxH) break; need += groupH; for (size_t i = 0; i < g.rows.size() && need + rowH <= maxH; ++i) { need += rowH; ++shown; } }
  const float px = ((float)W - pw) / 2, py = std::max(8 * s, ((float)H - need) / 2);
  text_hud_rect(4, px, py, pw, need, col(0x0b, 0x14, 0x24, 0xf5));
  const uint32_t gold = col(0xfb, 0xbf, 0x24), dim = col(0x9e, 0xaf, 0xc5), soft = col(0xc3, 0xcf, 0xdf), white = 0xFFFFFFFFu;
  const float bw = 2 * s; text_hud_rect(5, px, py, pw, bw, gold); text_hud_rect(5, px, py + need - bw, pw, bw, gold); text_hud_rect(5, px, py, bw, need, gold); text_hud_rect(5, px + pw - bw, py, bw, need, gold);
  auto text = [&](float x, float y, uint32_t ha, uint32_t sz, uint32_t rgba, const std::string& t) { if (!t.empty()) text_hud_lines(6, x, y, ha, 0, sz, rgba, 0, 0, (const uint8_t*)t.data(), (uint32_t)t.size(), 0); };
  const float x0 = px + pad; float y = py + pad;
  text(x0, y, 0, szTitle, gold, b.map); y += 38 * s;
  text(x0, y, 0, szRow, soft, b.info); y += headH - 38 * s;
  if (!b.cards.empty()) {   // six cards across (the page: grid of 6)
    const float gap = 8 * s, cw = (iw - gap * 5) / 6;
    for (size_t i = 0; i < b.cards.size() && i < 6; ++i) { const ScoreCard& c = b.cards[i]; const float cx = x0 + (float)i * (cw + gap);
      text_hud_rect(5, cx, y, cw, 70 * s, c.active ? col(0x1d, 0x25, 0x35) : col(0x15, 0x22, 0x38));
      if (c.active) { text_hud_rect(6, cx, y, cw, 2 * s, col(0x8a, 0x7a, 0x45)); text_hud_rect(6, cx, y + 68 * s, cw, 2 * s, col(0x8a, 0x7a, 0x45)); }
      text(cx + 9 * s, y + 6 * s, 0, szSmall, gold, c.title); text(cx + 9 * s, y + 24 * s, 0, szRow, white, c.time); text(cx + 9 * s, y + 46 * s, 0, szSmall, col(0xb8, 0xc8, 0xdc), c.name); }
    y += cardsH;
  }
  if (!b.notice.empty()) { text(x0, y, 0, szRow, soft, b.notice); y += 40 * s; }
  const float cn = x0 + 12 * s, cpb = x0 + iw * 0.68f - 12 * s, cbest = x0 + iw * 0.90f - 12 * s, cping = x0 + iw - 12 * s;
  size_t left = shown; bool first = true;
  for (const ScoreGroup& g : b.groups) {
    if (!left) break;
    text_hud_rect(5, x0, y, iw, groupH, col(0x19, 0x26, 0x3a)); text(cn, y + 6 * s, 0, szRow, gold, g.label);
    if (first) { text(cpb, y + 8 * s, 2, szSmall, dim, "PB"); text(cbest, y + 8 * s, 2, szSmall, dim, "Session best"); text(cping, y + 8 * s, 2, szSmall, dim, "Ping"); first = false; }
    y += groupH;
    for (const ScoreRow& r : g.rows) {
      if (!left) break; --left;
      if (r.you || r.watching) text_hud_rect(5, x0, y, iw, rowH, col(0x1d, 0x25, 0x35));
      text_hud_rect(5, x0, y + rowH - 1, iw, 1, col(0x26, 0x35, 0x4b));
      text(cn, y + 6 * s, 0, szRow, white, r.name); text(cpb, y + 6 * s, 2, szRow, white, r.pb); text(cbest, y + 6 * s, 2, szRow, white, r.best); text(cping, y + 6 * s, 2, szRow, white, r.ping);
      auto cmp = [&](float x, const std::string& c) { if (c.empty()) return; text(x, y + 24 * s, 2, szSmall, c[0] == '-' ? col(0x86, 0xef, 0xac) : c[0] == '+' ? col(0xfc, 0xa5, 0xa5) : soft, c + " vs WR"); };
      cmp(cpb, r.pbCmp); cmp(cbest, r.bestCmp);
      y += rowH;
    }
  }
  const float fy = py + need - tailH + 6 * s; text_hud_rect(5, x0, fy - 6 * s, iw, 1, col(0x40, 0x51, 0x6a));
  std::string sp; for (size_t i = 0; i < b.spectators.size(); ++i) { if (i) sp += "   "; sp += b.spectators[i]; }
  text(x0, fy + 4 * s, 0, szSmall, dim, "SPECTATORS:"); text(x0 + 110 * s, fy + 2 * s, 0, szRow, white, sp.empty() ? "None" : sp);
  text(x0, fy + 34 * s, 0, szSmall, dim, b.footer);
  {   // the browser footer's right-hand clock: new Date().toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}), the user's own locale, redrawn every frame
    char tb[64] = {};
#if defined(NC_WINDOWS)
    wchar_t w[64] = {}; if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, w, 64) > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, tb, (int)sizeof tb, nullptr, nullptr);
#else
    time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt); strftime(tb, sizeof tb, "%H:%M", &lt);
#endif
    text(x0 + iw, fy + 34 * s, 2, szSmall, dim, tb);
  }
}
uint32_t Hud::listBytes() const { return m->curBytes; }
uint32_t Hud::drawCount() const { return (uint32_t)m->draws.size(); }

void Hud::Impl::registerTexture(uint32_t id, const Image& im, bool nearest) {
  VkDescriptorSet set; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &setLayout; NC_CK(vkAllocateDescriptorSets(c.dev, &ai, &set));
  VkDescriptorImageInfo ii = {nearest ? sampNearest : sampLinear, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii; vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr); texSets[id] = set;
}

bool Hud::init(const Context& ctx, VkRenderPass renderPass, const std::string& fontDir, const std::string& assetsDir) {
  Impl& h = *m; h.c = ctx; h.rp = renderPass;
  VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &b;
  NC_CK(vkCreateDescriptorSetLayout(ctx.dev, &li, nullptr, &h.setLayout));
  VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &h.setLayout; NC_CK(vkCreatePipelineLayout(ctx.dev, &pl, nullptr, &h.layout));
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}; VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 64; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps; NC_CK(vkCreateDescriptorPool(ctx.dev, &dpi, nullptr, &h.pool));
  VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = VK_LOD_CLAMP_NONE;
  NC_CK(vkCreateSampler(ctx.dev, &si, nullptr, &h.sampLinear)); si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.maxLod = 0; NC_CK(vkCreateSampler(ctx.dev, &si, nullptr, &h.sampNearest));

  const char* vs = "#version 450\nlayout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col; layout(location=3) in vec3 circ; layout(location=0) out vec2 vUv; layout(location=1) out vec4 vCol; layout(location=2) flat out vec3 vCirc;\nvoid main(){ vUv=uv; vCol=col; vCirc=circ; gl_Position=vec4(pos,0.0,1.0); }\n";
  const char* fs = "#version 450\nlayout(set=0,binding=0) uniform sampler2D tex; layout(location=0) in vec2 vUv; layout(location=1) in vec4 vCol; layout(location=2) flat in vec3 vCirc; layout(location=0) out vec4 outColor;\nvoid main(){ outColor = texture(tex, vUv) * vCol; if (vCirc.z > 0.0) outColor.a *= clamp(vCirc.z - length(gl_FragCoord.xy - vCirc.xy) + 0.5, 0.0, 1.0); }\n";
  std::vector<uint32_t> sv, sf; std::string log;
  if (!compileGlsl(ShaderStage::Vertex, vs, sv, log) || !compileGlsl(ShaderStage::Fragment, fs, sf, log)) { logf("HUD shaders failed:\n%s", log.c_str()); return false; }
  VkShaderModule vm, fm; VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = sv.size() * 4; smi.pCode = sv.data(); NC_CK(vkCreateShaderModule(ctx.dev, &smi, nullptr, &vm)); smi.codeSize = sf.size() * 4; smi.pCode = sf.data(); NC_CK(vkCreateShaderModule(ctx.dev, &smi, nullptr, &fm));
  for (int blend = 0; blend < 3; ++blend) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm; st[1].pName = "main";
    VkVertexInputBindingDescription vb = {0, sizeof(HudVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 20}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};   // the HUD ignores depth
    VkPipelineColorBlendAttachmentState ba = {}; ba.colorWriteMask = 15; ba.blendEnable = blend != hud::B_OPAQUE; ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = blend == hud::B_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD; ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = blend == hud::B_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = h.layout; gi.renderPass = renderPass; NC_CK(vkCreateGraphicsPipelines(ctx.dev, VK_NULL_HANDLE, 1, &gi, nullptr, &h.pipes[blend]));
  }
  h.vb = makeBuf(ctx, sizeof(HudVertex) * 6 * 8192, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); h.stage = makeBuf(ctx, 2 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  const uint8_t white[4] = {255, 255, 255, 255}; h.images.push_back(makeTexture(ctx, white, 1, 1, false)); h.registerTexture(0, h.images.back(), false);

  // ---- the text engine: fonts in fallback order, then the monospace and black faces
  // The text engine lives in the core library, which stays loaded for the whole process while matches come and go: it is set up ONCE (its font data must outlive every match, and text_init is once only); a later match
  // only asks it to resend every glyph and page to the new GPU objects (text_hud_reset).
  static bool s_textReady = false; static float s_uiScale = 1.f; static std::vector<std::vector<uint8_t>> s_fonts;
  if (s_textReady) { h.uiScale = h.rasterScale = s_uiScale; text_hud_reset(); }
  else {
  text_init();
#if defined(NC_WINDOWS)
  const float uiDpi = std::min(4.f, std::max(1.f, floorf((float)GetDpiForSystem() / 96.f * 4.f + .5f) / 4.f));
#else
  const float uiDpi = 1.f;   // Linux: the compositor scales the surface; the HUD lays out in the swapchain's pixels (fractional scale is not read yet)
#endif
  h.uiScale = h.rasterScale = uiDpi; if (uiDpi != 1.f) text_set_ui_scale(uiDpi);
  s_fonts.reserve(8); FileSource fonts; fonts.root = fontDir;
  for (const char* f : {"Arimo.ttf", "NotoSansArabic.ttf", "NotoSansSymbols.ttf"}) {
    std::vector<uint8_t> b; if (!fonts.read(f, b)) { logf("HUD font %s missing in %s", f, fontDir.c_str()); return false; }
    s_fonts.push_back(std::move(b)); if (text_add_font(s_fonts.back().data(), (uint32_t)s_fonts.back().size()) < 0) { logf("HUD font %s rejected", f); return false; }
  }
  { std::vector<uint8_t> mono, black;
    if (fonts.read("JetBrainsMono.ttf", mono)) { s_fonts.push_back(std::move(mono)); text_add_style_font(2, s_fonts.back().data(), (uint32_t)s_fonts.back().size()); text_add_style_font(4, s_fonts.back().data(), (uint32_t)s_fonts.back().size()); }
    if (fonts.read("ArchivoBlack.ttf", black)) { s_fonts.push_back(std::move(black)); text_add_style_font(3, s_fonts.back().data(), (uint32_t)s_fonts.back().size()); } }
  text_hud_init(); s_textReady = true; s_uiScale = h.uiScale;
  }
  // the engine's constant tables and the player's layout (the game's defaults: render-core/text/hud-state.mjs textHudLayout of an untouched hud setting; the hud editor's settings will replace these)
  { int32_t lows[9]; for (int i = 0; i < 9; ++i) lows[i] = BAR_LOWS[i]; text_hud_set_weapon_table(BAR_COLORS, lows, 9); }
  { std::string labels; uint32_t lens[6]; for (int i = 0; i < 6; ++i) { lens[i] = (uint32_t)strlen(PU_LABELS[i]); labels += PU_LABELS[i]; } text_hud_set_powerup_table(PU_COLORS, PU_ICONS, 6, (const uint8_t*)labels.data(), lens); }
  { TextHudLayout& L = h.hudLayout; memset(&L, 0, sizeof L);
    L.crosshair = 0; L.crosshairSize = 20; L.crosshairThickness = 2; L.crosshairColor = 2583691263u; L.crosshairHidden = 0;
    L.timerX = L.timerY = -1; L.timerScale = 1; L.timerHidden = 2; L.vitalsX = L.vitalsY = -1; L.vitalsScale = 1; L.vitalsFlags = 16;
    L.speedBarMax = 1200; L.speedBarWidth = 190; L.speedBarHeight = 3; L.speedFlags = 0; L.speedX = L.speedY = -1; L.speedScale = 1;
    L.fpsX = L.fpsY = -1; L.fpsScale = 1; L.fpsHidden = 0; L.wbLayout = 0; L.wbFlags = 0; L.wbOnlyOwned = 1; L.wbOnlyWithAmmo = 0; L.wbX = L.wbY = -1; L.wbScale = 1;
    L.puLayout = 0; L.puFlags = 16; L.puX = L.puY = -1; L.puScale = 1;
    memcpy(text_hud_layout_addr(), &L, sizeof L); }
  text_hud_set_oled(0, 0);
  // ---- the quad registry: hud-quads.table.bin ("HQT1", u32 count, then {u32 id; u16 w, h; u32 filter 0 nearest / 1 linear; char file[32]}); id 0 is the white texel
  FileSource assets; assets.root = assetsDir; std::vector<uint8_t> t;
  if (assets.read("hud-quads.table.bin", t) && t.size() >= 8 && !memcmp(t.data(), "HQT1", 4)) {
    const uint32_t n = rd32(t.data() + 4);
    for (uint32_t i = 0; i < n && 8 + (size_t)(i + 1) * 44 <= t.size(); ++i) {
      const uint8_t* e = t.data() + 8 + (size_t)i * 44; const uint32_t id = rd32(e), filter = rd32(e + 8); const std::string file((const char*)e + 12, strnlen((const char*)e + 12, 32)); if (id == 0) continue;
      std::vector<uint8_t> rgba; int w = 0, hh = 0; if (!loadImage(assets, file, rgba, w, hh)) { logf("HUD quad %u (%s) did not load", id, file.c_str()); continue; }
      h.images.push_back(makeTexture(ctx, rgba.data(), w, hh, false)); h.registerTexture(id, h.images.back(), filter == 0);
    }
  } else logf("HUD quad table not found in %s: icons and crosshair sheets will be missing", assetsDir.c_str());
  h.draws.reserve(8192);
  h.ok = true; logf("HUD ready (ui scale %.2f)", h.uiScale); return true;
}

// per list, before the render pass: reset, atlas updates (copies), glyph defs (NATIVE-CLIENT.md 9.8)
void Hud::Impl::apply(VkCommandBuffer cb, const uint8_t* wire, size_t len) {
  const hud::Header* h = (const hud::Header*)wire; if (len < sizeof *h || h->magic != hud::MAGIC || h->version != hud::VERSION) { logf("HUD list refused (magic/version)"); return; }
  const hud::Item* it = (const hud::Item*)(wire + sizeof *h); const hud::Glyph* gl = (const hud::Glyph*)(it + h->itemCount); const hud::GlyphDef* dd = (const hud::GlyphDef*)(gl + h->glyphCount);
  const hud::AtlasUpdate* ups = (const hud::AtlasUpdate*)(dd + h->defCount); const uint8_t* upload = (const uint8_t*)(ups + h->updateCount);
  if (sizeof *h + (size_t)h->itemCount * sizeof *it + (size_t)h->glyphCount * sizeof *gl + (size_t)h->defCount * sizeof *dd + (size_t)h->updateCount * sizeof *ups + h->uploadBytes > len) { logf("HUD list truncated"); return; }
  if (h->flags & hud::F_RESET) defs.clear();   // the reset list resends every page in full
  if (h->uploadBytes > stage.size) {   // a full resend (a reset list after the HUD scale or font changed, a large console) can carry several MiB of atlas pages: the staging buffer grows to fit. Refusing the list lost the pages and left the text garbled.
    if (h->uploadBytes > (64u << 20)) { logf("HUD upload area too large (%u)", h->uploadBytes); return; }
    size_t want = stage.size; while (want < h->uploadBytes) want *= 2; vkDeviceWaitIdle(c.dev); vkDestroyBuffer(c.dev, stage.buf, nullptr); vkFreeMemory(c.dev, stage.mem, nullptr);
    stage = makeBuf(c, want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT); logf("HUD staging buffer grown to %zu bytes", want);
  }
  if (h->updateCount) {
    memcpy(stage.ptr, upload, h->uploadBytes);
    for (uint32_t i = 0; i < h->updateCount; ++i) {
      const hud::AtlasUpdate& u = ups[i]; Page& pg = pages[u.page];
      if (!pg.img.img) {
        pg.img = makeImage(c, 512, 512, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        vkDestroyImageView(c.dev, pg.img.view, nullptr);   // the view makeImage made has no swizzle; coverage must read as alpha with the run's colour
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = pg.img.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8_UNORM; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vi.components = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R}; NC_CK(vkCreateImageView(c.dev, &vi, nullptr, &pg.img.view)); registerTexture(PAGE_TEX + u.page, pg.img, false);
      }
      imageBarrier(cb, pg.img.img, pg.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pg.ready ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   pg.ready ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy bc = {}; bc.bufferOffset = u.uploadOffset; bc.bufferRowLength = u.w; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageOffset = {u.x, u.y, 0}; bc.imageExtent = {u.w, u.h, 1};   // tightly packed rows of w texels
      vkCmdCopyBufferToImage(cb, stage.buf, pg.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
      imageBarrier(cb, pg.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      pg.ready = true;
    }
  }
  for (uint32_t i = 0; i < h->defCount; ++i) defs[dd[i].glyphId] = dd[i];   // insert or REPLACE
}

// the executor proper: the list's bytes -> vertices and draw ranges
void Hud::Impl::buildVertices(const uint8_t* wire) {
  const hud::Header* h = (const hud::Header*)wire; const hud::Item* it = (const hud::Item*)(wire + sizeof(hud::Header)); const hud::Glyph* gl = (const hud::Glyph*)(it + h->itemCount);
  const size_t required = 6 * sizeof(HudVertex) * (size_t(h->itemCount) + h->glyphCount);
  if (required > vb.size) {
    size_t capacity = vb.size;
    while (capacity < required) capacity *= 2;
    vkDeviceWaitIdle(c.dev);
    vkDestroyBuffer(c.dev, vb.buf, nullptr);
    vkFreeMemory(c.dev, vb.mem, nullptr);
    vb = makeBuf(c, capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  }
  draws.clear(); HudVertex* v = (HudVertex*)vb.ptr; uint32_t nv = 0; const float sx = 2.f / h->fbWidth, sy = 2.f / h->fbHeight; const float S = uiScale;
  struct Clip { int x, y, w, h; }; Clip stack[5]; int depth = 0; stack[0] = {0, 0, (int)h->fbWidth, (int)h->fbHeight}; float circ[3] = {0, 0, 0};   // circ: the circle clip (centre, radius in pixels), radius 0 = none; circles do not nest
  auto emit = [&](const HudVertex* q, uint32_t n, uint32_t tex, uint32_t blend) {
    memcpy(v + nv, q, n * sizeof(HudVertex)); const Clip& cl = stack[depth];
    if (!draws.empty()) { HudDraw& d = draws.back(); if (d.tex == tex && d.blend == blend && d.sx == cl.x && d.sy == cl.y && d.sw == (uint32_t)cl.w && d.sh == (uint32_t)cl.h && d.first + d.count == nv) { d.count += n; nv += n; return; } }
    draws.push_back({tex, blend, nv, n, cl.x, cl.y, (uint32_t)cl.w, (uint32_t)cl.h}); nv += n;
  };
  auto ndc = [&](float x, float y, float u, float vv, uint32_t color) { return HudVertex{x * sx - 1, y * sy - 1, u, vv, color, circ[0], circ[1], circ[2]}; };
  auto add = [&](float x, float y, float w, float hh, float u0, float v0, float u1, float v1, uint32_t color, uint32_t tex, uint32_t blend) {
    const HudVertex q[6] = {ndc(x, y, u0, v0, color), ndc(x + w, y, u1, v0, color), ndc(x + w, y + hh, u1, v1, color), ndc(x, y, u0, v0, color), ndc(x + w, y + hh, u1, v1, color), ndc(x, y + hh, u0, v1, color)}; emit(q, 6, tex, blend);
  };
  for (uint32_t oi = 0; oi < h->itemCount; ++oi) {   // emission order is the z-order (the layer numbers are the engine's grouping, hud_list.hpp)
    const hud::Item& m = it[oi];
    const uint32_t blend = m.flags & 255u;
    switch (m.kind) {
      case hud::K_RECT:
        if (S != 1.f) { const float x0 = floorf(m.x * S + .5f), y0 = floorf(m.y * S + .5f); add(x0 / S, y0 / S, std::max(1.f, floorf(m.w * S + .5f)) / S, std::max(1.f, floorf(m.h * S + .5f)) / S, .5f, .5f, .5f, .5f, m.color, 0, blend); }   // whole device pixels, at least 1
        else add(m.x, m.y, m.w, m.h, .5f, .5f, .5f, .5f, m.color, 0, blend);
        break;
      case hud::K_QUAD: add(m.x, m.y, m.w, m.h, m.u0, m.v0, m.u1, m.v1, m.color, m.tex, blend); break;
      case hud::K_QUAD_ROT: {   // the rectangle turned by `first` (float bits, radians, clockwise on screen) around its centre
        float a; memcpy(&a, &m.first, 4); const float cs = cosf(a), sn = sinf(a), cx = m.x + m.w * .5f, cy = m.y + m.h * .5f, hx = m.w * .5f, hy = m.h * .5f;
        auto corner = [&](float dx, float dy, float u, float vv) { return ndc(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs, u, vv, m.color); };
        const HudVertex a0 = corner(-hx, -hy, m.u0, m.v0), b0 = corner(hx, -hy, m.u1, m.v0), c0 = corner(hx, hy, m.u1, m.v1), d0 = corner(-hx, hy, m.u0, m.v1); const HudVertex q[6] = {a0, b0, c0, a0, c0, d0}; emit(q, 6, m.tex, blend); break; }
      case hud::K_TRI: { const HudVertex q[3] = {ndc(m.x, m.y, .5f, .5f, m.color), ndc(m.w, m.h, .5f, .5f, m.color), ndc(m.u0, m.v0, .5f, .5f, m.color)}; emit(q, 3, 0, blend); break; }   // points (x,y), (w,h), (u0,v0)
      case hud::K_GLYPHS:
        if (m.tex != 0) break;   // the static interim atlases are not used by the engine's producers
        for (uint32_t k = 0; k < m.count; ++k) {
          const hud::Glyph& g = gl[m.first + k];
          const float px = S != 1.f ? floorf(S * (m.x + g.dx26 / 64.f) + .5f) / S : m.x + (float)((g.dx26 + 32) >> 6), py = S != 1.f ? floorf(S * (m.y + g.dy26 / 64.f) + .5f) / S : m.y + (float)((g.dy26 + 32) >> 6);   // whole pixels, rounded per glyph (9.5)
          auto d = defs.find(g.glyphId); if (d == defs.end() || !d->second.w) continue; const hud::GlyphDef& e = d->second;
          // The core's atlas keeps its raster scale for the life of the process.
          const float st = (e.reserved ? e.reserved : 256) / (256.f * rasterScale);
          add(px + e.bearingX * st, py + e.bearingY * st, e.w * st, e.h * st, e.x / 512.f, e.y / 512.f, (e.x + e.w) / 512.f, (e.y + e.h) / 512.f, m.color, PAGE_TEX + e.page, blend);
        }
        break;
      case hud::K_CLIP_PUSH: case hud::K_CIRCLE_CLIP_PUSH:
        if (depth < 4) {
          const Clip& cl = stack[depth]; const int x0 = std::max(cl.x, (int)m.x), y0 = std::max(cl.y, (int)m.y), x1 = std::min(cl.x + cl.w, (int)(m.x + m.w)), y1 = std::min(cl.y + cl.h, (int)(m.y + m.h));
          stack[depth + 1] = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)}; ++depth;
          if (m.kind == hud::K_CIRCLE_CLIP_PUSH) { circ[0] = m.x + m.w * .5f; circ[1] = m.y + m.h * .5f; circ[2] = std::min(m.w, m.h) * .5f; }   // the circle inscribed in the rectangle, anti-aliased in the fragment shader
        }
        break;
      case hud::K_CLIP_POP: if (depth > 0) --depth; circ[2] = 0; break;
    }
  }
  nVerts = nv;
}

void Hud::prepare(VkCommandBuffer cb, int W, int H, const HudNumbers& n) {
  Impl& h = *m; h.cur = nullptr; if (!h.ok) return;
  W = std::max(1, (int)std::lround((double)W / h.uiScale)); H = std::max(1, (int)std::lround((double)H / h.uiScale));   // the list is laid out in layout pixels (the page's CSS pixels); record() scales it to the framebuffer: Windows display scaling
  text_frame_begin(64); text_hud_begin((uint32_t)W, (uint32_t)H);
  // the core-produced HUD (text_api.h): the player's layout and this frame's numbers are copied into the engine, which draws timer, vitals, crosshair, speedometer, fps, weapon bar and powerups
  static_assert(sizeof(TextHudFrame) == 288, "TextHudFrame");
  TextHudFrame f; memset(&f, 0, sizeof f); f.health = n.health; f.armor = n.armor; f.ammo = n.ammo;
  for (int i = 0; i < TEXT_HUD_MAX_BAR; ++i) f.barAmmo[i] = n.barAmmo[i];
  for (int i = 0; i < TEXT_HUD_MAX_PU; ++i) { f.puLeft[i] = n.puLeft[i]; f.puIndex[i] = n.puIndex[i]; }
  f.fbWidth = (uint32_t)W; f.fbHeight = (uint32_t)H; f.timerMs = n.timerMs; f.held = n.held; f.speed = n.speed; f.fps = n.fps; f.barOwned = n.barOwned; f.puRaw = n.puCount; f.puCount = n.puCount;
  { const bool switching = n.weaponState == 2 || n.weaponState == 3 || n.selected != n.weaponInHand; uint32_t clk = (uint32_t)((uint64_t)nowMs() & 0x3FFFFFFFu); if (!clk) clk = 1; f.wbClock = clk | (switching ? 0x80000000u : 0u); }   // the weapon bar's autoHide (layout bit 16): the engine hides it 1500 ms after the held weapon changes unless a switch is running
  memcpy(text_hud_frame_addr(), &f, sizeof f);
  if (!h.timerStatus.empty()) text_hud_timer_status((const uint8_t*)h.timerStatus.data(), (uint32_t)h.timerStatus.size());   // READY - CROSS THE CYAN LINE, CHECKPOINT 2 / 5 ... (shown when the layout asks for it)
  text_hud_frame_go();
  for (const auto& kv : h.liveTpl) {   // the templates, filled with the client's own live data
    const Element& e = kv.second; char b[32];
    if (!h.playerHudVisible && (e.element == 5 || e.element == 12)) continue;
    if (e.element == 6 && !h.liveSlick) { text_hud_el_clear(6); continue; }
    if (e.element == 13 && !h.livePaused) { text_hud_el_clear(13); continue; }
    if (e.element == 5) { snprintf(b, sizeof b, "%.1f\xC2\xB0", h.livePitch); elSet(e, e.flags, b); }
    else if (e.element == 12) elSet(e, (e.flags & ~63u) | h.liveKeys, e.text);
    else if (e.element == 13 && e.text.empty()) elSet(e, e.flags, h.pausedHint);   // the page sends the template with an empty hint; the bound key is the client's (Unpause with "P" key)
    else elSet(e, e.flags, e.text);
  }
  if (h.liveLiquid != h.liquidSent) { h.liquidSent = h.liveLiquid; if (h.liveLiquid) { TextRecipeIn in; memset(&in, 0, sizeof in); in.elX = in.elY = -1; in.flags = (uint32_t)h.liveLiquid; text_hud_el_set(19, 19, &in, nullptr, 0); } else text_hud_el_clear(19); }
  text_hud_el_frame_part((uint32_t)W, (uint32_t)H, nowMs(), 0);   // the elements BELOW the chat box / console; part 1 (map info 16, loading 21, race panel 22 / invite 23) comes after them
  if (h.playerHudVisible && h.speedoMode >= 1 && h.speedoMode <= 3) text_hud_ql_speedo((uint32_t)W, (uint32_t)H, h.speedoMode, (double)n.speed, h.speedoMax, nowMs());
  h.pumpLog();
  if (h.mmView) { h.mm.fbWidth = (uint32_t)W; h.mm.fbHeight = (uint32_t)H; text_hud_minimap_live(&h.mm, h.mmView, h.mmTrail, h.mmGhost); }
  if (h.cgazOn) { TextCgazState* st = (TextCgazState*)h.cgazState; st->fbWidth = (uint32_t)W; st->fbHeight = (uint32_t)H; text_hud_cgaz(st); }
  {   // the chat box and the console: the core's text field draws the whole box (chrome, log view, field, close buttons); the fading feed shows while the chat box is closed
    const uint32_t st = tf::state();
    if (h.conHeight != h.tfCfgH || h.conOpacity != h.tfCfgO || h.conScale != h.tfCfgS) { h.tfCfgH = h.conHeight; h.tfCfgO = h.conOpacity; h.tfCfgS = h.conScale; text_tf_config(-1, -1, 0, 0, 0, h.conHeight, h.conOpacity, h.conScale); }   // on change only; the chat box keeps its default place and size
    if (st & (TF_STATE_CHAT | TF_STATE_TEAM | TF_STATE_CONSOLE)) text_tf_draw((uint32_t)W, (uint32_t)H, nowMs());
    if (!(st & (TF_STATE_CHAT | TF_STATE_TEAM | TF_STATE_CONSOLE)) && !h.chat.empty()) {   // the chat feed: bottom-left at 36 / 90, at most 640 px wide (half the window minus 170), the last lines in white / green
      const float s = 1.f; const float left = 36 * s, bottom = (float)H - 90 * s; const uint32_t width = (uint32_t)std::max(60.f, std::min(640.f * s, (float)W / 2.f - 170.f * s));
      const uint32_t sz = text_size_for_px((uint32_t)(16 * s + .5f)); const uint8_t* ct = (const uint8_t*)h.chat.data(); const uint32_t cn = (uint32_t)h.chat.size();
      if (h.chatHistory) {   // +chat held (hud-feed.mjs, ch.history): the same lines in a dark box, as tall as its wrapped lines (at most 440 px), padding 6 x 10, drawn with the same call inside it
        const float hw = (float)width; text_lv_config(0, 100, 100, sz, 0, 3, 200); const uint32_t lh = text_lv_line_height(0);
        const float bh = std::min(440.f * s, (float)(text_hud_chat(0, 0, (uint32_t)(hw - 20 * s), 0, sz, 0, ct, cn, 0, 2 | 0x100) & 0xFFFF) * (float)lh + 12 * s);
        text_hud_rect(3, left, bottom - bh, hw, bh, 0xCC1C150Cu);
        text_hud_chat(left + 10 * s, bottom - 6 * s, (uint32_t)(hw - 20 * s), (uint32_t)(428 * s), sz, 0xFFFFFFFFu, ct, cn, 0, 2);
      } else text_hud_chat(left, bottom, width, (uint32_t)(440 * s), sz, 0xFFFFFFFFu, ct, cn, 0, 0);
    }
    if (st & TF_STATE_COMPOSING) { float cr[4] = {}; text_tf_caret_rect(cr); tf::setCaret(cr[0] * h.uiScale, cr[1] * h.uiScale, cr[2] * h.uiScale, cr[3] * h.uiScale); }   // framebuffer px, for the IME's candidate window
  }
  text_hud_el_frame_part((uint32_t)W, (uint32_t)H, nowMs(), 1);   // above the chat / console
  {   // Clippy: the page decides when (24 h cooldown, 10 s); the core animates from the ms since he appeared. His rect is kept for click-to-dismiss.
    std::string txt; double ms; { std::lock_guard<std::mutex> lk(h.hitMu); ms = h.clippyOn ? std::max(0.0, nowMs() - h.clippyMs) : -1; if (h.clippyTextNew) { txt = h.clippyText; h.clippyTextNew = false; } }
    if (ms >= 0) { text_hud_clippy((uint32_t)W, (uint32_t)H, ms, (const uint8_t*)txt.data(), (uint32_t)txt.size()); float b4[4] = {}; const bool have = text_hud_box(HUD_BOX_CLIPPY, b4) != 0; std::lock_guard<std::mutex> lk(h.hitMu); if (have) memcpy(h.clippyRect, b4, sizeof b4); else memset(h.clippyRect, 0, sizeof b4); }
    else { std::lock_guard<std::mutex> lk(h.hitMu); memset(h.clippyRect, 0, sizeof h.clippyRect); }
  }
  {   // where the clickable elements drew their buttons / links (run flag 512), for hit-testing a click
    std::lock_guard<std::mutex> lk(h.hitMu);
    for (int el : {16, 22, 23}) { float rb[6 * 65]; const uint32_t nr = text_hud_el_rects((uint32_t)el, rb, 64); auto it = h.hits.find(el);
      if (nr == 0 && it == h.hits.end()) continue; auto& eh = h.hits[el]; eh.rects.clear();
      for (uint32_t i = 0; i < nr; ++i) { const float* r = rb + 6 * (i + 1); if (((uint32_t)r[4]) & 512u) eh.rects.push_back({r[0], r[1], r[2], r[3]}); }
      if (eh.rects.empty() && eh.links.empty()) h.hits.erase(el); }
  }
  if (h.boardOn) h.drawBoard(W, H);
  const uint32_t bytes = text_hud_finish(); if (!bytes) return;
  h.cur = text_hud_out(); h.curBytes = bytes; h.apply(cb, h.cur, bytes);
}

void Hud::prepareLoading(VkCommandBuffer cb, int W, int H, const std::string& text, double fraction) {
  Impl& h = *m; h.cur = nullptr; if (!h.ok) return;
  W = std::max(1, (int)std::lround((double)W / h.uiScale)); H = std::max(1, (int)std::lround((double)H / h.uiScale));
  text_frame_begin(64); text_hud_begin((uint32_t)W, (uint32_t)H);
  setLoading(text, fraction);
  text_hud_el_frame_part((uint32_t)W, (uint32_t)H, nowMs(), 1);
  const uint32_t bytes = text_hud_finish(); if (!bytes) return;
  h.cur = text_hud_out(); h.curBytes = bytes; h.apply(cb, h.cur, bytes);
}
void Hud::setLoading(const std::string& text, double fraction) {
  TextRecipeIn in{};
  in.elX = in.elY = -1;
  in.flags = fraction >= 0 ? 1u : 0u;
  in.num = fraction >= 0 ? fraction : 0;
  in.textLen = (uint32_t)text.size();
  text_hud_el_set(21, 21, &in, (const uint8_t*)text.data(), 0);
}
void Hud::endLoading() { text_hud_el_clear(21); }

void Hud::record(VkCommandBuffer cb, int W, int H) {
  Impl& h = *m; if (!h.ok || !h.cur) return; const hud::Header* hd = (const hud::Header*)h.cur;
  if (!h.haveSerial || hd->serial != h.serial || (hd->flags & hud::F_RESET)) { h.buildVertices(h.cur); h.serial = hd->serial; h.haveSerial = true; }   // serial unchanged: the vertices of the last frame are reused
  VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cb, 0, 1, &h.vb.buf, &zero); uint32_t curBlend = 99; uint32_t curTex = 0xffffffffu; VkViewport vp = {0, 0, (float)W, (float)H, 0, 1}; vkCmdSetViewport(cb, 0, 1, &vp);
  const float kx = (float)W / (float)hd->fbWidth, ky = (float)H / (float)hd->fbHeight;   // the clip boxes are in the list's pixels; the target may be another size
  for (const HudDraw& d : h.draws) {
    auto ts = h.texSets.find(d.tex); if (ts == h.texSets.end()) continue;
    if (d.blend != curBlend) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipes[std::min<uint32_t>(d.blend, 2)]); curBlend = d.blend; curTex = 0xffffffffu; }
    const int cx0 = std::max(0, (int)floorf(d.sx * kx)), cy0 = std::max(0, (int)floorf(d.sy * ky)), cx1 = std::min(W, (int)ceilf((d.sx + d.sw) * kx)), cy1 = std::min(H, (int)ceilf((d.sy + d.sh) * ky));
    if (cx1 <= cx0 || cy1 <= cy0) continue; VkRect2D sc = {{cx0, cy0}, {(uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0)}}; vkCmdSetScissor(cb, 0, 1, &sc);
    if (d.tex != curTex) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, h.layout, 0, 1, &ts->second, 0, nullptr); curTex = d.tex; }
    vkCmdDraw(cb, d.count, 1, d.first, 0);
  }
}

}  // namespace nc
