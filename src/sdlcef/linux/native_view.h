#pragma once

#include "../native_hub.h"
#include "nc_app.h"
#include "nc_sha256.h"
#include <SDL3/SDL_vulkan.h>

// nc_embed owns the render thread. The host owns the Wayland window and SDL events.
bool tc_prepare_core();
void tc_core_prepare(const char* cacheDir, bool fetch, bool staging);
void tc_core_wait();
void tc_start(NativeWindow window, const char* args);
void tc_stop();
void tc_set_visible(bool visible);
void tc_set_frame_cap(int fps);
void tc_set_menu_request(void (*callback)());
void tc_set_tab(void (*callback)(bool));
void tc_set_sim_out(void (*callback)(const char*));
void tc_set_js_out(void (*callback)(const char*));
bool tc_take_error(char* reason, size_t size);
std::string tc_error_code();

namespace QrazyLinuxNative {
inline constexpr struct Binding {
  const char* name;
  size_t limit;
} bindings[] = {
  {"qrazyNativeMatch", 12582912},
  {"qrazyNativeInfo", 262144},
  {"qrazyNativeSettings", 65536},
  {"qrazyNativeHud", 262144},
};

class View {
  SDL_Window* surface_ = nullptr;
  bool shown_ = false;
  bool started_ = false;
  bool captured_ = false;
  bool text_ = false;
  Uint64 next_pose_ = 0;
  std::string command_;
  std::vector<std::string> held_;
  std::atomic<bool> menu_{false};
  std::mutex messages_lock_;
  std::vector<std::string> messages_;
  inline static View* instance_ = nullptr;

  void Queue(std::string message) {
    std::lock_guard<std::mutex> lock(messages_lock_);
    if (messages_.size() < 256) messages_.push_back(std::move(message));
  }

