// SDL-owned Wayland production host. No copied game simulation or input sampling.
#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <csignal>
#include <sys/stat.h>
#include <sys/file.h>
#include "include/cef_cookie.h"
#include "include/base/cef_compiler_specific.h"
#include "include/cef_version.h"
#include "include/cef_load_handler.h"
#include "include/cef_download_handler.h"
#include "../demo_download.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_context_menu_handler.h"
#include "desktop_worker.h"
#include "server_retry.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_helpers.h"

namespace {
constexpr char kOrigin[] = "https://qrazy-game.onrender.com/";
SDL_Window* window = nullptr;
SDL_GLContext glcontext = nullptr;
bool closing = false, closed = false, captured = false, hidden_check = false;
int generation = 0, frames = 0, copies = 0, errors = 0, presentations = 0;
double copy_ms = 0, swap_ms = 0;
std::string bridge_source;
std::string close_action;
std::string installed_title="Qrazy";
bool amd_mesa_renderer = false;
QrazyWindows::ServerRetry server_retry;
uint64_t connect_started=0;
int render_rate = 0;
DesktopWorker desktop_worker;
#include "profile_lock.h"
#include "desktop_policy.h"
#include "window_state.h"
#include "recovery_ui.h"
struct DialogResult {
  int id; unsigned epoch; bool save, error; std::string path, text;
  std::chrono::steady_clock::time_point expires=std::chrono::steady_clock::now()+std::chrono::minutes(5);
};
std::mutex dialog_mutex;
std::deque<DialogResult> dialog_results;
std::atomic<bool> dialog_active{false};
void SDLCALL FileChosen(void* userdata,const char* const* files,int) {
  std::unique_ptr<DialogResult> result(static_cast<DialogResult*>(userdata));
  result->error=!files;result->path.clear();if(files&&files[0])result->path=files[0];
  std::lock_guard<std::mutex> lock(dialog_mutex);dialog_results.push_back(std::move(*result));dialog_active=false;
}
class CookieFlushed final : public CefCompletionCallback {
 public: bool done=false;void OnComplete() override { done=true; }
 private: IMPLEMENT_REFCOUNTING(CookieFlushed);
};

// CEF takes integer fps. Round up fractional Hz so it never caps below the
// reported display rate; SDL's swap interval controls visible presentation.
int RateForRefresh(double hz) {
  if (!std::isfinite(hz) || hz <= 0 || hz > std::numeric_limits<int>::max()) return 0;
  return static_cast<int>(std::ceil(hz));
}
int DisplayRenderRate() {
  auto display = SDL_GetDisplayForWindow(window);
  const auto* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
  if (!mode) { std::fprintf(stderr, "QRAZY cannot query display refresh: %s\n", SDL_GetError()); return 0; }
  double hz = mode->refresh_rate;
  if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
    hz = static_cast<double>(mode->refresh_rate_numerator)/mode->refresh_rate_denominator;
  int rate = RateForRefresh(hz);
  if (rate && rate != render_rate)
    std::fprintf(stderr, "QRAZY display=%u refresh-hz=%.6f CEF-target-fps=%d (not measured presentation)\n", display, hz, rate);
  return rate;
}

bool Trusted(const CefString& url) {
  CefURLParts parts;
  return CefParseURL(url, parts) && CefString(&parts.scheme) == "https" &&
    CefString(&parts.host) == "qrazy-game.onrender.com" &&
    (CefString(&parts.port).empty() || CefString(&parts.port) == "443") &&
    CefString(&parts.username).empty() && CefString(&parts.password).empty();
}
CefRefPtr<CefDictionaryValue> Dict() { return CefDictionaryValue::Create(); }

class Native final : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& value, CefString& exception) override {
    auto context = CefV8Context::GetCurrentContext();
    auto frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsMain() || !Trusted(frame->GetURL()) || args.size() != 3 || !args[2]->IsString() || args[2]->GetStringValue().length()>1500000 ||
        !args[0]->IsString() || !args[1]->IsInt()) { exception = "Unauthorized native bridge call"; return true; }
    const auto op = args[0]->GetStringValue().ToString();
    if (op != "capture" && op != "release" && op != "clock" && op != "quit" && op != "fullscreen-state" && op != "fullscreen-toggle" && op != "assets" && op != "status" && op != "refresh-game" && op != "retry" && op != "diagnostics" && op != "clipboard-write" && op != "config-import" && op != "config-export" && op != "update-notice" && op != "update-state" && op != "update-check" && op != "update-install" && op != "changelog") {
      exception = "Unknown native operation"; return true;
    }
    auto message = CefProcessMessage::Create("qrazy-command-v1");
    message->GetArgumentList()->SetString(0, op);
    message->GetArgumentList()->SetInt(1, args[1]->GetIntValue());
    message->GetArgumentList()->SetString(2,args[2]->GetStringValue());
    frame->SendProcessMessage(PID_BROWSER, message);
    value = CefV8Value::CreateUndefined(); return true;
  }
 private:
  IMPLEMENT_REFCOUNTING(Native);
};

class App final : public CefApp, public CefRenderProcessHandler, public CefBrowserProcessHandler {
  struct Binding { CefRefPtr<CefV8Context> context; CefRefPtr<CefV8Value> dispatch; };
  std::map<int, Binding> bindings;
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> line) override {
    if (line->GetSwitchValue("type") != "gpu-process") return;
    // Log only this fixed set of graphics controls, never the full command line.
    // Hardware captures confirm disable-angle-features already reaches the child.
    std::fprintf(stderr, "QRAZY GPU-child angle=%s disabled-angle-features=%s software-disabled=%d sandbox-fatal=%s\n",
      line->GetSwitchValue("use-angle").ToString().c_str(),
      line->GetSwitchValue("disable-angle-features").ToString().c_str(),
      line->HasSwitch("disable-software-rasterizer"),
      line->GetSwitchValue("gpu-sandbox-failures-fatal").ToString().c_str());
  }
  void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> line) override {
    // Deliberately no user-controlled switches in the browser process launch path.
    // Extensions are outside this game's trust boundary, including distro auto-installed ones.
    line->AppendSwitch("disable-extensions");
    line->AppendSwitchWithValue("ozone-platform", "wayland");
    // Select in the browser after SDL identifies the driver. GPU children must
    // retain the inherited selection rather than overwrite it with their default.
    if (process_type.empty()) {
      line->AppendSwitchWithValue("use-angle", amd_mesa_renderer ? "vulkan" : "gl-egl");
      line->AppendSwitch("disable-software-rasterizer");
      if (amd_mesa_renderer) {
        // VA-API warming can create radeonsi command/shader workers before
        // sandbox initialization even when ANGLE itself uses Vulkan.
        line->AppendSwitch("disable-accelerated-video-decode");
        line->AppendSwitch("disable-accelerated-video-encode");
        // These select synchronous cleanup work. Pinned ANGLE still creates its
        // cleanup thread unconditionally; this is not a sandbox-startup fix.
        line->AppendSwitchWithValue("disable-angle-features", "asyncGarbageCleanup,asyncCommandBufferReset");
      }
    }
    line->AppendSwitchWithValue("class", "qrazy-sdl-cef");
    line->AppendSwitchWithValue("enable-logging", "stderr");
    // Linux compatibility workaround: Mesa/ANGLE starts graphics worker threads
    // before Chromium's GPU sandbox, causing "Current process is not mono-threaded"
    // and repeated GPU-process crashes on the reported Radeon/Wayland system.
    // Disable ONLY GPU-process isolation; renderer sandboxes and hardware rendering
    // remain enabled. This reduces GPU-process containment and needs hardware retesting.
    if (process_type.empty() || process_type == "gpu-process")
      line->AppendSwitch("disable-gpu-sandbox");
  }
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    if (!frame->IsMain() || !Trusted(frame->GetURL())) return;
    auto native = CefV8Value::CreateFunction("qrazyNative", new Native);
    // Temporary bootstrap slot; removed before any website script runs. Dispatcher stays private.
    context->GetGlobal()->SetValue("__qrazyBootstrap", native, V8_PROPERTY_ATTRIBUTE_NONE);
    CefRefPtr<CefV8Value> dispatch; CefRefPtr<CefV8Exception> exception;
    bool ok = context->Eval(bridge_source + "(__qrazyBootstrap)", "qrazy-native-bridge", 1, dispatch, exception);
    context->GetGlobal()->DeleteValue("__qrazyBootstrap");
    if (ok && dispatch && dispatch->IsFunction()) {
      bindings[browser->GetIdentifier()] = {context, dispatch};
      std::fprintf(stderr, "QRAZY trusted main-frame bridge installed\n");
    }
    else std::fprintf(stderr, "QRAZY bridge bootstrap failed\n");
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    auto it = bindings.find(browser->GetIdentifier());
    if (frame->IsMain() && it != bindings.end() && it->second.context->IsSame(context)) bindings.erase(it);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message) override {
    if (source != PID_BROWSER || message->GetName() != "qrazy-event-v1" || !frame->IsMain() || !Trusted(frame->GetURL())) return false;
    auto it = bindings.find(browser->GetIdentifier());
    if (it == bindings.end() || !it->second.context->IsValid()) return true;
    auto& binding = it->second;
    binding.context->Enter();
    binding.dispatch->ExecuteFunction(nullptr, {CefV8Value::CreateString(message->GetArgumentList()->GetString(0))});
    binding.context->Exit(); return true;
  }
 private:
  IMPLEMENT_REFCOUNTING(App);
};

