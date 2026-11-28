#define _USE_MATH_DEFINES
#include "nc_input.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "text_api.h"
#if !defined(NC_WINDOWS)
#include <SDL3/SDL.h>
#endif

namespace nc {

double inputSeconds(int64_t at) {
  return double(at) / qpcFrequency();
}

void Input::motion(double dx, double dy, int64_t at) {
  if (dx == 0 && dy == 0) return;
  if (recordMotion) {
#if defined(NC_WINDOWS)
    const long message = GetMessageTime();
#else
    const long message = 0;
#endif
    if (!motionReceipt.oldQpc) {
      motionReceipt.oldQpc = at;
      motionReceipt.oldMessage = message;
    }
    motionReceipt.newQpc = at;
    motionReceipt.newMessage = message;
  }
  qc_ses_mouse(dx, dy, inputSeconds(at));
}

void Input::reset(double now) {
  qc_ses_reset(now, yaw, pitch);
  motionReceipt = {};
}

void Input::resetForPlacement(double now) {
  reset(now);
  // Repeat events restore the core's held state without firing press-only binds again.
  for (size_t scan = 0; scan < down_.size(); ++scan) {
    if (down_[scan] && qc_ses_key(codes_[scan].id, 2, now) < 0) overflow_ = true;
  }
}

void Input::releaseAllKeys() {
  modBits_ = 0;
  down_.fill(false);
  typeDown_.clear();
  typeRepeatScan_ = -1;
  wheelRemainder_ = 0;
  reset(inputSeconds(qpcNow()));
}

void Input::pollFocus() {
  const bool focused = !gate_ || gate_();
  if (focused_ && !focused) {
    tabEdge = 2;
    releaseAllKeys();
    setCapture(false);
  }
  focused_ = focused;
}

void Input::typingRepeat(int64_t nowUs) {
#if defined(NC_WINDOWS)
  if (!typing.load() || typeRepeatScan_ < 0 || !textKey || nowUs < typeNextUs_) return;
  DWORD speed = 31; SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0); const double cps = 2.5 + (double)speed * (30.0 - 2.5) / 31.0;
  keyScan(typeRepeatScan_, true, true, 0); typeNextUs_ = nowUs + (int64_t)(1e6 / cps);
#else
  (void)nowUs;
#endif
}

