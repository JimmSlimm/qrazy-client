int Modifiers(SDL_Keymod mod) {
  int result = 0;
  if (mod & SDL_KMOD_SHIFT) result |= EVENTFLAG_SHIFT_DOWN;
  if (mod & SDL_KMOD_CTRL) result |= EVENTFLAG_CONTROL_DOWN;
  if (mod & SDL_KMOD_ALT) result |= EVENTFLAG_ALT_DOWN;
  if (mod & SDL_KMOD_GUI) result |= EVENTFLAG_COMMAND_DOWN;
  if (mod & SDL_KMOD_CAPS) result |= EVENTFLAG_CAPS_LOCK_ON;
  if (mod & SDL_KMOD_NUM) result |= EVENTFLAG_NUM_LOCK_ON;
  return result;
}
int VirtualKey(SDL_Scancode code) {
  if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A'+code-SDL_SCANCODE_A;
  if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return '1'+code-SDL_SCANCODE_1;
  if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) return 0x70+code-SDL_SCANCODE_F1;
  switch (code) {
    case SDL_SCANCODE_KP_0:return 96;case SDL_SCANCODE_KP_1:return 97;case SDL_SCANCODE_KP_2:return 98;case SDL_SCANCODE_KP_3:return 99;
    case SDL_SCANCODE_KP_4:return 100;case SDL_SCANCODE_KP_5:return 101;case SDL_SCANCODE_KP_6:return 102;case SDL_SCANCODE_KP_7:return 103;case SDL_SCANCODE_KP_8:return 104;case SDL_SCANCODE_KP_9:return 105;
    case SDL_SCANCODE_KP_ENTER:return 13;case SDL_SCANCODE_KP_PLUS:return 107;case SDL_SCANCODE_KP_MINUS:return 109;case SDL_SCANCODE_KP_MULTIPLY:return 106;case SDL_SCANCODE_KP_DIVIDE:return 111;case SDL_SCANCODE_KP_PERIOD:return 110;case SDL_SCANCODE_NONUSBACKSLASH:return 226;
    case SDL_SCANCODE_0:return '0'; case SDL_SCANCODE_RETURN:return 13; case SDL_SCANCODE_ESCAPE:return 27;
    case SDL_SCANCODE_BACKSPACE:return 8; case SDL_SCANCODE_TAB:return 9; case SDL_SCANCODE_SPACE:return 32;
    case SDL_SCANCODE_LEFT:return 37; case SDL_SCANCODE_UP:return 38; case SDL_SCANCODE_RIGHT:return 39; case SDL_SCANCODE_DOWN:return 40;
    case SDL_SCANCODE_LCTRL:case SDL_SCANCODE_RCTRL:return 17; case SDL_SCANCODE_LSHIFT:case SDL_SCANCODE_RSHIFT:return 16;
    case SDL_SCANCODE_LALT:case SDL_SCANCODE_RALT:return 18; case SDL_SCANCODE_LGUI:return 91; case SDL_SCANCODE_RGUI:return 92;
    case SDL_SCANCODE_DELETE:return 46; case SDL_SCANCODE_INSERT:return 45; case SDL_SCANCODE_HOME:return 36; case SDL_SCANCODE_END:return 35;
    case SDL_SCANCODE_PAGEUP:return 33; case SDL_SCANCODE_PAGEDOWN:return 34; case SDL_SCANCODE_GRAVE:return 192;
    case SDL_SCANCODE_MINUS:return 189; case SDL_SCANCODE_EQUALS:return 187; case SDL_SCANCODE_LEFTBRACKET:return 219;
    case SDL_SCANCODE_RIGHTBRACKET:return 221; case SDL_SCANCODE_BACKSLASH:return 220; case SDL_SCANCODE_SEMICOLON:return 186;
    case SDL_SCANCODE_APOSTROPHE:return 222; case SDL_SCANCODE_COMMA:return 188; case SDL_SCANCODE_PERIOD:return 190; case SDL_SCANCODE_SLASH:return 191;
    default:return 0;
  }
}
int NativeKeyCode(const SDL_KeyboardEvent& event) {
  return event.raw ? event.raw : static_cast<int>(MapVirtualKeyW(VirtualKey(event.scancode),MAPVK_VK_TO_VSC_EX));
}
int MouseModifiers() {
  int mod = Modifiers(SDL_GetModState()); Uint32 buttons = SDL_GetMouseState(nullptr,nullptr);
  if (buttons & SDL_BUTTON_LMASK) mod |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  if (buttons & SDL_BUTTON_RMASK) mod |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  if (buttons & SDL_BUTTON_MMASK) mod |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  return mod;
}
void Keyboard(Client* client, const SDL_KeyboardEvent& event) {
  if (!client->browser) return;
  bool down = event.type == SDL_EVENT_KEY_DOWN;
  if ((event.mod & SDL_KMOD_ALT) && !(event.mod & SDL_KMOD_CTRL) && event.scancode == SDL_SCANCODE_F4) {
    if (down && !event.repeat) { client->Release("shortcut-close"); closing = true; } return;
  }
  if ((event.mod & SDL_KMOD_ALT) && event.scancode == SDL_SCANCODE_RETURN) {
    if (down && !event.repeat) SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));
    return;
  }
  if(client->recovering){if(down&&!event.repeat&&event.scancode==SDL_SCANCODE_R)client->Retry();return;}
  if(client->loading)return;
  // SDL can receive focus before the asynchronous browser exists. Reconcile
  // CEF focus when a real foreground keyboard event reaches the host.
  if(SDL_GetKeyboardFocus()!=window)return;
  client->browser->GetHost()->SetFocus(true);
  if(down&&!event.repeat&&(event.mod&SDL_KMOD_CTRL)&&!(event.mod&(SDL_KMOD_ALT|SDL_KMOD_GUI))&&!captured&&SDL_GetKeyboardFocus()==window) {
    if(event.scancode==SDL_SCANCODE_V){char* text=SDL_GetClipboardText();if(text){if(strlen(text)<=1048576)client->browser->GetHost()->ImeCommitText(text,CefRange(UINT32_MAX,UINT32_MAX),0);SDL_free(text);}return;}
    if(event.scancode==SDL_SCANCODE_C||event.scancode==SDL_SCANCODE_X){if(!client->selection.empty())SDL_SetClipboardText(client->selection.c_str());if(event.scancode==SDL_SCANCODE_X)client->browser->GetMainFrame()->Cut();return;}
  }
  // Native capture has no Chromium pointer-lock Escape interception. Deliver
  // both edges to the game's existing menu/chat/console handlers; their release
  // operation frees SDL capture and menu resume happens on keyup.
  // Leave compositor shortcuts ungrabbed. No keyboard grab/shortcut-inhibit request.
  CefKeyEvent key; key.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
  key.windows_key_code = VirtualKey(event.scancode);
  // CEF 154 converts native_key_code directly through NativeKeycodeToDomCode.
  // Windows expects the hardware scan code (including E0/E1), not WM_KEY LPARAM.
  key.native_key_code = NativeKeyCode(event);
  key.modifiers = Modifiers(event.mod);
  if(event.scancode>=SDL_SCANCODE_KP_DIVIDE&&event.scancode<=SDL_SCANCODE_KP_PERIOD)key.modifiers|=EVENTFLAG_IS_KEY_PAD;
  if(event.scancode==SDL_SCANCODE_LSHIFT||event.scancode==SDL_SCANCODE_LCTRL||event.scancode==SDL_SCANCODE_LALT||event.scancode==SDL_SCANCODE_LGUI)key.modifiers|=EVENTFLAG_IS_LEFT;
  if(event.scancode==SDL_SCANCODE_RSHIFT||event.scancode==SDL_SCANCODE_RCTRL||event.scancode==SDL_SCANCODE_RALT||event.scancode==SDL_SCANCODE_RGUI)key.modifiers|=EVENTFLAG_IS_RIGHT;
  key.is_system_key = !!(event.mod & SDL_KMOD_ALT);
  if (event.repeat) key.modifiers |= EVENTFLAG_IS_REPEAT;
  if (key.windows_key_code) client->browser->GetHost()->SendKeyEvent(key);
  if (down && (event.scancode == SDL_SCANCODE_RETURN || event.scancode == SDL_SCANCODE_KP_ENTER || event.scancode == SDL_SCANCODE_TAB || event.scancode == SDL_SCANCODE_BACKSPACE)) {
    key.type = KEYEVENT_CHAR; key.character = key.unmodified_character = static_cast<char16_t>(key.windows_key_code);
    client->browser->GetHost()->SendKeyEvent(key);
  }
}
