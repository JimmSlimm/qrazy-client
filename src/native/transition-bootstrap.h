#pragma once
// Selected ONLY for the legacy-compatible intermediate delivery. The published
// portable updater always invokes the replacement root EXE from PowerShell.
// That invocation must not open a client. The player manually double-clicks
// Qrazy afterward. Fail closed if parent provenance cannot be established (the
// helper can exit before this process begins running).
#include <windows.h>
#include <tlhelp32.h>
#include <string>
namespace TransitionBootstrap {
inline bool ExplorerImage(const std::wstring& image,const std::wstring& windows) {
  const std::wstring expected=windows+L"\\explorer.exe";
  return CompareStringOrdinal(image.c_str(),-1,expected.c_str(),-1,TRUE)==CSTR_EQUAL;
}
inline bool ManualStart() {
  DWORD parent=0;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return false;
  if(Process32FirstW(snapshot,&entry))do{
    if(entry.th32ProcessID==GetCurrentProcessId()){parent=entry.th32ParentProcessID;break;}
  }while(Process32NextW(snapshot,&entry));
  CloseHandle(snapshot);if(!parent)return false;
  HANDLE peer=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,parent);
  if(!peer)return false;
  FILETIME own{},created{},exit{},kernel{},user{};
  wchar_t image[32768]{},windows[32768]{};DWORD count=32768;
  const UINT n=GetWindowsDirectoryW(windows,32768);
  bool ok=n&&n<32768&&WaitForSingleObject(peer,0)==WAIT_TIMEOUT&&
    GetProcessTimes(peer,&created,&exit,&kernel,&user)&&
    GetProcessTimes(GetCurrentProcess(),&own,&exit,&kernel,&user)&&
    CompareFileTime(&created,&own)<=0&&QueryFullProcessImageNameW(peer,0,image,&count)&&
    ExplorerImage(std::wstring(image,count),std::wstring(windows,n));
  CloseHandle(peer);return ok;
}
}