// Platform key id -> the text field's key and the modifier bits it tracks (Windows: set-1 scan codes, +0x100 for E0 keys; Linux: SDL scancodes).
static uint32_t tfKeyOf(int scan) {
#if defined(NC_WINDOWS)
  switch (scan) { case 0x1C: return TF_KEY_ENTER; case 0x11C: return TF_KEY_KP_ENTER; case 0x0F: return TF_KEY_TAB; case 0x01: return TF_KEY_ESCAPE; case 0x0E: return TF_KEY_BACKSPACE; case 0x153: return TF_KEY_DELETE;
    case 0x14B: return TF_KEY_LEFT; case 0x14D: return TF_KEY_RIGHT; case 0x148: return TF_KEY_UP; case 0x150: return TF_KEY_DOWN; case 0x147: return TF_KEY_HOME; case 0x14F: return TF_KEY_END; case 0x149: return TF_KEY_PAGE_UP; case 0x151: return TF_KEY_PAGE_DOWN;
    case 0x1E: return TF_KEY_A; case 0x2E: return TF_KEY_C; case 0x2F: return TF_KEY_V; case 0x2D: return TF_KEY_X; case 0x152: return TF_KEY_INSERT; case 0x29: return TF_KEY_BACKQUOTE; case 0x57: return TF_KEY_F11; default: return TF_KEY_OTHER; }
#else
  switch (scan) { case SDL_SCANCODE_RETURN: return TF_KEY_ENTER; case SDL_SCANCODE_KP_ENTER: return TF_KEY_KP_ENTER; case SDL_SCANCODE_TAB: return TF_KEY_TAB; case SDL_SCANCODE_ESCAPE: return TF_KEY_ESCAPE; case SDL_SCANCODE_BACKSPACE: return TF_KEY_BACKSPACE; case SDL_SCANCODE_DELETE: return TF_KEY_DELETE;
    case SDL_SCANCODE_LEFT: return TF_KEY_LEFT; case SDL_SCANCODE_RIGHT: return TF_KEY_RIGHT; case SDL_SCANCODE_UP: return TF_KEY_UP; case SDL_SCANCODE_DOWN: return TF_KEY_DOWN; case SDL_SCANCODE_HOME: return TF_KEY_HOME; case SDL_SCANCODE_END: return TF_KEY_END; case SDL_SCANCODE_PAGEUP: return TF_KEY_PAGE_UP; case SDL_SCANCODE_PAGEDOWN: return TF_KEY_PAGE_DOWN;
    case SDL_SCANCODE_A: return TF_KEY_A; case SDL_SCANCODE_C: return TF_KEY_C; case SDL_SCANCODE_V: return TF_KEY_V; case SDL_SCANCODE_X: return TF_KEY_X; case SDL_SCANCODE_INSERT: return TF_KEY_INSERT; case SDL_SCANCODE_GRAVE: return TF_KEY_BACKQUOTE; case SDL_SCANCODE_F11: return TF_KEY_F11; default: return TF_KEY_OTHER; }
#endif
}
static uint32_t modBitOf(int scan) {   // 1 shift, 2 ctrl, 4 alt, 8 meta, each one bit per side (<< 4 for the right one)
#if defined(NC_WINDOWS)
  switch (scan) { case 0x2A: return 1; case 0x36: return 1 << 4; case 0x1D: return 2; case 0x11D: return 2 << 4; case 0x38: return 4; case 0x138: return 4 << 4; case 0x15B: return 8; case 0x15C: return 8 << 4; default: return 0; }
#else
  switch (scan) { case SDL_SCANCODE_LSHIFT: return 1; case SDL_SCANCODE_RSHIFT: return 1 << 4; case SDL_SCANCODE_LCTRL: return 2; case SDL_SCANCODE_RCTRL: return 2 << 4; case SDL_SCANCODE_LALT: return 4; case SDL_SCANCODE_RALT: return 4 << 4; case SDL_SCANCODE_LGUI: return 8; case SDL_SCANCODE_RGUI: return 8 << 4; default: return 0; }
#endif
}
uint32_t Input::mods(bool repeat) const { return ((modBits_ & 0x11) ? TF_MOD_SHIFT : 0u) | ((modBits_ & 0x22) ? TF_MOD_CTRL : 0u) | ((modBits_ & 0x44) ? TF_MOD_ALT : 0u) | ((modBits_ & 0x88) ? TF_MOD_META : 0u) | (repeat ? TF_MOD_REPEAT : 0u); }

