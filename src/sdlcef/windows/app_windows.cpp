// Windows host implementation in progress. No game/input/physics implementation.
#include <cctype>
#include <windows.h>
#include <shellapi.h>
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
#include <random>
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
#include "../date_binding.h"
#include "date_format.h"

#ifdef QRAZY_NATIVE_CLIENT
#include "qrazy_core_api.h"   /* QC_INTERFACE_VERSION: names the core manifest on the site */
#include "../native_hub.h"
#endif
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
/* Dev-only "true client" (CMake QRAZY_TRUE_CLIENT_DIR, only with CLIENT_MODE dev): a Vulkan renderer owns the window's swapchain and runs on its own thread; F2 hides the CEF child to show it. Defined in the renderer's source. */
void tc_start(HWND hwnd,const char* cmdline);void tc_set_visible(bool visible);void tc_stop();void tc_push_state(const uint8_t* data,size_t size);
#ifdef QRAZY_NATIVE_CLIENT
/* Native client (dev builds, CMake QRAZY_NATIVE_CLIENT_DIR): the page tells the host when a match starts or ends (qrazyNativeMatch, bridge_binding.h); the host starts or stops the native view. The token never touches the command line. */
void tc_set_frame_cap(int fps);
bool tc_take_error(char* buf,size_t n);void tc_set_menu_request(void (*f)());static std::atomic<bool> native_menu_requested{false};static void NativeMenuRequest(){native_menu_requested=true;}
void tc_core_prepare(const char* cacheDir,bool fetch,bool staging);void tc_core_wait();void tc_set_tab(void (*f)(bool));bool tc_get_pose(double* out14);bool tc_take_started();bool tc_hud_feed();bool tc_audio_feed();void tc_hud_ready();std::string tc_error_code();bool tc_take_pose_request();static std::string NativeMenuScript(){char b[400];double p[14];if(tc_get_pose(p))std::snprintf(b,sizeof b,"window.__qtcOn=false;window.dispatchEvent(new CustomEvent('qrazy-native-menu',{detail:{x:%.3f,y:%.3f,z:%.3f,yaw:%.6f,pitch:%.6f}}))",p[0],p[1],p[2],p[3],p[4]);else std::snprintf(b,sizeof b,"window.__qtcOn=false;window.dispatchEvent(new Event('qrazy-native-menu'))");return b;}   /* the Esc event carries where the player is: the page's menu background can look from there */
void tc_set_sim_out(void (*f)(const char*));static std::mutex native_sim_mutex;static std::vector<std::string> native_sim;static void NativeSimOut(const char* json){std::lock_guard<std::mutex> lock(native_sim_mutex);if(native_sim.size()<64)native_sim.push_back(json);}   /* sim events for the page's HUD texts, queued from the render thread */
void tc_log(const char* message);
static void SendNativeShortcut(CefRefPtr<CefBrowserHost> host,bool tab,bool down){
  const UINT vk=tab?VK_TAB:VK_ESCAPE;
  const char* code=tab?"Tab":"Escape";
  const bool shift=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0,ctrl=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0,alt=(GetAsyncKeyState(VK_MENU)&0x8000)!=0;
  char js[900];std::snprintf(js,sizeof js,"(function(){const e=new KeyboardEvent('%s',{code:'%s',key:'Unidentified',keyCode:%u,which:%u,shiftKey:%s,ctrlKey:%s,altKey:%s,bubbles:true,cancelable:true,composed:true});const c=document.querySelector('#qconsole.open .qcon-input'),w=document.getElementById('chat-input-wrap'),t=c||(w&&!w.hidden?document.getElementById('chat-input'):null)||document.activeElement||document;if(t!==document&&t.focus&&document.activeElement!==t)t.focus();t.dispatchEvent(e);})()",down?"keydown":"keyup",code,static_cast<unsigned>(vk),static_cast<unsigned>(vk),shift?"true":"false",ctrl?"true":"false",alt?"true":"false");host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js,"qrazy-native-key",0);
}
void tc_board(const char* json);void tc_settings(const char* json);void tc_hud(const char* json);static std::atomic<int> native_tab_edge{0};static void NativeTab(bool down){native_tab_edge=down?1:2;}   /* Tab in the native view: the main loop passes it to the page, which reports the information board */
#include "nc_hub.h"
bool tc_hub_ready();
void tc_hub_service();
void tc_hub_shutdown();

static std::mutex native_mutex;static std::vector<std::string> native_requests;   /* a queue: ops sent back to back (mode, setspawn) all arrive */static std::string native_running_cmd;   /* the command line of the running native match: an identical start while it runs is ignored */
/* The native view draws into its own child window of the host window (never a see-through page window): the host creates it on the main thread, the native thread only makes a Vulkan surface on it. While it is shown the page's child window is hidden. */
int tc_click(int x,int y,int* element,int* index,char* url,int cap);void tc_log_mouse(int kind,int x,int y,bool shift);void tc_log_wheel(double notches);
static std::mutex native_click_mutex;static std::vector<std::pair<int,int>> native_clicks;   /* clicks on the native view's buttons, for the page: (element, index) */
/* The one place that says how a click on a native element reaches the page: window.qrazyNativeClick({element,index}) for every element (map info 16: index -1 = Close; links are opened by the client itself, never sent; race panel 22 / race invite 23: index = button position in text order). */
/* Element 1000 = the x of the open chat box / console: the page gets the synthetic Escape key (the same path as the keyboard), see the consumer of native_clicks. */
static std::string NativeClickScript(int element,int index){return "window.qrazyNativeClick&&window.qrazyNativeClick({element:"+std::to_string(element)+",index:"+std::to_string(index)+"})";}
void tc_sim_op(const char* json);   /* an op of the page for the running sim: respawn, mode, cmd, giveitem, stat, removepowerups, pose (the raw JSON) */
bool tc_typing();   /* a chat / console box is open */
void tc_set_js_out(void (*f)(const char*));void tc_text_unit(unsigned unit);void tc_text_commit(const char* utf8);void tc_text_preedit(const char* utf8,unsigned a,unsigned b,unsigned c,unsigned d);bool tc_text_composing();bool tc_caret_rect(float* out4);
static std::mutex native_js_mutex;static std::vector<std::string> native_js;static void NativeJsOut(const char* js){std::lock_guard<std::mutex> lock(native_js_mutex);if(native_js.size()<256)native_js.push_back(js);}   /* page messages of the core's text field (textState / textSubmit / textEscapeReleased), queued from the render thread, run in the hidden page by the main loop */
#include <imm.h>
#pragma comment(lib,"imm32.lib")
static std::string WideToUtf8(const wchar_t* w,int n){if(n<=0)return std::string();const int len=WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);std::string out(static_cast<size_t>(len),'\0');if(len>0)WideCharToMultiByte(CP_UTF8,0,w,n,&out[0],len,nullptr,nullptr);return out;}
bool tc_cursor();   /* the mouse is free: chat / console open, or a clickable element on screen */
static bool native_drag=false;   /* a log selection drag is running (the view also holds the mouse capture while the mouse is free, so GetCapture alone no longer says it) */
static LRESULT CALLBACK NativeViewProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_ERASEBKGND)return 1;
  const int x=(short)LOWORD(l),y=(short)HIWORD(l);const bool shift=(w&MK_SHIFT)!=0;
  if(m==WM_LBUTTONDOWN){   /* a clickable element first (map info, race panel, race invite), else the open chat box / console log (selection) */
    char url[1024]="";int element=0,index=0;const int r=tc_click(x,y,&element,&index,url,sizeof url);
    if(r==1){std::lock_guard<std::mutex> lock(native_click_mutex);native_clicks.emplace_back(element,index);}
    else if(r==2){if(!strncmp(url,"https://",8)||!strncmp(url,"http://",7))ShellExecuteA(nullptr,"open",url,nullptr,nullptr,SW_SHOWNORMAL);}
    else if(tc_typing()){SetCapture(h);native_drag=true;tc_log_mouse(0,x,y,shift);}
    return 0;}
  if(m==WM_MOUSEMOVE){if(native_drag||tc_typing())tc_log_mouse(1,x,y,shift);return 0;}
  if(m==WM_LBUTTONUP){if(native_drag){native_drag=false;tc_log_mouse(2,x,y,shift);}return 0;}
  if(m==WM_MOUSEWHEEL){if(tc_typing())tc_log_wheel(GET_WHEEL_DELTA_WPARAM(w)/120.0);return 0;}
  if(m==WM_SETCURSOR){SetCursor(tc_cursor()?LoadCursorW(nullptr,IDC_ARROW):nullptr);return TRUE;}
  /* The core's text field: characters and IME text arrive here (the native view holds the keyboard focus while a box is open); the keys come from raw input on the render thread. */
  if(m==WM_CHAR){if(tc_typing())tc_text_unit(static_cast<unsigned>(w));return 0;}
  if(m==WM_IME_SETCONTEXT){return DefWindowProcW(h,m,w,l&~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));}   /* the engine draws the composition; the system keeps its candidate list */
  if(m==WM_IME_STARTCOMPOSITION)return 0;
  if(m==WM_IME_ENDCOMPOSITION){tc_text_preedit("",0,0,0,0);return 0;}
  if(m==WM_IME_COMPOSITION){
    if(HIMC imc=ImmGetContext(h)){
      if(l&GCS_RESULTSTR){const LONG n=ImmGetCompositionStringW(imc,GCS_RESULTSTR,nullptr,0);if(n>0){std::wstring ws(static_cast<size_t>(n)/2,L'\0');ImmGetCompositionStringW(imc,GCS_RESULTSTR,&ws[0],static_cast<DWORD>(n));tc_text_commit(WideToUtf8(ws.data(),static_cast<int>(ws.size())).c_str());}}
      if(l&GCS_COMPSTR){const LONG n=ImmGetCompositionStringW(imc,GCS_COMPSTR,nullptr,0);std::wstring ws(n>0?static_cast<size_t>(n)/2:0,L'\0');if(n>0)ImmGetCompositionStringW(imc,GCS_COMPSTR,&ws[0],static_cast<DWORD>(n));
        const auto bytes=[&](size_t units){return static_cast<unsigned>(WideToUtf8(ws.data(),static_cast<int>(std::min(units,ws.size()))).size());};
        const LONG cur=ImmGetCompositionStringW(imc,GCS_CURSORPOS,nullptr,0);const unsigned caret=bytes(cur<0?0:static_cast<size_t>(cur));unsigned ulFrom=0,ulTo=0;
        const LONG an=ImmGetCompositionStringW(imc,GCS_COMPATTR,nullptr,0);if(an>0&&static_cast<size_t>(an)==ws.size()){std::vector<BYTE> at(static_cast<size_t>(an));ImmGetCompositionStringW(imc,GCS_COMPATTR,at.data(),static_cast<DWORD>(an));size_t a=0,b=0;bool found=false;for(size_t i=0;i<at.size();++i)if(at[i]==ATTR_TARGET_CONVERTED||at[i]==ATTR_TARGET_NOTCONVERTED){if(!found){a=i;found=true;}b=i+1;}if(found){ulFrom=bytes(a);ulTo=bytes(b);}}   /* the clause being converted; none: the whole composition is underlined */
        tc_text_preedit(WideToUtf8(ws.data(),static_cast<int>(ws.size())).c_str(),caret,caret,ulFrom,ulTo);}
      else tc_text_preedit("",0,0,0,0);
      ImmReleaseContext(h,imc);}
    return 0;}
  return DefWindowProcW(h,m,w,l);}