  static std::filesystem::path Asset(const std::filesystem::path& profile,
      const std::string& kind, const std::string& key) {
    if (!QrazyNativeMatch::NativeText(key, 2048)) return {};
    const auto name = kind + '\0' + key;
    auto file = profile / "desktop-assets" / (nc::sha256Hex(name.data(), name.size()) + ".asset");
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, error))) return {};
    const auto bytes = QrazyNativeMatch::NativePayloadSize(file);
    if (!bytes || std::filesystem::file_size(file, error) != QrazyNativeMatch::AssetHeader + bytes || error) return {};
    return file;
  }

  static void Script(CefRefPtr<CefFrame> frame, const std::string& js) {
    if (frame) frame->ExecuteJavaScript(js, "qrazy-native-view", 0);
  }

  void Drain(CefRefPtr<CefFrame> frame) {
    std::vector<std::string> pending;
    {
      std::lock_guard<std::mutex> lock(messages_lock_);
      pending.swap(messages_);
    }
    for (const auto& js : pending) Script(frame, js);
  }

  static void State(CefRefPtr<CefFrame> frame, const char* state,
      const std::string& code = {}, const std::string& reason = {}) {
    auto packet = CefDictionaryValue::Create();
    packet->SetString("state", state);
    if (!code.empty()) packet->SetString("code", code);
    if (!reason.empty()) packet->SetString("reason", reason);
    if (std::strcmp(state, "started") == 0) {
      if (appHudFeed()) packet->SetInt("hudFeed", 1);
      if (appAudioFeed()) packet->SetInt("audioFeed", 1);
    }
    auto value = CefValue::Create();
    value->SetDictionary(packet);
    Script(frame, "window.qrazyNativeMatchState&&window.qrazyNativeMatchState(" +
        CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() + ")");
  }

  void Show(SDL_Window* page, CefRefPtr<CefFrame> frame, bool visible) {
    shown_ = visible;
    tc_set_visible(visible);
    if (visible) {
      SDL_ShowWindow(surface_);
      SDL_HideWindow(page);
    } else {
      SDL_SetWindowRelativeMouseMode(surface_, false);
      SDL_StopTextInput(surface_);
      captured_ = text_ = false;
      SDL_ShowWindow(page);
      SDL_HideWindow(surface_);
    }
    Script(frame, visible ? "window.__qtcOn=true" : "window.__qtcOn=false");
  }

 public:
  bool Shown() const { return shown_; }
  bool HasKeyboardFocus() const { return shown_ && SDL_GetKeyboardFocus() == surface_; }
  void SetFrameCap(int fps) { if (surface_) tc_set_frame_cap(fps); }

  void InstallHub() {
    nc::appHub().setSink([](const nc::HubEvent& event) {
      hub_view_->Queue("window.qrazyNativeHubEvent&&window.qrazyNativeHubEvent(" +
          QrazyNativeMatch::EncodeHubEvent(event) + ")");
    });
    hub_view_ = this;
  }

  void ShutdownHub() {
    nc::appHub().setSink(nullptr);
    nc::appHub().shutdown();
    hub_view_ = nullptr;
  }

  void Stop(SDL_Window* page, CefRefPtr<CefFrame> frame = {}) {
    if (!surface_) return;
    if (shown_) Show(page, frame, false);
    tc_stop();
    SDL_DestroyWindow(surface_);
    surface_ = nullptr;
    started_ = false;
    command_.clear();
    held_.clear();
    menu_ = false;
    instance_ = nullptr;
  }

  void Message(SDL_Window* page, CefRefPtr<CefFrame> frame, const std::filesystem::path& profile,
      int operation, const std::string& payload, bool vsync, int cap) {
    if (operation == 1) { appSetBoard(payload); return; }
    if (operation == 2) { appSetSettings(payload); return; }
    if (operation == 3) { appSetPageHud(payload); return; }
    if (QrazyNativeMatch::HandleHub(payload)) return;
    QrazyNativeMatch::NativeMatch request;
    if (!QrazyNativeMatch::ParseNativeMatch(payload,
        [&profile](const std::string& kind, const std::string& key) { return Asset(profile, kind, key); },
        profile / "native-match", request)) {
      State(frame, "refused", request.code, request.error);
      return;
    }
    if (request.stop) { Stop(page, frame); return; }
    if (request.resume) {
      if (started_ && !shown_) {
        for (const auto& pending : held_) appSimOp(pending);
        held_.clear();
        Show(page, frame, true);
      }
      return;
    }
    if (request.simop) {
      if (shown_ || request.soundfact || payload.find("\"runack\"") != std::string::npos) appSimOp(payload);
      else if (surface_ && held_.size() < 256) held_.push_back(payload);
      return;
    }
    if (!request.start || (surface_ && command_ == request.cmdline)) return;
    Stop(page, frame);
    if (!tc_prepare_core()) {
      char reason[512] = "Verified native core unavailable";
      tc_take_error(reason, sizeof(reason));
      State(frame, "refused", "core", reason);
      return;
    }
    int width, height;
    SDL_GetWindowSize(page, &width, &height);
    const auto flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        SDL_WINDOW_HIDDEN | (SDL_GetWindowFlags(page) & SDL_WINDOW_FULLSCREEN);
    surface_ = SDL_CreateWindow("Qrazy", width, height, flags);
    if (!surface_) {
      State(frame, "refused", "unsupported", SDL_GetError());
      return;
    }
    instance_ = this;
    tc_set_menu_request([] { instance_->menu_ = true; });
    tc_set_tab([](bool down) {
      // The game's Tab handlers listen on document.
      instance_->Queue(std::string("document.dispatchEvent(new KeyboardEvent('") +
          (down ? "keydown" : "keyup") + "',{code:'Tab',key:'Tab',bubbles:true,cancelable:true}))");
    });
    tc_set_sim_out([](const char* json) {
      instance_->Queue(std::string("window.qrazyNativeSim&&window.qrazyNativeSim(") + json + ")");
    });
    tc_set_js_out([](const char* js) { instance_->Queue(js); });
    command_ = request.cmdline;
    next_pose_ = 0;
    const std::string args = command_ + (vsync ? " --pm fifo" : " --pm immediate --cap " + std::to_string(cap));
    tc_start(surface_, args.c_str());
    Show(page, frame, true);
  }

  bool Event(const SDL_Event& event, bool& closing) {
    if (!surface_) return false;
    const SDL_WindowID id = SDL_GetWindowID(surface_);
    SDL_Window* event_window = SDL_GetWindowFromEvent(&event);
    if (!event_window || SDL_GetWindowID(event_window) != id) return shown_ && event_window;
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) closing = true;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT)) {
      SDL_SetWindowFullscreen(surface_, !(SDL_GetWindowFlags(surface_) & SDL_WINDOW_FULLSCREEN));
      return true;
    }
    if (shown_) {
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && !captured_) {
        const float density = SDL_GetWindowPixelDensity(surface_);
        int element = 0, index = 0;
        char url[1024] = {};
        const int result = appClickElement(static_cast<int>(event.button.x * density),
            static_cast<int>(event.button.y * density), &element, &index, url, sizeof(url));
        if (result == 1) {
          Queue("window.qrazyNativeClick&&window.qrazyNativeClick({element:" +
              std::to_string(element) + ",index:" + std::to_string(index) + "})");
          return true;
        }
        if (result == 2) {
          if (!std::strncmp(url, "https://", 8) || !std::strncmp(url, "http://", 7)) SDL_OpenURL(url);
          return true;
        }
      }
      appOnSdlEvent(event);
    }
    return true;
  }

  void Pump(SDL_Window* page, CefRefPtr<CefFrame> frame) {
    if (!surface_) {
      nc::appHub().service();
      Drain(frame);
      return;
    }
    char reason[512];
    if (tc_take_error(reason, sizeof(reason))) {
      State(frame, started_ ? "ended" : "refused", tc_error_code(), reason);
      Stop(page, frame);
      return;
    }
    if (appTakeStarted()) {
      started_ = true;
      State(frame, "started");
      appHudReady();
    }
    if (menu_.exchange(false) && shown_) {
      Show(page, frame, false);
      double p[14];
      char js[256];
      if (appGetPose(p)) {
        std::snprintf(js, sizeof(js), "window.dispatchEvent(new CustomEvent('qrazy-native-menu',{detail:{x:%.3f,y:%.3f,z:%.3f,yaw:%.6f,pitch:%.6f}}))", p[0], p[1], p[2], p[3], p[4]);
        Script(frame, js);
      } else Script(frame, "window.dispatchEvent(new Event('qrazy-native-menu'))");
    }
    if (shown_) {
      const bool capture = appCaptureWanted() && SDL_GetKeyboardFocus() == surface_;
      if (capture != captured_ && SDL_SetWindowRelativeMouseMode(surface_, capture)) captured_ = capture;
      const bool typing = appTextInputWanted();
      if (text_ != typing && (typing ? SDL_StartTextInput(surface_) : SDL_StopTextInput(surface_))) text_ = typing;
      if (appTakePoseRequest() || SDL_GetTicks() >= next_pose_) {
        next_pose_ = SDL_GetTicks() + 100;
        double p[14];
        char js[480];
        if (appGetPose(p)) {
          std::snprintf(js, sizeof(js), "window.qrazyNativePose&&window.qrazyNativePose({x:%.3f,y:%.3f,z:%.3f,vx:%.3f,vy:%.3f,vz:%.3f,yaw:%.6f,pitch:%.6f,ex:%.3f,ey:%.3f,ez:%.3f,ground:%s,crouched:%s,weapon:%d})",
              p[5], p[6], p[7], p[8], p[9], p[10], p[3], p[4], p[0], p[1], p[2],
              p[11] ? "true" : "false", p[12] ? "true" : "false", static_cast<int>(p[13]));
          Script(frame, js);
        }
      }
    }
    Drain(frame);
  }
 private:
  inline static View* hub_view_ = nullptr;
};
}  // namespace QrazyLinuxNative
