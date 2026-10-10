// Windows host implementation in progress. No game/input/physics implementation.
#include <windows.h>
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
#include "include/cef_sandbox_win.h"
#include "include/wrapper/cef_helpers.h"
#include "borrowed_texture.h"
#include "../desktop_policy.h"
#include "asset_worker.h"
#include "updater_worker.h"
#include "handoff_startup.h"
#include "server_retry.h"

namespace {
constexpr char kOrigin[]="https://qrazy-game.onrender.com/";
constexpr char kProfile[]="profile-sdlcef-windows";
SDL_Window* window=nullptr;
std::string client_title="Qrazy";
SDL_Renderer* renderer=nullptr;
HWND hwnd=nullptr;
bool closing=false,closed=false,captured=false;
int generation=0,render_rate=0;
#include "render_preferences.h"
RenderPreferences render_preferences;
std::string bridge_source;
QrazyWindows::AssetWorker asset_worker;
Production::Worker updater_worker;bool install_requested=false;Uint64 update_gesture=0;
#include "bridge_binding.h"
#include "window_state.h"
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
int Dip(float coordinate){return static_cast<int>(coordinate/CefScale());}
#include "frame_clock.h"
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
  public CefDownloadHandler,public CefDialogHandler,public CefContextMenuHandler {
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
  Uint64 frame_sent=0;
  void RequestFrame(DisplayBeginClock& clock) {
    clock.Configure(render_preferences.vsync,render_preferences.max_fps);
    if(!browser||closing)return;
    if(!clock.Take())return;
    const Uint64 now=SDL_GetTicksNS();
    const int target=render_preferences.CefTarget(render_rate);
    const bool paced_by_display=render_preferences.vsync&&render_preferences.max_fps>=render_rate;
    if(target<1||(!paced_by_display&&now-frame_sent<1000000000ull/static_cast<Uint64>(target)))return;
    frame_sent=now;browser->GetHost()->SendExternalBeginFrame();
  }
  void PresentationTick(){
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
    SDL_SystemCursor shape=type==CT_HAND?SDL_SYSTEM_CURSOR_POINTER:type==CT_IBEAM?SDL_SYSTEM_CURSOR_TEXT:type==CT_CROSS?SDL_SYSTEM_CURSOR_CROSSHAIR:SDL_SYSTEM_CURSOR_DEFAULT;
    auto next=SDL_CreateSystemCursor(shape);if(next){SDL_SetCursor(next);if(cursor)SDL_DestroyCursor(cursor);cursor=next;}return true;
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> value) override {browser=value;browser->GetHost()->SetFocus(SDL_GetKeyboardFocus()==window);browser->GetHost()->SetAudioMuted(true);}
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
  bool OnFileDialog(CefRefPtr<CefBrowser>,FileDialogMode,const CefString&,const CefString&,const std::vector<CefString>&,const std::vector<CefString>&,const std::vector<CefString>&,CefRefPtr<CefFileDialogCallback> callback) override{callback->Cancel();return true;}
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
    if(!captured)return;captured=false;++generation;
    if(SDL_GetKeyboardFocus()==window)SDL_StartTextInput(window);
    auto data=Dict();data->SetString("type","state");data->SetString("reason",reason);Send(data);
  }
  void Reset(const char* reason){Release(reason);asset_worker.Reset();popup_visible=false;selection.clear();composing=false;if(browser){browser->GetHost()->ImeCancelComposition();browser->GetHost()->SetAudioMuted(true);}}
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
    if((!recovering&&!server_retry.active)||closing||fatal||handoff_waiting||SDL_GetKeyboardFocus()!=window)return false;
    int w=0,h=0,pw=0,ph=0;SDL_GetWindowSize(window,&w,&h);SDL_GetRenderOutputSize(renderer,&pw,&ph);
    if(w<=0||h<=0)return false;x*=static_cast<float>(pw)/w;y*=static_cast<float>(ph)/h;
    if(x<40||x>260||y<200||y>248)return false;
    if(!server_retry.pending)Retry();return true;
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,CefRefPtr<CefRequest> request,bool gesture,bool) override {
    if(handoff_waiting)return !frame->IsMain()||request->GetURL()!="about:blank";
    if(!frame->IsMain())return false;if(!Trusted(request->GetURL())){OpenWebLink(frame,request->GetURL(),gesture);return true;}Reset("navigation");BeginLoad();load_started=SDL_GetTicks();return false;
  }
  void OnLoadStart(CefRefPtr<CefBrowser> b,CefRefPtr<CefFrame> frame,TransitionType) override{if(frame->IsMain()&&b&&b->GetHost()->GetZoomLevel()!=0.0)b->GetHost()->SetZoomLevel(0.0);  // zoom persists per site in the profile; undo earlier Ctrl+scroll zoom
    if(frame->IsMain()&&loading&&!recovering)SDL_SetWindowTitle(window,"Qrazy - connecting");}
  void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int code) override {
    if(!frame->IsMain()||!loading||recovering||!Trusted(frame->GetURL()))return;
    if(code>=400){Fail(code==502||code==503||code==504);return;}if(FinishLoad(true,code)){server_retry.Stop();SDL_SetWindowTitle(window,client_title.c_str());browser->GetHost()->SetFocus(SDL_GetKeyboardFocus()==window);browser->GetHost()->SetAudioMuted(SDL_GetKeyboardFocus()!=window);}
  }
  void OnLoadError(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,ErrorCode code,const CefString&,const CefString& url) override{if(frame->IsMain()&&loading&&!recovering&&Trusted(url)&&code!=ERR_ABORTED)Fail(code==ERR_CONNECTION_REFUSED||code==ERR_CONNECTION_RESET||code==ERR_CONNECTION_CLOSED||code==ERR_CONNECTION_FAILED||code==ERR_CONNECTION_TIMED_OUT||code==ERR_TIMED_OUT||code==ERR_NAME_NOT_RESOLVED||code==ERR_INTERNET_DISCONNECTED||code==ERR_NETWORK_CHANGED);}
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser>,TerminationStatus,int,const CefString&) override{Fail();}
  void OnPaint(CefRefPtr<CefBrowser>,PaintElementType,const RectList&,const void*,int,int) override{fatal=true;closing=true;Release("CPU-paint-rejected");}
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>,PaintElementType type,const RectList&,const CefAcceleratedPaintInfo& info) override {
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
    bool menu=!closing&&!recovering&&!loading&&!captured&&SDL_GetKeyboardFocus()==window;
    if(op=="vsync-get"||op=="max-fps-get"){Reply(id,op=="vsync-get"?render_preferences.VSyncState():render_preferences.MaxFpsState());return true;}
    if(op=="vsync-set"||op=="max-fps-set"){
      if(closing||recovering||loading||SDL_GetKeyboardFocus()!=window){Reply(id,QrazyWindows::Error("Return to the focused game before changing rendering settings"));return true;}
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
      if((!menu && !(op=="config-export" && !closing && !recovering && !loading && SDL_GetKeyboardFocus()==window)) || DemoDownload::active || dialog_active.exchange(true)){result->SetBool("ok",false);result->SetString("error","Config dialogs require a focused menu and no active dialog");Reply(id,result);return true;}
      auto selected=new DialogResult{id,asset_worker.epoch.load(),op=="config-export"};
      static const SDL_DialogFileFilter filter={"Qrazy configuration","cfg"};
      if(selected->save){std::string name=d?d->GetString("name"):CefString();selected->text=d?d->GetString("text"):CefString();
        if(name.size()<5||name.size()>100||name.substr(name.size()-4)!=".cfg"||name.find_first_of("/\\\r\n")!=std::string::npos||name.find('\0')!=std::string::npos||selected->text.size()>1048576||selected->text.find('\0')!=std::string::npos){delete selected;dialog_active=false;result->SetBool("ok",false);result->SetString("error","Expected a cfg filename and at most 1 MiB of text");Reply(id,result);return true;}
        Release("export-save");SDL_ShowSaveFileDialog(FileChosen,selected,window,&filter,1,name.c_str());
      }else SDL_ShowOpenFileDialog(FileChosen,selected,window,&filter,1,nullptr,false);
      return true;
    }else if(op=="update-notice"||op=="update-state"||op=="update-check"||op=="update-stage"||op=="update-install") {
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
      auto report=Dict();report->SetString("platform","Windows SDL3 + CEF");report->SetInt("cefTarget",render_preferences.CefTarget(render_rate));report->SetBool("sdlVSync",render_preferences.vsync);report->SetString("acceptance","Compiled source; manual Windows GPU, sandbox and desktop acceptance pending");Reply(id,QrazyWindows::Success(QrazyWindows::Value(report)));return true;
    }else if(op=="clock")result->SetDouble("time",SDL_GetTicksNS()/1e6);
    else if(op=="capture") {
      const char* why=closing?"closing":recovering?"recovering":loading?"loading":dialog_active?"dialog":SDL_GetKeyboardFocus()!=window?"no-keyboard-focus":nullptr;
      // The free cursor may have left the window during chat/console. The window
      // still has keyboard focus, so bring the cursor back before capturing.
      if(!why&&SDL_GetMouseFocus()!=window){int w=0,h=0;SDL_GetWindowSize(window,&w,&h);SDL_WarpMouseInWindow(window,w/2.f,h/2.f);}
      bool ok=!why&&SDL_SetWindowRelativeMouseMode(window,true);
      if(!why&&!ok)why="relative-mode-failed";
      if(why)result->SetString("reason",why);
      captured=ok;if(ok){SDL_StopTextInput(window);if(!SDL_DisableScreenSaver())std::fprintf(stderr,"PROTOTYPE idle inhibition failed\n");}
      result->SetBool("ok",ok);result->SetInt("generation",++generation);
    }else if(op=="quit"){Release("quit");closing=true;}
    else if(op=="refresh-game") {
      bool ok=browser&&!closing&&!fatal&&!handoff_waiting&&!loading&&SDL_GetKeyboardFocus()==window;
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
  Production::CleanEnvironment();Production::Pins pins;if(!pins.Check(base))return 2;
  updater_worker.root=base;
  auto profile=base/kProfile;std::error_code error;std::filesystem::create_directories(profile,error);
  if(error||!QrazyWindows::NoReparse(profile))return 2;ProfileLock lock;if(!lock.Acquire(profile/"host.lock"))return 2;
  render_preferences.Load(profile);
  asset_worker.Start(profile);
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER,"1");SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE,"0");
  SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,"0");SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE,"1");
  if(!SDL_Init(SDL_INIT_VIDEO))return 3;
  window=SDL_CreateWindow(client_title.c_str(),1280,800,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if(!window){SDL_Quit();return 3;}
  hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
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
  SDL_SetWindowTitle(window,client_title.c_str());
  int result=0;
  {
    auto request_context=CefRequestContext::GetGlobalContext();
    const bool transitioning=std::filesystem::exists(base/L".qrazy-transition");
    Handoff::StartupPeer peer;Handoff::PreGameGate gate;bool handoff_started=false,handoff_acknowledged=false;
    if(transitioning&&!peer.Start(base))result=8;
    CefRefPtr<Client> client=new Client;
    DisplayBeginClock frame_clock;
    frame_clock.Configure(render_preferences.vsync,render_preferences.max_fps);
    if(!frame_clock.Start(hwnd))result=5;
    client->handoff_waiting=transitioning;
    if(!client->view.Initialize(device)||!client->popup.Initialize(device))result=5;
    CefWindowInfo info;info.SetAsWindowless(hwnd);info.shared_texture_enabled=true;info.external_begin_frame_enabled=true;
    CefBrowserSettings bs;bs.windowless_frame_rate=render_preferences.CefTarget(render_rate);bs.background_color=CefColorSetARGB(255,0,0,0);
    if(result||!CefBrowserHost::CreateBrowser(info,client,transitioning?"about:blank":kOrigin,bs,nullptr,request_context)){if(!result)result=5;}
    bool close_sent=false;auto refresh=std::chrono::steady_clock::now();SDL_StartTextInput(window);
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
          if(client->loading||client->recovering||captured||SDL_GetKeyboardFocus()!=window){client->Reply(job.id,QrazyWindows::Error("Return to the focused menu and confirm installation again"));continue;}
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
        if(client->loading||client->recovering||captured||SDL_GetKeyboardFocus()!=window){client->Reply(choice.id,QrazyWindows::Error("Return to the focused menu and select the config action again"));continue;}
        auto list=CefListValue::Create();list->SetString(0,choice.path);if(choice.save)list->SetString(1,choice.text);
        if(!asset_worker.Submit(choice.id,choice.save?"config-write":"config-read",list))client->Reply(choice.id,QrazyWindows::Error("Config worker unavailable or busy"));
      }
      if(std::chrono::steady_clock::now()>=refresh){int rate=RefreshRate();if(rate&&rate!=render_rate){render_rate=rate;if(client->browser)client->browser->GetHost()->SetWindowlessFrameRate(render_preferences.CefTarget(rate));}refresh=std::chrono::steady_clock::now()+std::chrono::seconds(1);}
      SDL_Event e;auto move_batch=CefListValue::Create(); /* captured-mouse samples from this pass go out as one message */
      while(SDL_PollEvent(&e)) {
        if(((e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_LEFT)||(e.type==SDL_EVENT_KEY_DOWN&&(e.key.key==SDLK_RETURN||e.key.key==SDLK_SPACE)))&&SDL_GetKeyboardFocus()==window)update_gesture=SDL_GetTicksNS();
        if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)closing=true;
        if(e.type==SDL_EVENT_WINDOW_FOCUS_LOST||e.type==SDL_EVENT_WINDOW_MINIMIZED) {
          client->Release("focus");client->selection.clear();client->composing=false;if(client->browser){client->browser->GetHost()->ImeCancelComposition();client->browser->GetHost()->SetAudioMuted(true);client->browser->GetHost()->SetFocus(false);}
          auto d=Dict();d->SetString("type","focus");d->SetBool("focused",false);client->Send(d);
        }
        if(e.type==SDL_EVENT_WINDOW_FOCUS_GAINED){if(client->browser){client->browser->GetHost()->SetFocus(true);client->browser->GetHost()->SetAudioMuted(client->loading||client->recovering);}SDL_StartTextInput(window);auto d=Dict();d->SetString("type","focus");d->SetBool("focused",true);client->Send(d);}
        if(e.type==SDL_EVENT_WINDOW_RESIZED||e.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED){if(client->browser){client->browser->GetHost()->NotifyScreenInfoChanged();client->browser->GetHost()->WasResized();}client->dirty=true;}
        if(e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)client->FullscreenState();
        if(e.type==SDL_EVENT_WINDOW_MOVED||e.type==SDL_EVENT_WINDOW_RESIZED||e.type==SDL_EVENT_WINDOW_MAXIMIZED||e.type==SDL_EVENT_WINDOW_RESTORED||e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)saved_window.Observe();
        if(e.type==SDL_EVENT_KEY_DOWN||e.type==SDL_EVENT_KEY_UP)Keyboard(client.get(),e.key);
        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_LEFT&&client->RetryButton(e.button.x,e.button.y))continue;
        if(!client->browser||!DesktopPolicy::AllowInput(closing,client->recovering,client->loading))continue;
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
      if(closing&&client->browser&&!close_sent){client->Release("close");client->browser->GetHost()->CloseBrowser(true);close_sent=true;}
      if(client->dirty||client->loading||client->recovering)render_frame();
      if(frame_clock.Failed()){client->fatal=true;closing=true;}
      client->RequestFrame(frame_clock);
      SDL_Delay(1);
    }
    saved_window.Observe();saved_window.Save();if(client->fatal)result=6;
    if(!result){CefRefPtr<CookieFlushed> flushed=new CookieFlushed;bool requested=CefCookieManager::GetGlobalManager(nullptr)->FlushStore(flushed);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(requested&&!flushed->done&&std::chrono::steady_clock::now()<deadline){CefDoMessageLoopWork();SDL_Delay(1);}std::fprintf(stderr,"PROTOTYPE cookie flush completed=%d\n",flushed->done);if(!requested||!flushed->done)result=7;}
  }
  updater_worker.Stop();asset_worker.Stop();CefShutdown();app=nullptr;SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return !result&&install_requested?Production::InstallExit:result;
}
} // namespace
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance,LPWSTR,int,void* sandbox,cef_version_info_t*){return Run(instance,sandbox);}
