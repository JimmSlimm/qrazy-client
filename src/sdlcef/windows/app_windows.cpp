// Windows host implementation in progress. No game/input/physics implementation.
#include <cctype>
#include <windows.h>
#include <intrin.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include "include/cef_version.h"
#include <dxgi1_2.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdio>
#include <functional>
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_cookie.h"
#include "include/cef_request_context.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_download_handler.h"
#include "../demo_download.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_command_handler.h"
#include "include/cef_id_mappers.h"
#include "include/cef_sandbox_win.h"
#include "include/wrapper/cef_helpers.h"
#include "borrowed_texture.h"
#include "../desktop_policy.h"
#include "asset_worker.h"
#include "updater_worker.h"
#include "handoff_startup.h"
#include "server_retry.h"
#include "client_destination.h"

namespace {
constexpr auto& kOrigin=QrazyDestination::Origin;
constexpr auto& kProfile=QrazyDestination::Profile;
SDL_Window* window=nullptr;
std::string client_title="Qrazy";
SDL_Renderer* renderer=nullptr;
HWND hwnd=nullptr;
bool closing=false,closed=false,captured=false;
// Windows can clear cursor confinement independently of SDL's relative-mode
// flag (for example during native move/resize). Check the actual OS rectangle.
bool ConfineCapturedCursor() {
  if(!hwnd||GetForegroundWindow()!=hwnd||SDL_GetKeyboardFocus()!=window||IsIconic(hwnd))return false;
  RECT bounds{},clip{};POINT origin{};
  if(!GetClientRect(hwnd,&bounds)||!ClientToScreen(hwnd,&origin))return false;
  OffsetRect(&bounds,origin.x,origin.y);
  if(bounds.right<=bounds.left||bounds.bottom<=bounds.top)return false;
  if(GetClipCursor(&clip)&&clip.right>clip.left&&clip.bottom>clip.top&&
     clip.left>=bounds.left&&clip.top>=bounds.top&&clip.right<=bounds.right&&clip.bottom<=bounds.bottom)return true;
  return ClipCursor(&bounds)!=FALSE;
}
// The client's frame loop must not run below normal priority. Something outside our code was found lowering it to
// BelowNormal with priority boost off, so set it at startup and re-check periodically.
void EnsureNormalPriority() {
  const HANDLE self=GetCurrentProcess();
  const DWORD cls=GetPriorityClass(self);
  if(cls==IDLE_PRIORITY_CLASS||cls==BELOW_NORMAL_PRIORITY_CLASS)SetPriorityClass(self,NORMAL_PRIORITY_CLASS);
  BOOL disabled=FALSE;
  if(GetProcessPriorityBoost(self,&disabled)&&disabled)SetProcessPriorityBoost(self,FALSE);
}
int generation=0,render_rate=0;
#include "render_preferences.h"
RenderPreferences render_preferences;
// Opt-in windowed mode: CEF owns a child HWND that presents and receives input itself. The SDL window then only hosts it and the native status cards.
bool native_window=false,startup_vsync=true,pointer_locked=false; /* pointer_locked: the page reports Chromium pointer lock (native mode only) */
// In native mode the child HWND holds keyboard focus, so SDL reports the top-level window as unfocused; use the OS foreground instead.
bool Focused(){return native_window?GetForegroundWindow()==hwnd:SDL_GetKeyboardFocus()==window;}
std::string bridge_source;
QrazyWindows::AssetWorker asset_worker;
Production::Worker updater_worker;bool install_requested=false;Uint64 update_gesture=0;
#include "bridge_binding.h"
#include "window_state.h"
// Native mode: clicks land in the CEF child, which SDL never sees, so the update-button gesture is taken from the real OS mouse message.
// Chrome style shows a "To show your cursor, press Esc" bubble (a Chromium popup widget) when pointer lock starts and CEF has no setting for it.
// Right after a lock begins, hide visible Chromium popup widgets of this thread; the SDL window and the browser child (not WS_POPUP) are left alone.
BOOL CALLBACK HideLockBubbleWindow(HWND candidate,LPARAM){
  wchar_t name[64]={};
  if(candidate!=hwnd&&IsWindowVisible(candidate)&&(GetWindowLongPtrW(candidate,GWL_STYLE)&WS_POPUP)&&GetClassNameW(candidate,name,64)&&std::wstring(name).rfind(L"Chrome_WidgetWin",0)==0){
    /* An empty window region keeps the popup alive for Chromium (hiding it made it flicker as Chromium re-showed it) but nothing is drawn. */
    HRGN probe=CreateRectRgn(0,0,0,0);const int kind=GetWindowRgn(candidate,probe);DeleteObject(probe);
    if(kind!=NULLREGION)SetWindowRgn(candidate,CreateRectRgn(0,0,0,0),FALSE);
  }
  return TRUE;
}
bool lock_was_held=false;Uint64 lock_started_ns=0;
HHOOK gesture_hook=nullptr;
LRESULT CALLBACK GestureHook(int code,WPARAM message,LPARAM data){if(code>=0&&(message==WM_LBUTTONDOWN||message==WM_NCLBUTTONDOWN)&&GetForegroundWindow()==hwnd)update_gesture=SDL_GetTicksNS();return CallNextHookEx(nullptr,code,message,data);}
struct DialogResult {int id;unsigned epoch;bool save,error=false;std::string path,text;std::chrono::steady_clock::time_point expires=std::chrono::steady_clock::now()+std::chrono::minutes(5);};
std::mutex dialog_mutex;std::deque<DialogResult> dialog_results;std::atomic<bool> dialog_active{false};
void SDLCALL FileChosen(void* userdata,const char* const* files,int) {
  std::unique_ptr<DialogResult> result(static_cast<DialogResult*>(userdata));result->error=!files;
  if(files&&files[0])result->path=files[0];std::lock_guard<std::mutex> lock(dialog_mutex);dialog_results.push_back(std::move(*result));dialog_active=false;
}

struct ProfileLock {
  HANDLE file=INVALID_HANDLE_VALUE;
  ~ProfileLock(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
  bool Acquire(const std::filesystem::path& path) {
    // No inherited handles and no sharing: exclude all current prototype hosts.
    file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
  }
};
int NominalRefreshRate(double hz) {
  return std::isfinite(hz)&&hz>=1&&hz<=1000?static_cast<int>(std::round(hz)):0;
}
int RefreshRate() {
  auto display=SDL_GetDisplayForWindow(window);const auto* mode=SDL_GetCurrentDisplayMode(display);
  if(!mode)return 0;
  double hz=mode->refresh_rate;
  if(mode->refresh_rate_numerator>0&&mode->refresh_rate_denominator>0)
    hz=static_cast<double>(mode->refresh_rate_numerator)/mode->refresh_rate_denominator;
  return NominalRefreshRate(hz);
}
float CefScale(){float scale=SDL_GetWindowDisplayScale(window);return std::isfinite(scale)&&scale>0?scale:1.f;}
std::string PresentationGpu() {
  auto device=static_cast<ID3D11Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr));
  IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;DXGI_ADAPTER_DESC desc{};
  if(!device||FAILED(device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi))))return "Unavailable";
  const bool found=SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&desc));
  if(adapter)adapter->Release();dxgi->Release();
  if(!found)return "Unavailable";
  const int size=WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,nullptr,0,nullptr,nullptr);
  if(size<=1)return "Unavailable";
  std::string name(static_cast<size_t>(size),'\0');WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name.data(),size,nullptr,nullptr);name.pop_back();return name;
}
int Dip(float coordinate){return static_cast<int>(coordinate/CefScale());}
#include "frame_clock.h"
#include "graphics_report.h"
class Texture {
  QrazyWindows::BorrowedTextureCopy copy_;
  SDL_Texture* texture_=nullptr;
  ID3D11Texture2D* wrapped_=nullptr;
 public:
  ~Texture(){if(texture_)SDL_DestroyTexture(texture_);}
  bool Initialize(ID3D11Device* device){return copy_.Initialize(device);}
  bool Ready() const{return texture_!=nullptr;}
  bool Copy(const CefAcceleratedPaintInfo& info) {
    auto r=info.extra.visible_rect;
    if(r.x<0||r.y<0||r.width<=0||r.height<=0||
       !copy_.Copy(info.shared_texture_handle,static_cast<UINT>(r.x),static_cast<UINT>(r.y),static_cast<UINT>(r.width),static_cast<UINT>(r.height)))return false;
    if(wrapped_==copy_.Owned())return true;
    if(texture_){SDL_DestroyTexture(texture_);texture_=nullptr;}
    auto props=SDL_CreateProperties();D3D11_TEXTURE2D_DESC desc{};copy_.Owned()->GetDesc(&desc);
    SDL_SetPointerProperty(props,SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,copy_.Owned());
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,desc.Width);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,desc.Height);
    SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM?SDL_PIXELFORMAT_BGRA32:SDL_PIXELFORMAT_RGBA32);
    texture_=SDL_CreateTextureWithProperties(renderer,props);SDL_DestroyProperties(props);
    if(!texture_)return false;
    SDL_SetTextureBlendMode(texture_,SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    wrapped_=copy_.Owned();return true;
  }
  bool Draw(const SDL_FRect* rect=nullptr){
    if(!texture_)return false;
    // CEF rounds its frame to whole DIPs, so at fractional display scales the texture can be a pixel or two
    // off the window size. Stretching that with bilinear filtering softens the whole picture; draw 1:1 instead.
    SDL_FRect native;float tw=0,th=0;int ow=0,oh=0;
    const bool exact=!rect&&SDL_GetTextureSize(texture_,&tw,&th)&&SDL_GetCurrentRenderOutputSize(renderer,&ow,&oh)&&
      std::fabs(tw-ow)<=2.f&&std::fabs(th-oh)<=2.f;
    if(exact){native={0.f,0.f,tw,th};rect=&native;}
    SDL_SetTextureScaleMode(texture_,exact||rect?SDL_SCALEMODE_NEAREST:SDL_SCALEMODE_LINEAR);
    return SDL_RenderTexture(renderer,texture_,nullptr,rect);
  }
};
class Client final : public DesktopPolicy::RecoveryState,public CefClient,public CefRenderHandler,
  public CefLifeSpanHandler,public CefRequestHandler,public CefLoadHandler,public CefDisplayHandler,
  public CefDownloadHandler,public CefDialogHandler,public CefContextMenuHandler,public CefKeyboardHandler,public CefCommandHandler {
 public:
  CefRefPtr<CefBrowser> browser;
  Texture view,popup;
  CefRect popup_rect;
  bool popup_visible=false,popup_ready=false,dirty=false,composing=false,fatal=false;
  bool handoff_waiting=false;
  QrazyWindows::ServerRetry server_retry;
  Uint64 load_started=SDL_GetTicks();
  Uint64 presentation_started=SDL_GetTicksNS();
  unsigned presentation_count=0;
  // Without VSync the begin-frame rate is adaptive: every kRateWindowNs the paints that came back are compared with the
  // begin-frames sent. While paints keep up the allowed rate grows; when they fall behind (a GPU-bound spot) it drops to just
  // above the paint rate, so the pipeline is not flooded. With VSync the display-paced clock bounds the rate and this is idle.
  static constexpr Uint64 kRateWindowNs=200000000ull;
  static constexpr double kRateFloor=60.0,kRateCeiling=20000.0;
  double allowed_rate=kRateCeiling,tokens=0,known_good=0; // known_good: the paint rate last sustained without falling behind
  Uint64 rate_window_start=0,last_token_ns=0;
  unsigned window_sent=0,window_paints=0;
  bool RateAllows(Uint64 now) {
    if(render_preferences.vsync){allowed_rate=kRateCeiling;tokens=0;last_token_ns=now;rate_window_start=now;window_sent=window_paints=0;return true;}
    if(!rate_window_start){rate_window_start=last_token_ns=now;}
    tokens=std::min(4.0,tokens+allowed_rate*static_cast<double>(now-last_token_ns)/1e9);last_token_ns=now;
    if(now-rate_window_start>=kRateWindowNs){
      const double seconds=static_cast<double>(now-rate_window_start)/1e9,sent=window_sent/seconds,paints=window_paints/seconds;
      if(sent>=20.0){
        if(paints>=0.9*sent){
          if(paints>=0.95*sent)known_good=std::max(known_good*0.99,sent);
          // Climb quickly back to the last sustainable rate, but only gently above it, so a GPU-bound spot is not overshot and flooded.
          const double step=sent<known_good?std::min(sent*1.5,known_good*1.25):sent*1.25;
          allowed_rate=std::min(kRateCeiling,std::max(allowed_rate,step));
        }else{
          known_good=std::min(known_good,paints*1.1);
          allowed_rate=std::max(kRateFloor,paints*1.1);
        }
      }
      rate_window_start=now;window_sent=window_paints=0;
    }
    return tokens>=1.0;
  }
  void RequestFrame(DisplayBeginClock& clock) {
    if(native_window)return; // Chromium paces its own presentation in windowed mode
    clock.Configure(render_preferences.vsync,render_preferences.max_fps,render_rate);
    if(!browser||closing)return;
    const Uint64 rate_now=SDL_GetTicksNS();
    if(!RateAllows(rate_now))return;
    if(!clock.Take())return;
    // The clock already ticks at the target rate with absolute deadlines. A second elapsed-time gate here dropped ticks
    // whenever this loop noticed them late (the loop only looks every ~1.7 ms), so caps below the loop rate came out low.
    if(render_preferences.CefTarget(render_rate)<1)return;
    if(!render_preferences.vsync)tokens-=1.0;
    ++window_sent;
    browser->GetHost()->SendExternalBeginFrame();
  }
  void PresentationTick(){
    if(native_window)return; // there are no SDL presents to count; the game keeps its own FPS figure
    const Uint64 now=SDL_GetTicksNS(),elapsed=now-presentation_started;
    if(elapsed<500000000ull)return;
    auto data=CefDictionaryValue::Create();data->SetString("type","presentation-rate");
    data->SetDouble("fps",presentation_count*1e9/static_cast<double>(elapsed));
    data->SetDouble("intervalMs",elapsed/1e6);data->SetInt("presents",static_cast<int>(presentation_count));
    Send(data);presentation_count=0;presentation_started=now;
  }
  std::string selection;
  double wheel_x=0,wheel_y=0;
  CefRefPtr<CefRenderHandler> GetRenderHandler() override{return this;}
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override{return this;}
  CefRefPtr<CefRequestHandler> GetRequestHandler() override{return this;}
  CefRefPtr<CefLoadHandler> GetLoadHandler() override{return this;}
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override{return this;}
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override{return this;}
  CefRefPtr<CefDialogHandler> GetDialogHandler() override{return this;}
  void GetViewRect(CefRefPtr<CefBrowser>,CefRect& rect) override {
    int w,h;SDL_GetWindowSize(window,&w,&h);rect=CefRect(0,0,std::max(1,static_cast<int>(std::ceil(w/CefScale()))),std::max(1,static_cast<int>(std::ceil(h/CefScale()))));
  }
  bool GetScreenInfo(CefRefPtr<CefBrowser>,CefScreenInfo& info) override {
    CefRect rect;GetViewRect(nullptr,rect);info.device_scale_factor=CefScale();info.rect=rect;info.available_rect=rect;return true;
  }
  bool GetScreenPoint(CefRefPtr<CefBrowser>,int x,int y,int& sx,int& sy) override {
    float scale=SDL_GetWindowDisplayScale(window);POINT p{static_cast<LONG>(x*scale),static_cast<LONG>(y*scale)};
    if(!ClientToScreen(hwnd,&p))return false;sx=p.x;sy=p.y;return true;
  }
  SDL_Cursor* cursor=nullptr;
  ~Client() override{if(cursor){SDL_SetCursor(SDL_GetDefaultCursor());SDL_DestroyCursor(cursor);}}
  bool OnCursorChange(CefRefPtr<CefBrowser>,CefCursorHandle,cef_cursor_type_t type,const CefCursorInfo&) override {
    if(native_window)return false; // Chromium sets the cursor on its own window
    SDL_SystemCursor shape=type==CT_HAND?SDL_SYSTEM_CURSOR_POINTER:type==CT_IBEAM?SDL_SYSTEM_CURSOR_TEXT:type==CT_CROSS?SDL_SYSTEM_CURSOR_CROSSHAIR:SDL_SYSTEM_CURSOR_DEFAULT;
    auto next=SDL_CreateSystemCursor(shape);if(next){SDL_SetCursor(next);if(cursor)SDL_DestroyCursor(cursor);cursor=next;}return true;
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> value) override {browser=value;ResizeChild(true);browser->GetHost()->SetFocus(Focused());browser->GetHost()->SetAudioMuted(true);}
  // Native mode: keep the CEF child filling the SDL window's client area (physical pixels).
  int child_width=0,child_height=0;
  void ResizeChild(bool force=false){
    if(!native_window||!browser)return;
    HWND child=browser->GetHost()->GetWindowHandle();RECT area{};
    if(!child||!GetClientRect(hwnd,&area)||area.right<=0||area.bottom<=0)return;
    if(!force&&area.right==child_width&&area.bottom==child_height)return;
    child_width=area.right;child_height=area.bottom;SetWindowPos(child,nullptr,0,0,child_width,child_height,SWP_NOZORDER|SWP_NOACTIVATE);browser->GetHost()->WasResized();
  }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override{return this;}
  CefRefPtr<CefCommandHandler> GetCommandHandler() override{return this;}
  // Native mode (Chrome style): the client offers none of Chrome's own commands (DevTools, reload, zoom, fullscreen, history, print, tab/window commands);
  // only clipboard commands stay. Command ids are looked up by name so they follow the CEF version.
  bool OnChromeCommand(CefRefPtr<CefBrowser>,int command,cef_window_open_disposition_t) override{
    if(!native_window)return false;
    static const int allowed[]={cef_id_for_command_id_name("IDC_CUT"),cef_id_for_command_id_name("IDC_COPY"),cef_id_for_command_id_name("IDC_PASTE")};
    for(int id:allowed)if(id==command)return false;
    return true;
  }
  // Native mode only: the child HWND gets the keys, so client hotkeys are taken here before the page sees them.
  // Browser accelerators of windowed Chromium that this client never offered: DevTools, reload, caret browsing, find, print/save/view-source,
  // tab/window keys, page zoom and history keys. The offscreen path never turned keys into these, so they are consumed after the page saw them.
  static bool DevToolsShortcut(const CefKeyEvent& event){
    const int key=event.windows_key_code;const bool ctrl=!!(event.modifiers&EVENTFLAG_CONTROL_DOWN),shift=!!(event.modifiers&EVENTFLAG_SHIFT_DOWN),alt=!!(event.modifiers&EVENTFLAG_ALT_DOWN);
    if(key==VK_F12||key==VK_F11||key==VK_F5||key==VK_F3||key==VK_F7)return true; /* F11 is the browser's fullscreen key; this client uses Alt+Enter and the game's button, and F11 stays bindable in the game */
    if(ctrl&&shift&&(key=='I'||key=='J'||key=='C'||key=='R'||key=='N'||key=='T'))return true;
    if(ctrl&&!alt&&(key=='R'||key=='P'||key=='S'||key=='U'||key=='F'||key=='G'||key=='W'||key=='T'||key=='N'||key=='0'||key==VK_NUMPAD0||key==VK_OEM_PLUS||key==VK_OEM_MINUS||key==VK_ADD||key==VK_SUBTRACT))return true;
    return alt&&!ctrl&&(key==VK_LEFT||key==VK_RIGHT);
  }

  // DevTools is not offered by this client. In offscreen mode the host never turns keys into browser accelerators; in windowed mode Chromium
  // would, and F12 crashed inside libcef. The page still sees the key first (the game may bind it); only the accelerator is consumed.
  /* Escape key-up is consumed after the page has seen it: the game re-locks the pointer from that very key-up (menu resume), and Chromium's own
     handling of the same Escape would then release the fresh lock again within milliseconds (seen in the diagnostic log). */
  bool OnKeyEvent(CefRefPtr<CefBrowser>,const CefKeyEvent& event,CefEventHandle) override {return native_window&&(DevToolsShortcut(event)||(event.windows_key_code==VK_ESCAPE&&event.type==KEYEVENT_KEYUP));}
  bool OnPreKeyEvent(CefRefPtr<CefBrowser>,const CefKeyEvent& event,CefEventHandle,bool* is_keyboard_shortcut) override {
    if(!native_window||closing)return false;
    const bool down=event.type==KEYEVENT_RAWKEYDOWN||event.type==KEYEVENT_KEYDOWN,alt=!!(event.modifiers&EVENTFLAG_ALT_DOWN),first=down&&!(event.modifiers&EVENTFLAG_IS_REPEAT);
    /* Chromium exits pointer lock itself on Escape and then refuses a new lock for a while (same as in a browser). While the page holds the lock, take the
       Escape press here and let the page release the lock by script instead; the key-up still reaches the page. */
    if(pointer_locked&&event.windows_key_code==VK_ESCAPE&&down&&!(event.modifiers&(EVENTFLAG_ALT_DOWN|EVENTFLAG_CONTROL_DOWN))){if(first){auto data=Dict();data->SetString("type","escape");Send(data);}return true;}
    if(DevToolsShortcut(event)){*is_keyboard_shortcut=true;return false;}
    if(first&&(event.windows_key_code==VK_RETURN||event.windows_key_code==VK_SPACE))update_gesture=SDL_GetTicksNS();
    if(alt&&!(event.modifiers&EVENTFLAG_CONTROL_DOWN)&&event.windows_key_code==VK_F4){if(first){Release("shortcut-close");closing=true;}return true;}
    if(alt&&event.windows_key_code==VK_RETURN){if(first)SDL_SetWindowFullscreen(window,!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));return true;}
    return false;
  }
  void OnFullscreenModeChange(CefRefPtr<CefBrowser>,bool fullscreen) override{if(native_window&&!closing&&fullscreen!=!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))SDL_SetWindowFullscreen(window,fullscreen);}
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {Release("close");browser=nullptr;closed=true;}
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
  bool CanDownload(CefRefPtr<CefBrowser> value,const CefString& url,const CefString& method) override {return !closing && value && Trusted(value->GetMainFrame()->GetURL()) && method=="GET" && DemoDownload::TrustedBlob(url.ToString());}
  bool OnBeforeDownload(CefRefPtr<CefBrowser> value,CefRefPtr<CefDownloadItem> item,const CefString& name,CefRefPtr<CefBeforeDownloadCallback> callback) override {if(!closing && !dialog_active && value && Trusted(value->GetMainFrame()->GetURL()) && DemoDownload::TrustedBlob(item->GetOriginalUrl().ToString()) && DemoDownload::ExportName(name.ToString())) {Release("export-save");DemoDownload::Save(window,name.ToString(),callback);}return true;}
  void OnDownloadUpdated(CefRefPtr<CefBrowser>,CefRefPtr<CefDownloadItem> item,CefRefPtr<CefDownloadItemCallback> callback) override {if(!DemoDownload::TrustedBlob(item->GetOriginalUrl().ToString()))callback->Cancel();}
  // Single-file pickers from the trusted page are allowed; the dialog filter is derived from the page's accept list
  // (".ext" entries and a few known MIME types). Unparseable or risky entries cancel the dialog.
  struct PickerState{CefRefPtr<CefFileDialogCallback> callback;std::string pattern;SDL_DialogFileFilter filter;};
  static void SDLCALL PickerChosen(void* userdata,const char* const* files,int) {
    std::unique_ptr<PickerState> state(static_cast<PickerState*>(userdata));
    if(files&&files[0])state->callback->Continue({CefString(std::string(files[0]))});else state->callback->Cancel();
    dialog_active=false;
  }
  static bool BuildPickerPattern(const std::vector<CefString>& filters,std::string& pattern){
    static const char* denied[]={"exe","dll","bat","cmd","com","scr","msi","ps1","vbs","js","jse","wsf","lnk","reg","hta","jar","sh","pif","cpl"};
    std::vector<std::string> exts;
    auto add=[&](const std::string& e)->bool{
      if(e.empty()||e.size()>8)return false;
      for(char c:e)if(!std::isalnum(static_cast<unsigned char>(c)))return false;
      for(const char* d:denied)if(e==d)return false;
      for(const auto& x:exts)if(x==e)return true;
      exts.push_back(e);return true;
    };
    if(filters.empty())return false;
    for(const auto& entry:filters){
      std::string f=entry.ToString();for(char& c:f)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if(f.size()>1&&f[0]=='.'){if(!add(f.substr(1)))return false;}
      else if(f=="image/*"){for(const char* e:{"png","jpg","jpeg","gif","webp","bmp"})add(e);}
      else if(f=="image/png"){add("png");}
      else if(f=="image/jpeg"){add("jpg");add("jpeg");}
      else if(f=="image/gif"){add("gif");}
      else if(f=="image/webp"){add("webp");}
      else if(f=="image/bmp"){add("bmp");}
      else if(f=="model/gltf-binary"){add("glb");}
      else return false;
    }
    if(exts.empty())return false;
    for(size_t i=0;i<exts.size();i++){if(i)pattern+=';';pattern+=exts[i];}
    return true;
  }
  bool OnFileDialog(CefRefPtr<CefBrowser> value,FileDialogMode mode,const CefString&,const CefString&,const std::vector<CefString>& filters,const std::vector<CefString>&,const std::vector<CefString>&,CefRefPtr<CefFileDialogCallback> callback) override{
    std::string pattern;
    if((static_cast<int>(mode)&0xFF)!=static_cast<int>(FILE_DIALOG_OPEN)||!BuildPickerPattern(filters,pattern)||closing||recovering||loading||captured||!Focused()||DemoDownload::active||!value||!Trusted(value->GetMainFrame()->GetURL())||dialog_active.exchange(true)){callback->Cancel();return true;}
    auto* state=new PickerState{callback,std::move(pattern),{}};state->filter={"Files",state->pattern.c_str()};
    SDL_ShowOpenFileDialog(PickerChosen,state,window,&state->filter,1,nullptr,false);return true;
  }
  void OnTextSelectionChanged(CefRefPtr<CefBrowser>,const CefString& text,const CefRange&) override{selection=text.ToString().substr(0,1048576);}
  void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser>,const CefRange&,const RectList& bounds) override {
    if(bounds.empty())return;auto r=bounds.back();float scale=CefScale();SDL_Rect area{static_cast<int>(r.x*scale),static_cast<int>(r.y*scale),static_cast<int>(r.width*scale),static_cast<int>(r.height*scale)};SDL_SetTextInputArea(window,&area,area.w);
  }
  void Send(CefRefPtr<CefDictionaryValue> data) {
    if(!browser||!Trusted(browser->GetMainFrame()->GetURL()))return;
    auto v=CefValue::Create();v->SetDictionary(data);auto message=CefProcessMessage::Create("qrazy-event-v1");
    message->GetArgumentList()->SetString(0,CefWriteJSON(v,JSON_WRITER_DEFAULT));browser->GetMainFrame()->SendProcessMessage(PID_RENDERER,message);
  }
  void Reply(int id,CefRefPtr<CefDictionaryValue> value){auto data=Dict();data->SetString("type","reply");data->SetInt("id",id);data->SetDictionary("value",value);Send(data);}
  void Release(const char* reason) {
    SDL_SetWindowRelativeMouseMode(window,false);SDL_EnableScreenSaver();
    if(captured)ClipCursor(nullptr);
    if(!captured)return;captured=false;++generation;
    if(Focused())SDL_StartTextInput(window);
    auto data=Dict();data->SetString("type","state");data->SetString("reason",reason);Send(data);
  }
  void Reset(const char* reason){pointer_locked=false;Release(reason);asset_worker.Reset();popup_visible=false;selection.clear();composing=false;if(browser){browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);}}
  void Fail(bool retryable=false){Reset("failure");FailLoad();if(retryable)server_retry.Failure(SDL_GetTicks());else server_retry.Stop();SDL_SetWindowTitle(window,client_title.c_str());dirty=true;}
  void Retry(){if(!browser||closing||fatal||handoff_waiting||!server_retry.Attempt(SDL_GetTicks()))return;Reset("retry");BeginLoad();load_started=SDL_GetTicks();browser->GetMainFrame()->LoadURL(kOrigin);}
  void TickRetry(){
    if(!browser||closing||fatal||handoff_waiting)return;
    const auto now=SDL_GetTicks();
    if(server_retry.Expire(now)){browser->StopLoad();Reset("retry-expired");FailLoad();dirty=true;return;}
    if(loading&&now-load_started>=QrazyWindows::ServerConnectTimeoutMs){browser->StopLoad();Fail(true);}
    if(server_retry.Due(now))Retry();
  }
  bool RetryButton(float x,float y){
    if((!recovering&&!server_retry.active)||closing||fatal||handoff_waiting||!Focused())return false;
    int w=0,h=0,pw=0,ph=0;SDL_GetWindowSize(window,&w,&h);SDL_GetRenderOutputSize(renderer,&pw,&ph);
    if(w<=0||h<=0)return false;x*=static_cast<float>(pw)/w;y*=static_cast<float>(ph)/h;
    if(x<40||x>260||y<200||y>248)return false;
    if(!server_retry.pending)Retry();return true;
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,CefRefPtr<CefRequest> request,bool gesture,bool) override {
    /* Native mode: history traversal (Alt+Left/Right, mouse side buttons, gestures) is never wanted; reloads and our own loads are not affected. */
    if(native_window&&frame->IsMain()&&(request->GetTransitionType()&TT_FORWARD_BACK_FLAG))return true;
    if(handoff_waiting)return !frame->IsMain()||request->GetURL()!="about:blank";
    if(!frame->IsMain())return false;if(!Trusted(request->GetURL())){OpenWebLink(frame,request->GetURL(),gesture);return true;}Reset("navigation");BeginLoad();load_started=SDL_GetTicks();return false;
  }
  void OnLoadStart(CefRefPtr<CefBrowser> b,CefRefPtr<CefFrame> frame,TransitionType) override{if(frame->IsMain()&&b&&b->GetHost()->GetZoomLevel()!=0.0)b->GetHost()->SetZoomLevel(0.0);  // zoom persists per site in the profile; undo earlier Ctrl+scroll zoom
    if(frame->IsMain()&&loading&&!recovering)SDL_SetWindowTitle(window,"Qrazy - connecting");}
  void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int code) override {
    if(!frame->IsMain()||!loading||recovering||!Trusted(frame->GetURL()))return;
    if(code>=400){Fail(code==502||code==503||code==504);return;}if(FinishLoad(true,code)){server_retry.Stop();SDL_SetWindowTitle(window,client_title.c_str());browser->GetHost()->SetFocus(Focused());browser->GetHost()->SetAudioMuted(!Focused());}
  }
  void OnLoadError(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,ErrorCode code,const CefString&,const CefString& url) override{if(frame->IsMain()&&loading&&!recovering&&Trusted(url)&&code!=ERR_ABORTED)Fail(code==ERR_CONNECTION_REFUSED||code==ERR_CONNECTION_RESET||code==ERR_CONNECTION_CLOSED||code==ERR_CONNECTION_FAILED||code==ERR_CONNECTION_TIMED_OUT||code==ERR_TIMED_OUT||code==ERR_NAME_NOT_RESOLVED||code==ERR_INTERNET_DISCONNECTED||code==ERR_NETWORK_CHANGED);}
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser>,TerminationStatus,int,const CefString&) override{Fail();}
  void OnPaint(CefRefPtr<CefBrowser>,PaintElementType,const RectList&,const void*,int,int) override{fatal=true;closing=true;Release("CPU-paint-rejected");}
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>,PaintElementType type,const RectList&,const CefAcceleratedPaintInfo& info) override {
    if(type==PET_VIEW)++window_paints;
    bool copied=(type==PET_VIEW?view:popup).Copy(info);
    if(!copied){fatal=true;closing=true;Release("GPU-copy-failed");}else{if(type==PET_POPUP)popup_ready=true;dirty=true;}
  }
  // CEF announces a dropdown before its accelerated texture arrives. Wait for
  // this opening's paint rather than failing on a missing (or stale) texture.
  void OnPopupShow(CefRefPtr<CefBrowser>,bool show) override{popup_visible=show;popup_ready=false;dirty=true;}
  void OnPopupSize(CefRefPtr<CefBrowser>,const CefRect& rect) override{if(rect.width!=popup_rect.width||rect.height!=popup_rect.height)popup_ready=false;popup_rect=rect;dirty=true;}
  void FullscreenState(){auto data=Dict();data->SetString("type","fullscreen");data->SetBool("value",!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));Send(data);}
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> b,CefRefPtr<CefFrame> frame,CefProcessId source,CefRefPtr<CefProcessMessage> msg) override {
    if(msg->GetName()!="qrazy-command-v1")return false;
    if(source!=PID_RENDERER||!browser||b->GetIdentifier()!=browser->GetIdentifier()||!frame->IsMain()||
       frame->GetIdentifier()!=browser->GetMainFrame()->GetIdentifier()||!Trusted(frame->GetURL())||!Trusted(browser->GetMainFrame()->GetURL()))return true;
    auto args=msg->GetArgumentList();
    if(args->GetSize()!=3||args->GetType(0)!=VTYPE_STRING||args->GetType(1)!=VTYPE_INT||args->GetType(2)!=VTYPE_STRING||args->GetString(2).length()>1500000)return true;
    int id=args->GetInt(1);if(id<0)return true;std::string op=args->GetString(0);
    auto result=Dict();result->SetBool("ok",true);
    auto payload=CefParseJSON(args->GetString(2),JSON_PARSER_RFC);auto d=payload&&payload->GetType()==VTYPE_DICTIONARY?payload->GetDictionary():nullptr;
    bool menu=!closing&&!recovering&&!loading&&!captured&&Focused();
    if(op=="native-window-get"){Reply(id,render_preferences.NativeWindowState(native_window));return true;}
    if(op=="native-window-set"){Reply(id,QrazyWindows::Error("The native window is always on; there is no setting to change"));return true;}
    if(native_window&&op=="vsync-get"){Reply(id,render_preferences.VSyncState());return true;}
    if(native_window&&op=="vsync-set"){
      if(closing||recovering||loading||!Focused()||!d||d->GetType("enabled")!=VTYPE_BOOL){Reply(id,QrazyWindows::Error("Return to the focused game before changing rendering settings"));return true;}
      const auto previous=render_preferences;render_preferences.vsync=d->GetBool("enabled");
      if(!render_preferences.Save()){render_preferences=previous;Reply(id,QrazyWindows::Error("Setting could not be saved"));return true;}
      Reply(id,render_preferences.VSyncState()); /* applies at once: the bridge limiter reads it from the event below */
      auto event=Dict();event->SetString("type","render-settings");event->SetDictionary("vsync",render_preferences.VSyncState());event->SetDictionary("maxFps",render_preferences.MaxFpsState());Send(event);return true;
    }
    if(native_window&&op=="max-fps-set"){ /* native mode: only saved; the bridge enforces it with a MessageChannel-paced requestAnimationFrame limiter */
      if(closing||recovering||loading||!Focused()||!d||d->GetType("value")!=VTYPE_INT||d->GetInt("value")<30||d->GetInt("value")>10000){Reply(id,QrazyWindows::Error("Return to the focused game and give com_maxfps as an integer from 30 to 10000"));return true;}
      const auto previous=render_preferences;render_preferences.max_fps=d->GetInt("value");
      if(!render_preferences.Save()){render_preferences=previous;Reply(id,QrazyWindows::Error("Setting could not be saved"));return true;}
      Reply(id,render_preferences.MaxFpsState());
      auto event=Dict();event->SetString("type","render-settings");event->SetDictionary("vsync",render_preferences.VSyncState());event->SetDictionary("maxFps",render_preferences.MaxFpsState());Send(event);return true;
    }
    if(op=="vsync-get"||op=="max-fps-get"){Reply(id,op=="vsync-get"?render_preferences.VSyncState():render_preferences.MaxFpsState());return true;}
    if(op=="vsync-set"||op=="max-fps-set"){
      if(closing||recovering||loading||!Focused()){Reply(id,QrazyWindows::Error("Return to the focused game before changing rendering settings"));return true;}
      bool is_vsync=op=="vsync-set";
      if(!d||(is_vsync?d->GetType("enabled")!=VTYPE_BOOL:d->GetType("value")!=VTYPE_INT)||( !is_vsync&&(d->GetInt("value")<30||d->GetInt("value")>10000))){Reply(id,QrazyWindows::Error("Expected a VSync boolean or com_maxfps integer from 30 to 10000"));return true;}
      const auto previous=render_preferences;
      if(is_vsync)render_preferences.vsync=d->GetBool("enabled");else render_preferences.max_fps=d->GetInt("value");
      int actual=0;
      if(!SDL_SetRenderVSync(renderer,render_preferences.vsync?1:0)||!SDL_GetRenderVSync(renderer,&actual)||actual!=(render_preferences.vsync?1:0)||!render_preferences.Save()){
        render_preferences=previous;SDL_SetRenderVSync(renderer,previous.vsync?1:0);Reply(id,QrazyWindows::Error("Rendering setting could not be applied and saved"));return true;
      }
      browser->GetHost()->SetWindowlessFrameRate(render_preferences.CefTarget(render_rate));dirty=true;
      Reply(id,is_vsync?render_preferences.VSyncState():render_preferences.MaxFpsState());
      auto event=Dict();event->SetString("type","render-settings");event->SetDictionary("vsync",render_preferences.VSyncState());event->SetDictionary("maxFps",render_preferences.MaxFpsState());Send(event);return true;
    }
    if(op=="assets") {
      static const std::map<std::string,size_t> operations={{"has",2},{"openRead",2},{"readChunk",1},{"closeRead",1},{"beginWrite",1},{"writeChunk",2},{"finishWrite",1},{"abortWrite",1}};
      if(!d||d->GetType("op")!=VTYPE_STRING||d->GetType("args")!=VTYPE_LIST)return true;
      std::string operation=d->GetString("op");auto list=d->GetList("args");auto it=operations.find(operation);
      if(it==operations.end()||list->GetSize()!=it->second)return true;
      if(asset_worker.Submit(id,operation,list))return true;
      result->SetBool("ok",false);result->SetString("error","Desktop storage worker unavailable or busy; normal loading can continue");
    }else if(op=="release"){Release("release");return true;}
    else if(op=="config-import"||op=="config-export") {
      if((!menu && !(op=="config-export" && !closing && !recovering && !loading && Focused())) || DemoDownload::active || dialog_active.exchange(true)){result->SetBool("ok",false);result->SetString("error","Config dialogs require a focused menu and no active dialog");Reply(id,result);return true;}
      auto selected=new DialogResult{id,asset_worker.epoch.load(),op=="config-export"};
      static const SDL_DialogFileFilter filter={"Qrazy configuration","cfg"};
      if(selected->save){std::string name=d?d->GetString("name"):CefString();selected->text=d?d->GetString("text"):CefString();
        if(name.size()<5||name.size()>100||name.substr(name.size()-4)!=".cfg"||name.find_first_of("/\\\r\n")!=std::string::npos||name.find('\0')!=std::string::npos||selected->text.size()>1048576||selected->text.find('\0')!=std::string::npos){delete selected;dialog_active=false;result->SetBool("ok",false);result->SetString("error","Expected a cfg filename and at most 1 MiB of text");Reply(id,result);return true;}
        Release("export-save");SDL_ShowSaveFileDialog(FileChosen,selected,window,&filter,1,name.c_str());
      }else SDL_ShowOpenFileDialog(FileChosen,selected,window,&filter,1,nullptr,false);
      return true;
    }else if(op=="update-notice"||op=="update-state"||op=="update-check"||op=="update-stage"||op=="update-install") {
      if(QRAZY_DEV_MODE){Reply(id,QrazyWindows::Error("Updates are disabled in the development client"));return true;}
      if(!menu){Reply(id,QrazyWindows::Error("Return to the focused menu before updating"));return true;}
      std::string action=op=="update-notice"?"check":op=="update-state"?"state":op=="update-install"?"prepare":"update";
      if(action!="state"&&action!="check"){
        if(!update_gesture||SDL_GetTicksNS()-update_gesture>2'000'000'000ull){Reply(id,QrazyWindows::Error("Click the Update or Install button to continue"));return true;}update_gesture=0;
      }
      if(action=="prepare"){
        const SDL_MessageBoxButtonData buttons[]={{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Cancel"},{0,1,"Close and install"}};
        const SDL_MessageBoxData prompt={SDL_MESSAGEBOX_INFORMATION,window,"Qrazy update","Close Qrazy and install the verified update? Open Qrazy manually afterward. No previous version will be retained.",2,buttons,nullptr};int selected=0;
        if(!SDL_ShowMessageBox(&prompt,&selected)||selected!=1){auto state=Dict();state->SetString("phase","cancelled");state->SetString("message","Installation cancelled. Qrazy stays open.");Reply(id,QrazyWindows::Success(QrazyWindows::Value(state)));return true;}
      }
      if(!updater_worker.Submit(id,asset_worker.epoch.load(),action))Reply(id,QrazyWindows::Error("An update action is already running"));return true;
    }else if(op=="update-rollback") {Reply(id,QrazyWindows::Error("Previous versions are not retained"));return true;
    }else if(op=="changelog") {
      std::ifstream changes(updater_worker.root/"runtime"/"CHANGES.txt",std::ios::binary);std::string text(16001,'\0');changes.read(text.data(),16001);text.resize(static_cast<size_t>(changes.gcount()));if(!changes.is_open()||changes.bad()||text.size()>16000){Reply(id,QrazyWindows::Error("Release notes unavailable"));return true;}auto value=CefValue::Create();value->SetString(text);Reply(id,QrazyWindows::Success(value));return true;
    }else if(op=="diagnostics") {
      auto report=Dict();report->SetString("platform","Windows SDL3 + CEF");report->SetString("client",client_title);
      report->SetString("presentationGpu",PresentationGpu());report->SetString("renderer","Direct3D 11");report->SetBool("nativeWindow",native_window);
      report->SetInt("cefTarget",render_preferences.CefTarget(render_rate));report->SetInt("maxFps",render_preferences.max_fps);report->SetBool("sdlVSync",render_preferences.vsync);
      report->SetInt("refreshHz",RefreshRate());int width=0,height=0;SDL_GetWindowSizeInPixels(window,&width,&height);report->SetInt("pixelWidth",width);report->SetInt("pixelHeight",height);
      report->SetDouble("displayScale",SDL_GetWindowDisplayScale(window));SupportGraphics(report);Reply(id,QrazyWindows::Success(QrazyWindows::Value(report)));return true;
    }else if(op=="pointer-lock-activate"){if(native_window&&browser){CefKeyEvent k;k.windows_key_code=VK_F24;k.native_key_code=0x76<<16;k.is_system_key=false;k.modifiers=0;k.type=KEYEVENT_RAWKEYDOWN;browser->GetHost()->SendKeyEvent(k);k.type=KEYEVENT_KEYUP;browser->GetHost()->SendKeyEvent(k);}
    }else if(op=="pointer-lock-state"){pointer_locked=native_window&&d&&d->GetType("locked")==VTYPE_BOOL&&d->GetBool("locked");
    }else if(op=="refresh-hz-get")result->SetInt("hz",RefreshRate());
    else if(op=="clock")result->SetDouble("time",SDL_GetTicksNS()/1e6);
    else if(op=="capture") {
      const char* why=native_window?"native-window-mode":closing?"closing":recovering?"recovering":loading?"loading":dialog_active?"dialog":!Focused()?"no-keyboard-focus":nullptr;
      // The free cursor may have left the window during chat/console. The window
      // still has keyboard focus, so bring the cursor back before capturing.
      if(!why&&SDL_GetMouseFocus()!=window){int w=0,h=0;SDL_GetWindowSize(window,&w,&h);SDL_WarpMouseInWindow(window,w/2.f,h/2.f);}
      bool ok=!why&&SDL_SetWindowRelativeMouseMode(window,true);
      if(!why&&!ok)why="relative-mode-failed";
      if(ok&&!ConfineCapturedCursor()){ok=false;why="cursor-confinement-failed";SDL_SetWindowRelativeMouseMode(window,false);}
      if(why)result->SetString("reason",why);
      captured=ok;if(ok){SDL_StopTextInput(window);if(!SDL_DisableScreenSaver())std::fprintf(stderr,"PROTOTYPE idle inhibition failed\n");}
      result->SetBool("ok",ok);result->SetInt("generation",++generation);
    }else if(op=="quit"){Release("quit");closing=true;}
    else if(op=="refresh-game") {
      bool ok=browser&&!closing&&!fatal&&!handoff_waiting&&!loading&&Focused();
      result->SetBool("ok",ok);
      if(ok){server_retry.Stop();Reset("refresh");BeginLoad();load_started=SDL_GetTicks();server_retry.Start(load_started);server_retry.Attempt(load_started);browser->ReloadIgnoreCache();}
    }
    else if(op=="retry")Retry();
    else if(op=="fullscreen-toggle"||op=="fullscreen-state") {
      if(op=="fullscreen-toggle")result->SetBool("ok",SDL_SetWindowFullscreen(window,!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)));
      result->SetBool("fullscreen",!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));
    }else if(op=="clipboard-write") {
      std::string text=d?d->GetString("text"):CefString();result->SetBool("ok",menu&&text.size()<=1048576&&text.find('\0')==std::string::npos&&SDL_SetClipboardText(text.c_str()));
    }else if(op=="status") {
      if(!d||d->GetType("phase")!=VTYPE_STRING||d->GetType("message")!=VTYPE_STRING)return true;
      std::string phase=d->GetString("phase"),stage=d->GetString("stage"),recovery=d->GetString("recovery");
      if((phase!="ready"&&phase!="loading"&&phase!="warning"&&phase!="error")||
        (stage!="server"&&stage!="map"&&stage!="assets"&&stage!="graphics"&&stage!="game")||
        (recovery!="none"&&recovery!="reload")||(recovery=="reload"&&phase!="error")||d->GetString("message").length()>500||d->GetString("details").length()>2000||d->GetString("code").length()>80)return true;
      if(phase=="error")Release("game-error");
    }else {
      result->SetBool("ok",false);result->SetString("error","This Windows port component is not implemented yet; normal browser loading remains available");
    }
    Reply(id,result);return true;
  }
 private: IMPLEMENT_REFCOUNTING(Client);
};
#include "keyboard.h"
class CookieFlushed final : public CefCompletionCallback {
 public:bool done=false;void OnComplete() override{done=true;}
 private:IMPLEMENT_REFCOUNTING(CookieFlushed);
};

