#pragma once
// Fixed helper inputs are hash-pinned into both native host and launcher.
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include "updater_pins.h"
#pragma comment(lib,"bcrypt.lib")
namespace Production {
constexpr int InstallExit=42;
inline bool NoLinks(std::filesystem::path p){for(;;){DWORD a=GetFileAttributesW(p.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;auto next=p.parent_path();if(next==p||next.empty())break;p=next;}return true;}
struct Pins {
  std::vector<HANDLE> files;
  ~Pins(){for(HANDLE h:files)CloseHandle(h);}
  bool Check(const std::filesystem::path& root) {
    for(const auto& item:ControlPins) {
      auto p=root/L"updater"/item.name;if(!NoLinks(p))return false;
      HANDLE f=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
      if(f==INVALID_HANDLE_VALUE)return false;files.push_back(f);
      BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(f,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
      BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
      if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
      bool ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;std::array<unsigned char,65536> b{};DWORD n=0;
      while(ok){if(!ReadFile(f,b.data(),static_cast<DWORD>(b.size()),&n,nullptr)){ok=false;break;}if(!n)break;ok=BCryptHashData(hash,b.data(),n,0)>=0;}
      unsigned char digest[32]{};if(ok)ok=BCryptFinishHash(hash,digest,32,0)>=0;if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
      std::string hex;for(auto value:digest){hex+="0123456789abcdef"[value>>4];hex+="0123456789abcdef"[value&15];}
      if(!ok||hex!=item.sha256)return false;
    }return true;
  }
};
inline std::wstring Quote(const std::wstring& text){if(text.find(L'"')!=std::wstring::npos)return {};return L"\""+text+L"\"";}
inline bool Helper(const std::filesystem::path& root,const std::wstring& operation,std::string& output,std::atomic<bool>* cancel=nullptr,const std::filesystem::path& controls_root={}) {
  if(operation!=L"verify"&&operation!=L"check"&&operation!=L"state"&&operation!=L"update"&&operation!=L"prepare"&&operation!=L"install"&&operation!=L"transition-peer"&&operation!=L"transition-finish"&&operation!=L"transition-recovery")return false;
  const auto controls=controls_root.empty()?root:controls_root;
  if(controls!=root&&(operation!=L"transition-recovery"||controls!=root/L".qrazy-transition"/L"next"))return false;
  Pins pins;if(!pins.Check(controls))return false;
  // Node flags and environment are controlled by native startup, never website data.
  auto exe=controls/L"updater"/L"node.exe";auto script=operation==L"check"?root/L"runtime"/L"update-notice.cjs":operation==L"update"?root/L"runtime"/L"update-download.cjs":controls/L"updater"/L"production-runtime.cjs";
  std::wstring cmd=Quote(exe.wstring())+L" --use-bundled-ca --no-addons --no-global-search-paths "+Quote(script.wstring())+L" "+operation+L" "+Quote(root.wstring());
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;if(!CreatePipe(&read,&write,&sa,0))return false;
  SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
  HANDLE nul=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
  startup.StartupInfo.hStdOutput=write;startup.StartupInfo.hStdError=nul;startup.StartupInfo.hStdInput=nul;
  SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> attributes(bytes);startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  HANDLE inherited[]={write,nul};bool attributes_ok=InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes)&&UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr);
  HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  bool job_ok=job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));PROCESS_INFORMATION process{};
  bool launched=attributes_ok&&job_ok&&CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,root.c_str(),&startup.StartupInfo,&process);
  if(attributes_ok)DeleteProcThreadAttributeList(startup.lpAttributeList);CloseHandle(write);CloseHandle(nul);
  bool ok=false;
  if(launched) {
    if(AssignProcessToJobObject(job,process.hProcess)){ResumeThread(process.hThread);ULONGLONG deadline=GetTickCount64()+900000;bool expired=false;
      for(;;){DWORD available=0;if(PeekNamedPipe(read,nullptr,0,nullptr,&available,nullptr)&&available){char buffer[4096];DWORD n=0;if(!ReadFile(read,buffer,std::min<DWORD>(available,sizeof(buffer)),&n,nullptr))break;output.append(buffer,n);if(output.size()>4*1024*1024){expired=true;break;}continue;}
        if(WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0){DWORD code=1;GetExitCodeProcess(process.hProcess,&code);ok=code==0;break;}
        if(GetTickCount64()>deadline||(cancel&&cancel->load())){expired=true;break;}Sleep(10);
      }if(expired)TerminateJobObject(job,1);
    }else TerminateProcess(process.hProcess,1); // This task's suspended Node helper only.
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
  }
  if(job)CloseHandle(job);CloseHandle(read);return ok;
}
inline void CleanEnvironment(){for(auto name:{L"NODE_OPTIONS",L"NODE_PATH",L"NODE_EXTRA_CA_CERTS",L"OPENSSL_CONF",L"SSL_CERT_FILE",L"SSL_CERT_DIR"})SetEnvironmentVariableW(name,nullptr);}
}
