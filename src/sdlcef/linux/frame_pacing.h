#pragma once
// Frame pacing for Linux VSync-off mode (r_swapinterval 0); the saved setting
// applies on the NEXT start. A run that does not exit cleanly turns VSync back on, so a broken start cannot repeat.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>

// VSync on (default): the original pacing. VSync off: timer-driven frame requests with a sleep-then-spin timer, an adaptive
// request rate, a present limiter and a 32 MB V8 young generation (startup flag). The saved VSync setting applies on the next start.
struct FramePacing {
  bool active=false;  // this process runs the VSync-off pacing
  bool vsync=true;    // saved preference for the next start
  int max_fps=250;
  std::filesystem::path file,running;
  void Load(const std::filesystem::path& profile) {
    file=profile/"experimental-pacing.json";running=profile/"experimental-pacing.running";
    std::ifstream input(file,std::ios::binary);std::string text(4097,'\0');input.read(text.data(),4097);text.resize(static_cast<size_t>(input.gcount()));
    if(input.is_open()&&!input.bad()&&text.size()<=4096) {
      auto value=CefParseJSON(text,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
      if(d) {
        if(d->GetType("vsync")==VTYPE_BOOL)vsync=d->GetBool("vsync");
        if(d->GetType("com_maxfps")==VTYPE_INT){int n=d->GetInt("com_maxfps");if(n>=30&&n<=10000)max_fps=n;}
      }
    }
    std::error_code error;
    if(std::filesystem::exists(running,error)) {
      if(!vsync){vsync=true;Save();std::fprintf(stderr,"QRAZY VSync turned back on: the previous run did not exit cleanly\n");}
      std::filesystem::remove(running,error);
    }
  }
  bool Save() const {
    auto d=CefDictionaryValue::Create();d->SetBool("vsync",vsync);d->SetInt("com_maxfps",max_fps);
    auto v=CefValue::Create();v->SetDictionary(d);const auto text=CefWriteJSON(v,JSON_WRITER_DEFAULT).ToString();
    auto temporary=file;temporary+=".tmp";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<text;out.flush();if(!out)return false;}
    std::error_code error;std::filesystem::rename(temporary,file,error);return !error;
  }
  void MarkRunning() const {std::ofstream out(running,std::ios::binary|std::ios::trunc);out<<"1";}
  void MarkClean() const {std::error_code error;std::filesystem::remove(running,error);}
  // 0 = swap returns immediately (the Wayland compositor still syncs to the display); 1 = swap blocks until the display refresh.
  int SwapInterval() const {return active?0:1;}
  int Target(int refresh) const {return active?max_fps:refresh;}
  CefRefPtr<CefDictionaryValue> VSyncState() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",vsync);d->SetBool("activeEnabled",!active);d->SetBool("restartRequired",vsync==active);d->SetBool("canDisable",true);return d;
  }
  CefRefPtr<CefDictionaryValue> MaxFpsState() const {
    auto d=CefDictionaryValue::Create();d->SetInt("value",max_fps);d->SetInt("minimum",30);d->SetInt("maximum",10000);d->SetInt("defaultValue",250);return d;
  }
};

// One rate controller. Absolute monotonic deadlines preserve cadence despite dispatch jitter;
// missed deadlines are skipped, never replayed. The SDL thread alone calls CEF and owns graphics.
class TimerBeginClock {
  using Clock=std::chrono::steady_clock;
  std::mutex mutex_;
  std::condition_variable changed_,tick_;
  bool running_=false,pending_=false,spin_=false;
  long long period_ns_=4000000;
  unsigned generation_=0;
  std::thread thread_;
 public:
  ~TimerBeginClock(){Stop();}
  static Clock::time_point NextDeadline(Clock::time_point due,Clock::time_point now,long long period_ns) {
    const auto period=std::chrono::nanoseconds(period_ns);
    return due+period*((now-due)/period+1);
  }
  bool Start() {
    std::lock_guard<std::mutex> guard(mutex_);
    if(running_)return true;
    running_=true;
    thread_=std::thread([this]{
        std::unique_lock<std::mutex> lock(mutex_);
        unsigned generation=generation_;
        auto next=Clock::now()+std::chrono::nanoseconds(period_ns_);
        while(running_) {
          // Level 3: sleep until 0.3 ms before the deadline, then spin to it, so a late wake-up cannot delay the tick.
          const auto lead=std::chrono::microseconds(spin_?300:0);
          if(changed_.wait_until(lock,next-lead,[&]{return !running_||generation!=generation_;})) {
            if(!running_)break;
            generation=generation_;
            next=Clock::now()+std::chrono::nanoseconds(period_ns_);
            continue;
          }
          if(lead.count()) {
            lock.unlock();
            while(Clock::now()<next){}
            lock.lock();
            if(!running_)break;
            if(generation!=generation_)continue;
          }
          pending_=true;tick_.notify_one();
          next=NextDeadline(next,Clock::now(),period_ns_);
        }
    });
    return thread_.joinable();
  }
  void SetSpin(bool on){std::lock_guard<std::mutex> lock(mutex_);spin_=on;}
  bool Take(){std::lock_guard<std::mutex> lock(mutex_);bool tick=pending_;pending_=false;return tick;}
  void Configure(int fps){
    std::lock_guard<std::mutex> lock(mutex_);
    const long long period=1000000000ll/std::clamp(fps,30,10000);
    if(period==period_ns_)return;
    period_ns_=period;++generation_;pending_=false;changed_.notify_one();
  }
  void Wait(std::chrono::microseconds poll=std::chrono::microseconds(1000)){
    std::unique_lock<std::mutex> lock(mutex_);
    // Keep the existing <=1 ms polling interval for CEF/SDL events, but a frame
    // tick wakes us immediately instead of waiting for a whole extra millisecond.
    tick_.wait_for(lock,poll,[&]{return pending_||!running_;});
  }
  void Stop(){
    {std::lock_guard<std::mutex> lock(mutex_);running_=false;pending_=false;changed_.notify_one();tick_.notify_one();}
    if(thread_.joinable())thread_.join();
  }
};