static HWND native_child=nullptr;
static HWND EnsureNativeView(HWND parent){
  if(native_child)return native_child;
  WNDCLASSW wc{};wc.lpfnWndProc=NativeViewProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"QrazyNativeView";wc.hbrBackground=nullptr;RegisterClassW(&wc);
  RECT area{};GetClientRect(parent,&area);
  native_child=CreateWindowExW(0,L"QrazyNativeView",L"",WS_CHILD|WS_CLIPSIBLINGS,0,0,std::max<LONG>(1,area.right),std::max<LONG>(1,area.bottom),parent,nullptr,GetModuleHandleW(nullptr),nullptr);
  return native_child;
}
using QrazyNativeMatch::NativeMatch;
using QrazyNativeMatch::NativeText;
/* The page's view of the match (window.qrazyNativeMatchState): 'started', 'refused' (code, reason) and 'ended' (reason), each at most once per match. 0 no match, 1 start accepted, 2 started. Main loop only. */
static int native_match_state=0;
static std::string NativeJsText(const std::string& s){std::string r="\"";for(unsigned char c:s.substr(0,200)){if(c=='"'||c=='\\'){r+='\\';r+=static_cast<char>(c);}else if(c<32)r+=' ';else r+=static_cast<char>(c);}return r+"\"";}
static std::string NativeStateScript(const char* state,const std::string& code,const std::string& reason,bool hudFeed=false,bool audioFeed=false){std::string j=std::string("window.qrazyNativeMatchState&&window.qrazyNativeMatchState({state:'")+state+"'";if(!code.empty())j+=",code:'"+code+"'";if(!reason.empty())j+=",reason:"+NativeJsText(reason);if(hudFeed)j+=",hudFeed:1";if(audioFeed)j+=",audioFeed:1";return j+"})";}
static bool NativeReportOp(const std::string& r){return r.find("\"runack\"")!=std::string::npos&&r.size()<128||r.compare(0,32,"{\"op\":\"capture\"",15)==0||r.rfind("{\"op\":\"watchsound\"",0)==0;}   /* ops the native client answers while the menu covers the match */

static std::function<void(std::string)> native_hub_post;
static void NativeHubEvent(const nc::HubEvent& event) {
  if(native_hub_post)native_hub_post(QrazyNativeMatch::EncodeHubEvent(event));
}

