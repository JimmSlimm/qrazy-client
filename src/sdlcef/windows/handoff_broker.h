#pragma once
#include "handoff_image.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
namespace Handoff {
// The Electron main thread owns issue(). This worker handles native IPC only;
// it never calls Node/CEF APIs and never initializes a window or browser.
class Broker {
 public:
  enum State {Idle=0,Waiting=1,Ready=2,Passed=3,Failed=4};
  std::atomic<int> state{Idle};std::wstring name;
  ~Broker(){Stop();}
  bool Pin(const std::filesystem::path& path,const std::string& hash) {
    if(state!=Idle||pins_.size()>=4096)return false;
    auto pin=std::make_unique<ImagePin>();if(!pin->Open(path,hash))return false;
    pins_.push_back(std::move(pin));return true;
  }
  bool Start(const std::filesystem::path& executable) {
    if(state!=Idle||pins_.empty())return false;
    for(auto& p:pins_)if(p->path==std::filesystem::absolute(executable).lexically_normal())image_=p.get();
    if(!image_)return false;pipe_.value=Server(name);if(pipe_.value==INVALID_HANDLE_VALUE)return false;
    state=Waiting;worker_=std::thread([this]{Run();});return true;
  }
  bool Submit(const char* code,size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state!=Ready||submitted_||(size!=0&&size!=ticket_.code.size()))return false;
    if(!size){ticket_.anonymous=true;submitted_=true;condition_.notify_all();return true;}
    for(size_t i=0;i<size;++i)if(!((code[i]>='A'&&code[i]<='Z')||(code[i]>='a'&&code[i]<='z')||(code[i]>='0'&&code[i]<='9')||code[i]=='_'||code[i]=='-'))return false;
    std::copy(code,code+size,ticket_.code.begin());submitted_=true;condition_.notify_all();return true;
  }
  bool Authorize() {
    if(state!=Passed||!Peer(pipe_.value,peer_.value,true))return false;
    char install='I',receipt=0;const auto deadline=GetTickCount64()+5000;
    return IO(pipe_.value,&install,1,true,deadline)&&IO(pipe_.value,&receipt,1,false,deadline)&&receipt=='A'&&Peer(pipe_.value,peer_.value,true);
  }
  bool Track(DWORD pid,const std::filesystem::path& executable) {
    if(state!=Idle||tracked_.value!=INVALID_HANDLE_VALUE)return false;ImagePin* image=nullptr;
    for(auto& pin:pins_)if(pin->path==std::filesystem::absolute(executable).lexically_normal())image=pin.get();
    tracked_.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    return image&&image->Process(tracked_.value);
  }
  bool TrackedExited() {
    bool done=tracked_.value&&tracked_.value!=INVALID_HANDLE_VALUE&&WaitForSingleObject(tracked_.value,0)==WAIT_OBJECT_0;
    if(done)pins_.clear();return done;
  }
  void Stop() {
    stop_=true;condition_.notify_all();if(pipe_.value!=INVALID_HANDLE_VALUE)CancelIoEx(pipe_.value,nullptr);
    if(worker_.joinable())worker_.join();ticket_.Erase();pins_.clear();image_=nullptr;
  }
 private:
  Handle pipe_,peer_,tracked_;std::vector<std::unique_ptr<ImagePin>> pins_;ImagePin* image_=nullptr;
  std::thread worker_;std::atomic<bool> stop_{false};std::mutex mutex_;std::condition_variable condition_;
  Ticket ticket_;bool submitted_=false;
  void Run() {
    bool connected=false;const ULONGLONG deadline=GetTickCount64()+300000;
    while(!stop_&&GetTickCount64()<deadline) {
      if(Connect(pipe_.value,GetTickCount64()+250)){connected=true;break;}
    }
    ULONG pid=0;
    if(!connected||stop_||!GetNamedPipeClientProcessId(pipe_.value,&pid)) {state=Failed;return;}
    peer_.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if(!image_->Process(peer_.value)||!Peer(pipe_.value,peer_.value,true)) {state=Failed;return;}
    state=Ready;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait_for(lock,std::chrono::seconds(6),[this]{return submitted_||stop_;});
      if(!submitted_||stop_) {ticket_.Erase();state=Failed;return;}
    }
    const ULONGLONG exchange=GetTickCount64()+20000;
    if(!ticket_.Send(pipe_.value,peer_.value,exchange)) {state=Failed;return;}
    char acknowledgement=0;
    if(!IO(pipe_.value,&acknowledgement,1,false,exchange)||acknowledgement!='K'||!Peer(pipe_.value,peer_.value,true)) {state=Failed;return;}
    char complete='D';state=IO(pipe_.value,&complete,1,true,exchange)?Passed:Failed;
  }
};
}