bool Input::keyScan(int scan, bool down, bool repeat, int64_t at) {
#if defined(NC_WINDOWS)
  const int ESC = 0x01;
#else
  const int ESC = SDL_SCANCODE_ESCAPE;
#endif
  { const uint32_t bit = modBitOf(scan); if (bit) modBits_ = down ? (modBits_ | bit) : (modBits_ & ~bit); }
  const int BACKQUOTE =
#if defined(NC_WINDOWS)
      0x29;
#else
      SDL_SCANCODE_GRAVE;
#endif
  const bool boxOpen = textState.load() != 0;
  if (textKey && (boxOpen || scan == BACKQUOTE)) {   // the core's text field: every key while it is open (the state also covers an Escape held down), and the console key always
    const bool isDown = std::find(typeDown_.begin(), typeDown_.end(), scan) != typeDown_.end();
    if (boxOpen && down && isDown && !repeat) return true;   // the same press twice (two raw-input routes)
    if (down && !isDown) typeDown_.push_back(scan); if (!down) typeDown_.erase(std::remove(typeDown_.begin(), typeDown_.end(), scan), typeDown_.end());
    uint32_t textMods = mods(repeat);
#if defined(NC_WINDOWS)
    if (scan == BACKQUOTE && down) {
      // A system shortcut can consume modifier releases during a window move.
      textMods &= ~(TF_MOD_CTRL | TF_MOD_ALT | TF_MOD_META);
      if (GetAsyncKeyState(VK_CONTROL) & 0x8000) textMods |= TF_MOD_CTRL;
      if (GetAsyncKeyState(VK_MENU) & 0x8000) textMods |= TF_MOD_ALT;
      if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) textMods |= TF_MOD_META;
    }
#endif
    const uint32_t r = textKey(tfKeyOf(scan), textMods, down, (uint32_t)scan + 1u);
    if (boxOpen) {
#if defined(NC_WINDOWS)
      { const bool modifier = modBitOf(scan) != 0 || scan == 0x3A;
        if (down && !isDown && !modifier) { DWORD delay = 1, speed = 31; SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0); SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0); typeRepeatScan_ = scan; typeNextUs_ = qpcNow() * 1000000 / qpcFrequency() + (int64_t)(250 + 250 * (int)delay) * 1000; }
        else if (!down && scan == typeRepeatScan_) typeRepeatScan_ = -1; }
#endif
      return true;
    }
    if (typeRepeatScan_ == scan) typeRepeatScan_ = -1;
    if (r & TF_R_CONSUMED) return true;   // the console key opened the box (or its release / repeat was swallowed)
  }
  if (!typeDown_.empty()) typeDown_.clear(); typeRepeatScan_ = -1;
#if defined(NC_WINDOWS)
  if (scan == 0x0F && !repeat) tabEdge = down ? 1 : 2;
#else
  if (scan == SDL_SCANCODE_TAB && !repeat) tabEdge = down ? 1 : 2;
#endif
  if (scan == ESC) { if (down) { quitRequested = true; menuRequested = true; } return true; }
  if (scan <= 0 || scan >= int(codes_.size()) || !codes_[scan].id) return false;
  const bool wasDown = down_[scan];
  down_[scan] = down;
  sendKey(scan, down ? (repeat || wasDown ? 2 : 1) : 0, at);
  return true;
}

void Input::sendKey(int scan, int kind, int64_t at) {
  if (const uint32_t id = codes_[scan].id) {
    if (qc_ses_key(id, kind, inputSeconds(at ? at : qpcNow())) < 0) overflow_ = true;
  }
}

void Input::mouseButton(int dom, bool down, int64_t at) {
  down_[1000 + dom] = down;   // held across a placement (resetForPlacement), as a key is
  sendKey(1000 + dom, down ? 1 : 0, at);
}

void Input::wheel(double notches, int64_t at) {
  wheelRemainder_ += notches;
  while (std::abs(wheelRemainder_) >= 1) {
    const bool up = wheelRemainder_ > 0;
    sendKey(up ? 1100 : 1101, 3, at);
    wheelRemainder_ += up ? -1 : 1;
  }
}

const char* Input::domCode(uint32_t id, int* scan) const {
  for (size_t i = 0; i < codes_.size(); ++i) {
    if (id && codes_[i].id == id) {
      if (scan) *scan = int(i);
      return codes_[i].dom;
    }
  }
  return "";
}

