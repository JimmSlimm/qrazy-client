// Platform input and host text routing. The core owns gameplay input state.
#pragma once
#include "nc_latency.h"
#include "qrazy_core_api.h"
#include <cstdint>
#include <atomic>
#include <array>
#include <functional>
#include <vector>
#if defined(NC_WINDOWS)
#include "nc_vk.h"
#else
union SDL_Event; struct SDL_Window;
#endif

namespace nc {

double inputSeconds(int64_t at);

class Input {
 public:
#if defined(NC_WINDOWS)
  void attach(HWND hwnd);                      // registers raw input for the mouse (a normal window)
  // Embedded in a host window: raw mouse AND keyboard are delivered to this thread's own message-only window `msgWnd`, even when the host window has the focus. `gate` says when they count (host visible and focused).
  void attachRaw(HWND msgWnd, HWND host, std::function<bool()> gate);   // host: the window whose client area the cursor is confined to while captured
  // Window messages. Returns true when the message was consumed. The event time is the QPC time at which the message was handled.
  bool handle(UINT msg, WPARAM w, LPARAM l);
#else
  // Linux: SDL delivers the events. Standalone, the client pumps SDL itself; embedded, the host forwards every event to onSdlEvent (SDL events can only be pumped on the main thread).
  // `window` is the SDL window the mouse is captured in (relative mouse mode); `gate` says when events count (the native view is showing and the window has the focus).
  // mainThread: setCapture may call SDL itself (standalone). Embedded, SDL video calls belong to the host's main thread: setCapture only records the wish and the host applies it (captureWanted()).
  void attachSdl(SDL_Window* window, std::function<bool()> gate, bool mainThread);
  bool captureWanted() const { return wantCapture_.load(); }
  bool onSdlEvent(const SDL_Event& e);         // true = consumed
#endif
  void setCapture(bool on);                    // hide and confine the cursor (gameplay), or release it
  bool captured() const { return captured_; }
  void cacheCodes();
  void pollFocus();
  bool focused() const { return focused_; }
  void reset(double now);
  void resetForPlacement(double now);
  const char* domCode(uint32_t id, int* scan = nullptr) const;
  bool keyStillDown(int scan) const { return scan > 0 && scan < int(down_.size()) && down_[scan]; }
  bool takeOverflow() { const bool had = overflow_; overflow_ = false; return had; }
  bool recordMotion = false;
  MotionReceipt motionReceipt;
  double yaw = 0, pitch = 0;
  bool quitRequested = false;
  std::atomic<bool> typing{false};   // a text box (the core's text field) is open: every key goes to it
  std::atomic<bool> cursor{false};   // the page wants a free mouse (chat / console open, or a clickable element on screen): no capture, no look, no clicks into the game. Keyboard routing stays `typing`.
  std::atomic<bool> paused{false};   // the page's /pause: the mouse is free and nothing reaches the game (the app sets it each frame)
  std::atomic<bool> followingPov{false};
  bool mouseFree() const { return typing.load() || cursor.load() || paused.load() || followingPov.load(); }
  // The core text field (chat / team chat / console): while its state is non-zero (or for the console key, always) every key goes to `textKey` on the render thread instead of the game or the page:
  // (TF_KEY_*, TF_MOD_* bits, down, host key id) -> TF_R_* bits (bit 0 = consumed). The app keeps `textState` (TF_STATE_* bits) current after each call; `typing` is the app's "the state is non-zero".
  std::function<uint32_t(uint32_t, uint32_t, bool, uint32_t)> textKey;
  std::atomic<uint32_t> textState{0};
  void typingRepeat(int64_t nowUs);
  int tabEdge = 0;                            // Tab went down (1) or up (2) since the app last looked: the host asks the page for the information board
  bool menuRequested = false;                 // Esc: the host owns it (show the menu); standalone ends the program
  void releaseAllKeys();                       // focus lost: no key stays stuck
 private:
  struct Code { const char* dom = ""; uint32_t id = 0; };
  std::array<Code, 1102> codes_{};
  std::array<bool, 1102> down_{};
  uint32_t modBits_ = 0;
  uint32_t mods(bool repeat) const;
  std::vector<int> typeDown_;
  int typeRepeatScan_ = -1;
  int64_t typeNextUs_ = 0;
  bool captured_ = false, focused_ = true, overflow_ = false;
  double wheelRemainder_ = 0;
  std::function<bool()> gate_;
  void motion(double dx, double dy, int64_t at);
  void sendKey(int scan, int kind, int64_t at);
  void mouseButton(int dom, bool down, int64_t at);
  void wheel(double notches, int64_t at);
  bool keyScan(int scan, bool down, bool repeat, int64_t at);
#if defined(NC_WINDOWS)
  HWND hwnd_ = nullptr, capWnd_ = nullptr; bool rawKeys_ = false;
#else
  SDL_Window* window_ = nullptr; bool mainThread_ = true; std::atomic<bool> wantCapture_{false};
#endif
};

}  // namespace nc
