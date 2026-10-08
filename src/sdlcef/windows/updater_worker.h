#pragma once
#include "updater_control.h"
namespace Production {
struct Reply {int id;unsigned epoch;std::string operation,text;bool ok;};
class Worker {
  std::thread thread_;std::atomic<bool> cancelled_{false},busy_{false};std::mutex mutex_;std::deque<Reply> replies_;
 public:
  std::filesystem::path root;
  ~Worker(){Stop();}
  bool Submit(int id,unsigned epoch,const std::string& op){if(busy_.exchange(true))return false;if(thread_.joinable())thread_.join();cancelled_=false;
    thread_=std::thread([this,id,epoch,op]{std::string text;bool ok=Helper(root,std::wstring(op.begin(),op.end()),text,&cancelled_);{std::lock_guard<std::mutex> guard(mutex_);replies_.push_back({id,epoch,op,text,ok});}busy_=false;});return true;}
  std::deque<Reply> Take(){std::lock_guard<std::mutex> guard(mutex_);std::deque<Reply> result;result.swap(replies_);return result;}
  void Stop(){cancelled_=true;if(thread_.joinable())thread_.join();}
};
}
