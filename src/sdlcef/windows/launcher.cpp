#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <aclapi.h>
#include <sddl.h>
#include "updater_control.h"
#include "handoff_image.h"
#include <sstream>
#include <cstdlib>
namespace {
struct Lock {
  HANDLE handle=INVALID_HANDLE_VALUE;
  ~Lock(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
  bool Open(const std::filesystem::path& p){if(!Production::NoLinks(p))return false;handle=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);return handle!=INVALID_HANDLE_VALUE;}
};
int Failure(const wchar_t* text){MessageBoxW(nullptr,text,L"Qrazy",MB_OK|MB_ICONERROR);return 1;}
bool SandboxRead(const std::filesystem::path& runtime) {
  if(!Production::NoLinks(runtime))return false;
  PSECURITY_DESCRIPTOR security=nullptr;PACL old_acl=nullptr,new_acl=nullptr;
  if(GetNamedSecurityInfoW(runtime.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&old_acl,nullptr,&security)!=ERROR_SUCCESS)return false;
  PSID packages=nullptr,restricted=nullptr;bool ok=ConvertStringSidToSidW(L"S-1-15-2-1",&packages)&&ConvertStringSidToSidW(L"S-1-15-2-2",&restricted);
  if(ok){EXPLICIT_ACCESSW grants[2]{};for(int i=0;i<2;++i){grants[i].grfAccessPermissions=FILE_GENERIC_READ|FILE_GENERIC_EXECUTE;grants[i].grfAccessMode=GRANT_ACCESS;grants[i].grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;grants[i].Trustee.TrusteeForm=TRUSTEE_IS_SID;grants[i].Trustee.TrusteeType=TRUSTEE_IS_GROUP;grants[i].Trustee.ptstrName=static_cast<LPWSTR>(i?restricted:packages);}
    ok=SetEntriesInAclW(2,grants,old_acl,&new_acl)==ERROR_SUCCESS&&SetNamedSecurityInfoW(const_cast<LPWSTR>(runtime.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,new_acl,nullptr)==ERROR_SUCCESS;
  }if(new_acl)LocalFree(new_acl);if(packages)LocalFree(packages);if(restricted)LocalFree(restricted);LocalFree(security);return ok;
}
int RecoverTransition(const std::filesystem::path& root) {
  std::string output;
  if(!Production::Helper(root,L"transition-recovery",output)) {
    output.clear();if(!Production::Helper(root,L"transition-recovery",output,nullptr,root/L".qrazy-transition"/L"next"))return -1;
  }
  std::istringstream lines(output);std::string line;if(!std::getline(lines,line))return -1;
  DWORD resident=0;
  if(line.starts_with("RESIDENT\t")) {
    auto pid=line.substr(9);if(pid.empty()||pid.size()>10||pid.find_first_not_of("0123456789")!=std::string::npos)return -1;
    auto n=std::strtoul(pid.c_str(),nullptr,10);if(!n||n>0x7fffffff)return -1;resident=static_cast<DWORD>(n);
  }else if(line!="RECOVERY")return -1;
  std::vector<std::unique_ptr<Handoff::ImagePin>> pins;bool executable=false,asar=false,addon=false;
  Handoff::ImagePin* broker=nullptr;
  while(std::getline(lines,line)) {
    auto tab=line.find('\t');if(tab==std::string::npos||pins.size()>=4096)return -1;
    auto relative=line.substr(0,tab),hash=line.substr(tab+1);
    if(relative.empty()||relative.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_. /-")!=std::string::npos)return -1;
    std::filesystem::path p(relative);if(p.is_absolute())return -1;for(auto& part:p)if(part==L".."||part==L"."||part.empty())return -1;
    auto pin=std::make_unique<Handoff::ImagePin>();if(!pin->Open(root/L".qrazy-transition"/L"auth"/p,hash))return -1;
    executable|=relative=="runtime/electron.exe";asar|=relative=="runtime/resources/app.asar";addon|=relative=="runtime/resources/qrazy-handoff.node";
    if(relative=="runtime/electron.exe")broker=pin.get();
    pins.push_back(std::move(pin));
  }
  if(!executable||!asar||!addon)return -1;
  if(resident){Handoff::Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,resident));if(broker&&broker->Process(peer.value))return 0;}
  auto exe=root/L".qrazy-transition"/L"auth"/L"runtime"/L"electron.exe";
  std::wstring command=Production::Quote(exe.wstring())+L" "+Production::Quote(L"--qrazy-auth-recover="+root.wstring())+
    L" --qrazy-recovery-parent="+std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&child))return -1;
  CloseHandle(child.hThread);CloseHandle(child.hProcess);return 1;
}
}
static void PreferHighPerformanceGpu(const std::wstring& executable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
      L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
      KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  DWORD type = 0, bytes = 0;
  const LSTATUS existing = RegQueryValueExW(key, executable.c_str(), nullptr, &type, nullptr, &bytes);
  std::wstring preference;
  bool writable = existing == ERROR_FILE_NOT_FOUND;
  if (existing == ERROR_SUCCESS && type == REG_SZ && bytes <= 65536 && bytes % sizeof(wchar_t) == 0) {
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD readBytes = bytes;
    if (RegQueryValueExW(key, executable.c_str(), nullptr, &type,
        reinterpret_cast<BYTE*>(data.data()), &readBytes) == ERROR_SUCCESS && type == REG_SZ) {
      preference.assign(data.data());
      writable = true;
      size_t start = 0;
      while (start < preference.size()) {
        const size_t end = preference.find(L';', start);
        const std::wstring field = preference.substr(start, end == std::wstring::npos ? end : end - start);
        if (field.compare(0, 14, L"GpuPreference=") == 0) {
          if (field == L"GpuPreference=0" || field == L"GpuPreference=1") preference.replace(start, field.size(), L"GpuPreference=2");
          // Preserve high performance and unknown future preference values.
          else writable = false;
          break;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
      }
    }
  }
  if (writable) {
    if (preference.find(L"GpuPreference=") == std::wstring::npos) {
      if (!preference.empty() && preference.back() != L';') preference += L';';
      preference += L"GpuPreference=2;";
    }
    RegSetValueExW(key, executable.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(preference.c_str()),
        static_cast<DWORD>((preference.size() + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(key);
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR command,int) {
  if(command&&*command)return 2;wchar_t name[32768];DWORD n=GetModuleFileNameW(nullptr,name,32768);if(!n||n>=32768)return 2;
  auto root=std::filesystem::path(name).parent_path();Production::CleanEnvironment();
  Lock installation;if(!installation.Open(root/L"install.lock"))return Failure(L"Qrazy is already running, or an update is in progress. Close it normally before trying again.");
  if(std::filesystem::exists(root/L".qrazy-transition")) {
    int recovered=RecoverTransition(root);
    if(recovered<0)return Failure(L"The transition could not be safely recovered. Keep this folder and both profiles intact. Replace the application files from an official full download if recovery remains unavailable.");
    if(recovered>0){MessageBoxW(nullptr,L"Close this notice, then reopen Qrazy in a few seconds. The previous client will be restored and your profiles kept.",L"Qrazy recovery",MB_OK|MB_ICONINFORMATION);return 0;}
  }
  auto profile=root/L"profile-sdlcef-windows";std::error_code error;std::filesystem::create_directories(profile,error);if(error||!Production::NoLinks(profile))return Failure(L"The Qrazy profile folder is unavailable.");
  std::string output;
  {Lock host,worker;if(!host.Open(profile/L"host.lock")||!worker.Open(profile/L"desktop-worker.lock"))return Failure(L"Close the running Qrazy client before starting this copy.");
    if(!Production::Helper(root,L"verify",output))return Failure(L"Qrazy runtime verification failed. Keep the whole folder together. No unverified runtime was started.");}
  auto runtime=root/L"runtime";auto exe=runtime/L"Qrazy.exe";
  if(!SandboxRead(runtime))return Failure(L"Qrazy could not prepare sandbox read access for its public runtime files.");
  PreferHighPerformanceGpu(std::filesystem::path(name).wstring());PreferHighPerformanceGpu(exe.wstring());
  SetEnvironmentVariableW(L"SHIM_MCCOMPAT",L"0x800000001");
  if(!SetEnvironmentVariableW(L"QRAZY_SDLCEF_ROOT",root.c_str()))return 2;
  auto cmd=Production::Quote(exe.wstring());STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
  if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,runtime.c_str(),&startup,&process))return Failure(L"Unable to start the verified Qrazy runtime.");
  CloseHandle(process.hThread);WaitForSingleObject(process.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
  if(code==Production::InstallExit){Lock host,worker;if(!host.Open(profile/L"host.lock")||!worker.Open(profile/L"desktop-worker.lock"))return Failure(L"Update refused because a Qrazy process still owns the profile. Close it normally and try again.");
    output.clear();if(!Production::Helper(root,L"install",output))return Failure(L"Update installation failed. Keep the folder intact for recovery on your next manual launch.");
    MessageBoxW(nullptr,L"Update installed. Open Qrazy manually.",L"Qrazy",MB_OK|MB_ICONINFORMATION);return 0;
  }
  if(code==8)return Failure(L"Session transfer did not finish. Both profiles and the previous client are kept. Reopen Qrazy to recover the previous client and retry.");
  return static_cast<int>(code);
}