void Input::cacheCodes() {
  struct KeyCode { int scan; const char* name; };
#if defined(NC_WINDOWS)
  static constexpr KeyCode keys[] = {
    {0x10,"KeyQ"},{0x11,"KeyW"},{0x12,"KeyE"},{0x13,"KeyR"},{0x14,"KeyT"},{0x15,"KeyY"},{0x16,"KeyU"},{0x17,"KeyI"},{0x18,"KeyO"},{0x19,"KeyP"},{0x1E,"KeyA"},{0x1F,"KeyS"},{0x20,"KeyD"},{0x21,"KeyF"},{0x22,"KeyG"},{0x23,"KeyH"},{0x24,"KeyJ"},{0x25,"KeyK"},{0x26,"KeyL"},
    {0x2C,"KeyZ"},{0x2D,"KeyX"},{0x2E,"KeyC"},{0x2F,"KeyV"},{0x30,"KeyB"},{0x31,"KeyN"},{0x32,"KeyM"},{0x02,"Digit1"},{0x03,"Digit2"},{0x04,"Digit3"},{0x05,"Digit4"},{0x06,"Digit5"},{0x07,"Digit6"},{0x08,"Digit7"},{0x09,"Digit8"},{0x0A,"Digit9"},{0x0B,"Digit0"},
    {0x39,"Space"},{0x0F,"Tab"},{0x2A,"ShiftLeft"},{0x36,"ShiftRight"},{0x1D,"ControlLeft"},{0x11D,"ControlRight"},{0x38,"AltLeft"},{0x138,"AltRight"},{0x29,"Backquote"},{0x1C,"Enter"},{0x3A,"CapsLock"},
    {0x148,"ArrowUp"},{0x150,"ArrowDown"},{0x14B,"ArrowLeft"},{0x14D,"ArrowRight"},{0x33,"Comma"},{0x34,"Period"},{0x35,"Slash"},{0x27,"Semicolon"},{0x28,"Quote"},{0x1A,"BracketLeft"},{0x1B,"BracketRight"},{0x2B,"Backslash"},{0x0C,"Minus"},{0x0D,"Equal"},
    {0x3B,"F1"},{0x3C,"F2"},{0x3D,"F3"},{0x3E,"F4"},{0x3F,"F5"},{0x40,"F6"},{0x41,"F7"},{0x42,"F8"},{0x43,"F9"},{0x44,"F10"},{0x57,"F11"},{0x58,"F12"},
    {0x47,"Numpad7"},{0x48,"Numpad8"},{0x49,"Numpad9"},{0x4A,"NumpadSubtract"},{0x4B,"Numpad4"},{0x4C,"Numpad5"},{0x4D,"Numpad6"},{0x4E,"NumpadAdd"},{0x4F,"Numpad1"},{0x50,"Numpad2"},{0x51,"Numpad3"},{0x52,"Numpad0"},{0x53,"NumpadDecimal"},{0x37,"NumpadMultiply"},{0x11C,"NumpadEnter"},{0x135,"NumpadDivide"},
    {0x147,"Home"},{0x149,"PageUp"},{0x14F,"End"},{0x151,"PageDown"},{0x152,"Insert"},{0x153,"Delete"},{0x45,"NumLock"},{0x46,"ScrollLock"},{0x245,"Pause"},{0x0E,"Backspace"},{0x56,"IntlBackslash"},{0x137,"PrintScreen"},{0x15B,"MetaLeft"},{0x15C,"MetaRight"},{0x15D,"ContextMenu"},{0x64,"F13"},{0x65,"F14"},{0x66,"F15"}};   // 0x245: the Pause key (raw input sends it as an E1 sequence; the handler below gives it this id)
#else
  static constexpr KeyCode keys[] = {
    {SDL_SCANCODE_Q,"KeyQ"},{SDL_SCANCODE_W,"KeyW"},{SDL_SCANCODE_E,"KeyE"},{SDL_SCANCODE_R,"KeyR"},{SDL_SCANCODE_T,"KeyT"},{SDL_SCANCODE_Y,"KeyY"},{SDL_SCANCODE_U,"KeyU"},{SDL_SCANCODE_I,"KeyI"},{SDL_SCANCODE_O,"KeyO"},{SDL_SCANCODE_P,"KeyP"},{SDL_SCANCODE_A,"KeyA"},{SDL_SCANCODE_S,"KeyS"},{SDL_SCANCODE_D,"KeyD"},{SDL_SCANCODE_F,"KeyF"},{SDL_SCANCODE_G,"KeyG"},{SDL_SCANCODE_H,"KeyH"},{SDL_SCANCODE_J,"KeyJ"},{SDL_SCANCODE_K,"KeyK"},{SDL_SCANCODE_L,"KeyL"},
    {SDL_SCANCODE_Z,"KeyZ"},{SDL_SCANCODE_X,"KeyX"},{SDL_SCANCODE_C,"KeyC"},{SDL_SCANCODE_V,"KeyV"},{SDL_SCANCODE_B,"KeyB"},{SDL_SCANCODE_N,"KeyN"},{SDL_SCANCODE_M,"KeyM"},{SDL_SCANCODE_1,"Digit1"},{SDL_SCANCODE_2,"Digit2"},{SDL_SCANCODE_3,"Digit3"},{SDL_SCANCODE_4,"Digit4"},{SDL_SCANCODE_5,"Digit5"},{SDL_SCANCODE_6,"Digit6"},{SDL_SCANCODE_7,"Digit7"},{SDL_SCANCODE_8,"Digit8"},{SDL_SCANCODE_9,"Digit9"},{SDL_SCANCODE_0,"Digit0"},
    {SDL_SCANCODE_SPACE,"Space"},{SDL_SCANCODE_TAB,"Tab"},{SDL_SCANCODE_LSHIFT,"ShiftLeft"},{SDL_SCANCODE_RSHIFT,"ShiftRight"},{SDL_SCANCODE_LCTRL,"ControlLeft"},{SDL_SCANCODE_RCTRL,"ControlRight"},{SDL_SCANCODE_LALT,"AltLeft"},{SDL_SCANCODE_RALT,"AltRight"},{SDL_SCANCODE_GRAVE,"Backquote"},{SDL_SCANCODE_RETURN,"Enter"},{SDL_SCANCODE_CAPSLOCK,"CapsLock"},
    {SDL_SCANCODE_UP,"ArrowUp"},{SDL_SCANCODE_DOWN,"ArrowDown"},{SDL_SCANCODE_LEFT,"ArrowLeft"},{SDL_SCANCODE_RIGHT,"ArrowRight"},{SDL_SCANCODE_COMMA,"Comma"},{SDL_SCANCODE_PERIOD,"Period"},{SDL_SCANCODE_SLASH,"Slash"},{SDL_SCANCODE_SEMICOLON,"Semicolon"},{SDL_SCANCODE_APOSTROPHE,"Quote"},{SDL_SCANCODE_LEFTBRACKET,"BracketLeft"},{SDL_SCANCODE_RIGHTBRACKET,"BracketRight"},{SDL_SCANCODE_BACKSLASH,"Backslash"},{SDL_SCANCODE_MINUS,"Minus"},{SDL_SCANCODE_EQUALS,"Equal"},
    {SDL_SCANCODE_F1,"F1"},{SDL_SCANCODE_F2,"F2"},{SDL_SCANCODE_F3,"F3"},{SDL_SCANCODE_F4,"F4"},{SDL_SCANCODE_F5,"F5"},{SDL_SCANCODE_F6,"F6"},{SDL_SCANCODE_F7,"F7"},{SDL_SCANCODE_F8,"F8"},{SDL_SCANCODE_F9,"F9"},{SDL_SCANCODE_F10,"F10"},{SDL_SCANCODE_F11,"F11"},{SDL_SCANCODE_F12,"F12"},
    {SDL_SCANCODE_KP_7,"Numpad7"},{SDL_SCANCODE_KP_8,"Numpad8"},{SDL_SCANCODE_KP_9,"Numpad9"},{SDL_SCANCODE_KP_MINUS,"NumpadSubtract"},{SDL_SCANCODE_KP_4,"Numpad4"},{SDL_SCANCODE_KP_5,"Numpad5"},{SDL_SCANCODE_KP_6,"Numpad6"},{SDL_SCANCODE_KP_PLUS,"NumpadAdd"},{SDL_SCANCODE_KP_1,"Numpad1"},{SDL_SCANCODE_KP_2,"Numpad2"},{SDL_SCANCODE_KP_3,"Numpad3"},{SDL_SCANCODE_KP_0,"Numpad0"},{SDL_SCANCODE_KP_PERIOD,"NumpadDecimal"},{SDL_SCANCODE_KP_MULTIPLY,"NumpadMultiply"},{SDL_SCANCODE_KP_ENTER,"NumpadEnter"},{SDL_SCANCODE_KP_DIVIDE,"NumpadDivide"},
    {SDL_SCANCODE_HOME,"Home"},{SDL_SCANCODE_PAGEUP,"PageUp"},{SDL_SCANCODE_END,"End"},{SDL_SCANCODE_PAGEDOWN,"PageDown"},{SDL_SCANCODE_INSERT,"Insert"},{SDL_SCANCODE_DELETE,"Delete"},{SDL_SCANCODE_NUMLOCKCLEAR,"NumLock"},{SDL_SCANCODE_SCROLLLOCK,"ScrollLock"},{SDL_SCANCODE_PAUSE,"Pause"},{SDL_SCANCODE_BACKSPACE,"Backspace"},{SDL_SCANCODE_NONUSBACKSLASH,"IntlBackslash"},{SDL_SCANCODE_PRINTSCREEN,"PrintScreen"},{SDL_SCANCODE_LGUI,"MetaLeft"},{SDL_SCANCODE_RGUI,"MetaRight"},{SDL_SCANCODE_APPLICATION,"ContextMenu"},{SDL_SCANCODE_F13,"F13"},{SDL_SCANCODE_F14,"F14"},{SDL_SCANCODE_F15,"F15"}};   // 0x245: the Pause key (raw input sends it as an E1 sequence; the handler below gives it this id)
#endif
  const auto cache = [&](int scan, const char* dom) {
    codes_[scan] = {dom, qc_ses_code(dom, int(strlen(dom)))};
  };
  for (const KeyCode& key : keys) cache(key.scan, key.name);
  static constexpr const char* buttons[] = {"Mouse0", "Mouse1", "Mouse2", "Mouse3", "Mouse4"};
  for (int i = 0; i < 5; ++i) cache(1000 + i, buttons[i]);
  cache(1100, "WheelUp");
  cache(1101, "WheelDown");
}

