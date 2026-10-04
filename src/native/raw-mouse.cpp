#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <vector>
// Stable Node-API C ABI resolved from Electron's host, avoiding V8 ABI and
// a downloaded SDK/import library. https://nodejs.org/api/n-api.html
struct napi_env__; struct napi_value__; struct napi_callback_info__;
using Env=napi_env__*; using Value=napi_value__*; using Info=napi_callback_info__*;
using Callback=Value(__cdecl*)(Env,Info);
using GetArgs=int(__cdecl*)(Env,Info,size_t*,Value*,Value*,void**);
using GetBuffer=int(__cdecl*)(Env,Value,void**,size_t*);
using MakeFunction=int(__cdecl*)(Env,const char*,size_t,Callback,void*,Value*);
using SetProperty=int(__cdecl*)(Env,Value,const char*,Value);
using MakeNumber=int(__cdecl*)(Env,double,Value*);
using MakeObject=int(__cdecl*)(Env,Value*);
using MakeBoolean=int(__cdecl*)(Env,bool,Value*);
using MakeUndefined=int(__cdecl*)(Env,Value*);
using ThrowError=int(__cdecl*)(Env,const char*,const char*);
using Cleanup=int(__cdecl*)(Env,void(__cdecl*)(void*),void*);
#define API(name,type) static type name=nullptr
API(napi_get_cb_info,GetArgs); API(napi_get_buffer_info,GetBuffer);
API(napi_create_function,MakeFunction); API(napi_set_named_property,SetProperty);
API(napi_create_double,MakeNumber); API(napi_create_object,MakeObject);
API(napi_get_boolean,MakeBoolean); API(napi_get_undefined,MakeUndefined);
API(napi_throw_error,ThrowError); API(napi_add_env_cleanup_hook,Cleanup);
static HWND target=nullptr;
static bool captured=false,hadPrevious=false;
static RAWINPUTDEVICE previous{};
static LARGE_INTEGER frequency;
double clockMs(){LARGE_INTEGER time;QueryPerformanceCounter(&time);return double(time.QuadPart)*1000.0/double(frequency.QuadPart);}
Value boolean(Env env,bool b){Value value;napi_get_boolean(env,b,&value);return value;}
Value undefined(Env env){Value value;napi_get_undefined(env,&value);return value;}
bool foreground(){return target&&IsWindow(target)&&!IsIconic(target)&&GetForegroundWindow()==GetAncestor(target,GA_ROOT);}
bool confine(){
  RECT rect;if(!GetClientRect(target,&rect)||rect.right<=0||rect.bottom<=0)return false;
  POINT points[2]={{rect.left,rect.top},{rect.right,rect.bottom}};
  MapWindowPoints(target,nullptr,points,2);
  RECT screen{points[0].x,points[0].y,points[1].x,points[1].y};return ClipCursor(&screen)!=0;
}
bool mouseRegistration(RAWINPUTDEVICE& device){
  UINT count=0;if(GetRegisteredRawInputDevices(nullptr,&count,sizeof(device))==UINT(-1))return false;
  std::vector<RAWINPUTDEVICE> devices(count);
  if(count&&GetRegisteredRawInputDevices(devices.data(),&count,sizeof(device))==UINT(-1))return false;
  for(const auto& entry:devices)if(entry.usUsagePage==1&&entry.usUsage==2){device=entry;return true;}
  return false;
}
void releaseCapture(){
  if(!captured)return;captured=false;ClipCursor(nullptr);
  RAWINPUTDEVICE current{};
  // Do not overwrite a registration another Chromium input mode installed.
  if(mouseRegistration(current)&&current.hwndTarget==target&&current.dwFlags==0){
    RAWINPUTDEVICE device=hadPrevious?previous:RAWINPUTDEVICE{1,2,RIDEV_REMOVE,nullptr};
    RegisterRawInputDevices(&device,1,sizeof(device));
  }
  target=nullptr;
}
void __cdecl cleanup(void*){releaseCapture();}
uintptr_t handleArgument(Env env,Info info){
  size_t count=1,size=0;Value value;void* data=nullptr;
  if(napi_get_cb_info(env,info,&count,&value,nullptr,nullptr)!=0||count!=1||
     napi_get_buffer_info(env,value,&data,&size)!=0||size!=sizeof(uintptr_t)){
    napi_throw_error(env,nullptr,"Expected a native Windows handle buffer");return 0;
  }
  uintptr_t handle;memcpy(&handle,data,sizeof(handle));return handle;
}
Value capture(Env env,Info info){
  const uintptr_t handle=handleArgument(env,info);if(!handle)return boolean(env,false);
  releaseCapture();target=reinterpret_cast<HWND>(handle);
  DWORD pid=0;GetWindowThreadProcessId(target,&pid);
  if(pid!=GetCurrentProcessId()){target=nullptr;napi_throw_error(env,nullptr,"Native capture target is outside the client process");return nullptr;}
  if(!foreground()){target=nullptr;return boolean(env,false);}
  hadPrevious=mouseRegistration(previous);
  // Foreground-only input on the client's HWND. No background INPUTSINK,
  // NOLEGACY, synthetic mouse movement or browser pointer-lock dependency.
  RAWINPUTDEVICE device{1,2,0,target};
  if(!RegisterRawInputDevices(&device,1,sizeof(device))){target=nullptr;return boolean(env,false);}
  captured=true;if(!confine()){releaseCapture();return boolean(env,false);}
  return boolean(env,true);
}
Value release(Env env,Info){releaseCapture();return undefined(env);}
Value refresh(Env env,Info){const bool ok=captured&&foreground()&&confine();if(!ok)releaseCapture();return boolean(env,ok);}
Value clock(Env env,Info){Value value;napi_create_double(env,clockMs(),&value);return value;}
Value read(Env env,Info info){
  const uintptr_t handle=handleArgument(env,info);if(!handle)return undefined(env);
  if(!captured||!foreground()){releaseCapture();return undefined(env);}
  RAWINPUT raw{};UINT size=sizeof(raw);
  if(GetRawInputData(reinterpret_cast<HRAWINPUT>(handle),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==UINT(-1)||
     raw.header.dwType!=RIM_TYPEMOUSE||(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE))return undefined(env);
  const auto& mouse=raw.data.mouse;if(!mouse.lLastX&&!mouse.lLastY)return undefined(env);
  Value result;napi_create_object(env,&result);
  const auto set=[&](const char* name,double number){Value value;napi_create_double(env,number,&value);napi_set_named_property(env,result,name,value);};
  set("dx",mouse.lLastX);set("dy",mouse.lLastY);set("time",clockMs());
  // Electron's window procedure remains responsible for WM_INPUT cleanup.
  return result;
}
extern "C" __declspec(dllexport) int32_t node_api_module_get_api_version_v1(){return 8;}
extern "C" __declspec(dllexport) Value napi_register_module_v1(Env env,Value exports){
  const HMODULE host=GetModuleHandleW(nullptr);
#define LOAD(name) name=reinterpret_cast<decltype(name)>(GetProcAddress(host,#name));if(!name)return nullptr
  LOAD(napi_get_cb_info);LOAD(napi_get_buffer_info);LOAD(napi_create_function);LOAD(napi_set_named_property);
  LOAD(napi_create_double);LOAD(napi_create_object);LOAD(napi_get_boolean);LOAD(napi_get_undefined);
  LOAD(napi_throw_error);LOAD(napi_add_env_cleanup_hook);
  QueryPerformanceFrequency(&frequency);napi_add_env_cleanup_hook(env,cleanup,nullptr);
  const auto bind=[&](const char* name,Callback callback){Value value;napi_create_function(env,name,SIZE_MAX,callback,nullptr,&value);napi_set_named_property(env,exports,name,value);};
  bind("capture",capture);bind("release",release);bind("refresh",refresh);bind("clock",clock);bind("read",read);return exports;
}