static bool SameFile(const std::filesystem::path& a,const std::filesystem::path& b) {
  std::error_code error;
  if(std::filesystem::file_size(a,error)!=std::filesystem::file_size(b,error)||error)return false;
  std::ifstream first(a,std::ios::binary),second(b,std::ios::binary);
  std::ostringstream x,y;x<<first.rdbuf();y<<second.rdbuf();
  return x.str()==y.str();
}
// The launcher sits outside the runtime folder that updates replace, so a newer one
// ships inside the runtime and is swapped in here. A running exe can be renamed.
static void RefreshLauncher(const std::filesystem::path& base,const std::filesystem::path& runtime) {
  std::error_code error;
  const auto fresh=runtime/L"Qrazy-launcher.exe",live=base/L"Qrazy.exe",staged=base/L"Qrazy.exe.new",previous=base/L"Qrazy.exe.old";
  std::filesystem::remove(previous,error);std::filesystem::remove(staged,error);
  if(!std::filesystem::is_regular_file(fresh,error)||!std::filesystem::is_regular_file(live,error)||SameFile(fresh,live))return;
  std::filesystem::copy_file(fresh,staged,std::filesystem::copy_options::overwrite_existing,error);
  if(error)return;
  std::filesystem::rename(live,previous,error);
  if(error){std::filesystem::remove(staged,error);return;}
  std::filesystem::rename(staged,live,error);
  if(error){std::error_code undo;std::filesystem::rename(previous,live,undo);std::filesystem::remove(staged,undo);}
}