#if defined(NC_WINDOWS)
// ---- Windows: raw input and window messages
void Input::attach(HWND hwnd) {
  hwnd_ = capWnd_ = hwnd;
  rawKeys_ = false;
  gate_ = [hwnd] { return GetForegroundWindow() == hwnd; };
  RAWINPUTDEVICE rid = {0x01, 0x02, 0, hwnd};   // generic desktop / mouse, delivered to this window even when the legacy mouse messages are not
  if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) logf("RegisterRawInputDevices failed: %lu", GetLastError());
}

void Input::attachRaw(HWND msgWnd, HWND host, std::function<bool()> gate) {
  hwnd_ = msgWnd; capWnd_ = host; rawKeys_ = true; gate_ = std::move(gate);
  RAWINPUTDEVICE rid[2] = {{0x01, 0x02, RIDEV_INPUTSINK, msgWnd}, {0x01, 0x06, RIDEV_INPUTSINK, msgWnd}};   // mouse and keyboard
  if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) logf("RegisterRawInputDevices failed: %lu", GetLastError());
}

void Input::setCapture(bool on) {
  if (on == captured_) return; captured_ = on;
  if (on) { RECT r; GetClientRect(capWnd_, &r); POINT a = {r.left, r.top}, b = {r.right, r.bottom}; ClientToScreen(capWnd_, &a); ClientToScreen(capWnd_, &b); RECT c = {a.x, a.y, b.x, b.y}; ClipCursor(&c); if (!rawKeys_) while (ShowCursor(FALSE) >= 0) {} }   // embedded: the host's WM_SETCURSOR hides it (a display counter lives per thread queue: the render thread's is not the host's)
  else { ClipCursor(nullptr); if (!rawKeys_) while (ShowCursor(TRUE) < 0) {} }
}