#endif
static void QuitLog(const char* s){std::fprintf(stderr,"PROTOTYPE %s\n",s);std::fflush(stderr);
#ifdef QRAZY_NATIVE_CLIENT
  tc_log(s);
#endif
}
static void StartQuitWatchdog(int code){static std::atomic<bool> started{false};if(started.exchange(true))return;std::thread([code]{Sleep(8000);QuitLog("quit watchdog: process still alive 8 s after close was requested; terminating");Sleep(100);TerminateProcess(GetCurrentProcess(),static_cast<UINT>(code));}).detach();}   /* last resort: a stuck CEF close or native-view join must never leave the client running */
static bool tc_synth_flag=false;static std::atomic<bool> tc_view_on{false}; /* F2 view active; the synthetic page script only takes pointer lock / moves its camera while this is true, so the game's menus stay usable */ /* dev test: the browser process runs a small page script that sends synthetic game state through qrazyFrame() */
static const char* const kTcSynthJs=R"JS((()=>{if(window.__qtcSynth||typeof qrazyFrame!=='function')return;window.__qtcSynth=true;const b=new ArrayBuffer(1136),dv=new DataView(b),t0=performance.now(),enc=new TextEncoder();let f=0,last=t0;
const chat=enc.encode('^2Synthetic sender^7: WASD moves, click then move the mouse to look (Esc releases), Shift = fast\n^3Page^7: the camera below is driven by input that reached THIS page');
const keys=new Set();let yaw=100,pitch=0,px=1824,py=804,pz=6092;
addEventListener('keydown',e=>{keys.add(e.code);},true);addEventListener('keyup',e=>{keys.delete(e.code);},true);window.addEventListener('blur',()=>keys.clear());
addEventListener('mousedown',()=>{if(window.__qtcOn&&!document.pointerLockElement&&document.documentElement.requestPointerLock)document.documentElement.requestPointerLock();},true);
addEventListener('mousemove',e=>{if(window.__qtcOn&&document.pointerLockElement){yaw-=e.movementX*0.1;pitch=Math.max(-89,Math.min(89,pitch-e.movementY*0.1));}},true);
const pump=()=>{const now=performance.now(),dt=Math.min(0.1,(now-last)/1000),t=(now-t0)/1000;last=now;
const sp=(keys.has('ShiftLeft')?900:300)*dt,r=Math.PI/180,fx=Math.cos(yaw*r),fz=-Math.sin(yaw*r),rx=Math.sin(yaw*r),rz=Math.cos(yaw*r);
if(keys.has('KeyW')){px+=fx*sp;pz+=fz*sp;}if(keys.has('KeyS')){px-=fx*sp;pz-=fz*sp;}if(keys.has('KeyD')){px+=rx*sp;pz+=rz*sp;}if(keys.has('KeyA')){px-=rx*sp;pz-=rz*sp;}if(keys.has('Space'))py+=sp;if(keys.has('ControlLeft'))py-=sp;
dv.setUint32(0,0x31435451,true);dv.setUint32(4,1,true);dv.setUint32(8,1136,true);dv.setBigUint64(16,BigInt(f++),true);dv.setFloat64(24,t,true);
dv.setFloat64(32,px,true);dv.setFloat64(40,py,true);dv.setFloat64(48,pz,true);dv.setFloat64(56,yaw,true);dv.setFloat64(64,pitch,true);dv.setFloat64(72,100,true);
dv.setInt32(80,Math.max(1,100-((t*3)|0)%90),true);dv.setInt32(84,50,true);dv.setInt32(88,25,true);dv.setInt32(92,((t*1000)|0)+83456,true);dv.setInt32(96,(640+300*Math.sin(t))|0,true);dv.setInt32(100,4,true);
dv.setUint32(104,chat.length,true);new Uint8Array(b,112,chat.length).set(chat);qrazyFrame(b);requestAnimationFrame(pump);};pump();})();)JS";
#define TC_OR ||tc_on
#else
#define TC_OR
#endif
namespace {
bool localhost_mode=QRAZY_DEV_MODE;
const char* kOrigin=QrazyDestination::Origin(localhost_mode);
SDL_Window* window=nullptr;
std::string client_title="Qrazy";
SDL_Renderer* renderer=nullptr;
HWND hwnd=nullptr;
std::atomic<bool> closing{false},closed{false},captured{false}; /* atomic: read by CEF callbacks on CEF's UI thread in multi-threaded mode */
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
#include "net_diag.h"
// Opt-in windowed mode: CEF owns a child HWND that presents and receives input itself. The SDL window then only hosts it and the native status cards.
bool native_window=false,startup_vsync=true;std::atomic<bool> pointer_locked{false}; /* pointer_locked: the page reports Chromium pointer lock (native mode only) */
/* Multi-threaded CEF message loop (native window only, not during the login handoff): CEF's UI thread is then separate from this (SDL) thread, so raw input
   is handled without waiting for this loop. Every CEF browser call from this thread is posted to CEF's UI thread (OnUi), and every CEF callback that touches
   SDL or loop state is posted back here (ToMain, run by SDL_PollEvent). Neither direction ever waits for the other, so they cannot deadlock.
   With mt_loop false (offscreen backup, handoff) both helpers run the code inline and behave exactly as the single-threaded client did. */
bool mt_loop=false;
template<class F> class UiTask final : public CefTask {
 public:explicit UiTask(F f):f_(std::move(f)){}
 void Execute() override{f_();}
 private:F f_;IMPLEMENT_REFCOUNTING(UiTask);
};
template<class F> void OnUi(F f){if(!mt_loop||CefCurrentlyOn(TID_UI)){f();return;}CefPostTask(TID_UI,new UiTask<F>(std::move(f)));}
template<class F> bool ToMain(F f){ /* true: queued for this loop's thread, the caller returns; false: already there, the caller runs its code inline */
  if(!mt_loop||SDL_IsMainThread())return false;
  F* copy=new F(std::move(f));
  if(!SDL_RunOnMainThread([](void* p){std::unique_ptr<F> owned(static_cast<F*>(p));(*owned)();},copy,false))delete copy;
  return true;
}
// In native mode the child HWND holds keyboard focus, so SDL reports the top-level window as unfocused; use the OS foreground instead.
bool Focused(){return native_window?GetForegroundWindow()==hwnd:SDL_GetKeyboardFocus()==window;}
std::string bridge_source;
QrazyWindows::AssetWorker asset_worker;
Production::Worker updater_worker;bool install_requested=false;std::atomic<Uint64> update_gesture{0};
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
/* A save dialog opened right after a typed command (/writeconfig + Enter) showed no mouse pointer; moving the mouse did not bring it back. The game's page-side fix for browsers
   (qlrace2 browser.js nudgeCursor) is a pointer lock and unlock just before the dialog, because Chromium then moves the real cursor. Native dialogs bypass that path, so do the
   same here: move the cursor one pixel and back right before the dialog opens. */
void NudgeCursor(){POINT p;if(!GetCursorPos(&p))return;const int step=p.x>0?-1:1;SetCursorPos(p.x+step,p.y);SetCursorPos(p.x,p.y);}
bool lock_was_held=false;Uint64 lock_started_ns=0;
HHOOK gesture_hook=nullptr,ui_gesture_hook=nullptr; /* ui_gesture_hook: the same hook on CEF's UI thread (multi-threaded mode), where the browser child's clicks arrive */
LRESULT CALLBACK GestureHook(int code,WPARAM message,LPARAM data){if(code>=0&&(message==WM_LBUTTONDOWN||message==WM_NCLBUTTONDOWN)&&GetForegroundWindow()==hwnd)update_gesture=SDL_GetTicksNS();return CallNextHookEx(nullptr,code,message,data);}
struct DialogResult {int id;unsigned epoch;bool save,error=false;std::string path,text;std::chrono::steady_clock::time_point expires=std::chrono::steady_clock::now()+std::chrono::minutes(5);};
std::mutex dialog_mutex;std::deque<DialogResult> dialog_results;std::atomic<bool> dialog_active{false};
void SDLCALL FileChosen(void* userdata,const char* const* files,int) {
  std::unique_ptr<DialogResult> result(static_cast<DialogResult*>(userdata));result->error=!files;
  if(files&&files[0]){result->path=files[0];if(result->save){const std::string& p=result->path;if(p.size()<4||_stricmp(p.c_str()+p.size()-4,".cfg")!=0)result->path+=".cfg";}}std::lock_guard<std::mutex> lock(dialog_mutex);dialog_results.push_back(std::move(*result));dialog_active=false;
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
/* Steam login for the desktop client: the page asks for it (qrazyDesktop.openSteam), the real browser opens the sign-in URL, and this polls
   /auth/steam/claim?c=<code>. The server answers 302 once the browser finished (setting the session cookie in this client's cookie jar), and the
   game page then loads the returned location. Only one claim runs at a time; a new code or a cancel stops the old one. UI thread only. */
class SteamClaim final : public CefURLRequestClient {
 public:
  using Done=std::function<void(const std::string&)>;
  using Fail=std::function<void(const char*)>;
  static bool ValidCode(const std::string& code){
    if(code.size()<32||code.size()>64)return false;
    for(char c:code)if(!std::isalnum(static_cast<unsigned char>(c))&&c!='-'&&c!='_')return false;
    return true;
  }
  static void Cancel(){current_.clear();}
  static void Start(const std::string& origin,const std::string& code,Done done,Fail fail){
    current_=code;
    Poll(origin,code,std::move(done),std::move(fail),std::chrono::steady_clock::now()+std::chrono::minutes(10));
  }
 private:
  SteamClaim(std::string origin,std::string code,Done done,Fail fail,std::chrono::steady_clock::time_point deadline):origin_(std::move(origin)),code_(std::move(code)),done_(std::move(done)),fail_(std::move(fail)),deadline_(deadline){}
  static void Poll(const std::string& origin,const std::string& code,Done done,Fail fail,std::chrono::steady_clock::time_point deadline){
    if(code!=current_)return;
    auto request=CefRequest::Create();request->SetURL(origin+"/auth/steam/claim?c="+code);request->SetMethod("GET");
    request->SetFlags(UR_FLAG_STOP_ON_REDIRECT|UR_FLAG_SKIP_CACHE|UR_FLAG_ALLOW_STORED_CREDENTIALS); /* without the last flag the session cookie of the 302 is not saved */
    request->SetFirstPartyForCookies(origin+"/"); /* the cookie is SameSite=Strict */
    CefURLRequest::Create(request,new SteamClaim(origin,code,std::move(done),std::move(fail),deadline),CefRequestContext::GetGlobalContext());
  }
  void Again(int delay_ms){
    if(code_!=current_)return;
    if(std::chrono::steady_clock::now()>=deadline_){current_.clear();if(fail_)fail_("timeout");return;}
    auto self=CefRefPtr<SteamClaim>(this);
    CefPostDelayedTask(TID_UI,new UiTask<std::function<void()>>([self]{Poll(self->origin_,self->code_,self->done_,self->fail_,self->deadline_);}),delay_ms);
  }
  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override{
    if(code_!=current_)return;
    auto response=request->GetResponse();const int status=response?response->GetStatus():0;
    if(status==301||status==302||status==303){
      std::string location=response->GetHeaderByName("Location").ToString();
      if(!location.empty()&&location[0]=='/')location=origin_+location;
      if(Trusted(location)){current_.clear();if(done_)done_(location);return;}
      current_.clear();if(fail_)fail_("failed");return;
    }
    /* The server only knows the code once the browser has opened the sign-in URL, so an early 404 just means "not yet". */
    if(status==404&&std::chrono::steady_clock::now()>=deadline_-std::chrono::minutes(10)+std::chrono::seconds(45)){current_.clear();if(fail_)fail_("expired");return;}
    Again(status==429?10000:2000); /* 204 pending; 429 backs off; network errors and 5xx keep trying until the deadline */
  }
  void OnUploadProgress(CefRefPtr<CefURLRequest>,int64_t,int64_t) override{}
  void OnDownloadProgress(CefRefPtr<CefURLRequest>,int64_t,int64_t) override{}
  void OnDownloadData(CefRefPtr<CefURLRequest>,const void*,size_t) override{}
  bool GetAuthCredentials(bool,const CefString&,int,const CefString&,const CefString&,CefRefPtr<CefAuthCallback>) override{return false;}
  std::string origin_,code_;Done done_;Fail fail_;std::chrono::steady_clock::time_point deadline_;
  static inline std::string current_;
  IMPLEMENT_REFCOUNTING(SteamClaim);
};
class Client final : public DesktopPolicy::RecoveryState,public CefClient,public CefRenderHandler,
  public CefLifeSpanHandler,public CefRequestHandler,public CefLoadHandler,public CefDisplayHandler,
  public CefDownloadHandler,public CefDialogHandler,public CefContextMenuHandler,public CefKeyboardHandler,public CefCommandHandler {
 public:
  CefRefPtr<CefBrowser> browser;
  Texture view,popup;
  CefRect popup_rect;
  bool popup_visible=false,popup_ready=false,dirty=false,composing=false,fatal=false;
  std::atomic<bool> handoff_waiting{false};
  /* Browser handles: `browser` belongs to the SDL thread, `ui_browser` to CEF's UI thread (both are the same thread unless mt_loop). */
  CefRefPtr<CefBrowser> ui_browser;
  std::atomic<HWND> child_hwnd{nullptr};
  /* Run f(browser) / f(host) on the thread CEF requires (posted when mt_loop). Called from the SDL thread. */
  template<class F> void OnBrowser(F f){auto b=browser;if(!b)return;if(!mt_loop){f(b);return;}OnUi([b,f]{f(b);});}
  template<class F> void Host(F f){OnBrowser([f](CefRefPtr<CefBrowser> b){f(b->GetHost());});}
  /* A trusted, unbound key press (F24): Chromium refuses pointer lock without transient user activation, and this grants it. */
  static void ActivateKey(CefRefPtr<CefBrowserHost> host){CefKeyEvent k;k.windows_key_code=VK_F24;k.native_key_code=0x76<<16;k.is_system_key=false;k.modifiers=0;k.type=KEYEVENT_RAWKEYDOWN;host->SendKeyEvent(k);k.type=KEYEVENT_KEYUP;host->SendKeyEvent(k);}
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
#ifdef QRAZY_NATIVE_CLIENT
  /* the page's warnings and errors go to the native log (there is no DevTools in this client) */
  bool OnConsoleMessage(CefRefPtr<CefBrowser>,cef_log_severity_t level,const CefString& message,const CefString& source,int line) override{
    if(level>=LOGSEVERITY_WARNING){std::string text="page console: "+message.ToString().substr(0,300)+" ("+source.ToString().substr(0,80)+":"+std::to_string(line)+")";tc_log(text.c_str());}
    return false;
  }
#endif
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
  void OnAfterCreated(CefRefPtr<CefBrowser> value) override {
    if(mt_loop){ /* on CEF's UI thread */
      ui_browser=value;child_hwnd=value->GetHost()->GetWindowHandle();
      ui_gesture_hook=SetWindowsHookExW(WH_MOUSE,GestureHook,nullptr,GetCurrentThreadId());
      ToMain([self=CefRefPtr<Client>(this),value]{self->browser=value;self->ResizeChild(true);const bool focused=Focused();self->Host([focused](CefRefPtr<CefBrowserHost> host){host->SetFocus(focused);host->SetAudioMuted(true);});});
      return;
    }
    browser=value;ResizeChild(true);browser->GetHost()->SetFocus(Focused());browser->GetHost()->SetAudioMuted(true);
  }
  // Native mode: keep the CEF child filling the SDL window's client area (physical pixels).
  int child_width=0,child_height=0;
  void ResizeChild(bool force=false){
    if(!native_window||!browser)return;
    RECT area{};
    if(!GetClientRect(hwnd,&area)||area.right<=0||area.bottom<=0)return;
    if(!force&&area.right==child_width&&area.bottom==child_height)return;
    child_width=area.right;child_height=area.bottom;
    const int width=child_width,height=child_height;
    Host([width,height](CefRefPtr<CefBrowserHost> host){HWND child=host->GetWindowHandle();if(!child)return;SetWindowPos(child,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);host->WasResized();});
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
  bool OnKeyEvent(CefRefPtr<CefBrowser>,const CefKeyEvent& event,CefEventHandle) override {
    return native_window&&(DevToolsShortcut(event)||(event.windows_key_code==VK_ESCAPE&&event.type==KEYEVENT_KEYUP));}
  bool OnPreKeyEvent(CefRefPtr<CefBrowser>,const CefKeyEvent& event,CefEventHandle,bool* is_keyboard_shortcut) override {
    if(!native_window||closing)return false;
    const bool down=event.type==KEYEVENT_RAWKEYDOWN||event.type==KEYEVENT_KEYDOWN,alt=!!(event.modifiers&EVENTFLAG_ALT_DOWN),first=down&&!(event.modifiers&EVENTFLAG_IS_REPEAT);
    /* Chromium exits pointer lock itself on Escape and then refuses a new lock for a while (same as in a browser). While the page holds the lock, take the
       Escape press here and let the page release the lock by script instead; the key-up still reaches the page. */
    if(pointer_locked&&event.windows_key_code==VK_ESCAPE&&down&&!(event.modifiers&(EVENTFLAG_ALT_DOWN|EVENTFLAG_CONTROL_DOWN))){if(first){auto data=Dict();data->SetString("type","escape");Send(data);}return true;}
    if(DevToolsShortcut(event)){*is_keyboard_shortcut=true;return false;}
    if(first&&(event.windows_key_code==VK_RETURN||event.windows_key_code==VK_SPACE))update_gesture=SDL_GetTicksNS();
    if(alt&&!(event.modifiers&EVENTFLAG_CONTROL_DOWN)&&event.windows_key_code==VK_F4){if(first){QuitLog("close: Alt+F4");Release("shortcut-close");closing=true;}return true;}
    if(alt&&event.windows_key_code==VK_RETURN){if(first)ToggleFullscreen();return true;}
    return false;
  }
  void ToggleFullscreen(){
    if(ToMain([self=CefRefPtr<Client>(this)]{self->ToggleFullscreen();}))return;
    const bool entering=!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN);
#ifdef QRAZY_NATIVE_CLIENT
    const auto started=SDL_GetTicksNS();
    tc_log(entering?"fullscreen: enter requested":"fullscreen: leave requested");
#endif
    SDL_SetWindowFullscreen(window,entering);
#ifdef QRAZY_NATIVE_CLIENT
    char timing[120];std::snprintf(timing,sizeof timing,"fullscreen: SDL request returned after %.1f ms",(SDL_GetTicksNS()-started)/1000000.0);tc_log(timing);
#endif
  }
  void OnFullscreenModeChange(CefRefPtr<CefBrowser>,bool fullscreen) override{
    if(ToMain([self=CefRefPtr<Client>(this),fullscreen]{self->OnFullscreenModeChange(nullptr,fullscreen);}))return;
    if(native_window&&!closing&&fullscreen!=!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN))SDL_SetWindowFullscreen(window,fullscreen);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    QuitLog("close: OnBeforeClose reached");
    if(mt_loop){ /* CEF's UI thread: drop its handle and hook now, the SDL thread's state follows in order */
      ui_browser=nullptr;child_hwnd=nullptr;if(ui_gesture_hook){UnhookWindowsHookEx(ui_gesture_hook);ui_gesture_hook=nullptr;}
      ToMain([self=CefRefPtr<Client>(this)]{self->Release("close");self->browser=nullptr;closed=true;});
      return;
    }
    Release("close");browser=nullptr;closed=true;
  }
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
    if(scheme=="https" || scheme=="http"){
      const std::string target=url.ToString();
      if(!ToMain([target]{SDL_OpenURL(target.c_str());}))SDL_OpenURL(target.c_str());
    }
  }
  bool OnBeforePopup(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int,const CefString& url,const CefString&,
      cef_window_open_disposition_t,bool gesture,const CefPopupFeatures&,CefWindowInfo&,CefRefPtr<CefClient>&,CefBrowserSettings&,CefRefPtr<CefDictionaryValue>&,bool*) override {
    OpenWebLink(frame,url,gesture);return true;
  }
  bool CanDownload(CefRefPtr<CefBrowser> value,const CefString& url,const CefString& method) override {return !closing && value && Trusted(value->GetMainFrame()->GetURL()) && method=="GET" && DemoDownload::TrustedBlob(url.ToString());}
  bool OnBeforeDownload(CefRefPtr<CefBrowser> value,CefRefPtr<CefDownloadItem> item,const CefString& name,CefRefPtr<CefBeforeDownloadCallback> callback) override {if(!closing && !dialog_active && value && Trusted(value->GetMainFrame()->GetURL()) && DemoDownload::TrustedBlob(item->GetOriginalUrl().ToString()) && DemoDownload::ExportName(name.ToString())) {
    auto save=[self=CefRefPtr<Client>(this),file=name.ToString(),callback]{self->Release("export-save");NudgeCursor();DemoDownload::Save(window,file,callback);};
    if(!ToMain(save))save();
  }return true;}
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
      else if(f=="application/json"){add("json");}
      else return false;
    }
    if(exts.empty())return false;
    for(size_t i=0;i<exts.size();i++){if(i)pattern+=';';pattern+=exts[i];}
    return true;
  }
  bool OnFileDialog(CefRefPtr<CefBrowser> value,FileDialogMode mode,const CefString&,const CefString&,const std::vector<CefString>& filters,const std::vector<CefString>&,const std::vector<CefString>&,CefRefPtr<CefFileDialogCallback> callback) override{
    if(ToMain([self=CefRefPtr<Client>(this),value,mode,filters,callback]{self->OnFileDialog(value,mode,CefString(),CefString(),filters,{},{},callback);}))return true;
    std::string pattern;
    if((static_cast<int>(mode)&0xFF)!=static_cast<int>(FILE_DIALOG_OPEN)||!BuildPickerPattern(filters,pattern)||closing||recovering||loading||captured||!Focused()||DemoDownload::active||!value||!Trusted(value->GetMainFrame()->GetURL())||dialog_active.exchange(true)){callback->Cancel();return true;}
    auto* state=new PickerState{callback,std::move(pattern),{}};state->filter={"Files",state->pattern.c_str()};
    SDL_ShowOpenFileDialog(PickerChosen,state,window,&state->filter,1,nullptr,false);return true;
  }
  void OnTextSelectionChanged(CefRefPtr<CefBrowser>,const CefString& text,const CefRange&) override{
    if(ToMain([self=CefRefPtr<Client>(this),copy=text.ToString().substr(0,1048576)]{self->selection=copy;}))return;
    selection=text.ToString().substr(0,1048576);
  }
  void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser>,const CefRange&,const RectList& bounds) override {
    if(ToMain([self=CefRefPtr<Client>(this),bounds]{self->OnImeCompositionRangeChanged(nullptr,CefRange(),bounds);}))return;
    if(bounds.empty())return;auto r=bounds.back();float scale=CefScale();SDL_Rect area{static_cast<int>(r.x*scale),static_cast<int>(r.y*scale),static_cast<int>(r.width*scale),static_cast<int>(r.height*scale)};SDL_SetTextInputArea(window,&area,area.w);
  }
  void Send(CefRefPtr<CefDictionaryValue> data) {
    if(mt_loop){ /* callable from either thread: serialize here, deliver on CEF's UI thread */
      auto value=CefValue::Create();value->SetDictionary(data);
      OnUi([self=CefRefPtr<Client>(this),json=CefWriteJSON(value,JSON_WRITER_DEFAULT)]{
        auto current=self->ui_browser;if(!current)return;auto frame=current->GetMainFrame();if(!frame||!Trusted(frame->GetURL()))return;
        auto message=CefProcessMessage::Create("qrazy-event-v1");message->GetArgumentList()->SetString(0,json);frame->SendProcessMessage(PID_RENDERER,message);
      });
      return;
    }
    if(!browser||!Trusted(browser->GetMainFrame()->GetURL()))return;
    auto v=CefValue::Create();v->SetDictionary(data);auto message=CefProcessMessage::Create("qrazy-event-v1");
    message->GetArgumentList()->SetString(0,CefWriteJSON(v,JSON_WRITER_DEFAULT));browser->GetMainFrame()->SendProcessMessage(PID_RENDERER,message);
  }
  void Reply(int id,CefRefPtr<CefDictionaryValue> value){auto data=Dict();data->SetString("type","reply");data->SetInt("id",id);data->SetDictionary("value",value);Send(data);}
  void Release(const char* reason) {
    if(ToMain([self=CefRefPtr<Client>(this),why=std::string(reason)]{self->Release(why.c_str());}))return;
    SDL_SetWindowRelativeMouseMode(window,false);SDL_EnableScreenSaver();
    if(captured)ClipCursor(nullptr);
    if(!captured)return;captured=false;++generation;
    if(Focused())SDL_StartTextInput(window);
    auto data=Dict();data->SetString("type","state");data->SetString("reason",reason);Send(data);
  }
  void Reset(const char* reason){
    if(ToMain([self=CefRefPtr<Client>(this),why=std::string(reason)]{self->Reset(why.c_str());}))return;
    pointer_locked=false;Release(reason);asset_worker.Reset();popup_visible=false;selection.clear();composing=false;
    Host([](CefRefPtr<CefBrowserHost> host){host->ImeCancelComposition();host->SetAudioMuted(true);});
  }
  void Fail(bool retryable=false){
    if(ToMain([self=CefRefPtr<Client>(this),retryable]{self->Fail(retryable);}))return;
    Reset("failure");FailLoad();if(retryable)server_retry.Failure(SDL_GetTicks());else server_retry.Stop();SDL_SetWindowTitle(window,client_title.c_str());dirty=true;
  }
  void Retry(){if(!browser||closing||fatal||handoff_waiting||!server_retry.Attempt(SDL_GetTicks()))return;Reset("retry");BeginLoad();load_started=SDL_GetTicks();OnBrowser([](CefRefPtr<CefBrowser> b){b->GetMainFrame()->LoadURL(kOrigin);});}
  void TickRetry(){
    if(!browser||closing||fatal||handoff_waiting)return;
    const auto now=SDL_GetTicks();
    if(server_retry.Expire(now)){OnBrowser([](CefRefPtr<CefBrowser> b){b->StopLoad();});Reset("retry-expired");FailLoad();dirty=true;return;}
    if(loading&&now-load_started>=QrazyWindows::ServerConnectTimeoutMs){OnBrowser([](CefRefPtr<CefBrowser> b){b->StopLoad();});Fail(true);}
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
    if(!frame->IsMain())return false;
    if(!Trusted(request->GetURL())){OpenWebLink(frame,request->GetURL(),gesture);return true;}
    if(ToMain([self=CefRefPtr<Client>(this)]{self->Reset("navigation");self->BeginLoad();self->load_started=SDL_GetTicks();}))return false;
    Reset("navigation");BeginLoad();load_started=SDL_GetTicks();return false;
  }
  void OnLoadStart(CefRefPtr<CefBrowser> b,CefRefPtr<CefFrame> frame,TransitionType) override{if(frame->IsMain()&&b&&b->GetHost()->GetZoomLevel()!=0.0)b->GetHost()->SetZoomLevel(0.0);  // zoom persists per site in the profile; undo earlier Ctrl+scroll zoom
    const bool main=frame->IsMain();
    if(ToMain([self=CefRefPtr<Client>(this),main]{if(main&&self->loading&&!self->recovering)SDL_SetWindowTitle(window,"Qrazy - connecting");}))return;
    if(main&&loading&&!recovering)SDL_SetWindowTitle(window,"Qrazy - connecting");}
  void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int code) override {
    const bool main=frame->IsMain(),trusted=Trusted(frame->GetURL());
#ifdef QRAZY_NATIVE_CLIENT
    if(main)frame->ExecuteJavaScript("console.warn('native probe: trusted="+std::string(trusted?"yes":"no")+" match='+typeof qrazyNativeMatch+' settings='+typeof qrazyNativeSettings+' quit='+typeof window.qrazyDesktop?.quit+' url='+location.href.slice(0,60))","qrazy-native-probe",0);
#endif
#ifdef QRAZY_TRUE_CLIENT
    if(main&&trusted&&tc_synth_flag)frame->ExecuteJavaScript(std::string(tc_view_on?"window.__qtcOn=true;":"window.__qtcOn=false;")+kTcSynthJs,"qrazy-tc-synth",0);
#endif
    if(ToMain([self=CefRefPtr<Client>(this),main,trusted,code]{self->LoadEnded(main,trusted,code);}))return;
    LoadEnded(main,trusted,code);
  }
  void LoadEnded(bool main,bool trusted,int code){
    if(!main||!loading||recovering||!trusted)return;
    if(code>=400){Fail(code==502||code==503||code==504);return;}if(FinishLoad(true,code)){server_retry.Stop();SDL_SetWindowTitle(window,client_title.c_str());const bool focused=Focused();Host([focused](CefRefPtr<CefBrowserHost> host){host->SetFocus(focused);host->SetAudioMuted(!focused);});}
  }
  void OnLoadError(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,ErrorCode code,const CefString&,const CefString& url) override{
    const bool main=frame->IsMain(),trusted=Trusted(url),aborted=code==ERR_ABORTED;
    const bool retryable=code==ERR_CONNECTION_REFUSED||code==ERR_CONNECTION_RESET||code==ERR_CONNECTION_CLOSED||code==ERR_CONNECTION_FAILED||code==ERR_CONNECTION_TIMED_OUT||code==ERR_TIMED_OUT||code==ERR_NAME_NOT_RESOLVED||code==ERR_INTERNET_DISCONNECTED||code==ERR_NETWORK_CHANGED;
    if(ToMain([self=CefRefPtr<Client>(this),main,trusted,aborted,retryable]{self->LoadFailed(main,trusted,aborted,retryable);}))return;
    LoadFailed(main,trusted,aborted,retryable);
  }
  void LoadFailed(bool main,bool trusted,bool aborted,bool retryable){if(main&&loading&&!recovering&&trusted&&!aborted)Fail(retryable);}
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser>,TerminationStatus,int,const CefString&) override{
    if(ToMain([self=CefRefPtr<Client>(this)]{self->Fail();}))return;
    Fail();
  }
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
#ifdef QRAZY_NATIVE_CLIENT
    if(msg->GetName()=="qrazy-native-v1"){   /* the page: a match started or ended; the main loop acts on it */
      if(source==PID_RENDERER&&frame->IsMain()&&Trusted(frame->GetURL())){auto a=msg->GetArgumentList();if(a->GetSize()==1&&a->GetType(0)==VTYPE_STRING){std::lock_guard<std::mutex> lock(native_mutex);if(native_requests.size()<64)native_requests.push_back(a->GetString(0).ToString());}}
      return true;
    }
