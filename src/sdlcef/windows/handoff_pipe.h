#pragma once
// Native-only IPC for a process launched from a pinned, authenticated image.
// Callers retain the expected process HANDLE throughout the exchange. A PID or
// website-supplied path alone is never sufficient authority to send a ticket.
#include <windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
#include <string>
#include <vector>
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"bcrypt.lib")
namespace Handoff {
constexpr DWORD Maximum=512;
struct Handle {
  HANDLE value=INVALID_HANDLE_VALUE;
  Handle()=default;explicit Handle(HANDLE v):value(v){}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
  ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
inline bool SameUser(HANDLE peer) {
  Handle own,other;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&own.value)||!OpenProcessToken(peer,TOKEN_QUERY,&other.value))return false;
  DWORD a=0,b=0;GetTokenInformation(own.value,TokenUser,nullptr,0,&a);GetTokenInformation(other.value,TokenUser,nullptr,0,&b);
  if(!a||!b||a>65536||b>65536)return false;
  std::vector<unsigned char> x(a),y(b);
  return GetTokenInformation(own.value,TokenUser,x.data(),a,&a)&&GetTokenInformation(other.value,TokenUser,y.data(),b,&b)&&
    EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid);
}
inline bool Peer(HANDLE pipe,HANDLE expected,bool server) {
  ULONG pid=0;if(!(server?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid)))return false;
  if(!pid||pid!=GetProcessId(expected)||WaitForSingleObject(expected,0)!=WAIT_TIMEOUT)return false;
  Handle actual(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
  if(actual.value==nullptr)return false;
  FILETIME c1{},c2{},exit{},kernel{},user{};
  return GetProcessTimes(expected,&c1,&exit,&kernel,&user)&&GetProcessTimes(actual.value,&c2,&exit,&kernel,&user)&&
    c1.dwLowDateTime==c2.dwLowDateTime&&c1.dwHighDateTime==c2.dwHighDateTime&&SameUser(actual.value);
}
inline bool Complete(HANDLE pipe,OVERLAPPED& pending,DWORD& bytes,ULONGLONG deadline) {
  const ULONGLONG now=GetTickCount64();
  if(now>=deadline||WaitForSingleObject(pending.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){
    CancelIoEx(pipe,&pending);WaitForSingleObject(pending.hEvent,INFINITE);return false;
  }
  return GetOverlappedResult(pipe,&pending,&bytes,FALSE)!=FALSE;
}
inline bool IO(HANDLE pipe,void* data,DWORD length,bool write,ULONGLONG deadline) {
  if(!length||length>Maximum||deadline<=GetTickCount64()||deadline-GetTickCount64()>30000)return false;
  Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
  OVERLAPPED pending{};pending.hEvent=event.value;DWORD bytes=0;
  const BOOL done=write?WriteFile(pipe,data,length,&bytes,&pending):ReadFile(pipe,data,length,&bytes,&pending);
  if(!done&&(GetLastError()!=ERROR_IO_PENDING||!Complete(pipe,pending,bytes,deadline)))return false;
  return bytes==length;
}
inline HANDLE Server(std::wstring& name) {
  std::array<unsigned char,16> random{};
  if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return INVALID_HANDLE_VALUE;
  name=L"\\\\.\\pipe\\Qrazy-handoff-";
  for(auto b:random){name+=L"0123456789abcdef"[b>>4];name+=L"0123456789abcdef"[b&15];}
  Handle token;DWORD size=0;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return INVALID_HANDLE_VALUE;
  GetTokenInformation(token.value,TokenUser,nullptr,0,&size);if(!size||size>65536)return INVALID_HANDLE_VALUE;
  std::vector<unsigned char> info(size);
  if(!GetTokenInformation(token.value,TokenUser,info.data(),size,&size))return INVALID_HANDLE_VALUE;
  LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid,&sid))return INVALID_HANDLE_VALUE;
  std::wstring acl=L"D:P(A;;GA;;;";acl+=sid;acl+=L")";LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
  HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,Maximum,Maximum,0,&security);
  LocalFree(descriptor);return pipe;
}
inline bool Connect(HANDLE pipe,ULONGLONG deadline) {
  if(deadline<=GetTickCount64()||deadline-GetTickCount64()>30000)return false;
  Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;
  OVERLAPPED pending{};pending.hEvent=event.value;DWORD bytes=0;
  if(ConnectNamedPipe(pipe,&pending))return true;
  DWORD error=GetLastError();return error==ERROR_PIPE_CONNECTED||(error==ERROR_IO_PENDING&&Complete(pipe,pending,bytes,deadline));
}
struct Ticket {
  std::array<char,43> code{};bool consumed=false,anonymous=false;
  Ticket()=default;Ticket(const Ticket&)=delete;Ticket& operator=(const Ticket&)=delete;
  ~Ticket(){Erase();}
  void Erase(){SecureZeroMemory(code.data(),code.size());consumed=true;}
  bool Send(HANDLE pipe,HANDLE expected,ULONGLONG deadline) {
    if(consumed||!Peer(pipe,expected,true)){Erase();return false;}
    if(anonymous){consumed=true;char kind='G';bool ok=IO(pipe,&kind,1,true,deadline);Erase();return ok;}
    for(char c:code)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')){Erase();return false;}
    consumed=true;char kind='S';bool ok=IO(pipe,&kind,1,true,deadline)&&IO(pipe,code.data(),static_cast<DWORD>(code.size()),true,deadline);Erase();return ok;
  }
};
}