bool Input::handle(UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_INPUT: {
      if (gate_ && !gate_()) return true;   // embedded and not the active view: raw input is ignored
      if (!captured_ && !rawKeys_) return false;
      alignas(8) uint8_t buf[64]; UINT size = sizeof buf;
      if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return true;
      const RAWINPUT* ri = (const RAWINPUT*)buf;
      if (ri->header.dwType == RIM_TYPEKEYBOARD && rawKeys_) { const RAWKEYBOARD& k = ri->data.keyboard; keyScan(k.VKey == VK_PAUSE ? 0x245 : (k.MakeCode | ((k.Flags & RI_KEY_E0) ? 0x100 : 0)), !(k.Flags & RI_KEY_BREAK), false, 0); return true; }
      if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        const bool free = mouseFree();   // the mouse is free (cursor shown): no look, no clicks, no wheel into the game
        const int64_t at = qpcNow();
        if (!free) motion(ri->data.mouse.lLastX, ri->data.mouse.lLastY, at);
        const USHORT bf = ri->data.mouse.usButtonFlags;
        static const struct { USHORT down, up; int dom; } buttons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0}, {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 1}, {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2}, {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3}, {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4}};
        for (const auto& bt : buttons) { if ((bf & bt.down) && !free) mouseButton(bt.dom, true, at); if (bf & bt.up) mouseButton(bt.dom, false, at); }   // a release still arrives when the mouse is free
        if ((bf & RI_MOUSE_WHEEL) && !free) { const short delta = (short)ri->data.mouse.usButtonData; wheel(double(delta) / WHEEL_DELTA, at); }
      }
      return true;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
      if (rawKeys_) return false;   // embedded: keys come from raw input, not from this window's messages
      const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN; return keyScan((int)(((l >> 16) & 0xFF) | (((l >> 24) & 1) ? 0x100 : 0)), down, (l & (1 << 30)) != 0, 0);
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return true;   // the attack key comes from raw input
    case WM_KILLFOCUS: tabEdge = 2; releaseAllKeys(); setCapture(false); return false;   // keys do not stay stuck when focus leaves
  }
  return false;
}