#endif
#ifdef QRAZY_NATIVE_CLIENT
    if(msg->GetName()=="qrazy-native-info-v1"||msg->GetName()=="qrazy-native-settings-v1"||msg->GetName()=="qrazy-native-hud-v1"){   /* data from the page: the information board, the player's settings, the chat lines and typing line */
      if(source==PID_RENDERER&&frame->IsMain()&&Trusted(frame->GetURL())){auto a=msg->GetArgumentList();if(a->GetSize()==1&&a->GetType(0)==VTYPE_STRING){const std::string text=a->GetString(0).ToString();const std::string n=msg->GetName();if(n=="qrazy-native-info-v1")tc_board(text.c_str());else if(n=="qrazy-native-settings-v1")tc_settings(text.c_str());else tc_hud(text.c_str());}}
      return true;
    }
#endif
#ifdef QRAZY_TRUE_CLIENT
    if(msg->GetName()=="qrazy-frame-v1"){   /* Stage 2: the page's game state (dev builds only); runs on CEF's thread, tc_push_state is thread safe */
      if(source==PID_RENDERER&&frame->IsMain()&&Trusted(frame->GetURL())){auto a=msg->GetArgumentList();if(a->GetSize()==1&&a->GetType(0)==VTYPE_BINARY){auto bin=a->GetBinary(0);std::vector<uint8_t> buf(bin->GetSize());if(!buf.empty()&&bin->GetData(buf.data(),buf.size(),0)==buf.size())tc_push_state(buf.data(),buf.size());}}
      return true;
    }
