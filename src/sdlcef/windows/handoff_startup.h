#pragma once
#include "handoff_image.h"
#include "handoff_redeem.h"
#include "updater_control.h"
#include <thread>
#include <mutex>
#include <condition_variable>
namespace Handoff {
// Only used when a full-distribution transaction is present. Ordinary startups
// do not discover peers or request authentication handoff codes.
class StartupPeer {
 public:
  ~StartupPeer(){Stop();}
  bool Start(const std::filesystem::path& root) {
    root_=root;std::string output;
    if(!Production::Helper(root,L"transition-peer",output))return false;
    auto value=CefParseJSON(output,JSON_PARSER_RFC);auto outer=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
    auto data=outer&&outer->GetBool("ok")?outer->GetDictionary("data"):nullptr;
    if(!data||data->GetSize()!=4||data->GetType("pid")!=VTYPE_INT||data->GetInt("pid")<1||
      data->GetType("pipe")!=VTYPE_STRING||data->GetType("identity")!=VTYPE_STRING||data->GetType("files")!=VTYPE_LIST)return false;
    auto name=data->GetString("pipe").ToWString();const std::wstring prefix=L"\\\\.\\pipe\\Qrazy-handoff-";
    if(name.size()!=prefix.size()+32||name.substr(0,prefix.size())!=prefix)return false;
    for(size_t i=prefix.size();i<name.size();++i)if(!((name[i]>='a'&&name[i]<='f')||(name[i]>='0'&&name[i]<='9')))return false;
    auto files=data->GetList("files");if(!files||files->GetSize()>4096)return false;
    ImagePin* executable=nullptr;
    for(size_t i=0;i<files->GetSize();++i) {
      auto item=files->GetDictionary(i);if(!item)return false;
      auto relative=item->GetString("path").ToWString();
      // The pinned helper already checks signed paths. Check components again
      // before native path composition; unsigned endpoint fields select none.
      if(relative.empty()||relative.find(L':')!=std::wstring::npos||relative.find(L'\\')!=std::wstring::npos||relative.front()==L'/')return false;
      auto p=std::filesystem::path(relative);for(const auto& part:p)if(part==L".."||part==L".")return false;
      auto pin=std::make_unique<ImagePin>();if(!pin->Open(root/L".qrazy-transition"/L"auth"/p,item->GetString("sha256")))return false;
      if(relative==L"runtime/electron.exe")executable=pin.get();pins_.push_back(std::move(pin));
    }
    peer_.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,static_cast<DWORD>(data->GetInt("pid")));
    if(!executable||!executable->Process(peer_.value))return false;
    worker_=std::thread([this,name]{
      pipe_.value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
      if(pipe_.value==INVALID_HANDLE_VALUE||!Peer(pipe_.value,peer_.value,false)) {state_=Failed;return;}
      auto deadline=GetTickCount64()+15000;char kind=0;
      if(!IO(pipe_.value,&kind,1,false,deadline)||(kind!='G'&&kind!='S')||
        (kind=='S'&&!IO(pipe_.value,code_.data(),static_cast<DWORD>(code_.size()),false,deadline))) {SecureZeroMemory(code_.data(),code_.size());state_=Failed;return;}
      anonymous_=kind=='G';
      state_=TicketReady;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock,std::chrono::seconds(15),[this]{return acknowledged_||stop_;});
      }
      if(!acknowledged_||stop_||!Peer(pipe_.value,peer_.value,false)) {state_=Failed;return;}
      char receipt='K',complete=0;deadline=GetTickCount64()+5000;
      if(!IO(pipe_.value,&receipt,1,true,deadline)||!IO(pipe_.value,&complete,1,false,deadline)||complete!='D'||
        WaitForSingleObject(peer_.value,5000)!=WAIT_OBJECT_0) {state_=Failed;return;}
      // Recovery contains the broker's running executable. Release its file
      // pins only AFTER that process exits normally, then request cleanup.
      pins_.clear();std::string cleaned;
      state_=Production::Helper(root_,L"transition-finish",cleaned)?Passed:Failed;
    });return true;
  }
  bool Take(std::array<char,43>& code) {
    if(state_!=TicketReady||taken_)return false;taken_=true;code=code_;SecureZeroMemory(code_.data(),code_.size());return true;
  }
  void Acknowledge(){std::lock_guard<std::mutex> lock(mutex_);acknowledged_=true;condition_.notify_all();}
  bool Done() const{return state_==Passed;}
  bool Anonymous() const{return anonymous_;}
  bool Bad() const{return state_==Failed;}
  void Stop() {
    stop_=true;condition_.notify_all();if(pipe_.value!=INVALID_HANDLE_VALUE)CancelIoEx(pipe_.value,nullptr);
    if(worker_.joinable())worker_.join();SecureZeroMemory(code_.data(),code_.size());pins_.clear();
  }
 private:
  enum State {Waiting,TicketReady,Passed,Failed};std::atomic<int> state_{Waiting};
  Handle pipe_,peer_;std::vector<std::unique_ptr<ImagePin>> pins_;std::thread worker_;
  std::mutex mutex_;std::condition_variable condition_;std::atomic<bool> stop_{false};bool acknowledged_=false,taken_=false,anonymous_=false;
  std::array<char,43> code_{};std::filesystem::path root_;
};
}
