#pragma once
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
/* Opt-in network diagnostics: create an empty file "net-log.on" in the profile folder (next to chromium.log) and restart.
   - Chromium's net log goes to net-log.json (previous launches rotate to net-log.1.json, net-log.2.json; older ones are deleted).
     Capture mode Default: no cookies, credentials or payload bytes.
   - Windows network-interface changes (the OS side of ERR_NETWORK_CHANGED) are appended with timestamps to network-events.log (truncated past 512 KB at startup). */
namespace NetDiag {
inline std::wstring net_log_path; /* empty = off; read by App::OnBeforeCommandLineProcessing in the browser process */
inline std::filesystem::path events_path;
inline HANDLE notify=nullptr;
inline void Event(const char* text) {
  if(events_path.empty())return;
  SYSTEMTIME t;GetLocalTime(&t);char stamp[48];
  std::snprintf(stamp,sizeof stamp,"%04d-%02d-%02d %02d:%02d:%02d.%03d ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
  std::ofstream out(events_path,std::ios::app|std::ios::binary);out<<stamp<<text<<"\r\n";
}
inline VOID CALLBACK OnInterfaceChange(PVOID,PMIB_IPINTERFACE_ROW row,MIB_NOTIFICATION_TYPE type) {
  char line[160];const char* kind=type==MibParameterNotification?"parameter":type==MibAddInstance?"add":type==MibDeleteInstance?"delete":type==MibInitialNotification?"initial":"other";
  std::snprintf(line,sizeof line,"interface-change %s family=%s luid=%llu",kind,row?(row->Family==AF_INET?"ipv4":row->Family==AF_INET6?"ipv6":"?"):"?",row?static_cast<unsigned long long>(row->InterfaceLuid.Value):0ull);
  Event(line);
}
inline void Start(const std::filesystem::path& profile) {
  std::error_code ec;
  if(!std::filesystem::exists(profile/L"net-log.on",ec))return;
  events_path=profile/L"network-events.log";
  if(std::filesystem::file_size(events_path,ec)>512*1024)std::filesystem::remove(events_path,ec);
  std::filesystem::remove(profile/L"net-log.2.json",ec);
  std::filesystem::rename(profile/L"net-log.1.json",profile/L"net-log.2.json",ec);
  std::filesystem::rename(profile/L"net-log.json",profile/L"net-log.1.json",ec);
  net_log_path=(profile/L"net-log.json").wstring();
  Event("client start, net log enabled");
  NotifyIpInterfaceChange(AF_UNSPEC,OnInterfaceChange,nullptr,FALSE,&notify);
}
inline void Stop() {
  if(notify){CancelMibChangeNotify2(notify);notify=nullptr;}
  Event("client stop");
}
}