#else
// ---- Linux: SDL events. Relative mouse mode gives unaccelerated motion (the host sets SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE 0), key events carry their own nanosecond timestamps.
void Input::attachSdl(SDL_Window* window, std::function<bool()> gate, bool mainThread) { window_ = window; gate_ = std::move(gate); mainThread_ = mainThread; }

void Input::setCapture(bool on) {
  if (on == captured_) return; captured_ = on;
  wantCapture_ = on; if (window_ && mainThread_) SDL_SetWindowRelativeMouseMode(window_, on);
}

bool Input::onSdlEvent(const SDL_Event& e) {
  if (gate_ && !gate_()) { if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST && focused_) { tabEdge = 2; releaseAllKeys(); setCapture(false); } return false; }
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return keyScan((int)e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat, (int64_t)e.key.timestamp);
    case SDL_EVENT_MOUSE_MOTION: if (!captured_ || mouseFree()) return false; motion(e.motion.xrel, e.motion.yrel, (int64_t)e.motion.timestamp); return true;   // relative mode: the raw device motion
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: { const int dom = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_MIDDLE ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : e.button.button == SDL_BUTTON_X1 ? 3 : e.button.button == SDL_BUTTON_X2 ? 4 : -1;
      if (dom < 0) return false; const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN; const bool gameMouse = captured_ && !mouseFree(); if (!down || gameMouse) mouseButton(dom, down, (int64_t)e.button.timestamp); return gameMouse; }
    case SDL_EVENT_MOUSE_WHEEL: if (!captured_ || mouseFree()) return false; wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y, (int64_t)e.wheel.timestamp); return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST: tabEdge = 2; releaseAllKeys(); setCapture(false); return false;
  }
  return false;
}
#endif

}  // namespace nc