#endif
    if(msg->GetName()!="qrazy-command-v1")return false;
    CefRefPtr<CefBrowser> current=mt_loop?ui_browser:browser; /* this callback runs on CEF's UI thread: use that thread's handle */
    if(source!=PID_RENDERER||!current||b->GetIdentifier()!=current->GetIdentifier()||!frame->IsMain()||
       frame->GetIdentifier()!=current->GetMainFrame()->GetIdentifier()||!Trusted(frame->GetURL())||!Trusted(current->GetMainFrame()->GetURL()))return true;
    auto args=msg->GetArgumentList();
    if(args->GetSize()!=3||args->GetType(0)!=VTYPE_STRING||args->GetType(1)!=VTYPE_INT||args->GetType(2)!=VTYPE_STRING||args->GetString(2).length()>1500000)return true;
    int id=args->GetInt(1);if(id<0)return true;std::string op=args->GetString(0),command_json=args->GetString(2);
    /* The command handler uses SDL and the loop's state, so it runs on the SDL thread. */
    if(ToMain([self=CefRefPtr<Client>(this),id,op,command_json]{self->Command(id,op,command_json);}))return true;
    return Command(id,op,command_json);
  }
  bool Command(int id,const std::string& op,const std::string& command_json) {
    auto result=Dict();result->SetBool("ok",true);
    auto payload=CefParseJSON(command_json,JSON_PARSER_RFC);auto d=payload&&payload->GetType()==VTYPE_DICTIONARY?payload->GetDictionary():nullptr;
    bool menu=!closing&&!recovering&&!loading&&!captured&&Focused();
    if(op=="native-window-get"){Reply(id,render_preferences.NativeWindowState(native_window));return true;}
    if(op=="native-window-set"){Reply(id,QrazyWindows::Error("The native window is always on; there is no setting to change"));return true;}
    if(native_window&&op=="vsync-get"){Reply(id,render_preferences.VSyncState());return true;}
    if(native_window&&op=="vsync-set"){
      if(closing||recovering||loading||!Focused()||!d||d->GetType("enabled")!=VTYPE_BOOL){Reply(id,QrazyWindows::Error("Return to the focused game before changing rendering settings"));return true;}
      const auto previous=render_preferences;render_preferences.vsync=d->GetBool("enabled");
      if(!render_preferences.Save()){render_preferences=previous;Reply(id,QrazyWindows::Error("Setting could not be saved"));return true;}
#ifdef QRAZY_NATIVE_CLIENT
      tc_set_frame_cap(render_preferences.vsync ? 0 : render_preferences.max_fps);
#endif
      Reply(id,render_preferences.VSyncState());
      auto event=Dict();event->SetString("type","render-settings");event->SetDictionary("vsync",render_preferences.VSyncState());event->SetDictionary("maxFps",render_preferences.MaxFpsState());Send(event);return true;
    }
    if(native_window&&op=="max-fps-set"){
      if(closing||recovering||loading||!Focused()||!d||d->GetType("value")!=VTYPE_INT||d->GetInt("value")<30||d->GetInt("value")>10000){Reply(id,QrazyWindows::Error("Return to the focused game and give com_maxfps as an integer from 30 to 10000"));return true;}
      const auto previous=render_preferences;render_preferences.max_fps=d->GetInt("value");
      if(!render_preferences.Save()){render_preferences=previous;Reply(id,QrazyWindows::Error("Setting could not be saved"));return true;}
#ifdef QRAZY_NATIVE_CLIENT
      tc_set_frame_cap(render_preferences.vsync ? 0 : render_preferences.max_fps);
#endif
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
        Release("export-save");NudgeCursor();SDL_ShowSaveFileDialog(FileChosen,selected,window,&filter,1,name.c_str());
      }else{NudgeCursor();SDL_ShowOpenFileDialog(FileChosen,selected,window,&filter,1,nullptr,false);}
      return true;
    }else if(op=="update-notice"||op=="update-state"||op=="update-check"||op=="update-stage"||op=="update-install") {
      if(localhost_mode
#ifdef QRAZY_STAGE0
         || true
#endif
      ){Reply(id,QrazyWindows::Error("Updates are disabled in the development client"));return true;}
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
    }else if(op=="pointer-lock-activate"){if(native_window&&browser)Host(ActivateKey);
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
    }else if(op=="quit"){QuitLog("close: qrazyDesktop.quit op");Release("quit");closing=true;}
    else if(op=="refresh-game") {
      bool ok=browser&&!closing&&!fatal&&!handoff_waiting&&!loading&&Focused();
      result->SetBool("ok",ok);
      if(ok){server_retry.Stop();Reset("refresh");BeginLoad();load_started=SDL_GetTicks();server_retry.Start(load_started);server_retry.Attempt(load_started);OnBrowser([](CefRefPtr<CefBrowser> b){b->ReloadIgnoreCache();});}
    }
    else if(op=="steam-cancel")OnUi([]{SteamClaim::Cancel();});
    else if(op=="steam-start") {
      const std::string code=d&&d->GetType("code")==VTYPE_STRING?d->GetString("code").ToString():"",url=d&&d->GetType("url")==VTYPE_STRING?d->GetString("url").ToString():"";
      const bool ok=!closing&&!fatal&&SteamClaim::ValidCode(code)&&SteamExternalUrl(url);
      result->SetBool("ok",ok);
      if(!ok)result->SetString("error","Steam login request refused");
      else{
        SDL_OpenURL(url.c_str());const CefString site=kOrigin;const std::string origin=SteamOrigin(site);
        auto self=CefRefPtr<Client>(this);
        OnUi([self,origin,code]{SteamClaim::Start(origin,code,
          [self](const std::string& location){self->OnBrowser([location](CefRefPtr<CefBrowser> b){b->GetMainFrame()->LoadURL(location);});},
          [self](const char* reason){auto event=Dict();event->SetString("type","steam-failed");event->SetString("reason",reason);self->Send(event);});});
      }
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
 public:std::atomic<bool> done{false};void OnComplete() override{done=true;}
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
[[maybe_unused]] static void RefreshLauncher(const std::filesystem::path& base,const std::filesystem::path& runtime) {
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
  // Read the launcher's selection before CEF starts the browser or renderer.
  wchar_t local_marker[2]={};
  localhost_mode=QRAZY_DEV_MODE||(GetEnvironmentVariableW(QrazyDestination::LocalhostMarker,local_marker,2)==1&&local_marker[0]==L'1');
  kOrigin=QrazyDestination::Origin(localhost_mode);
  CefMainArgs args(instance);CefRefPtr<App> app=new App;
  int sub=CefExecuteProcess(args,app,sandbox);if(sub>=0)return sub;
  auto command=CefCommandLine::CreateCommandLine();command->InitFromString(GetCommandLineW());
  CefCommandLine::SwitchMap switches;CefCommandLine::ArgumentList arguments;command->GetSwitches(switches);command->GetArguments(arguments);
  if(!switches.empty()||!arguments.empty())return 2;
  auto base=root.parent_path();wchar_t marker[32768];DWORD marker_size=GetEnvironmentVariableW(L"QRAZY_SDLCEF_ROOT",marker,32768);
  if(!marker_size||marker_size>=32768||std::filesystem::path(marker)!=base||root.filename()!=L"runtime")return 2;
  Production::CleanEnvironment();
  updater_worker.root=base;
  auto profile=base/QrazyDestination::Profile(localhost_mode);std::error_code error;std::filesystem::create_directories(profile,error);
  if(error||!QrazyWindows::NoReparse(profile))return 2;ProfileLock lock;if(!lock.Acquire(profile/"host.lock"))return 2;
#ifdef QRAZY_NATIVE_CLIENT
  struct CoreUpdateWait { ~CoreUpdateWait() { tc_core_wait(); } } core_update_wait;
#if defined(QRAZY_STAGE0) && !defined(QRAZY_LOCAL_CORE_SMOKE)
  tc_core_prepare((profile/"core-updates-v11").string().c_str(),false,false);
#else
  tc_core_prepare((profile/"core-updates-v11").string().c_str(),true,localhost_mode);
#endif
#endif
#ifndef QRAZY_STAGE0
  RefreshLauncher(base,root);
#endif
  NetDiag::Start(profile);
  render_preferences.Load(profile);native_window=render_preferences.native_window;startup_vsync=render_preferences.vsync;
  { /* CEF's own UI thread, so raw input never waits for this loop. Not for the offscreen backup, and not for the login handoff, whose gate must run on CEF's UI thread inside this loop. */
    std::error_code handoff_probe;mt_loop=native_window&&!std::filesystem::exists(base/L".qrazy-transition",handoff_probe);
  }
#ifdef QRAZY_NATIVE_CLIENT
#ifdef QRAZY_STAGE0
  const auto nativeFixDir=profile/"native-match";
#else
  const auto nativeFixDir=std::filesystem::temp_directory_path(error)/"qrazy-native-match";
#endif
#endif
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
#ifdef QRAZY_TRUE_CLIENT
  /* no SDL renderer: its D3D11 swapchain would occupy the window that Vulkan presents to */
  if((render_rate=RefreshRate())==0){SDL_DestroyWindow(window);SDL_Quit();return 3;}
  ID3D11Device* device=nullptr;
#else
  renderer=SDL_CreateRenderer(window,"direct3d11");
  int vsync=0;
  if(!renderer||!SDL_SetRenderVSync(renderer,render_preferences.vsync?1:0)||!SDL_GetRenderVSync(renderer,&vsync)||vsync!=(render_preferences.vsync?1:0)||(render_rate=RefreshRate())==0){SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return 3;}
  auto device=static_cast<ID3D11Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr));
  if(!device)return 3;
