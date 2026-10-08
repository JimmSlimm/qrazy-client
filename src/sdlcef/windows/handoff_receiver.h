#pragma once
#include "handoff_image.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <tlhelp32.h>
namespace Handoff {
class Receiver {
 public:
  enum State {Idle,Waiting,Ready,Passed,Failed};std::atomic<int> state{Idle};
  ~Receiver(){Stop();}
  bool Pin(const std::filesystem::path& path,const std::string& hash) {
    if(state!=Idle||pins_.size()>=4096)return false;auto pin=std::make_unique<ImagePin>();
    if(!pin->Open(path,hash))return false;pins_.push_back(std::move(pin));return true;
  }
  bool Start(const std::wstring& name,DWORD pid,const std::filesystem::path& executable) {
    if(state!=Idle||!pid)return false;ImagePin* image=nullptr;
    for(auto& p:pins_)if(p->path==std::filesystem::absolute(executable).lexically_normal())image=p.get();
    peer_.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if(!image||!image->Process(peer_.value))return false;
    // Retain the source launcher too. Original Electron runtime exit alone
    // does not prove the launcher has released its installation lock.
    DWORD parent=0;PROCESSENTRY32W e{};e.dwSize=sizeof(e);Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(snapshot.value==INVALID_HANDLE_VALUE)return false;
    if(Process32FirstW(snapshot.value,&e))do{if(e.th32ProcessID==pid){parent=e.th32ParentProcessID;break;}}while(Process32NextW(snapshot.value,&e));
    launcher_.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,parent);
    ImagePin* launcher=nullptr;for(auto& p:pins_)if(p->path==executable.parent_path().parent_path()/L"Qrazy.exe")launcher=p.get();
    if(!launcher||!launcher->Process(launcher_.value))return false;
    state=Waiting;worker_=std::thread([this,name]{
      pipe_.value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
      char kind=0;auto deadline=GetTickCount64()+15000;
      if(pipe_.value==INVALID_HANDLE_VALUE||!Peer(pipe_.value,peer_.value,false)||!IO(pipe_.value,&kind,1,false,deadline)||
        (kind!='G'&&kind!='S')||(kind=='S'&&!IO(pipe_.value,code_.data(),static_cast<DWORD>(code_.size()),false,deadline))) {state=Failed;return;}
      anonymous_=kind=='G';
      state=Ready;
      {std::unique_lock<std::mutex> lock(mutex_);condition_.wait_for(lock,std::chrono::seconds(15),[this]{return acknowledged_||stop_;});}
      char receipt='K',done=0;deadline=GetTickCount64()+5000;
      state=acknowledged_&&!stop_&&Peer(pipe_.value,peer_.value,false)&&IO(pipe_.value,&receipt,1,true,deadline)&&
        IO(pipe_.value,&done,1,false,deadline)&&done=='D'?Passed:Failed;
      if(state!=Passed)return;
      deadline=GetTickCount64()+300000;
      while(!stop_&&GetTickCount64()<deadline) {
        DWORD available=0;
        if(!PeekNamedPipe(pipe_.value,nullptr,0,nullptr,&available,nullptr)){state=Failed;return;}
        if(available) {
          char install=0;
          if(IO(pipe_.value,&install,1,false,GetTickCount64()+1000)&&install=='I'&&Peer(pipe_.value,peer_.value,false)){
            char consent_receipt='A';if(IO(pipe_.value,&consent_receipt,1,true,GetTickCount64()+1000)){authorized_=true;return;}
          }
          state=Failed;return;
        }
        if(WaitForSingleObject(peer_.value,0)==WAIT_OBJECT_0){state=Failed;return;}
        Sleep(20);
      }
      state=Failed;
    });return true;
  }
  bool Take(std::array<char,43>& code) {
    if(state!=Ready||taken_)return false;taken_=true;code=code_;SecureZeroMemory(code_.data(),code_.size());return true;
  }
  void Acknowledge(){std::lock_guard<std::mutex> lock(mutex_);acknowledged_=true;condition_.notify_all();}
  bool SourceExited() const {
    return state==Passed&&authorized_&&WaitForSingleObject(peer_.value,0)==WAIT_OBJECT_0&&WaitForSingleObject(launcher_.value,0)==WAIT_OBJECT_0;
  }
  bool Anonymous() const{return anonymous_;}
  void ReleaseFiles(){if(SourceExited())pins_.clear();}
  void Stop() {
    stop_=true;condition_.notify_all();if(pipe_.value!=INVALID_HANDLE_VALUE)CancelIoEx(pipe_.value,nullptr);
    if(worker_.joinable())worker_.join();SecureZeroMemory(code_.data(),code_.size());pins_.clear();
  }
 private:
  Handle pipe_,peer_,launcher_;std::vector<std::unique_ptr<ImagePin>> pins_;std::thread worker_;std::mutex mutex_;
  std::condition_variable condition_;std::atomic<bool> stop_{false},authorized_{false};bool acknowledged_=false,taken_=false,anonymous_=false;std::array<char,43> code_{};
};
}