class Texture {
  GLuint texture = 0, program = 0;
  int width = 0, height = 0;
  bool ready = false;
 public:
  ~Texture() { if (texture) glDeleteTextures(1, &texture); if (program) glDeleteProgram(program); }
  bool Copy(const CefAcceleratedPaintInfo& info) {
    if (!SDL_GL_MakeCurrent(window, glcontext)) return false;
    auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto bind = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create || !destroy || !bind || info.plane_count != 1 ||
        (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) return false;
    const auto& p = info.planes[0];
    if (p.offset > INT32_MAX || p.stride > INT32_MAX) return false;
    const EGLint fourcc = info.format == CEF_COLOR_TYPE_BGRA_8888 ? 0x34325241 : 0x34324241;
    std::vector<EGLint> attrs = {EGL_WIDTH, info.extra.coded_size.width, EGL_HEIGHT, info.extra.coded_size.height,
      EGL_LINUX_DRM_FOURCC_EXT, fourcc, EGL_DMA_BUF_PLANE0_FD_EXT, p.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(p.offset), EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(p.stride)};
    if (info.modifier != UINT64_C(0x00ffffffffffffff)) attrs.insert(attrs.end(), {
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(info.modifier & 0xffffffff),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(info.modifier >> 32)});
    attrs.push_back(EGL_NONE);
    auto display = eglGetCurrentDisplay();
    auto image = create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image == EGL_NO_IMAGE_KHR) { std::fprintf(stderr, "QRAZY DMA-BUF import failed EGL=%x modifier=%llx\n", eglGetError(), static_cast<unsigned long long>(info.modifier)); return false; }
    GLuint source = 0, fbo = 0;
    glGenTextures(1, &source); glBindTexture(GL_TEXTURE_2D, source); bind(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
      if (!texture) glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      const auto& rect = info.extra.visible_rect;
      if (width != rect.width || height != rect.height) {
        width = rect.width; height = rect.height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      }
      glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rect.x, rect.y, width, height);
      // Safe first prototype: copy completion before CEF reuses its pool buffer.
      // A later fenced ring needs an explicit producer/consumer lifetime proof.
      glFinish(); ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &source); destroy(display, image);
    ready = ok; return ok;
  }
  bool Draw(const CefRect* area = nullptr, bool force = false) {
    if (!texture || (!ready && !force)) return false;
    if (!program) {
      auto compile = [](GLenum type, const char* code) {
        GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &code, nullptr); glCompileShader(shader);
        GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); std::fprintf(stderr, "QRAZY shader: %s\n", log); }
        return shader;
      };
      GLuint vs = compile(GL_VERTEX_SHADER, "attribute vec2 p;attribute vec2 uv;varying vec2 t;void main(){gl_Position=vec4(p,0.,1.);t=uv;}");
      GLuint fs = compile(GL_FRAGMENT_SHADER, "precision mediump float;varying vec2 t;uniform sampler2D tex;void main(){gl_FragColor=texture2D(tex,t);}");
      program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glBindAttribLocation(program, 0, "p"); glBindAttribLocation(program, 1, "uv"); glLinkProgram(program);
      glDeleteShader(vs); glDeleteShader(fs);
      GLint ok; glGetProgramiv(program, GL_LINK_STATUS, &ok); if (!ok) return false;
    }
    int pw, ph; SDL_GetWindowSizeInPixels(window, &pw, &ph);
    if (area) {
      int lw,lh; SDL_GetWindowSize(window, &lw,&lh);
      glViewport(area->x*pw/lw, ph-(area->y+area->height)*ph/lh, area->width*pw/lw, area->height*ph/lh);
      glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else { glViewport(0, 0, pw, ph); glDisable(GL_BLEND); }
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texture); glUniform1i(glGetUniformLocation(program, "tex"), 0);
    const GLfloat quad[] = {-1,1,0,0, -1,-1,0,1, 1,1,1,0, 1,-1,1,1};
    glBindBuffer(GL_ARRAY_BUFFER, 0); glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), quad+2); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ready = false; return glGetError() == GL_NO_ERROR;
  }
};