#endif
  CefSettings settings;settings.windowless_rendering_enabled=true;settings.no_sandbox=false;settings.command_line_args_disabled=true;settings.multi_threaded_message_loop=mt_loop;
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
  if(localhost_mode)client_title+=" [DEV localhost:5173]";
  SDL_SetWindowTitle(window,client_title.c_str());
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
  bool tc_started=false,tc_on=false,tc_applied=false;
#endif
#ifdef QRAZY_TRUE_CLIENT
  {std::ifstream in(base/"true-client.cmdline");std::string line;if(in&&std::getline(in,line)&&!line.empty()){tc_start(hwnd,line.c_str());tc_started=true;tc_synth_flag=line.find("--synth")!=std::string::npos;}}
#endif
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
    CefWindowInfo info;auto mode=CefDictionaryValue::Create();
    mode->SetBool("localhostMode",localhost_mode);
    if(native_window){RECT area{};GetClientRect(hwnd,&area);info.SetAsChild(hwnd,CefRect(0,0,std::max<LONG>(1,area.right),std::max<LONG>(1,area.bottom)));mode->SetBool("nativeWindow",true);}
    else{info.SetAsWindowless(hwnd);info.shared_texture_enabled=true;info.external_begin_frame_enabled=true;}
#ifdef QRAZY_NATIVE_CLIENT
    native_hub_post=[client](std::string event){
      client->Host([event=std::move(event)](CefRefPtr<CefBrowserHost> host){
        auto frame=host->GetBrowser()->GetMainFrame();
        if(frame&&Trusted(frame->GetURL()))
          frame->ExecuteJavaScript("window.qrazyNativeHubEvent&&window.qrazyNativeHubEvent("+event+")","qrazy-native-hub",0);
      });
    };
    nc::appHub().setSink(NativeHubEvent);
    mode->SetBool("nativeHub",tc_hub_ready());
#endif
    CefBrowserSettings bs;bs.windowless_frame_rate=render_preferences.CefTarget(render_rate);bs.background_color=CefColorSetARGB(255,0,0,0);
    if(result||!CefBrowserHost::CreateBrowser(info,client,transitioning?"about:blank":kOrigin,bs,mode,request_context)){
      /* No silent fallback to offscreen: a native-window failure has to be reported so it can be fixed. */
      if(!result&&native_window)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Qrazy","Qrazy could not create its game window (error 5).\n\nPlease report this in the Qrazy Discord and include your graphics card, graphics driver version and Windows version.",window);
      if(!result)result=5;
    }
    bool close_sent=false,child_hidden=false,child_enabled=true;Uint64 zoom_checked=0,close_sent_ns=0;auto refresh=std::chrono::steady_clock::now();if(!native_window)SDL_StartTextInput(window);
    /* Chromium's child window never sees the parent change monitor, scale or refresh, so it can keep rendering at a stale scale (blurry) or vsync rate. Tell it, and nudge the child size so the compositor re-evaluates the display. */
    auto resync_child=[&](){
      if(!native_window||!client->browser)return;
      client->Host([](CefRefPtr<CefBrowserHost> host){host->NotifyScreenInfoChanged();});
      RECT area{};
      if(GetClientRect(hwnd,&area)&&area.right>1&&area.bottom>1){const int width=area.right-1,height=area.bottom;client->Host([width,height](CefRefPtr<CefBrowserHost> host){HWND child=host->GetWindowHandle();if(child){SetWindowPos(child,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);host->WasResized();}});client->ResizeChild(true);}
    };
    auto startup_resync=std::chrono::steady_clock::now()+std::chrono::milliseconds(1500);int startup_resyncs=3; /* the first layout can land before the window has settled on its monitor */
    auto update_display=[&](){
      if(native_window){
        static SDL_DisplayID last_display=0;static int last_rate=0;static float last_scale=0;
        SDL_DisplayID now_display=SDL_GetDisplayForWindow(window);int now_rate=RefreshRate();float now_scale=CefScale();
        if(last_display&&(now_display!=last_display||now_rate!=last_rate||now_scale!=last_scale))resync_child();
        else if(startup_resyncs>0&&client->browser&&!captured&&std::chrono::steady_clock::now()>=startup_resync){--startup_resyncs;startup_resync=std::chrono::steady_clock::now()+std::chrono::seconds(2);resync_child();}
        last_display=now_display;last_rate=now_rate;last_scale=now_scale;
        if(now_rate)render_rate=now_rate;
        return;
      }
      if(!frame_clock.UpdateDisplay(hwnd)){client->fatal=true;closing=true;return;}
      int rate=RefreshRate();if(rate&&rate!=render_rate){render_rate=rate;if(client->browser)client->browser->GetHost()->SetWindowlessFrameRate(render_preferences.CefTarget(rate));}
    };
    auto render_frame=[&](){if(!renderer)return;client->dirty=false;SDL_SetRenderDrawColor(renderer,6,12,22,255);SDL_RenderClear(renderer);
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
      if(!mt_loop)CefDoMessageLoopWork();
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
        if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED){QuitLog(e.type==SDL_EVENT_QUIT?"close: SDL_EVENT_QUIT":"close: SDL_EVENT_WINDOW_CLOSE_REQUESTED (X button / WM_CLOSE)");closing=true;}
        /* In native mode SDL reports focus loss whenever the CEF child takes keyboard focus; only a real loss of the foreground counts. */
        if((e.type==SDL_EVENT_WINDOW_FOCUS_LOST&&!(native_window&&Focused()))||e.type==SDL_EVENT_WINDOW_MINIMIZED) {
          client->Release("focus");client->selection.clear();client->composing=false;client->Host([](CefRefPtr<CefBrowserHost> host){host->ImeCancelComposition();host->SetAudioMuted(true);host->SetFocus(false);});
          auto d=Dict();d->SetString("type","focus");d->SetBool("focused",false);client->Send(d);
        }
        if(e.type==SDL_EVENT_WINDOW_FOCUS_GAINED){{const bool focus_page=!child_hidden,mute=client->loading||client->recovering;client->Host([focus_page,mute](CefRefPtr<CefBrowserHost> host){if(focus_page)host->SetFocus(true);host->SetAudioMuted(mute);});}if(!native_window)SDL_StartTextInput(window);
          /* Chromium refuses requestPointerLock without user activation (the page's capture restore after alt-tab runs on this focus message, often seconds after the last input). A trusted F24 press (unbound, never Escape) grants activation first. */
          if(native_window)client->Host(Client::ActivateKey);
          auto d=Dict();d->SetString("type","focus");d->SetBool("focused",true);client->Send(d);}
        if(e.type==SDL_EVENT_WINDOW_RESIZED||e.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED){if(native_window){
          if(e.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED||e.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED)resync_child();
          else client->ResizeChild();
        }else if(client->browser){client->browser->GetHost()->NotifyScreenInfoChanged();client->browser->GetHost()->WasResized();}client->dirty=true;}
        if(native_window&&(e.type==SDL_EVENT_WINDOW_MAXIMIZED||e.type==SDL_EVENT_WINDOW_RESTORED||e.type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN||e.type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)){client->ResizeChild();if(e.type!=SDL_EVENT_WINDOW_MAXIMIZED)resync_child();}
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
      if(closing&&!close_sent_ns){close_sent_ns=SDL_GetTicksNS();StartQuitWatchdog(install_requested?Production::InstallExit:0);
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
        if(native_match_state&&client->browser){native_match_state=0;client->Host([js=NativeStateScript("ended","","window")](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js,"qrazy-native-state",0);});}
        if(tc_started){QuitLog("close: stopping native view");tc_set_visible(false);tc_stop();tc_started=false;tc_on=false;tc_view_on=false;QuitLog("close: native view stopped");}
