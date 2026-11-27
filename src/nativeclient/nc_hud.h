// The in-match HUD: the render core's text engine writes one 2D draw list per frame (hud_list.hpp, NATIVE-CLIENT.md section 9) and this executor draws it with Vulkan.
// The executor reads only the list's bytes: rectangles, quads, rotated quads, triangles, glyph runs from the engine's dynamic R8 atlas, scissor and circle clips, three blend modes.
#pragma once
#include "nc_vk.h"
#include <cstdint>
#include <string>
#include <vector>

struct QcMinimapOut;
struct TextMinimapState;

namespace nc {

// The game numbers one HUD frame needs. The sim fills most of them (hud_frame.hpp); fps is the host's.
struct HudNumbers {
  double health = 100, armor = 0, ammo = -1;
  double barAmmo[16] = {};     // per bar weapon (core order: gauntlet, mg, sg, gl, rl, rg, pg, bfg, grapple): ammo, infinity = infinite, 0 when not owned
  double puLeft[8] = {};       // per drawn powerup: seconds left, infinity = no countdown
  int timerMs = -1, held = -1, speed = 0, fps = -1;
  uint32_t barOwned = 0, puCount = 0, puIndex[8] = {};
  int weaponInHand = 0, selected = 0, weaponState = 0; double cooldownMs = 0;   // the sim's weapon state: the weapon id drawn now, the wanted one, 0 ready / 1 firing / 2 dropping / 3 raising, ms to ready
};

// The hold-Tab information board (the page's qrazyDesktop.information snapshot, layout 3), reduced to what is drawn.
struct ScoreRow { std::string name, pb, best, ping, pbCmp, bestCmp; bool you = false, watching = false; };   // name: QL colour codes, "#n " number and clan tag already joined
struct ScoreGroup { std::string label; std::vector<ScoreRow> rows; };
struct ScoreCard { std::string title, time, name; bool active = false; };
struct Scoreboard { bool held = false; std::string map, info, notice, footer; std::vector<ScoreCard> cards; std::vector<ScoreGroup> groups; std::vector<std::string> spectators; };
// json: {"held":bool,"snap":{...}} from the host bridge. Returns false when it cannot be read. Keeps the last snapshot when only `held` changes.
bool parseScoreboard(const std::string& json, Scoreboard& board);

// The core text field (text_tf_*, CORE-TEXT-FIELD-DESIGN.md 4.2): chat, team chat and the console. Keys come from the render thread's raw input; characters and IME text from the host window's message thread (queued).
namespace tf {
void setJsOut(void (*f)(const char* js));            // where the page messages (qrazyNativeTextState / qrazyNativeTextSubmit / qrazyNativeTextEscapeReleased) go: a script for the hidden page
void queueChar(uint32_t codepoint);                  // any thread: WM_CHAR (surrogates already combined) / the toolkit's text input
void queueCommit(const std::string& utf8);           // any thread: an IME result
void queuePreedit(const std::string& utf8, uint32_t curFrom, uint32_t curTo, uint32_t ulFrom, uint32_t ulTo);   // any thread: the IME composition (byte offsets); empty ends it
uint32_t key(uint32_t tfKey, uint32_t mods, bool down, uint32_t hostId);   // render thread: TF_R_* bits (1 = consumed); the events it queued are handled
bool open(uint32_t mode, uint32_t hostId);           // render thread: the page's openText op
void close(uint32_t reason);
void frame(double dtMs);                             // render thread, once a frame: queued text input, events, the drag tick
uint32_t state();                                    // TF_STATE_* bits as of the last call (any thread)
bool composing();
void caretRect(float out[4]);                        // any thread: the caret in framebuffer px, as of the last draw
void reset();                                        // a new match: closed, queues empty
void flushEvents();                                  // poll the field's events to empty (after every input call)
void setCaret(float x, float y, float w, float h);   // the Hud keeps the caret rect (framebuffer px) for the host
}

class Hud {
 public:
  Hud();
  ~Hud();
  // fontDir: Arimo.ttf, NotoSansArabic.ttf, NotoSansSymbols.ttf, JetBrainsMono.ttf, ArchivoBlack.ttf. assetsDir: hud-quads.table.bin and the PNG sheets it names (crosshairs, icons).
  bool init(const Context& c, VkRenderPass renderPass, const std::string& fontDir, const std::string& assetsDir);
  // Once per frame OUTSIDE the render pass: builds the list for this framebuffer size and applies its atlas data (page copies are transfer commands).
  void prepare(VkCommandBuffer cb, int fbW, int fbH, const HudNumbers& n);
  // A loading frame (no match yet): only the loading screen element 21, text + bar (fraction 0..1, < 0 no bar). Same call order as prepare / record; endLoading clears the element.
  void prepareLoading(VkCommandBuffer cb, int fbW, int fbH, const std::string& text, double fraction);
  void endLoading();
  void setLoading(const std::string& text, double fraction);
  // Inside the render pass, after everything else is drawn.
  void record(VkCommandBuffer cb, int fbW, int fbH);
  // The fading chat feed (box closed): lines joined by newlines (oldest first, in the game's chatLineText form: 0x01 + RRGGBB colour escapes, QL colour codes), drawn bottom-left. Empty = nothing drawn.
  void setChat(const std::string& joined);
  void setChatHistory(bool on);   // the '+chat held' view (page flag history): the lines in the engine's dark history box instead of the fading feed
  // The player's HUD layout from the page (TextHudLayout fields by name, text_api.h; a missing name keeps the current value) and the window's pixel ratio (the page's devicePixelRatio, 0 = keep).
  void setLayout(const std::vector<std::pair<std::string, double>>& fields);
  void setUiScale(float scale);
  float uiScale() const;   // layout pixels -> framebuffer pixels (the page's devicePixelRatio)
  void setTimerStatus(const std::string& text);   // the timer's status line text from the page (runState)
  void setConsoleLook(double height, double opacity, double scale);   // con_height (0.1-1 of the picture), con_opacity, con_scale