class Client final : public DesktopPolicy::RecoveryState, public CefClient, public CefRenderHandler, public CefLifeSpanHandler,
                     public CefRequestHandler, public CefDisplayHandler, public CefLoadHandler, public CefDownloadHandler, public CefDialogHandler,public CefContextMenuHandler {
 public:
  CefRefPtr<CefBrowser> browser;
  Texture view;
  Texture popup;
  CefRect popup_rect;
  bool popup_visible = false, dirty = false;
  SDL_Cursor* cursor = nullptr;
  bool composing=false;
  bool install_confirmation=false;
  int install_reply=0;
  void ConfirmInstall(bool accepted) {
    if(!install_confirmation)return;
    install_confirmation=false;
    auto result=Dict();result->SetBool("ok",true);
    if(!accepted||!DesktopPolicy::AllowCloseAction(closing,recovering,loading,captured,SDL_GetKeyboardFocus()==window,hidden_check)){
      auto data=Dict();data->SetString("phase","cancelled");data->SetString("message","Installation cancelled. Qrazy remains open.");result->SetDictionary("data",data);Reply(install_reply,result);return;
    }
    auto request=Dict();request->SetString("op","update-install");request->SetList("args",CefListValue::Create());auto value=CefValue::Create();value->SetDictionary(request);
    if(!desktop_worker.Submit(install_reply,desktop_worker.epoch.load(),CefWriteJSON(value,JSON_WRITER_DEFAULT))){result->SetBool("ok",false);result->SetString("error","Updater busy; try again");Reply(install_reply,result);}
  }
  std::string selection;
  double wheel_x=0,wheel_y=0;
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
  bool CanDownload(CefRefPtr<CefBrowser> value,const CefString& url,const CefString& method) override {return !closing && value && Trusted(value->GetMainFrame()->GetURL()) && method=="GET" && DemoDownload::TrustedBlob(url.ToString());}
  bool OnBeforeDownload(CefRefPtr<CefBrowser> value,CefRefPtr<CefDownloadItem> item,const CefString& name,CefRefPtr<CefBeforeDownloadCallback> callback) override {if(!closing && !dialog_active && value && Trusted(value->GetMainFrame()->GetURL()) && DemoDownload::TrustedBlob(item->GetOriginalUrl().ToString()) && DemoDownload::ExportName(name.ToString())) {Release("export-save");DemoDownload::Save(window,name.ToString(),callback);}return true;}
  void OnDownloadUpdated(CefRefPtr<CefBrowser>,CefRefPtr<CefDownloadItem> item,CefRefPtr<CefDownloadItemCallback> callback) override {if(!DemoDownload::TrustedBlob(item->GetOriginalUrl().ToString()))callback->Cancel();}
  bool OnFileDialog(CefRefPtr<CefBrowser>,FileDialogMode,const CefString&,const CefString&,const std::vector<CefString>&,const std::vector<CefString>&,const std::vector<CefString>&,CefRefPtr<CefFileDialogCallback> callback) override { callback->Cancel();return true; }
  bool GetScreenInfo(CefRefPtr<CefBrowser>,CefScreenInfo& info) override {
    int w,h,pw,ph;SDL_GetWindowSize(window,&w,&h);SDL_GetWindowSizeInPixels(window,&pw,&ph);
    info.device_scale_factor=DesktopPolicy::PixelScale(w,pw);
    info.rect=CefRect(0,0,w,h);info.available_rect=info.rect;return true;
  }
  void OnTextSelectionChanged(CefRefPtr<CefBrowser>,const CefString& text,const CefRange&) override { selection=text.ToString().substr(0,1048576); }
  void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser>,const CefRange&,const RectList& bounds) override {
    if(hidden_check||bounds.empty())return;
    const auto& r=bounds.back();SDL_Rect rect{r.x,r.y,r.width,r.height};SDL_SetTextInputArea(window,&rect,r.width);
  }
  void OnLoadStart(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,TransitionType) override {
    if(!frame->IsMain())return;
    // Admission occurs in OnBeforeBrowse/Retry. A late start must not clear failure.
    if(loading&&!recovering)SDL_SetWindowTitle(window,installed_title.c_str());
  }
  void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int code) override {
    if(!frame->IsMain()||!loading||recovering||!Trusted(frame->GetURL()))return;
    if(code>=400){Fail("GAME SERVER UNAVAILABLE",code==502||code==503||code==504);return;}
    if(!FinishLoad(Trusted(frame->GetURL()),code))return;
    // A completed navigation ends connection recovery. Leaving its timer active
    // interrupts an already connected game when the five-minute window expires.
    server_retry.Stop();
    SDL_SetWindowTitle(window,"Qrazy SDL3 + CEF — experimental");
    if(browser){
      // Retry can replace the renderer without an SDL focus event. Refresh CEF's
      // focus from the actual window so capture does not depend on Alt+Tab.
      // The game still decides when to request capture; menus stay uncaptured.
      const bool focused=!hidden_check&&SDL_GetKeyboardFocus()==window;
      browser->GetHost()->SetFocus(focused);
      browser->GetHost()->SetAudioMuted(!focused);
    }
  }
  void OnLoadError(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,ErrorCode code,const CefString&,const CefString& failed_url) override {
    if(frame->IsMain()&&loading&&!recovering&&code!=ERR_ABORTED)Fail("CONNECTION FAILED",Trusted(failed_url)&&(code==ERR_CONNECTION_TIMED_OUT||code==ERR_TIMED_OUT||code==ERR_CONNECTION_RESET||code==ERR_CONNECTION_CLOSED||code==ERR_CONNECTION_REFUSED||code==ERR_CONNECTION_FAILED||code==ERR_NAME_NOT_RESOLVED||code==ERR_INTERNET_DISCONNECTED||code==ERR_NETWORK_CHANGED));
  }
  void Fail(const char* title,bool temporary=false) {
    Release("failure");desktop_worker.Reset();FailLoad();popup_visible=false;selection.clear();
    if(temporary){server_retry.Failure(SDL_GetTicks());}else server_retry.Stop();
    if(browser){browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);}composing=false;
    SDL_SetWindowTitle(window,installed_title.c_str());std::fprintf(stderr,"QRAZY %s; Retry now or R; Alt-F4 exit\n",title);
  }
  void Retry(bool automatic=false) {
    if(!browser||closing)return;
    auto now=SDL_GetTicks();
    if(!automatic){if(hidden_check||SDL_GetKeyboardFocus()!=window)return;if(server_retry.pending)return;if(!server_retry.active)server_retry.Start(now);}
    if(!server_retry.Attempt(now))return;
    browser->StopLoad();Release("retry");desktop_worker.Reset();BeginLoad();connect_started=now;popup_visible=false;selection.clear();composing=false;
    browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);
    browser->GetMainFrame()->LoadURL(kOrigin);
  }
  void PumpRetry() {
    if(closing||!browser)return;
    auto now=SDL_GetTicks();
    if(server_retry.Expire(now)){browser->StopLoad();Fail("RETRY WINDOW EXPIRED");server_retry.expired=true;return;}
    if(loading&&server_retry.pending&&now-connect_started>=QrazyWindows::ServerConnectTimeoutMs){browser->StopLoad();Fail("CONNECTION TIMED OUT",true);}
    if(server_retry.Due(now))Retry(true);
  }
  void Reply(int id,CefRefPtr<CefDictionaryValue> result) {
    auto data=Dict();data->SetString("type","reply");data->SetInt("id",id);data->SetDictionary("value",result);Send(data);
  }
  void PumpDesktop() {
    for(auto& job:desktop_worker.Take()) {
      if(job.epoch!=desktop_worker.epoch.load())continue;
      auto value=CefParseJSON(job.response,JSON_PARSER_RFC);
      if(value&&value->GetType()==VTYPE_DICTIONARY) {
        auto response=value->GetDictionary();
        auto request=CefParseJSON(job.request,JSON_PARSER_RFC);
        auto command=request?request->GetDictionary():nullptr;
        auto data=response->GetDictionary("data");
        if(command&&response->GetBool("ok")&&data&&data->GetString("phase")=="close-required"&&
           (command->GetString("op")=="update-install")) {
          if(DesktopPolicy::AllowCloseAction(closing,recovering,loading,captured,SDL_GetKeyboardFocus()==window,hidden_check)) {
            close_action="install";closing=true;
          }else{response->SetBool("ok",false);response->SetString("error","Installation cancelled: return to the focused menu and select it again");}
        }
        Reply(job.id,response);
      }
    }
    std::deque<DialogResult> results;{std::lock_guard<std::mutex> lock(dialog_mutex);results.swap(dialog_results);}
    for(auto& selected:results) {
      if(closing||selected.epoch!=desktop_worker.epoch.load())continue;
      auto result=Dict();result->SetBool("ok",!selected.error);
      if(std::chrono::steady_clock::now()>=selected.expires){result->SetBool("ok",false);result->SetString("error","File dialog expired; select the action again");Reply(selected.id,result);continue;}
      if(selected.path.empty()){auto data=Dict();data->SetBool("cancelled",true);result->SetDictionary("data",data);if(selected.error)result->SetString("error","Native file dialog unavailable");Reply(selected.id,result);continue;}
      auto request=Dict();request->SetString("op",selected.save?"config-write":"config-read");auto args=CefListValue::Create();args->SetString(0,selected.path);if(selected.save)args->SetString(1,selected.text);request->SetList("args",args);
      auto value=CefValue::Create();value->SetDictionary(request);
      if(!desktop_worker.Submit(selected.id,selected.epoch,CefWriteJSON(value,JSON_WRITER_DEFAULT))){result->SetBool("ok",false);result->SetString("error","Desktop worker busy; try again");Reply(selected.id,result);}
    }
  }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  // Website console payloads can contain user data. Keep them out of native
  // production diagnostics; loading/rendering failures have separate handlers.
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString&,
                        const CefString&, int) override { return true; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { int w,h; SDL_GetWindowSize(window, &w,&h); rect = CefRect(0,0,std::max(w,1),std::max(h,1)); }
  void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
    browser = b;
    browser->GetHost()->SetWindowlessFrameRate(render_rate);
    if (hidden_check) browser->GetHost()->SetAudioMuted(true);
    else browser->GetHost()->SetFocus(SDL_GetKeyboardFocus() == window);
    server_retry.Start(SDL_GetTicks());server_retry.Attempt(SDL_GetTicks());connect_started=SDL_GetTicks();
    std::fprintf(stderr, "QRAZY browser-created renderer-sandbox requested=true GPU-sandbox disabled=true\n");
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override { Release("close"); browser = nullptr; closed = true; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  void OnBeforeContextMenu(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,CefRefPtr<CefContextMenuParams> params,CefRefPtr<CefMenuModel> model) override {
    model->Clear();
    if(!captured && !params->GetSelectionText().empty())model->AddItem(MENU_ID_COPY,"Copy");
  }
  void OpenWebLink(CefRefPtr<CefFrame> frame,const CefString& url,bool gesture) {
    if(!gesture || captured || closing || !frame || !frame->IsMain() || !Trusted(frame->GetURL()))return;
    CefURLParts parts;
    if(!CefParseURL(url,parts) || CefString(&parts.host).empty() ||
       !CefString(&parts.username).empty() || !CefString(&parts.password).empty())return;
    auto scheme=CefString(&parts.scheme).ToString();
    if(scheme=="https" || scheme=="http")SDL_OpenURL(url.ToString().c_str());
  }
  bool OnBeforePopup(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int,const CefString& url,const CefString&,
      cef_window_open_disposition_t,bool gesture,const CefPopupFeatures&,CefWindowInfo&,CefRefPtr<CefClient>&,CefBrowserSettings&,CefRefPtr<CefDictionaryValue>&,bool*) override {
    OpenWebLink(frame,url,gesture);return true;
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool gesture, bool) override {
    if (frame->IsMain()) {
      if(!Trusted(request->GetURL())){OpenWebLink(frame,request->GetURL(),gesture);return true;}
      Release("navigation");desktop_worker.Reset();selection.clear();popup_visible=false;BeginLoad();
      if(browser){browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);}composing=false;
      return false;
    }
    return false;
  }
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int code, const CefString&) override {
    Fail("RENDERER FAILED"); ++errors; if(hidden_check)closing=true;
    std::fprintf(stderr, "QRAZY renderer terminated code=%d\n",code);
  }
  void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override { popup_visible=show; dirty=true; }
  void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_rect=rect; dirty=true; }
  bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo&) override {
    if (hidden_check) return true;
    SDL_SystemCursor shape = SDL_SYSTEM_CURSOR_DEFAULT;
    if (type == CT_HAND) shape = SDL_SYSTEM_CURSOR_POINTER;
    else if (type == CT_IBEAM) shape = SDL_SYSTEM_CURSOR_TEXT;
    else if (type == CT_CROSS) shape = SDL_SYSTEM_CURSOR_CROSSHAIR;
    auto next = SDL_CreateSystemCursor(shape);
    if (next) { SDL_SetCursor(next); if (cursor) SDL_DestroyCursor(cursor); cursor = next; }
    return true;
  }
  void Send(CefRefPtr<CefDictionaryValue> data) {
    if (!browser || !Trusted(browser->GetMainFrame()->GetURL())) return;
    auto value = CefValue::Create(); value->SetDictionary(data);
    auto msg = CefProcessMessage::Create("qrazy-event-v1"); msg->GetArgumentList()->SetString(0, CefWriteJSON(value, JSON_WRITER_DEFAULT));
    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
  }
  void Release(const char* reason) {
    // Must be safe even if SDL's compositor constraint disappeared first.
    SDL_SetWindowRelativeMouseMode(window, false);
    if(!hidden_check&&!SDL_ScreenSaverEnabled()&&!SDL_EnableScreenSaver())std::fprintf(stderr,"QRAZY native idle inhibition release failed: %s\n",SDL_GetError());
    if (!captured) return;
    captured = false; ++generation;
    if(!hidden_check&&SDL_GetKeyboardFocus()==window)SDL_StartTextInput(window);
    auto data = Dict(); data->SetString("type", "state"); data->SetString("reason", reason); Send(data);
    std::fprintf(stderr, "QRAZY capture-released reason=%s\n", reason);
  }
  void FullscreenState() {
    auto data=Dict(); data->SetString("type","fullscreen"); data->SetBool("value",!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)); Send(data);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> msg) override {
    if (msg->GetName() != "qrazy-command-v1") return false;
    if (source != PID_RENDERER || !browser || b->GetIdentifier() != browser->GetIdentifier() || !frame->IsMain() ||
        frame->GetIdentifier() != browser->GetMainFrame()->GetIdentifier() || !Trusted(frame->GetURL()) || !Trusted(browser->GetMainFrame()->GetURL())) return true;
    auto args = msg->GetArgumentList();
    if (args->GetSize() != 3 || args->GetType(2)!=VTYPE_STRING || args->GetString(2).length()>1500000 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_INT) return true;
    const auto op = args->GetString(0).ToString(); int id = args->GetInt(1); if (id < 0) return true;
    auto result = Dict(); result->SetBool("ok", true);
    auto payload=CefParseJSON(args->GetString(2),JSON_PARSER_RFC);
    auto data_payload=payload&&payload->GetType()==VTYPE_DICTIONARY?payload->GetDictionary():nullptr;
    if(op=="assets"||op=="update-notice"||op=="update-state"||op=="update-check"||op=="update-install"||op=="changelog") {
      auto request=Dict();
      if((op=="update-install")&&!DesktopPolicy::AllowCloseAction(closing,recovering,loading,captured,SDL_GetKeyboardFocus()==window,hidden_check)) {
        result->SetBool("ok",false);result->SetString("error","Installation requires a focused menu");Reply(id,result);return true;
      }
      if(op=="assets") {
        static const std::map<std::string,size_t> operations={{"has",2},{"openRead",2},{"readChunk",1},{"closeRead",1},{"beginWrite",1},{"writeChunk",2},{"finishWrite",1},{"abortWrite",1}};
        if(!data_payload||data_payload->GetType("op")!=VTYPE_STRING||data_payload->GetType("args")!=VTYPE_LIST)return true;
        auto operation=data_payload->GetString("op").ToString();auto list=data_payload->GetList("args");auto it=operations.find(operation);
        if(it==operations.end()||list->GetSize()!=it->second)return true;
        request->SetString("op",operation);request->SetList("args",list->Copy());
      }else{request->SetString("op",op);auto list=CefListValue::Create();
        if(op=="update-install") {
          if(install_confirmation){result->SetBool("ok",false);result->SetString("error","Installation confirmation already open");Reply(id,result);return true;}
          install_confirmation=true;install_reply=id;return true;
        }
        request->SetList("args",list); }
      auto value=CefValue::Create();value->SetDictionary(request);
      if(desktop_worker.Submit(id,desktop_worker.epoch.load(),CefWriteJSON(value,JSON_WRITER_DEFAULT)))return true;
      result->SetBool("ok",false);result->SetString("error","Desktop worker unavailable or busy; normal loading can continue");Reply(id,result);return true;
    }
    if(op=="status") {
      if(!data_payload||data_payload->GetType("phase")!=VTYPE_STRING||data_payload->GetType("message")!=VTYPE_STRING)return true;
      std::string phase=data_payload->GetString("phase"),stage=data_payload->GetString("stage"),recovery=data_payload->GetString("recovery");
      if((phase!="ready"&&phase!="loading"&&phase!="warning"&&phase!="error")||
        (stage!="server"&&stage!="map"&&stage!="assets"&&stage!="graphics"&&stage!="game")||
        (recovery!="none"&&recovery!="reload")||(recovery=="reload"&&phase!="error")||data_payload->GetString("message").length()>500||data_payload->GetString("details").length()>2000||data_payload->GetString("code").length()>80)return true;
      if(phase=="error")Release("game-error");
    } else if(op=="refresh-game") {
      bool ok=browser&&!closing&&!loading&&!hidden_check&&SDL_GetKeyboardFocus()==window;result->SetBool("ok",ok);
      if(ok){Release("refresh");desktop_worker.Reset();BeginLoad();connect_started=SDL_GetTicks();server_retry.Start(connect_started);server_retry.Attempt(connect_started);popup_visible=false;selection.clear();composing=false;browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);browser->ReloadIgnoreCache();}
    } else if(op=="retry") { Retry(); }
    else if(op=="clipboard-write") {
      auto text=data_payload?data_payload->GetString("text").ToString():std::string();
      bool allowed=!hidden_check&&!captured&&!closing&&!recovering&&!loading&&SDL_GetKeyboardFocus()==window&&text.size()<=1048576&&text.find('\0')==std::string::npos;
      result->SetBool("ok",allowed&&SDL_SetClipboardText(text.c_str()));
      if(!result->GetBool("ok"))result->SetString("error","Clipboard unavailable; focus the menu and try again");
    } else if(op=="diagnostics") {
      if(!SDL_GL_MakeCurrent(window,glcontext)){result->SetBool("ok",false);result->SetString("error","Graphics report unavailable");Reply(id,result);return true;}
      auto d=Dict();d->SetString("client",installed_title);d->SetInt("sdlVersion",SDL_GetVersion());
      d->SetString("cefVersion",CEF_VERSION);
      d->SetString("videoDriver",SDL_GetCurrentVideoDriver());d->SetString("glRenderer",reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
      d->SetString("glVersion",reinterpret_cast<const char*>(glGetString(GL_VERSION)));d->SetInt("cefTargetFps",render_rate);
      int interval=0;d->SetInt("swapInterval",SDL_GL_GetSwapInterval(&interval)?interval:-1);d->SetDouble("displayScale",SDL_GetWindowDisplayScale(window));
      int w,h;SDL_GetWindowSizeInPixels(window,&w,&h);d->SetInt("pixelWidth",w);d->SetInt("pixelHeight",h);
      d->SetBool("sandboxRequested",true);d->SetBool("gpuSandboxDisabled",true);d->SetBool("gpuSandboxFailuresFatal",false);d->SetBool("mesaDiskCachesDisabled",true);
      d->SetString("verification","Sandbox configuration, not a live per-thread audit. Presentation, input latency and physical GPU performance unverified.");result->SetDictionary("data",d);
    } else if(op=="config-import"||op=="config-export") {
      bool allowed=!hidden_check&&(!captured||op=="config-export")&&!closing&&!recovering&&!loading&&SDL_GetKeyboardFocus()==window&&!DemoDownload::active&&!dialog_active.exchange(true);
      if(!allowed){result->SetBool("ok",false);result->SetString("error","File dialogs require focused menus and no active dialog");Reply(id,result);return true;}
      auto selected=new DialogResult{id,desktop_worker.epoch.load(),op=="config-export",false,{},{}};
      static const SDL_DialogFileFilter filter={"Qrazy configuration","cfg"};
      if(selected->save){
        std::string name=data_payload?data_payload->GetString("name").ToString():"";selected->text=data_payload?data_payload->GetString("text").ToString():"";
        bool valid=name.size()>=5&&name.size()<=100&&name.substr(name.size()-4)==".cfg"&&name.find_first_of("/\\\r\n")==std::string::npos&&selected->text.size()<=1048576&&selected->text.find('\0')==std::string::npos;
        if(!valid){delete selected;dialog_active=false;result->SetBool("ok",false);result->SetString("error","Expected a cfg filename and at most 1 MiB of text");Reply(id,result);return true;}
        selected->path=name;
        Release("export-save");SDL_ShowSaveFileDialog(FileChosen,selected,window,&filter,1,selected->path.c_str());
      }else SDL_ShowOpenFileDialog(FileChosen,selected,window,&filter,1,nullptr,false);
      return true;
    } else if (op == "release") { Release("release"); return true; }
    else if (op == "clock") result->SetDouble("time", SDL_GetTicksNS()/1e6);
    else if (op == "capture") {
      bool ok = !hidden_check && !closing && !recovering && !loading && !dialog_active && SDL_GetKeyboardFocus() == window && SDL_GetMouseFocus() == window && SDL_SetWindowRelativeMouseMode(window, true);
      captured = ok;
      if(ok){SDL_StopTextInput(window);if(!SDL_DisableScreenSaver())std::fprintf(stderr,"QRAZY native idle inhibition request failed: %s\n",SDL_GetError());}
      result->SetBool("ok", ok); result->SetInt("generation", ++generation);
      std::fprintf(stderr, "QRAZY capture-request accepted=%d%s\n", ok, ok ? " (compositor acknowledgement not exposed by SDL)" : SDL_GetError());
    } else if (op == "quit") { Release("quit"); closing = true; browser->GetHost()->CloseBrowser(true); }
    else if (op == "fullscreen-toggle") {
      bool enabled = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
      result->SetBool("ok", !hidden_check && SDL_SetWindowFullscreen(window, enabled));
      result->SetBool("fullscreen", !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    } else if (op == "fullscreen-state") result->SetBool("fullscreen", !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    else return true;
    Reply(id,result);
    return true;
  }
  void Motion(const SDL_MouseMotionEvent& e) {
    if (!captured || SDL_GetKeyboardFocus() != window) return;
    auto sample = Dict(); sample->SetInt("generation", generation); sample->SetDouble("time", e.timestamp/1e6); sample->SetDouble("dx", e.xrel); sample->SetDouble("dy", e.yrel);
    auto list = CefListValue::Create(); list->SetDictionary(0, sample);
    auto data = Dict(); data->SetString("type", "move"); data->SetList("samples", list); Send(data);
  }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {
    ++errors; std::fprintf(stderr, "QRAZY required accelerated rendering missing; CPU paint rejected\n"); closing = true;
  }
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const CefAcceleratedPaintInfo& info) override {
    ++frames;
    const auto begin = std::chrono::steady_clock::now();
    if ((type==PET_VIEW ? view : popup).Copy(info)) { ++copies; dirty=true; }
    else { ++errors; Release("graphics-error"); closing = true; }
    copy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
  }
  bool Draw() {
    if (!dirty) return false;
    dirty=false;
    if (!view.Draw(nullptr,true)) { ++errors; return false; }
    if (popup_visible) popup.Draw(&popup_rect,true);
    return true;
  }
 private:
  IMPLEMENT_REFCOUNTING(Client);
};

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
// Hardware positions: SDL USB scancode -> Linux evdev code. CEF expects evdev+8 on Linux.
int Evdev(SDL_Scancode code) {
  static const int letters[] = {30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
  if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return letters[code-SDL_SCANCODE_A];
  if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return 2 + code-SDL_SCANCODE_1;
  if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F10) return 59 + code-SDL_SCANCODE_F1;
  switch (code) {
    case SDL_SCANCODE_0:return 11; case SDL_SCANCODE_RETURN:return 28; case SDL_SCANCODE_ESCAPE:return 1;
    case SDL_SCANCODE_BACKSPACE:return 14; case SDL_SCANCODE_TAB:return 15; case SDL_SCANCODE_SPACE:return 57;
    case SDL_SCANCODE_MINUS:return 12; case SDL_SCANCODE_EQUALS:return 13; case SDL_SCANCODE_LEFTBRACKET:return 26;
    case SDL_SCANCODE_RIGHTBRACKET:return 27; case SDL_SCANCODE_BACKSLASH:return 43; case SDL_SCANCODE_SEMICOLON:return 39;
    case SDL_SCANCODE_APOSTROPHE:return 40; case SDL_SCANCODE_GRAVE:return 41; case SDL_SCANCODE_COMMA:return 51;
    case SDL_SCANCODE_PERIOD:return 52; case SDL_SCANCODE_SLASH:return 53; case SDL_SCANCODE_CAPSLOCK:return 58;
    case SDL_SCANCODE_F11:return 87; case SDL_SCANCODE_F12:return 88; case SDL_SCANCODE_PRINTSCREEN:return 99;
    case SDL_SCANCODE_SCROLLLOCK:return 70; case SDL_SCANCODE_PAUSE:return 119; case SDL_SCANCODE_INSERT:return 110;
    case SDL_SCANCODE_HOME:return 102; case SDL_SCANCODE_PAGEUP:return 104; case SDL_SCANCODE_DELETE:return 111;
    case SDL_SCANCODE_END:return 107; case SDL_SCANCODE_PAGEDOWN:return 109; case SDL_SCANCODE_RIGHT:return 106;
    case SDL_SCANCODE_LEFT:return 105; case SDL_SCANCODE_DOWN:return 108; case SDL_SCANCODE_UP:return 103;
    case SDL_SCANCODE_KP_0:return 82;case SDL_SCANCODE_KP_1:return 79;case SDL_SCANCODE_KP_2:return 80;case SDL_SCANCODE_KP_3:return 81;
    case SDL_SCANCODE_KP_4:return 75;case SDL_SCANCODE_KP_5:return 76;case SDL_SCANCODE_KP_6:return 77;case SDL_SCANCODE_KP_7:return 71;case SDL_SCANCODE_KP_8:return 72;case SDL_SCANCODE_KP_9:return 73;
    case SDL_SCANCODE_KP_ENTER:return 96;case SDL_SCANCODE_KP_PLUS:return 78;case SDL_SCANCODE_KP_MINUS:return 74;case SDL_SCANCODE_KP_MULTIPLY:return 55;case SDL_SCANCODE_KP_DIVIDE:return 98;case SDL_SCANCODE_KP_PERIOD:return 83;case SDL_SCANCODE_NONUSBACKSLASH:return 86;
    case SDL_SCANCODE_LCTRL:return 29; case SDL_SCANCODE_RCTRL:return 97; case SDL_SCANCODE_LSHIFT:return 42;
    case SDL_SCANCODE_RSHIFT:return 54; case SDL_SCANCODE_LALT:return 56; case SDL_SCANCODE_RALT:return 100;
    case SDL_SCANCODE_LGUI:return 125; case SDL_SCANCODE_RGUI:return 126;
    default:return 0;
  }
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
  if(down&&!event.repeat&&(event.mod&SDL_KMOD_CTRL)&&!(event.mod&(SDL_KMOD_ALT|SDL_KMOD_GUI))&&!captured&&SDL_GetKeyboardFocus()==window) {
    if(event.scancode==SDL_SCANCODE_V){char* text=SDL_GetClipboardText();if(text){if(strlen(text)<=1048576)client->browser->GetHost()->ImeCommitText(text,CefRange(UINT32_MAX,UINT32_MAX),0);SDL_free(text);}return;}
    if(event.scancode==SDL_SCANCODE_C||event.scancode==SDL_SCANCODE_X){if(!client->selection.empty())SDL_SetClipboardText(client->selection.c_str());if(event.scancode==SDL_SCANCODE_X)client->browser->GetMainFrame()->Cut();return;}
  }
  // Native capture has no Chromium pointer-lock Escape interception. Deliver
  // both edges to the game's existing menu/chat/console handlers; their release
  // operation frees SDL capture and menu resume happens on keyup.
  // Leave compositor shortcuts ungrabbed. No keyboard grab/shortcut-inhibit request.
  CefKeyEvent key; key.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
  key.windows_key_code = VirtualKey(event.scancode); key.native_key_code = Evdev(event.scancode) + 8;
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
} // namespace