#endif
      }
      if(closing&&client->browser&&!close_sent){QuitLog("close: CloseBrowser(true) sent");client->Release("close");client->Host([](CefRefPtr<CefBrowserHost> host){host->CloseBrowser(true);});close_sent=true;close_sent_ns=SDL_GetTicksNS();}
      if(close_sent&&!closed&&SDL_GetTicksNS()-close_sent_ns>5000000000ull){QuitLog("browser did not close in 5 s; exiting");closed=true;} /* last resort so a stuck windowed close cannot leave a window that ignores input */
      if(native_window&&client->browser) {
        /* Cards are drawn by SDL under the CEF child, so the child is hidden only while one is showing (recovery or server retry). Ordinary loading keeps the page visible and running. */
#ifdef QRAZY_TRUE_CLIENT
        {static bool f2=false;const bool down=Focused()&&(GetAsyncKeyState(VK_F2)&0x8000)!=0;if(down&&!f2&&tc_started){tc_on=!tc_on;tc_view_on=tc_on;tc_set_visible(tc_on);client->Host([on=tc_on](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(on?"window.__qtcOn=true":"window.__qtcOn=false","qrazy-tc-view",0);});}f2=down;} /* F2: native view <-> menus */
#endif
#ifdef QRAZY_NATIVE_CLIENT
        const auto native_state=[&](const char* state,const std::string& code,const std::string& why){const std::string js=NativeStateScript(state,code,why,std::strcmp(state,"started")==0&&tc_hud_feed(),std::strcmp(state,"started")==0&&tc_audio_feed());tc_log(js.c_str());client->Host([js](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js,"qrazy-native-state",0);});};
        if(tc_started&&tc_take_started()&&native_match_state==1){native_match_state=2;native_state("started","","");tc_hud_ready();}
        {char reason[512];if(tc_started&&tc_take_error(reason,sizeof reason)){   /* the native view stopped by itself: there is no other renderer, so the player is told and returned to the menus */
           tc_set_visible(false);tc_stop();tc_started=false;tc_on=false;tc_view_on=false;
           client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(NativeMenuScript(),"qrazy-tc-view",0);});
           const std::string text=std::string("Qrazy's graphics could not run this match.\n\n")+reason+"\n\nQrazy needs a graphics card with Vulkan support and an up-to-date graphics driver. If this keeps happening, please report it in the Qrazy Discord and include this text.";
           tc_log((std::string("native view failed: ")+reason).c_str());
           if(native_match_state==1)native_state("refused",tc_error_code(),reason);else if(native_match_state==2)native_state("ended","","crash");native_match_state=0;
           {const std::string full=text+"\n\nReload the game now (RELOAD)? Choose No to stay in the menu.";const int wn=MultiByteToWideChar(CP_UTF8,0,full.c_str(),-1,nullptr,0);std::wstring wide(static_cast<size_t>(std::max(wn,1)),L'\0');MultiByteToWideChar(CP_UTF8,0,full.c_str(),-1,&wide[0],wn);
            if(MessageBoxW(hwnd,wide.c_str(),L"Qrazy",MB_YESNO|MB_ICONERROR|MB_SETFOREGROUND)==IDYES){tc_log("native view failed: RELOAD chosen");client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->ReloadIgnoreCache();});}}}}
        {std::vector<std::pair<int,int>> clicks;{std::lock_guard<std::mutex> click_lock(native_click_mutex);clicks.swap(native_clicks);}if(!clicks.empty()&&tc_on)client->Host([clicks=std::move(clicks)](CefRefPtr<CefBrowserHost> host){for(const auto& c:clicks){if(c.first==1000){SendNativeShortcut(host,false,true);SendNativeShortcut(host,false,false);}else host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(NativeClickScript(c.first,c.second),"qrazy-native-click",0);}});}
        {std::vector<std::string> scripts;{std::lock_guard<std::mutex> js_lock(native_js_mutex);scripts.swap(native_js);}if(!scripts.empty()&&tc_started)client->Host([scripts=std::move(scripts)](CefRefPtr<CefBrowserHost> host){for(const auto& js:scripts)host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js,"qrazy-native-text",0);});}
        if(native_child&&tc_on){static bool was_typing=false;const bool typing_now=tc_typing();   /* the native view holds the keyboard focus while a box is open (WM_CHAR and the IME go to it); it is given back when the box closes */
          if(typing_now&&Focused()&&GetFocus()!=native_child)SetFocus(native_child);
          if(!typing_now&&was_typing&&GetFocus()==native_child)SetFocus(hwnd);was_typing=typing_now;
          if(typing_now&&tc_text_composing()){float r[4];if(tc_caret_rect(r)){if(HIMC imc=ImmGetContext(native_child)){COMPOSITIONFORM cf{};cf.dwStyle=CFS_POINT;cf.ptCurrentPos={static_cast<LONG>(r[0]),static_cast<LONG>(r[1])};ImmSetCompositionWindow(imc,&cf);CANDIDATEFORM cd{};cd.dwIndex=0;cd.dwStyle=CFS_CANDIDATEPOS;cd.ptCurrentPos={static_cast<LONG>(r[0]),static_cast<LONG>(r[1]+r[3])};ImmSetCandidateWindow(imc,&cd);ImmReleaseContext(native_child,imc);}}}}
        {std::vector<std::string> sims;{std::lock_guard<std::mutex> sim_lock(native_sim_mutex);sims.swap(native_sim);}if(!sims.empty()&&tc_started)client->Host([sims=std::move(sims)](CefRefPtr<CefBrowserHost> host){for(const auto& j:sims)host->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.qrazyNativeSim&&window.qrazyNativeSim("+j+")","qrazy-native-sim",0);});}
        {static Uint64 pose_sent=0;const Uint64 now=SDL_GetTicksNS();   /* the player's position and view angles for the page (its /viewpos), 10 Hz: window.qrazyNativePose({x,y,z (feet), vx,vy,vz, yaw,pitch (radians), ex,ey,ez (eye), ground, crouched}) */
         if(tc_on&&(tc_take_pose_request()||now-pose_sent>=100000000ull)){pose_sent=now;double p[14];if(tc_get_pose(p)){char js[480];std::snprintf(js,sizeof js,"window.qrazyNativePose&&window.qrazyNativePose({x:%.3f,y:%.3f,z:%.3f,vx:%.3f,vy:%.3f,vz:%.3f,yaw:%.6f,pitch:%.6f,ex:%.3f,ey:%.3f,ez:%.3f,ground:%s,crouched:%s,weapon:%d})",p[5],p[6],p[7],p[8],p[9],p[10],p[3],p[4],p[0],p[1],p[2],p[11]!=0?"true":"false",p[12]!=0?"true":"false",static_cast<int>(p[13]));client->Host([s=std::string(js)](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(s,"qrazy-native-pose",0);});}}}
        if(const int tab=native_tab_edge.exchange(0)){if(tc_on)client->Host([tab](CefRefPtr<CefBrowserHost> host){   /* the page is hidden and gets no keys: give it the Tab press so it reports the board */
           if(tab==1)host->SetFocus(true);SendNativeShortcut(host,true,tab==1);tc_log(tab==1?"host: Tab down passed to the page":"host: Tab up passed to the page");});}
        if(!tc_started)tc_hub_service();
        if(native_menu_requested.exchange(false)&&tc_on){tc_on=false;tc_view_on=false;tc_set_visible(false);client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(NativeMenuScript(),"qrazy-tc-view",0);});}   /* Esc in the native view: the page shows its menu */
        {std::vector<std::string> requests;{std::lock_guard<std::mutex> native_lock(native_mutex);requests.swap(native_requests);}
         static std::vector<std::string> held_sim_ops;   /* sim ops (mode, respawn, taint...) the page sent while the menu had the match suspended: applied on resume, in order */
         for(const std::string& request:requests)if(!request.empty()){
           if(QrazyNativeMatch::HandleHub(request))continue;
           NativeMatch match;
           if(!QrazyNativeMatch::ParseNativeMatch(request,[](const std::string& kind,const std::string& key){return asset_worker.PathOf(kind,key);},nativeFixDir,match)){std::fprintf(stderr,"native match request refused: %s\n",match.error.c_str());tc_log(("request refused: "+match.error).c_str());
             if(match.start){if(tc_started){tc_set_visible(false);tc_stop();tc_started=false;}tc_on=false;tc_view_on=false;held_sim_ops.clear();native_match_state=0;native_state("refused",match.code,match.error);client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript(NativeMenuScript(),"qrazy-tc-view",0);});}}   /* no half-started match */
           else if(match.simop){if(tc_started&&(tc_on||match.soundfact||NativeReportOp(request))){tc_sim_op(request.c_str());}else if(tc_started&&(request.find("\"openText\"")!=std::string::npos||request.find("\"closeText\"")!=std::string::npos)){tc_log("text box op from the page while the menu is open: dropped");}else if(tc_started){if(held_sim_ops.size()<256)held_sim_ops.push_back(request);tc_log("sim op from the page while the menu is open: held until resume");}else tc_log("sim op from the page: no native match is running, ignored");}
           else if(match.resume){if(tc_started&&!tc_on){for(const std::string& held:held_sim_ops)tc_sim_op(held.c_str());held_sim_ops.clear();tc_on=true;tc_view_on=true;tc_set_visible(true);client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__qtcOn=true","qrazy-tc-view",0);});}}
           else if(match.stop){held_sim_ops.clear();if(tc_started&&(tc_hud_feed()||(native_match_state==1&&native_running_cmd.find(" --hud-feed")!=std::string::npos))){tc_set_visible(false);tc_stop();tc_started=false;}if(native_match_state){native_match_state=0;native_state("ended","","stop");}if(tc_on){tc_on=false;tc_view_on=false;tc_set_visible(false);client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__qtcOn=false","qrazy-tc-view",0);});}}
           else if(tc_started&&tc_on&&match.cmdline==native_running_cmd){tc_log("start for the match that is already running: ignored");if(native_match_state==2)native_state("started","","");else if(native_match_state==0)native_match_state=1;}
           else{held_sim_ops.clear();if(tc_started){tc_set_visible(false);tc_stop();tc_started=false;}native_running_cmd=match.cmdline;{const size_t at=match.cmdline.find("--bsp \"");tc_log(((at==std::string::npos?std::string("match start: runway"):"match start: bsp "+match.cmdline.substr(at+7,match.cmdline.find('"',at+7)-at-7))+(match.cmdline.find(" --hud-feed")!=std::string::npos?", page offers hudFeed":", legacy page")).c_str());}tc_set_menu_request(NativeMenuRequest);tc_set_tab(NativeTab);tc_set_sim_out(NativeSimOut);tc_set_js_out(NativeJsOut);{std::string pacing=render_preferences.vsync?std::string(" --pm fifo"):" --pm immediate --cap "+std::to_string(render_preferences.max_fps);native_match_state=1;tc_start(EnsureNativeView(hwnd),(match.cmdline+pacing).c_str());}   /* the player's own VSync / Max FPS settings pace the native view too */tc_started=true;tc_on=true;tc_view_on=true;tc_set_visible(true);client->Host([](CefRefPtr<CefBrowserHost> host){host->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__qtcOn=true","qrazy-tc-view",0);});}
         }}
#endif
        const bool card=client->recovering||client->server_retry.active;
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
        {static bool was_free=false;const bool now_free=tc_cursor();if(now_free!=was_free){was_free=now_free;tc_log(now_free?"cursor: free (arrow shown)":"cursor: taken (hidden)");if(native_child)PostMessageW(native_child,WM_SETCURSOR,(WPARAM)native_child,MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));}}   /* the cursor shows / hides at once, not at the next mouse move */
        /* The one free-mouse rule on this side. While the mouse is free (a text box open, or a clickable element on screen) the native view owns the pointer in every way Windows lets a window own it, so nothing the page's window or Chromium's pointer lock does can hide it: it is the topmost child (the page's window is shown under it while a text box is open for the real keyboard), it holds the mouse capture (WM_SETCURSOR and the clicks go to the capturing window whatever lies under the pointer), the cursor clip and this thread's display counter are reset, and the arrow is set every frame. Taken: the capture is let go. */
        if(native_child&&tc_on){static bool above_logged=false;static std::string last_state;
          if(GetTopWindow(hwnd)!=native_child){SetWindowPos(native_child,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);if(!above_logged){above_logged=true;tc_log("cursor: the native view was not the topmost child (the page's window was above it): raised");}}
          const bool free=tc_cursor();
          if(free){
            /* only while the pointer is inside the client area: a held capture would take the click on the title bar X, the frame and other windows */
            {POINT in{};RECT area{};const bool inside=native_drag||(GetCursorPos(&in)&&ScreenToClient(hwnd,&in)&&GetClientRect(hwnd,&area)&&PtInRect(&area,in));if(!inside){if(GetCapture()==native_child)ReleaseCapture();}else if(Focused()&&GetCapture()!=native_child)SetCapture(native_child);}
            RECT clip{};const int sw=GetSystemMetrics(SM_CXVIRTUALSCREEN),sh=GetSystemMetrics(SM_CYVIRTUALSCREEN);if(GetClipCursor(&clip)&&(clip.right-clip.left<sw||clip.bottom-clip.top<sh))ClipCursor(nullptr);
            int level=ShowCursor(TRUE);while(level<0)level=ShowCursor(TRUE);if(level>0)ShowCursor(FALSE);
            SetCursor(LoadCursorW(nullptr,IDC_ARROW));
          }else{if(GetCapture()==native_child&&!native_drag)ReleaseCapture();POINT pt;if(GetCursorPos(&pt)&&WindowFromPoint(pt)==native_child)SetCursor(nullptr);}
          if(free){   /* the evidence for the next report: what Windows thinks the pointer is doing while it is free */
            CURSORINFO ci{sizeof ci};GetCursorInfo(&ci);POINT pt{};GetCursorPos(&pt);HWND under=WindowFromPoint(pt),cap=GetCapture();char uc[64]="",cc[64]="";if(under)GetClassNameA(under,uc,sizeof uc);if(cap)GetClassNameA(cap,cc,sizeof cc);
            char st[320];std::snprintf(st,sizeof st,"cursor state (free): showing %d, under pointer '%s'%s, capture '%s'%s, page pointer lock %d",(ci.flags&CURSOR_SHOWING)?1:0,uc,under==native_child?" (native view)":"",cc,cap==native_child?" (native view)":"",(int)pointer_locked.load());
            if(last_state!=st){last_state=st;tc_log(st);}
          }else last_state.clear();}
#endif
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
        const bool page_hidden=card||(tc_on&&native_child!=nullptr);   /* the core's text field takes the keyboard itself: the page window stays hidden while a chat / console box is open */   /* the page keeps running while hidden (see the renderer-backgrounding switches) */
#else
        const bool page_hidden=card;
#endif
        HWND child=mt_loop?client->child_hwnd.load():client->browser->GetHost()->GetWindowHandle(); /* mt_loop: the handle CEF's UI thread stored; the window itself belongs to that thread, so it is only touched there */
#if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
        /* Native view: its own child window is shown and the page's child window is hidden (below); the window follows the host window's size. */
        if(native_child){
          RECT area{};if(GetClientRect(hwnd,&area)&&area.right>0&&area.bottom>0){RECT mine{};GetClientRect(native_child,&mine);if(mine.right!=area.right||mine.bottom!=area.bottom)SetWindowPos(native_child,HWND_TOP,0,0,area.right,area.bottom,SWP_NOACTIVATE);}
          if(tc_on!=tc_applied){tc_applied=tc_on;ShowWindow(native_child,tc_on?SW_SHOW:SW_HIDE);if(tc_on)SetWindowPos(native_child,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}
        }
#endif
        if(child&&page_hidden!=child_hidden) {
          child_hidden=page_hidden;OnUi([child,page_hidden]{ShowWindow(child,page_hidden?SW_HIDE:SW_SHOW);});
          if(page_hidden){SetFocus(hwnd);client->dirty=true;}else{client->ResizeChild(true);const bool focused=Focused();client->Host([focused](CefRefPtr<CefBrowserHost> host){host->SetFocus(focused);});}
        }
        /* Same input gate as offscreen mode (no input while loading or recovering): a disabled child window receives no mouse or keys. */
        const bool allow=DesktopPolicy::AllowInput(false,client->recovering,client->loading); /* closing does not disable the child: a close that stalls must not also leave a dead window */
        if(child&&allow!=child_enabled){
          child_enabled=allow;OnUi([child,allow]{EnableWindow(child,allow);});
          if(!allow)SetFocus(hwnd);else if(!child_hidden){const bool focused=Focused();client->Host([focused](CefRefPtr<CefBrowserHost> host){host->SetFocus(focused);});}
        }
        /* Ctrl+wheel zoom reaches the page as in a browser; undo any zoom within 50 ms (offscreen mode stripped Ctrl from wheel events and reset zoom on load). */
        const Uint64 zoom_now=SDL_GetTicksNS();
        if(pointer_locked&&!lock_was_held)lock_started_ns=zoom_now;
        lock_was_held=pointer_locked;
        if(pointer_locked&&zoom_now-lock_started_ns<10000000000ull)OnUi([]{EnumThreadWindows(GetCurrentThreadId(),HideLockBubbleWindow,0);}); /* the bubble belongs to the thread CEF runs on */
        if(zoom_now-zoom_checked>=50000000ull){zoom_checked=zoom_now;client->Host([](CefRefPtr<CefBrowserHost> host){if(host->GetZoomLevel()!=0.0)host->SetZoomLevel(0.0);});}
        if(card&&(client->dirty||client->loading||client->recovering))render_frame();
      }else if(client->dirty||client->loading||client->recovering)render_frame();
      if(frame_clock.Failed()){client->fatal=true;closing=true;}
      client->RequestFrame(frame_clock);
      SDL_Delay(1);
    }
    saved_window.Observe();saved_window.Save();if(client->fatal)result=6;
    if(!result){CefRefPtr<CookieFlushed> flushed=new CookieFlushed;bool requested=CefCookieManager::GetGlobalManager(nullptr)->FlushStore(flushed);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(requested&&!flushed->done&&std::chrono::steady_clock::now()<deadline){if(!mt_loop)CefDoMessageLoopWork();else SDL_PumpEvents();SDL_Delay(1);}std::fprintf(stderr,"PROTOTYPE cookie flush completed=%d\n",static_cast<int>(flushed->done.load()));if(!requested||!flushed->done)result=7;}
  }
  if(gesture_hook)UnhookWindowsHookEx(gesture_hook);
  #if defined(QRAZY_TRUE_CLIENT)||defined(QRAZY_NATIVE_CLIENT)
  if(tc_started)tc_stop();
#ifdef QRAZY_NATIVE_CLIENT
  tc_hub_shutdown();
  native_hub_post={};
#endif
#ifdef QRAZY_NATIVE_CLIENT
  if(native_child){DestroyWindow(native_child);native_child=nullptr;}
#endif
#endif
  NetDiag::Stop();updater_worker.Stop();asset_worker.Stop();CefShutdown();app=nullptr;if(renderer)SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return !result&&install_requested?Production::InstallExit:result;
}
} // namespace
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance,LPWSTR,int,void* sandbox,cef_version_info_t*){return Run(instance,sandbox);}