  // The page's text elements (pickup, records, notices, players, race, clock, spectator / vote bars ... ): the engine owns them (text_hud_el_*); the page says what an element shows or that it is gone. `until` is on the clock of nowMs() (0 = until cleared).
  struct Element { int element = 0; uint32_t kind = 0, color = 0, color2 = 0, flags = 0, aux = 0; double elX = -1, elY = -1, elScale = 0, num = 0, d0 = 0, d1 = 0, d2 = 0, until = 0; std::string text; std::vector<std::string> links; };
  static double nowMs();
  void setElement(const Element& e);
  void clearElement(int element);
  void clearAllElements();
  // Clickable elements: the map-info popup (16: its links in the page's "links" order, then Close), the race panel (22) and the race challenge invite (23): the flagged runs (RUN_LINK) the engine drew are their buttons, in text order. Thread safe. px / py in framebuffer pixels.
  struct Click { int element = 0; int index = 0; std::string url; };   // index: the i-th flagged run; element 16: -1 = Close, and url = the link's address
  static constexpr int CLOSE_BOX = 1000;   // Click.element for the x of the chat box / console (the host sends the page Escape)
  static bool isClickable(int element) { return element == 16 || element == 22 || element == 23; }
  bool hitClickable(double px, double py, Click* out);
  // The four elements the page sends as templates (angle 5, slick 6, input commands 12, paused 13): each frame the client refreshes them from its own state: the pitch, whether the surface is slick, the pressed movement keys (the sim's key bits 0-5), whether the game is paused.
  void setPausedHint(const std::string& hint);   // the second line of the paused element 13 (the page's template carries none)
  void setLive(double pitchDeg, bool slickOn, uint32_t keyMask, bool paused, int liquid);   // liquid: the eye point's contents bits, 0 = out of it (the tint element 19 is the client's to set in a native match)
  bool minimapBox(int width, int height, float out[4]) const;   // framebuffer pixels, after prepare()
  // The view and point arrays must stay valid through prepare().
  void setMinimap(const TextMinimapState& state, const QcMinimapOut& view, const float* trail, const float* ghost);
  void setSpeedometer(uint32_t mode, double maxSpeed);   // "speedometer": 0 off, 1-3 QL cg_speedometer styles (the engine draws them), 4 the Qrazy one (part of the core HUD)
  void setCgaz(bool on, const void* state136, double yawDeg, double fovX, double ypos, uint32_t flags);
  // The information board drawn over the HUD while board.held is true; nullptr or not held = nothing.
  void setBoard(const Scoreboard* board);
  // The log views: the open chat box (view 0) and the console (view 1) are the engine's scrollable, selectable text views. The page's operations arrive in order (render thread): 'c' clear, 'a' append (text, rgba, flags, group), 'r' repeat (n), 'p' pop, 'g' collapse (group, n = collapsed).
  void logOp(int view, char op, const std::string& text, uint32_t rgba, uint32_t flags, uint32_t group, uint32_t n);
  // The mouse on the open text field (chat box / console): queued from any thread, applied in prepare through text_tf_pointer. kind: 0 down, 1 move, 2 up (double / triple clicks are counted here); px / py in framebuffer pixels. wheel: notches (+1 = up, towards older lines).
  enum { LOG_DOWN = 0, LOG_MOVE = 1, LOG_UP = 2 };
  void logMouse(int kind, double px, double py, bool shift);
  void logWheel(double notches);
  // Clippy (the page decides when: its qrazyNativeHud "clippy":{ms,text}): ms since he appeared, < 0 = not shown; text = the bubble (only sent when it changes; empty keeps the last). His rect for click-to-dismiss is read from the core.
  void setClippy(double ms, const std::string& text);
  bool hitClippy(double px, double py);   // thread safe; framebuffer px
  uint32_t listBytes() const;
  uint32_t drawCount() const;
 private:
  struct Impl;
  Impl* m;
};

}  // namespace nc