int Run(HINSTANCE instance,void* sandbox) {
  if(!sandbox)return 2; // Bootstrap sandbox is required, never silently disabled.
  wchar_t name[32768];DWORD n=GetModuleFileNameW(nullptr,name,32768);if(!n||n>=32768)return 2;
  auto root=std::filesystem::path(name).parent_path();
  std::ifstream bridge(root/"bridge.js");std::ostringstream content;content<<bridge.rdbuf();bridge_source=content.str();if(bridge_source.empty())return 2;
  CefMainArgs args(instance);CefRefPtr<App> app=new App;
  int sub=CefExecuteProcess(args,app,sandbox);if(sub>=0)return sub;
  auto command=CefCommandLine::CreateCommandLine();command->InitFromString(GetCommandLineW());
  CefCommandLine::SwitchMap switches;CefCommandLine::ArgumentList arguments;command->GetSwitches(switches);command->GetArguments(arguments);
  if(!switches.empty()||!arguments.empty())return 2;
  auto base=root.parent_path();wchar_t marker[32768];DWORD marker_size=GetEnvironmentVariableW(L"QRAZY_SDLCEF_ROOT",marker,32768);
  if(!marker_size||marker_size>=32768||std::filesystem::path(marker)!=base||root.filename()!=L"runtime")return 2;
  Production::CleanEnvironment();
  updater_worker.root=base;
  auto profile=base/kProfile;std::error_code error;std::filesystem::create_directories(profile,error);
  if(error||!QrazyWindows::NoReparse(profile))return 2;ProfileLock lock;if(!lock.Acquire(profile/"host.lock"))return 2;
  RefreshLauncher(base,root);
  render_preferences.Load(profile);native_window=render_preferences.native_window;startup_vsync=render_preferences.vsync;
  asset_worker.Start(profile);
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER,"1");SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE,"0");
  SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,"0");SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE,"1");
  EnsureNormalPriority();
  if(!SDL_Init(SDL_INIT_VIDEO))return 3;
  window=SDL_CreateWindow(client_title.c_str(),1280,800,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if(!window){SDL_Quit();return 3;}
  hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
  if(native_window)gesture_hook=SetWindowsHookExW(WH_MOUSE,GestureHook,nullptr,GetCurrentThreadId());
  saved_window.profile=profile;saved_window.Restore();
  // Start ordinary windows maximized, including previously saved small windows.
  // A saved fullscreen session still restores fullscreen.
  if(!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))SDL_MaximizeWindow(window);
  auto icon_module=GetModuleHandleW(L"Qrazy.dll");
  for(int size:{ICON_SMALL,ICON_BIG}){
    auto icon=LoadImageW(icon_module,MAKEINTRESOURCEW(1),IMAGE_ICON,
      GetSystemMetrics(size==ICON_SMALL?SM_CXSMICON:SM_CXICON),
      GetSystemMetrics(size==ICON_SMALL?SM_CYSMICON:SM_CYICON),LR_SHARED);
    if(icon)SendMessageW(hwnd,WM_SETICON,size,reinterpret_cast<LPARAM>(icon));
  }
  renderer=SDL_CreateRenderer(window,"direct3d11");
  int vsync=0;
  if(!renderer||!SDL_SetRenderVSync(renderer,render_preferences.vsync?1:0)||!SDL_GetRenderVSync(renderer,&vsync)||vsync!=(render_preferences.vsync?1:0)||(render_rate=RefreshRate())==0){SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return 3;}
  auto device=static_cast<ID3D11Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr));
  if(!device)return 3;
  CefSettings settings;settings.windowless_rendering_enabled=true;settings.no_sandbox=false;settings.command_line_args_disabled=true;
  settings.persist_session_cookies=false;
  settings.log_severity=LOGSEVERITY_WARNING;
  CefString(&settings.log_file)=(profile/L"chromium.log").wstring();
  CefString(&settings.root_cache_path)=profile.wstring();CefString(&settings.cache_path)=profile.wstring();
  CefString(&settings.resources_dir_path)=root.wstring();CefString(&settings.locales_dir_path)=(root/"locales").wstring();
  if(!CefInitialize(args,settings,app,sandbox))return 4;
  // The state helper authenticates current.json and its runtime inventory.
  std::string version_state;
  if(Production::Helper(base,L"state",version_state)) {
    auto parsed=CefParseJSON(version_state,JSON_PARSER_RFC);
    auto outer=parsed&&parsed->GetType()==VTYPE_DICTIONARY?parsed->GetDictionary():nullptr;
    auto data=outer&&outer->GetBool("ok")?outer->GetDictionary("data"):nullptr;
    if(data&&data->GetType("installedVersion")==VTYPE_STRING) {
      const std::string version=data->GetString("installedVersion");
      if(!version.empty()&&version.size()<64&&version.find_first_not_of("0123456789.")==std::string::npos)client_title="Qrazy v"+version;
    }
  }
  if(QRAZY_DEV_MODE)client_title+=" [DEV localhost:5173]";
  SDL_SetWindowTitle(window,client_title.c_str());
  int result=0;
  {
    auto request_context=CefRequestContext::GetGlobalContext();
    const bool transitioning=std::filesystem::exists(base/L".qrazy-transition");
    Handoff::StartupPeer peer;Handoff::PreGameGate gate;bool handoff_started=false,handoff_acknowledged=false;
    if(transitioning&&!peer.Start(base))result=8;
    CefRefPtr<Client> client=new Client;
    DisplayBeginClock frame_clock;
    frame_clock.Configure(render_preferences.vsync,render_preferences.max_fps,render_rate);
    if(!native_window&&!frame_clock.Start(hwnd))result=5;
    client->handoff_waiting=transitioning;
    if(!native_window&&(!client->view.Initialize(device)||!client->popup.Initialize(device)))result=5;
    CefWindowInfo info;CefRefPtr<CefDictionaryValue> mode; /* extra_info reaches the render process; null in offscreen mode */
    if(native_window){mode=CefDictionaryValue::Create();RECT area{};GetClientRect(hwnd,&area);info.SetAsChild(hwnd,CefRect(0,0,std::max<LONG>(1,area.right),std::max<LONG>(1,area.bottom)));mode->SetBool("nativeWindow",true);}
    else{info.SetAsWindowless(hwnd);info.shared_texture_enabled=true;info.external_begin_frame_enabled=true;}
    CefBrowserSettings bs;bs.windowless_frame_rate=render_preferences.CefTarget(render_rate);bs.background_color=CefColorSetARGB(255,0,0,0);
    if(result||!CefBrowserHost::CreateBrowser(info,client,transitioning?"about:blank":kOrigin,bs,mode,request_context)){
      /* No silent fallback to offscreen: a native-window failure has to be reported so it can be fixed. */
      if(!result&&native_window)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Qrazy","Qrazy could not create its game window (error 5).\n\nPlease report this in the Qrazy Discord and include your graphics card, graphics driver version and Windows version.",window);
      if(!result)result=5;
    }
    bool close_sent=false,child_hidden=false,child_enabled=true;Uint64 zoom_checked=0,close_sent_ns=0;auto refresh=std::chrono::steady_clock::now();if(!native_window)SDL_StartTextInput(window);
    auto update_display=[&](){
      if(native_window){
        /* Chromium's child window never sees the parent move between monitors, so its vsync interval (and rAF rate) stays on the old display's refresh. Tell it, and nudge the child size so the compositor re-evaluates the display. */
        static SDL_DisplayID last_display=0;static int last_rate=0;
        SDL_DisplayID now_display=SDL_GetDisplayForWindow(window);int now_rate=RefreshRate();
        if(last_display&&(now_display!=last_display||now_rate!=last_rate)&&client->browser){
          client->browser->GetHost()->NotifyScreenInfoChanged();
          HWND child=client->browser->GetHost()->GetWindowHandle();RECT area{};
          if(child&&GetClientRect(hwnd,&area)&&area.right>1&&area.bottom>1){SetWindowPos(child,nullptr,0,0,area.right-1,area.bottom,SWP_NOZORDER|SWP_NOACTIVATE);client->browser->GetHost()->WasResized();client->ResizeChild(true);}
        }
        last_display=now_display;last_rate=now_rate;
        if(now_rate)render_rate=now_rate;
        return;
      }
      if(!frame_clock.UpdateDisplay(hwnd)){client->fatal=true;closing=true;return;}
      int rate=RefreshRate();if(rate&&rate!=render_rate){render_rate=rate;if(client->browser)client->browser->GetHost()->SetWindowlessFrameRate(render_preferences.CefTarget(rate));}
    };
    auto render_frame=[&](){client->dirty=false;SDL_SetRenderDrawColor(renderer,6,12,22,255);SDL_RenderClear(renderer);
        if(client->loading||client->recovering){
          SDL_SetRenderScale(renderer,2,2);SDL_SetRenderDrawColor(renderer,251,191,36,255);
          SDL_RenderDebugText(renderer,20,35,client->server_retry.expired?"Still can't reach the server.":client->server_retry.active?"The Qrazy server is waking up.":client->recovering?"Connection failed - R to retry":"Connecting to game - please wait");
          SDL_RenderDebugText(renderer,20,50,client->server_retry.expired?"Check your internet connection":client->server_retry.active?"Retrying automatically for":"Alt-F4 to close");
          if(client->server_retry.expired||client->server_retry.active)SDL_RenderDebugText(renderer,20,62,client->server_retry.expired?"or ask in the Qrazy Discord.":"up to 5 minutes.");
          if(client->server_retry.active){
            const std::string attempt="Attempt "+std::to_string(client->server_retry.attempt);
            SDL_RenderDebugText(renderer,20,80,attempt.c_str());
            const float dot=20.f+static_cast<float>(attempt.size()*8)+8.f;SDL_FRect mark{dot,83,2,2};SDL_RenderFillRect(renderer,&mark);
            const std::string countdown=client->server_retry.pending?"Connecting...":"Retrying in "+std::to_string(client->server_retry.Countdown(SDL_GetTicks()))+" s";
            SDL_RenderDebugText(renderer,dot+10,80,countdown.c_str());
          }
          SDL_SetRenderScale(renderer,1,1);
          if(client->recovering||client->server_retry.active){SDL_FRect button{40,200,220,48};SDL_SetRenderDrawColor(renderer,52,48,30,255);SDL_RenderFillRect(renderer,&button);SDL_SetRenderDrawColor(renderer,251,191,36,255);SDL_RenderRect(renderer,&button);SDL_SetRenderScale(renderer,2,2);SDL_RenderDebugText(renderer,30,108,"Retry now");SDL_SetRenderScale(renderer,1,1);}
        }else{if(client->view.Ready()&&!client->view.Draw())client->fatal=closing=true;if(client->popup_visible&&client->popup_ready&&client->popup.Ready()&&client->popup_rect.width>0&&client->popup_rect.height>0){int w,h,pw,ph;SDL_GetWindowSize(window,&w,&h);SDL_GetWindowSizeInPixels(window,&pw,&ph);CefRect dip;client->GetViewRect(nullptr,dip);SDL_FRect rect{client->popup_rect.x*static_cast<float>(pw)/dip.width,client->popup_rect.y*static_cast<float>(ph)/dip.height,client->popup_rect.width*static_cast<float>(pw)/dip.width,client->popup_rect.height*static_cast<float>(ph)/dip.height};if(!client->popup.Draw(&rect))client->fatal=closing=true;}}
        if(!SDL_RenderPresent(renderer))client->fatal=closing=true;else ++client->presentation_count;
    };
    while(!closed&&(!result||client->browser)) {
      CefDoMessageLoopWork();
      client->PresentationTick();
      client->TickRetry();
      if(transitioning&&!handoff_acknowledged) {
        std::array<char,43> code{};
        if(!handoff_started&&peer.Take(code)){handoff_started=true;if(peer.Anonymous()){peer.Acknowledge();handoff_acknowledged=true;}else if(!gate.Start(code,request_context))result=8;}
        if(handoff_started&&!peer.Anonymous())gate.Poll();
        if(gate.Failed()||peer.Bad())result=8;
        if(gate.Passed()&&!handoff_acknowledged){peer.Acknowledge();handoff_acknowledged=true;}
      }
      if(transitioning&&handoff_acknowledged&&client->browser) {
        if(peer.Bad())result=8;
        else if(peer.Done()&&client->handoff_waiting){client->handoff_waiting=false;client->browser->GetMainFrame()->LoadURL(kOrigin);}
      }
      if(result&&client->browser){closing=true;}
      for(auto& job:updater_worker.Take()) {
        if(job.epoch!=asset_worker.epoch.load()||closing)continue;
        auto response=CefParseJSON(job.text,JSON_PARSER_RFC);auto reply=response&&response->GetType()==VTYPE_DICTIONARY?response->GetDictionary():QrazyWindows::Error("Update helper failed; no installation was started");
        if(!job.ok)reply=QrazyWindows::Error("Update verification or staging failed. The installed runtime is unchanged; retry Update to download again.");
        if(job.operation=="prepare"&&job.ok&&reply->GetBool("ok")){
          if(client->loading||client->recovering||captured||!Focused()){client->Reply(job.id,QrazyWindows::Error("Return to the focused menu and confirm installation again"));continue;}
          install_requested=true;client->Release("update-install");closing=true;
        }
        client->Reply(job.id,reply);
      }
      for(auto& job:asset_worker.Take())if(job.epoch==asset_worker.epoch.load()) {auto reply=CefParseJSON(job.response,JSON_PARSER_RFC);if(reply&&reply->GetType()==VTYPE_DICTIONARY)client->Reply(job.id,reply->GetDictionary());}
      std::deque<DialogResult> selected;{std::lock_guard<std::mutex> guard(dialog_mutex);selected.swap(dialog_results);}
      for(auto& choice:selected) {
        if(closing||choice.epoch!=asset_worker.epoch.load())continue;
        if(choice.error||std::chrono::steady_clock::now()>=choice.expires){client->Reply(choice.id,QrazyWindows::Error("Config dialog failed or expired"));continue;}
        if(choice.path.empty()){auto cancelled=Dict();cancelled->SetBool("cancelled",true);client->Reply(choice.id,QrazyWindows::Success(QrazyWindows::Value(cancelled)));continue;}
        if(client->loading||client->recovering||captured||!Focused()){client->Reply(choice.id,QrazyWindows::Error("Return to the focused menu and select the config action again"));continue;}
        auto list=CefListValue::Create();list->SetString(0,choice.path);if(choice.save)list->SetString(1,choice.text);
        if(!asset_worker.Submit(choice.id,choice.save?"config-write":"config-read",list))client->Reply(choice.id,QrazyWindows::Error("Config worker unavailable or busy"));
      }
      if(std::chrono::steady_clock::now()>=refresh){EnsureNormalPriority();update_display();client->ResizeChild();refresh=std::chrono::steady_clock::now()+std::chrono::seconds(1);}
      SDL_Event e;auto move_batch=CefListValue::Create(); /* captured-mouse samples from this pass go out as one message */
      while(SDL_PollEvent(&e)) {
        if(e.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED||e.type==SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED||e.type==SDL_EVENT_DISPLAY_ADDED||e.type==SDL_EVENT_DISPLAY_REMOVED)update_display();
        if(((e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_LEFT)||(e.type==SDL_EVENT_KEY_DOWN&&(e.key.key==SDLK_RETURN||e.key.key==SDLK_SPACE)))&&Focused())update_gesture=SDL_GetTicksNS();
        if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)closing=true;
        /* In native mode SDL reports focus loss whenever the CEF child takes keyboard focus; only a real loss of the foreground counts. */
        if((e.type==SDL_EVENT_WINDOW_FOCUS_LOST&&!(native_window&&Focused()))||e.type==SDL_EVENT_WINDOW_MINIMIZED) {
          client->Release("focus");client->selection.clear();client->composing=false;if(client->browser){client->browser->GetHost()->ImeCancelComposition();client->browser->GetHost()->SetAudioMuted(true);client->browser->GetHost()->SetFocus(false);}
          auto d=Dict();d->SetString("type","focus");d->SetBool("focused",false);client->Send(d);
        }
        if(e.type==SDL_EVENT_WINDOW_FOCUS_GAINED){if(client->browser){if(!child_hidden)client->browser->GetHost()->SetFocus(true);client->browser->GetHost()->SetAudioMuted(client->loading||client->recovering);}if(!native_window)SDL_StartTextInput(window);
          /* Chromium refuses requestPointerLock without user activation (the page's capture restore after alt-tab runs on this focus message, often seconds after the last input). A trusted F24 press (unbound, never Escape) grants activation first. */
          if(native_window&&client->browser){CefKeyEvent k;k.windows_key_code=VK_F24;k.native_key_code=0x76<<16;k.is_system_key=false;k.modifiers=0;k.type=KEYEVENT_RAWKEYDOWN;client->browser->GetHost()->SendKeyEvent(k);k.type=KEYEVENT_KEYUP;client->browser->GetHost()->SendKeyEvent(k);}
          auto d=Dict();d->SetString("type","focus");d->SetBool("focused",true);client->Send(d);}
        if(e.type==SDL_EVENT_WINDOW_RESIZED||e.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED){if(native_window)client->ResizeChild();else if(client->browser){client->browser->GetHost()->NotifyScreenInfoChanged();client->browser->GetHost()->WasResized();}client->dirty=true;}
        if(native_window&&(e.type==SDL_EVENT_WINDOW_MAXIMIZED||e.type==SDL_EVENT_WINDOW_RESTORED||e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN))client->ResizeChild();
        if(e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)client->FullscreenState();
        if(e.type==SDL_EVENT_WINDOW_MOVED||e.type==SDL_EVENT_WINDOW_RESIZED||e.type==SDL_EVENT_WINDOW_MAXIMIZED||e.type==SDL_EVENT_WINDOW_RESTORED||e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)saved_window.Observe();
        if(e.type==SDL_EVENT_KEY_DOWN||e.type==SDL_EVENT_KEY_UP)Keyboard(client.get(),e.key);
        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_LEFT&&client->RetryButton(e.button.x,e.button.y))continue;
        if(!client->browser||native_window||!DesktopPolicy::AllowInput(closing,client->recovering,client->loading))continue;
        if(e.type==SDL_EVENT_TEXT_EDITING&&!captured) {
          CefString text(e.edit.text);auto utf=text.ToString16();
          auto index=[&](int points){size_t pos=0;for(int i=0;i<std::max(0,points)&&pos<utf.size();++i){char16_t ch=utf[pos++];if(ch>=0xd800&&ch<=0xdbff&&pos<utf.size()&&utf[pos]>=0xdc00&&utf[pos]<=0xdfff)++pos;}return static_cast<uint32_t>(pos);};
          if(utf.empty()){client->browser->GetHost()->ImeCancelComposition();client->composing=false;}else{client->composing=true;CefCompositionUnderline line;line.range=CefRange(0,static_cast<uint32_t>(utf.size()));line.color=CefColorSetARGB(255,255,255,255);client->browser->GetHost()->ImeSetComposition(text,{line},CefRange(UINT32_MAX,UINT32_MAX),CefRange(index(e.edit.start),index(e.edit.start+std::max(0,e.edit.length))));}
        }
        if(e.type==SDL_EVENT_TEXT_INPUT&&!captured){if(client->composing)client->browser->GetHost()->ImeCommitText(e.text.text,CefRange(UINT32_MAX,UINT32_MAX),0);else{CefString text(e.text.text);for(char16_t ch:text.ToString16()){CefKeyEvent key;key.type=KEYEVENT_CHAR;key.windows_key_code=ch;key.character=key.unmodified_character=ch;key.modifiers=Modifiers(SDL_GetModState());client->browser->GetHost()->SendKeyEvent(key);}}client->composing=false;}
        if(e.type==SDL_EVENT_MOUSE_MOTION){if(captured){auto sample=Dict();sample->SetInt("generation",generation);sample->SetDouble("time",e.motion.timestamp/1e6);sample->SetDouble("dx",e.motion.xrel);sample->SetDouble("dy",e.motion.yrel);move_batch->SetDictionary(move_batch->GetSize(),sample);}else{CefMouseEvent mouse;mouse.x=Dip(e.motion.x);mouse.y=Dip(e.motion.y);mouse.modifiers=MouseModifiers();client->browser->GetHost()->SendMouseMoveEvent(mouse,false);}}
        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN||e.type==SDL_EVENT_MOUSE_BUTTON_UP){CefMouseEvent mouse;mouse.x=Dip(e.button.x);mouse.y=Dip(e.button.y);mouse.modifiers=MouseModifiers();if(e.button.button<=SDL_BUTTON_RIGHT){auto button=e.button.button==SDL_BUTTON_LEFT?MBT_LEFT:e.button.button==SDL_BUTTON_RIGHT?MBT_RIGHT:MBT_MIDDLE;client->browser->GetHost()->SendMouseClickEvent(mouse,button,e.type==SDL_EVENT_MOUSE_BUTTON_UP,std::clamp<int>(e.button.clicks,1,2));}else if(e.button.button==SDL_BUTTON_X1||e.button.button==SDL_BUTTON_X2){auto d=Dict();d->SetString("type","button");d->SetInt("button",e.button.button==SDL_BUTTON_X1?3:4);d->SetBool("down",e.type==SDL_EVENT_MOUSE_BUTTON_DOWN);Uint32 bits=SDL_GetMouseState(nullptr,nullptr);d->SetInt("buttons",(bits&SDL_BUTTON_LMASK?1:0)|(bits&SDL_BUTTON_RMASK?2:0)|(bits&SDL_BUTTON_MMASK?4:0)|(bits&SDL_BUTTON_X1MASK?8:0)|(bits&SDL_BUTTON_X2MASK?16:0));client->Send(d);}}
        if(e.type==SDL_EVENT_MOUSE_WHEEL){CefMouseEvent mouse;mouse.x=Dip(e.wheel.mouse_x);mouse.y=Dip(e.wheel.mouse_y);mouse.modifiers=MouseModifiers()&~static_cast<uint32_t>(EVENTFLAG_CONTROL_DOWN);float sign=e.wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-1.f:1.f;client->browser->GetHost()->SendMouseWheelEvent(mouse,static_cast<int>(client->wheel_x+=120*e.wheel.x*sign),static_cast<int>(client->wheel_y+=120*e.wheel.y*sign));client->wheel_x-=static_cast<int>(client->wheel_x);client->wheel_y-=static_cast<int>(client->wheel_y);}
      }
      if(move_batch->GetSize()&&client->browser){auto d=Dict();d->SetString("type","move");d->SetList("samples",move_batch);client->Send(d);}
      if(captured&&!ConfineCapturedCursor())client->Release("cursor-confinement-lost");
      if(closing&&client->browser&&!close_sent){client->Release("close");client->browser->GetHost()->CloseBrowser(true);close_sent=true;close_sent_ns=SDL_GetTicksNS();}
      if(native_window&&close_sent&&!closed&&SDL_GetTicksNS()-close_sent_ns>5000000000ull){std::fprintf(stderr,"PROTOTYPE native browser did not close in 5 s; exiting\n");closed=true;} /* last resort so a stuck windowed close cannot leave a window that ignores input */
      if(native_window&&client->browser) {
        /* Cards are drawn by SDL under the CEF child, so the child is hidden only while one is showing (recovery or server retry). Ordinary loading keeps the page visible and running. */
        const bool card=client->recovering||client->server_retry.active;
        HWND child=client->browser->GetHost()->GetWindowHandle();
        if(child&&card!=child_hidden) {
          child_hidden=card;ShowWindow(child,card?SW_HIDE:SW_SHOW);
          if(card){SetFocus(hwnd);client->dirty=true;}else{client->ResizeChild(true);client->browser->GetHost()->SetFocus(Focused());}
        }
        /* Same input gate as offscreen mode (no input while loading or recovering): a disabled child window receives no mouse or keys. */
        const bool allow=DesktopPolicy::AllowInput(false,client->recovering,client->loading); /* closing does not disable the child: a close that stalls must not also leave a dead window */
        if(child&&allow!=child_enabled){
          child_enabled=allow;EnableWindow(child,allow);
          if(!allow)SetFocus(hwnd);else if(!child_hidden)client->browser->GetHost()->SetFocus(Focused());
        }
        /* Ctrl+wheel zoom reaches the page as in a browser; undo any zoom within 50 ms (offscreen mode stripped Ctrl from wheel events and reset zoom on load). */
        const Uint64 zoom_now=SDL_GetTicksNS();
        if(pointer_locked&&!lock_was_held)lock_started_ns=zoom_now;
        lock_was_held=pointer_locked;
        if(pointer_locked&&zoom_now-lock_started_ns<10000000000ull)EnumThreadWindows(GetCurrentThreadId(),HideLockBubbleWindow,0);
        if(zoom_now-zoom_checked>=50000000ull){zoom_checked=zoom_now;if(client->browser->GetHost()->GetZoomLevel()!=0.0)client->browser->GetHost()->SetZoomLevel(0.0);}
        if(card&&(client->dirty||client->loading||client->recovering))render_frame();
      }else if(client->dirty||client->loading||client->recovering)render_frame();
      if(frame_clock.Failed()){client->fatal=true;closing=true;}
      client->RequestFrame(frame_clock);
      SDL_Delay(1);
    }
    saved_window.Observe();saved_window.Save();if(client->fatal)result=6;
    if(!result){CefRefPtr<CookieFlushed> flushed=new CookieFlushed;bool requested=CefCookieManager::GetGlobalManager(nullptr)->FlushStore(flushed);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(requested&&!flushed->done&&std::chrono::steady_clock::now()<deadline){CefDoMessageLoopWork();SDL_Delay(1);}std::fprintf(stderr,"PROTOTYPE cookie flush completed=%d\n",flushed->done);if(!requested||!flushed->done)result=7;}
  }
  if(gesture_hook)UnhookWindowsHookEx(gesture_hook);
  updater_worker.Stop();asset_worker.Stop();CefShutdown();app=nullptr;SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return !result&&install_requested?Production::InstallExit:result;
}
} // namespace
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance,LPWSTR,int,void* sandbox,cef_version_info_t*){return Run(instance,sandbox);}