// Only the tiny process entry point may remain on Chromium's fork stack.
// Keep application/browser work in a separate stack-protected, non-inlined frame.
NOINLINE int RunBrowser(int argc, char** argv, const CefMainArgs& args, CefRefPtr<App> app,
                       const std::filesystem::path& executable) {
  // Mesa 26.2 creates cache queues even with MESA_SHADER_CACHE_DISABLE=true.
  // Their pthread_setschedparam calls target other threads and violate CEF's
  // GPU seccomp policy. Disable all disk-cache backends, not the sandbox.
  setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
  setenv("MESA_DISK_CACHE_MULTI_FILE", "false", 1);
  setenv("MESA_DISK_CACHE_DATABASE", "false", 1);
  setenv("MESA_DISK_CACHE_SINGLE_FILE", "false", 1);
  auto cache_root=getenv("QRAZY_ROOT")?std::filesystem::path(getenv("QRAZY_ROOT")):executable.parent_path();
  setenv("XDG_CACHE_HOME", (cache_root / "profile-sdlcef-linux" / "cache-graphics").c_str(), 1);
  if(argc==2&&std::string(argv[1])=="--desktop-worker-check") {
    auto fixture=executable/("worker-fixture-"+std::to_string(getpid()));
    if(std::filesystem::exists(fixture)||!std::filesystem::create_directory(fixture))return 25;
    {ProfileLock first,second;if(!first.Acquire(fixture)||second.Acquire(fixture))return 27;}
    {ProfileLock again;if(!again.Acquire(fixture))return 28;}
    bool ok=false;
    {
      DesktopWorker worker;
      if(worker.Start(executable,fixture)) {
        auto exchange=[&](int id,const std::string& request){
          if(!worker.Submit(id,worker.epoch.load(),request))return CefRefPtr<CefDictionaryValue>();
          auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
          while(std::chrono::steady_clock::now()<deadline){for(auto& job:worker.Take())if(job.id==id){auto value=CefParseJSON(job.response,JSON_PARSER_RFC);if(value&&value->GetType()==VTYPE_DICTIONARY)return value->GetDictionary();}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
          return CefRefPtr<CefDictionaryValue>();
        };
        auto reply=exchange(1,"{\"op\":\"beginWrite\",\"args\":[{\"kind\":\"map\",\"key\":\"fixture\",\"size\":5}]}");
        if(reply&&reply->GetBool("ok")){auto token=reply->GetDictionary("data")->GetString("token").ToString();
          reply=exchange(2,"{\"op\":\"writeChunk\",\"args\":[\""+token+"\",\"aGVsbG8=\"]}");
          if(reply&&reply->GetBool("ok")){worker.Reset();reply=exchange(3,"{\"op\":\"has\",\"args\":[\"map\",\"fixture\"]}");ok=reply&&reply->GetBool("ok")&&!reply->GetBool("data");}
        }
      }
    }
    std::filesystem::remove_all(fixture);
    std::fprintf(stderr,"DESKTOP native worker check=%s; no SDL/browser initialization or live profile\n",ok?"passed":"failed");return ok?0:26;
  }
  if (argc==2 && std::string(argv[1])=="--policy-check") {
    for (auto hz : {59.94, 60.0, 74.99, 75.0, 119.88, 144.0, 165.0, 239.76, 360.0, 500.0}) {
      int rate = RateForRefresh(hz);
      if (rate < hz || rate >= hz+1) return 23;
    }
    if (RateForRefresh(0) || RateForRefresh(-1) || RateForRefresh(std::numeric_limits<double>::infinity()) ||
        RateForRefresh(std::numeric_limits<double>::quiet_NaN())) return 24;
    const char* good[]={kOrigin,"https://qrazy-game.onrender.com/path?query=1","https://qrazy-game.onrender.com:443/"};
    const char* bad[]={"http://qrazy-game.onrender.com/","https://qrazy-game.onrender.com.evil.example/","https://evil.example/?q=https://qrazy-game.onrender.com/","https://user@qrazy-game.onrender.com/","https://qrazy-game.onrender.com:444/","blob:https://qrazy-game.onrender.com/test","http://localhost:5173/","file:///tmp/test.html","https://qrazy-game.onrender.com./"};
    for(auto url:good) if(!Trusted(url)) return 20;
    for(auto url:bad) if(Trusted(url)) return 21;
    std::fprintf(stderr,"POLICY trusted-origin cases passed (no SDL or browser initialization)\n"); return 0;
  }
  for (int i=1;i<argc;++i) {
    if (std::string(argv[i]) == "--background-check") hidden_check = true;
    else { std::fprintf(stderr, "QRAZY unknown browser-process argument rejected: %s\n", argv[i]); return 3; }
  }
  auto profile_root=executable;
  const char* version=getenv("QRAZY_INSTALLED_VERSION");
  if(!version||!getenv("QRAZY_ROOT")){std::fprintf(stderr,"Launch using the distribution launch.sh so signatures and recovery are verified first\n");return 4;}
  installed_title="Qrazy v"+std::string(version);
  if(const char* configured=getenv("QRAZY_ROOT")) {
    std::error_code ec;auto candidate=std::filesystem::canonical(configured,ec);
    if(!ec&&std::filesystem::is_directory(candidate))profile_root=candidate;
    else { std::fprintf(stderr,"QRAZY invalid isolated root\n");return 4; }
  }
  prototype_profile=profile_root/"profile-sdlcef-linux";
  std::error_code profile_error;std::filesystem::create_directories(prototype_profile,profile_error);
  if(profile_error||std::filesystem::is_symlink(prototype_profile)){std::fprintf(stderr,"QRAZY unsafe/unavailable profile directory\n");return 4;}
  chmod(prototype_profile.c_str(),0700);signal(SIGPIPE,SIG_IGN);
  ProfileLock profile_lock;
  if(!profile_lock.Acquire(prototype_profile)){std::fprintf(stderr,"QRAZY profile already in use or unavailable; close its existing client before relaunching\n");return 4;}
  if(!desktop_worker.Start(executable,prototype_profile))std::fprintf(stderr,"QRAZY desktop worker unavailable; browser loading fallback retained\n");
  // Configure flags inside CefApp rather than accepting arbitrary runtime security flags.
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER,"1");
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland"); SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0"); SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1");
  SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
  SDL_SetHint(SDL_HINT_APP_ID, "qrazy-sdl-cef");
  if (!SDL_Init(SDL_INIT_VIDEO)) return 4;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  auto flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (hidden_check ? SDL_WINDOW_HIDDEN : 0);
  window = SDL_CreateWindow(installed_title.c_str(), 1280, 800, flags);
  if (!window || !(glcontext = SDL_GL_CreateContext(window))) { std::fprintf(stderr, "QRAZY SDL: %s\n", SDL_GetError()); return 5; }
  if(!hidden_check)saved_window.Restore();
  render_rate = DisplayRenderRate();
  if (!render_rate) { std::fprintf(stderr, "QRAZY display refresh unavailable; refusing arbitrary FPS fallback\n"); return 5; }
  if (!hidden_check) {
    SDL_StartTextInput(window);
    int interval = 0;
    if (!SDL_GL_SetSwapInterval(1) || !SDL_GL_GetSwapInterval(&interval) || interval != 1) {
      std::fprintf(stderr, "QRAZY VSync swap interval 1 unavailable: %s\n", SDL_GetError()); return 5;
    }
    std::fprintf(stderr, "QRAZY SDL swap interval=%d (presentation timing unmeasured)\n", interval);
  }
  std::fprintf(stderr, "QRAZY SDL=%s version=%d hidden=%d renderer=%s\n", SDL_GetCurrentVideoDriver(), SDL_GetVersion(), hidden_check, glGetString(GL_RENDERER));
  const auto renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  amd_mesa_renderer = renderer && std::strstr(renderer, "radeonsi");
  std::fprintf(stderr, "QRAZY graphics-candidate=gpu-sandbox-workaround-seq7 CEF-angle=%s GPU-sandbox=disabled renderer-sandbox=enabled software-rasterizer=disabled AMD-video-acceleration=%s AMD-ANGLE-async-cleanup=%s\n", amd_mesa_renderer ? "vulkan" : "gl-egl", amd_mesa_renderer ? "disabled" : "default", amd_mesa_renderer ? "disabled" : "default");
  CefSettings settings; settings.windowless_rendering_enabled = true; settings.no_sandbox = false; settings.command_line_args_disabled = true;
  CefString(&settings.root_cache_path) = prototype_profile.string();
  CefString(&settings.cache_path) = prototype_profile.string();
  CefString(&settings.log_file) = (prototype_profile/"chromium.log").string();
  // Keep session cookies transient; game/server owns remember-me policy.
  CefString(&settings.resources_dir_path) = executable.string(); CefString(&settings.locales_dir_path) = (executable/"locales").string();
  if (!CefInitialize(args, settings, app, nullptr)) return 6;
  int result = 0;
  {
    CefRefPtr<Client> client = new Client;
    CefWindowInfo info; info.SetAsWindowless(0); info.shared_texture_enabled = true;
    CefBrowserSettings bs; bs.windowless_frame_rate = render_rate; bs.background_color = CefColorSetARGB(255,0,0,0);
    if (!CefBrowserHost::CreateBrowser(info, client, kOrigin, bs, nullptr, nullptr)) return 7;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    auto inspect_at = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool inspected=false;
    bool close_sent = false;
    auto refresh_at = std::chrono::steady_clock::now();
    while (!closed) {
      CefDoMessageLoopWork();client->PumpDesktop();client->PumpRetry();
      // Also catch refresh changes that do not move or resize the window.
      if (std::chrono::steady_clock::now() >= refresh_at) {
        int rate = DisplayRenderRate();
        if (rate && rate != render_rate) {
          render_rate = rate;
          if (client->browser) client->browser->GetHost()->SetWindowlessFrameRate(rate);
        } else if (!rate) {
          std::fprintf(stderr, "QRAZY display refresh unavailable; retaining last known target=%d\n", render_rate);
        }
        refresh_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      }
      if (hidden_check && !inspected && std::chrono::steady_clock::now()>inspect_at) {
        auto data=Dict();data->SetString("type","inspect");client->Send(data);inspected=true;
      }
      if (hidden_check && std::chrono::steady_clock::now() > end) closing = true;
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (hidden_check) continue; // Never act on input, focus or cursor during background checks.
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) closing = true;
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MINIMIZED) {
          client->ConfirmInstall(false);
          if (client->browser) { client->browser->GetHost()->SetFocus(false); client->browser->GetHost()->SetAudioMuted(true); }
          client->Release("focus");client->selection.clear();if(client->browser)client->browser->GetHost()->ImeCancelComposition();client->composing=false;
          auto data=Dict(); data->SetString("type","focus"); data->SetBool("focused",false); client->Send(data);
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
          if(!captured)SDL_StartTextInput(window);
          if (client->browser) { client->browser->GetHost()->SetFocus(true); client->browser->GetHost()->SetAudioMuted(client->recovering||client->loading); }
          auto data=Dict(); data->SetString("type","focus"); data->SetBool("focused",true); client->Send(data);
        }
        if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
          if (client->browser) client->browser->GetHost()->WasResized();
        }
        if(event.type==SDL_EVENT_WINDOW_MOVED||event.type==SDL_EVENT_WINDOW_RESIZED||event.type==SDL_EVENT_WINDOW_MAXIMIZED||event.type==SDL_EVENT_WINDOW_RESTORED||event.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||event.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)saved_window.Observe();
        if(event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED||event.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED){if(client->browser){client->browser->GetHost()->NotifyScreenInfoChanged();client->browser->GetHost()->WasResized();}}
        if(event.type==SDL_EVENT_TEXT_EDITING&&client->browser&&!captured&&!client->recovering&&!client->loading) {
          CefString text(event.edit.text);auto utf16=text.ToString16();
          auto index=[&](int points){size_t n=0;for(int i=0;i<std::max(0,points)&&n<utf16.size();++i){char16_t c=utf16[n++];if(c>=0xd800&&c<=0xdbff&&n<utf16.size()&&utf16[n]>=0xdc00&&utf16[n]<=0xdfff)++n;}return static_cast<uint32_t>(n);};
          if(utf16.empty()){client->browser->GetHost()->ImeCancelComposition();client->composing=false;}
          else {client->composing=true;CefCompositionUnderline line;line.range=CefRange(0,utf16.size());line.color=CefColorSetARGB(255,255,255,255);line.thick=false;
            client->browser->GetHost()->ImeSetComposition(text,{line},CefRange(UINT32_MAX,UINT32_MAX),CefRange(index(event.edit.start),index(event.edit.start+std::max(0,event.edit.length))));}
        }
        if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) client->FullscreenState();
        if(client->install_confirmation){
          if(event.type==SDL_EVENT_KEY_DOWN&&!event.key.repeat){
            if(event.key.scancode==SDL_SCANCODE_ESCAPE)client->ConfirmInstall(false);
            else if(event.key.scancode==SDL_SCANCODE_RETURN)client->ConfirmInstall(true);
          }
          if(event.type==SDL_EVENT_MOUSE_BUTTON_UP&&event.button.button==SDL_BUTTON_LEFT&&SDL_GetKeyboardFocus()==window){
            int w,h;SDL_GetWindowSize(window,&w,&h);
            if(event.button.y>=h/2+85&&event.button.y<=h/2+125){
              if(event.button.x>=w/2-250&&event.button.x<=w/2-10)client->ConfirmInstall(false);
              else if(event.button.x>=w/2+10&&event.button.x<=w/2+250)client->ConfirmInstall(true);
            }
          }
          continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) Keyboard(client.get(),event.key);
        if((client->recovering||client->loading)&&event.type==SDL_EVENT_MOUSE_BUTTON_UP&&event.button.button==SDL_BUTTON_LEFT&&!server_retry.pending&&SDL_GetKeyboardFocus()==window){
          int w,h;SDL_GetWindowSize(window,&w,&h);
          if(event.button.x>=w/2-130&&event.button.x<=w/2+130&&event.button.y>=h/2+85&&event.button.y<=h/2+125)client->Retry();
        }
        if(!DesktopPolicy::AllowInput(closing,client->recovering,client->loading))continue;
        if (event.type == SDL_EVENT_TEXT_INPUT && client->browser && !client->recovering && !captured) {
          if(client->composing)client->browser->GetHost()->ImeCommitText(event.text.text,CefRange(UINT32_MAX,UINT32_MAX),0);
          else {
            // Normal typing must respect the game's keydown preventDefault
            // (notably the console toggle); IME commits are only for preedit.
            CefString text(event.text.text);
            for(char16_t ch:text.ToString16()){CefKeyEvent key;key.type=KEYEVENT_CHAR;key.windows_key_code=ch;key.character=key.unmodified_character=ch;key.modifiers=Modifiers(SDL_GetModState());client->browser->GetHost()->SendKeyEvent(key);}
          }
          client->composing=false;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && client->browser) {
          if (captured) client->Motion(event.motion);
          else { CefMouseEvent mouse; mouse.x=static_cast<int>(event.motion.x); mouse.y=static_cast<int>(event.motion.y); mouse.modifiers=MouseModifiers(); client->browser->GetHost()->SendMouseMoveEvent(mouse,false); }
        }
        if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) && client->browser) {
          CefMouseEvent mouse; mouse.x=static_cast<int>(event.button.x); mouse.y=static_cast<int>(event.button.y); mouse.modifiers=MouseModifiers();
          if (event.button.button <= SDL_BUTTON_RIGHT) {
            auto button=event.button.button==SDL_BUTTON_LEFT ? MBT_LEFT : event.button.button==SDL_BUTTON_RIGHT ? MBT_RIGHT : MBT_MIDDLE;
            client->browser->GetHost()->SendMouseClickEvent(mouse,button,event.type==SDL_EVENT_MOUSE_BUTTON_UP,std::clamp<int>(event.button.clicks,1,2));
          } else if (event.button.button==SDL_BUTTON_X1 || event.button.button==SDL_BUTTON_X2) {
            auto data=Dict(); data->SetString("type","button"); data->SetInt("button",event.button.button==SDL_BUTTON_X1?3:4);
            data->SetBool("down",event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
            Uint32 bits=SDL_GetMouseState(nullptr,nullptr);
            int dom=(bits&SDL_BUTTON_LMASK?1:0)|(bits&SDL_BUTTON_RMASK?2:0)|(bits&SDL_BUTTON_MMASK?4:0)|(bits&SDL_BUTTON_X1MASK?8:0)|(bits&SDL_BUTTON_X2MASK?16:0);
            data->SetInt("buttons",dom);client->Send(data);
          }
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && client->browser) {
          CefMouseEvent mouse; mouse.x=static_cast<int>(event.wheel.mouse_x); mouse.y=static_cast<int>(event.wheel.mouse_y); mouse.modifiers=MouseModifiers();
          float sign=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
          client->browser->GetHost()->SendMouseWheelEvent(mouse,static_cast<int>(client->wheel_x+=120*event.wheel.x*sign),static_cast<int>(client->wheel_y+=120*event.wheel.y*sign));
          client->wheel_x-=static_cast<int>(client->wheel_x);client->wheel_y-=static_cast<int>(client->wheel_y);
        }
      }
      if (closing && client->browser && !close_sent) { client->Release("close"); client->browser->GetHost()->CloseBrowser(true); close_sent=true; }
      bool native_card=!hidden_check&&(client->install_confirmation||client->recovering||client->loading);
      if(client->install_confirmation)RecoveryCard("INSTALL VERIFIED UPDATE","QRAZY WILL CLOSE. REOPEN MANUALLY",true);
      else if(native_card){
        char status[160];auto now=SDL_GetTicks();
        if(server_retry.expired)std::snprintf(status,sizeof(status),"RETRIES STOPPED AFTER 5 MINUTES");
        else if(server_retry.active&&!server_retry.pending)std::snprintf(status,sizeof(status),"ATTEMPT %u - RETRY IN %u SECONDS",server_retry.attempt,server_retry.Countdown(now));
        else std::snprintf(status,sizeof(status),"ATTEMPT %u - PLEASE WAIT",server_retry.attempt);
        RecoveryCard(client->recovering?"GAME CONNECTION FAILED":"CONNECTING TO GAME",status);
      }
      if (native_card || client->Draw()) {
        ++presentations;
        if (!hidden_check) {
          auto begin=std::chrono::steady_clock::now();
          if (!SDL_GL_SwapWindow(window)) {
            ++errors; client->Release("presentation-error"); closing=true;
            std::fprintf(stderr, "QRAZY SDL swap failed: %s\n", SDL_GetError());
          }
          swap_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        }
      }
      SDL_Delay(1);
    }
    desktop_worker.Reset();if(!hidden_check){saved_window.Observe();saved_window.Save();}
    CefRefPtr<CookieFlushed> flushed=new CookieFlushed;
    bool requested=CefCookieManager::GetGlobalManager(nullptr)->FlushStore(flushed);
    auto flush_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(requested&&!flushed->done&&std::chrono::steady_clock::now()<flush_deadline){CefDoMessageLoopWork();SDL_Delay(1);}
    std::fprintf(stderr,"QRAZY cookie flush completed=%d; Chromium shutdown follows\n",flushed->done);
    if (client->cursor) { SDL_SetCursor(SDL_GetDefaultCursor()); SDL_DestroyCursor(client->cursor); client->cursor=nullptr; }
    std::fprintf(stderr, "QRAZY result frames=%d copies=%d draws=%d errors=%d copy-total-ms=%.3f swap-total-ms=%.3f hidden=%d\n",frames,copies,presentations,errors,copy_ms,swap_ms,hidden_check);
    result = errors || (hidden_check && !frames) ||
      (!close_action.empty()&&!DesktopPolicy::ShutdownReady(requested,flushed->done)) ? 8 : 0;
    if(!close_action.empty()&&!DesktopPolicy::ShutdownReady(requested,flushed->done))
      std::fprintf(stderr,"QRAZY offline action refused: cookie flush failed or timed out; runtime unchanged\n");
  }
  CefShutdown(); app=nullptr; SDL_GL_DestroyContext(glcontext); SDL_DestroyWindow(window); SDL_Quit();
  desktop_worker.Stop();
  if(result==0&&!close_action.empty())return 42; // Launcher installs only after this orderly, flushed shutdown.
  return result;
}

// CEF resets the stack guard after zygote fork. Its documented Linux contract
// requires this annotation on every frame leading to CefExecuteProcess. Do not
// disable guard changes, global stack protection, or the Chromium sandbox.
NO_STACK_PROTECTOR int main(int argc, char** argv) {
  const auto executable = std::filesystem::canonical("/proc/self/exe").parent_path();
  std::ifstream bridge(executable / "bridge.js");
  std::ostringstream contents; contents << bridge.rdbuf(); bridge_source = contents.str();
  if (bridge_source.empty()) { std::fprintf(stderr, "QRAZY missing bridge.js\n"); return 2; }
  CefRefPtr<App> app = new App;
  CefMainArgs args(argc, argv);
  const int sub = CefExecuteProcess(args, app, nullptr);
  if (sub >= 0) return sub;
  return RunBrowser(argc, argv, args, app, executable);
}
