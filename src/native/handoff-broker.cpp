#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../sdlcef/windows/handoff_broker.h"
#include "../sdlcef/windows/handoff_receiver.h"
struct napi_env__;struct napi_value__;struct napi_callback_info__;
using Env=napi_env__*;using Value=napi_value__*;using Info=napi_callback_info__*;using Callback=Value(__cdecl*)(Env,Info);
static int(__cdecl* args)(Env,Info,size_t*,Value*,Value*,void**);
static int(__cdecl* text)(Env,Value,char16_t*,size_t,size_t*);
static int(__cdecl* buffer)(Env,Value,void**,size_t*);
static int(__cdecl* number)(Env,double,Value*);
static int(__cdecl* string)(Env,const char16_t*,size_t,Value*);
static int(__cdecl* boolean)(Env,bool,Value*);
static int(__cdecl* function)(Env,const char*,size_t,Callback,void*,Value*);
static int(__cdecl* property)(Env,Value,const char*,Value);
static int(__cdecl* cleanup)(Env,void(__cdecl*)(void*),void*);
static int(__cdecl* uint32)(Env,Value,uint32_t*);
static int(__cdecl* copyBuffer)(Env,size_t,const void*,void**,Value*);
static std::unique_ptr<Handoff::Broker> broker;
static std::unique_ptr<Handoff::Receiver> receiver;
static std::vector<std::unique_ptr<Handoff::Handle>> locks;
static std::wstring Read(Env env,Value value) {
  size_t size=0;if(text(env,value,nullptr,0,&size)||!size||size>32767)return {};
  std::vector<char16_t> data(size+1);if(text(env,value,data.data(),data.size(),&size))return {};
  std::wstring result(reinterpret_cast<const wchar_t*>(data.data()),size);
  if(result.find(L'\0')!=std::wstring::npos)return {};return result;
}
static Value Bool(Env env,bool ok){Value result{};boolean(env,ok,&result);return result;}
static Value Pin(Env env,Info info) {
  Value values[2]{};size_t count=2;if(args(env,info,&count,values,nullptr,nullptr)||count!=2||!broker)return Bool(env,false);
  auto path=Read(env,values[0]),hash=Read(env,values[1]);
  std::string ascii;for(wchar_t c:hash){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return Bool(env,false);ascii+=static_cast<char>(c);}
  return Bool(env,hash.size()==64&&(receiver?receiver->Pin(path,ascii):broker->Pin(path,ascii)));
}
static Value Start(Env env,Info info) {
  Value value{};size_t count=1;Value result{};
  if(args(env,info,&count,&value,nullptr,nullptr)||count!=1||!broker||!broker->Start(Read(env,value)))return Bool(env,false);
  string(env,reinterpret_cast<const char16_t*>(broker->name.data()),broker->name.size(),&result);return result;
}
static Value Poll(Env env,Info){Value result{};number(env,receiver?receiver->state.load():broker?broker->state.load():Handoff::Broker::Failed,&result);return result;}
static Value Submit(Env env,Info info) {
  Value value{};size_t count=1,size=0;void* bytes=nullptr;
  if(args(env,info,&count,&value,nullptr,nullptr)||count!=1||buffer(env,value,&bytes,&size)||!broker)return Bool(env,false);
  bool ok=broker->Submit(static_cast<const char*>(bytes),size);if(bytes)SecureZeroMemory(bytes,size);return Bool(env,ok);
}
static Value Mode(Env env,Info){if(receiver||!broker||broker->state!=Handoff::Broker::Idle)return Bool(env,false);receiver=std::make_unique<Handoff::Receiver>();return Bool(env,true);}
static Value Connect(Env env,Info info) {
  Value values[3]{};size_t count=3;uint32_t pid=0;
  if(args(env,info,&count,values,nullptr,nullptr)||count!=3||uint32(env,values[1],&pid)||!receiver)return Bool(env,false);
  const auto name=Read(env,values[0]);const std::wstring prefix=L"\\\\.\\pipe\\Qrazy-handoff-";
  if(name.size()!=prefix.size()+32||name.substr(0,prefix.size())!=prefix)return Bool(env,false);
  for(size_t i=prefix.size();i<name.size();++i)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return Bool(env,false);
  return Bool(env,receiver->Start(name,pid,Read(env,values[2])));
}
static Value Take(Env env,Info) {
  std::array<char,43> code{};if(!receiver||!receiver->Take(code))return Bool(env,false);
  Value result{};copyBuffer(env,receiver->Anonymous()?0:code.size(),code.data(),nullptr,&result);SecureZeroMemory(code.data(),code.size());return result;
}
static Value Ack(Env env,Info){if(!receiver||receiver->state!=Handoff::Receiver::Ready)return Bool(env,false);receiver->Acknowledge();return Bool(env,true);}
static Value Exited(Env env,Info){bool ok=receiver&&receiver->SourceExited();if(ok)receiver->ReleaseFiles();return Bool(env,ok);}
static Value Sender(Env env,Info){if(!receiver||!receiver->SourceExited())return Bool(env,false);receiver.reset();return Bool(env,true);}
static Value Authorize(Env env,Info){return Bool(env,broker&&broker->Authorize());}
static Value Track(Env env,Info info) {
  Value values[2]{};size_t count=2;uint32_t pid=0;
  if(args(env,info,&count,values,nullptr,nullptr)||count!=2||uint32(env,values[0],&pid)||!broker)return Bool(env,false);
  return Bool(env,broker->Track(pid,Read(env,values[1])));
}
static Value TrackedExited(Env env,Info){return Bool(env,broker&&broker->TrackedExited());}
static Value Lock(Env env,Info info) {
  Value value{};size_t count=1;if(args(env,info,&count,&value,nullptr,nullptr)||count!=1||locks.size()>=3)return Bool(env,false);
  auto path=std::filesystem::path(Read(env,value));if(path.empty()||!Handoff::NoLinks(path.parent_path()))return Bool(env,false);
  DWORD attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&attributes&FILE_ATTRIBUTE_REPARSE_POINT)return Bool(env,false);
  auto handle=std::make_unique<Handoff::Handle>(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(handle->value==INVALID_HANDLE_VALUE)return Bool(env,false);locks.push_back(std::move(handle));return Bool(env,true);
}
static Value Unlock(Env env,Info){locks.clear();return Bool(env,true);}
static Value AssetMove(Env env,Info info) {
  Value values[2]{};size_t count=2;
  if(args(env,info,&count,values,nullptr,nullptr)||count!=2||locks.size()!=2)return Bool(env,false);
  const std::filesystem::path from(Read(env,values[0])),to(Read(env,values[1]));
  if(!from.is_absolute()||!to.is_absolute()||from.parent_path()!=to.parent_path()||to.parent_path().filename()!=L"desktop-assets"||
    to.parent_path().parent_path().filename()!=L"profile-sdlcef-windows"||!Handoff::NoLinks(from))return Bool(env,false);
  auto a=from.filename().wstring(),b=to.filename().wstring();
  if(a.size()!=50||a.substr(0,10)!=L"migration-"||a.substr(42)!=L".partial"||b.size()!=70||b.substr(64)!=L".asset")return Bool(env,false);
  for(size_t i=10;i<42;++i)if(!((a[i]>='0'&&a[i]<='9')||(a[i]>='a'&&a[i]<='f')))return Bool(env,false);
  for(size_t i=0;i<64;++i)if(!((b[i]>='0'&&b[i]<='9')||(b[i]>='a'&&b[i]<='f')))return Bool(env,false);
  // Atomic no-replace rename works on portable Windows filesystems without
  // requiring hard-link support. Native host/worker exclusion is held by caller.
  return Bool(env,MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);
}
static Value SwapLauncher(Env env,Info info) {
  Value values[2]{};size_t count=2;if(args(env,info,&count,values,nullptr,nullptr)||count!=2||locks.size()!=3)return Bool(env,false);
  const std::filesystem::path from(Read(env,values[0])),to(Read(env,values[1]));
  if(!from.is_absolute()||!to.is_absolute()||from.filename()!=L"Qrazy.exe"||to.filename()!=L"Qrazy.exe"||
    (from.parent_path().filename()!=L"next"&&from.parent_path().filename()!=L"previous")||
    from.parent_path().parent_path()!=to.parent_path()/L".qrazy-transition"||!Handoff::NoLinks(from)||!Handoff::NoLinks(to))return Bool(env,false);
  return Bool(env,MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE);
}
static Value Stop(Env env,Info){broker.reset();receiver.reset();return Bool(env,true);}
static void Cleanup(void*){broker.reset();receiver.reset();locks.clear();}
extern "C" __declspec(dllexport) int32_t node_api_module_get_api_version_v1(){return 8;}
extern "C" __declspec(dllexport) Value napi_register_module_v1(Env env,Value exports) {
  if(broker)return nullptr;auto host=GetModuleHandleW(nullptr);
#define LOAD(local,name) local=reinterpret_cast<decltype(local)>(GetProcAddress(host,name));if(!local)return nullptr
  LOAD(args,"napi_get_cb_info");LOAD(text,"napi_get_value_string_utf16");LOAD(buffer,"napi_get_buffer_info");
  LOAD(number,"napi_create_double");LOAD(string,"napi_create_string_utf16");LOAD(boolean,"napi_get_boolean");
  LOAD(function,"napi_create_function");LOAD(property,"napi_set_named_property");LOAD(cleanup,"napi_add_env_cleanup_hook");
  LOAD(uint32,"napi_get_value_uint32");LOAD(copyBuffer,"napi_create_buffer_copy");
  broker=std::make_unique<Handoff::Broker>();cleanup(env,Cleanup,nullptr);
  for(auto entry:{std::pair{"pin",Pin},std::pair{"start",Start},std::pair{"poll",Poll},std::pair{"submit",Submit},std::pair{"stop",Stop},
    std::pair{"receiver",Mode},std::pair{"sender",Sender},std::pair{"connect",Connect},std::pair{"take",Take},std::pair{"acknowledge",Ack},std::pair{"sourceExited",Exited},std::pair{"lock",Lock},std::pair{"unlock",Unlock},std::pair{"authorize",Authorize},std::pair{"track",Track},std::pair{"trackedExited",TrackedExited},std::pair{"assetMove",AssetMove},std::pair{"swapLauncher",SwapLauncher}}) {
    Value value{};function(env,entry.first,SIZE_MAX,entry.second,nullptr,&value);property(env,exports,entry.first,value);
  }
  return exports;
}
