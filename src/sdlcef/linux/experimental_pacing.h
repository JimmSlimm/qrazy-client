#pragma once
// Opt-in experimental frame pacing for Linux. Off unless the player enables it from the game console; the saved flag
// applies on the NEXT start. A run that does not exit cleanly clears the flag again, so a broken start cannot repeat.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>

// Highest experiment level this build knows. Level 1 = timer clock; 2 = 1 + present limiter; 3 = 2 + faster input polling; 4 = 3 + begin-frame rate follows what CEF delivers; 5 = 4 using the best delivery rate of the last second; 6 = 5 plus, with VSync on, ask 20% above the refresh rate; 7 = 6 without the blocking buffer swap (presents from our own timer).
constexpr int kMaxLevel=7;
struct ExperimentalPacing {
  bool enabled=false; // saved preference for the next start
  bool active=false;  // this process runs the experimental clock
  bool vsync=true;
  int level=0;        // saved experiment level for the next start (0 off, see kMaxLevel)
  int active_level=0; // level this process runs
  int max_fps=250;
  std::filesystem::path file,running;
  void Load(const std::filesystem::path& profile) {
    file=profile/"experimental-pacing.json";running=profile/"experimental-pacing.running";
    std::ifstream input(file,std::ios::binary);std::string text(4097,'\0');input.read(text.data(),4097);text.resize(static_cast<size_t>(input.gcount()));
    if(input.is_open()&&!input.bad()&&text.size()<=4096) {
      auto value=CefParseJSON(text,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
      if(d) {
        if(d->GetType("enabled")==VTYPE_BOOL)enabled=d->GetBool("enabled");
        level=enabled?1:0;
        if(d->GetType("level")==VTYPE_INT){int n=d->GetInt("level");if(n>=0&&n<=kMaxLevel){level=n;enabled=n>=1;}}
        if(d->GetType("vsync")==VTYPE_BOOL)vsync=d->GetBool("vsync");
        if(d->GetType("com_maxfps")==VTYPE_INT){int n=d->GetInt("com_maxfps");if(n>=30&&n<=10000)max_fps=n;}
      }
    }
    std::error_code error;
    if(std::filesystem::exists(running,error)) {
      if(enabled){enabled=false;level=0;vsync=true;Save();std::fprintf(stderr,"QRAZY experimental pacing turned off: the previous run did not exit cleanly\n");}
      std::filesystem::remove(running,error);
    }
  }
  bool Save() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",enabled);d->SetInt("level",level);d->SetBool("vsync",vsync);d->SetInt("com_maxfps",max_fps);
    auto v=CefValue::Create();v->SetDictionary(d);const auto text=CefWriteJSON(v,JSON_WRITER_DEFAULT).ToString();
    auto temporary=file;temporary+=".tmp";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<text;out.flush();if(!out)return false;}
    std::error_code error;std::filesystem::rename(temporary,file,error);return !error;
  }
  void MarkRunning() const {std::ofstream out(running,std::ios::binary|std::ios::trunc);out<<"1";}
  void MarkClean() const {std::error_code error;std::filesystem::remove(running,error);}
  // 0 = swap returns immediately (the Wayland compositor still syncs to the display); 1 = swap blocks until the display refresh.
  int SwapInterval() const {return active&&(!vsync||active_level==7)?0:1;}
  int Target(int refresh) const {
    if(vsync&&active_level>=6)return std::min(max_fps,(refresh*6+4)/5); // headroom: Chromium drops a frame when two arrive closer than its frame period
    return vsync?std::min(refresh,max_fps):max_fps;
  }
  CefRefPtr<CefDictionaryValue> State() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",enabled);d->SetBool("active",active);d->SetInt("level",level);d->SetInt("maxLevel",kMaxLevel);d->SetInt("activeLevel",active?active_level:0);d->SetBool("restartRequired",enabled!=active);return d;
  }
  CefRefPtr<CefDictionaryValue> VSyncState() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",vsync);d->SetBool("activeEnabled",vsync);d->SetBool("restartRequired",false);d->SetBool("canDisable",true);return d;
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
  bool running_=false,pending_=false;
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
          if(changed_.wait_until(lock,next,[&]{return !running_||generation!=generation_;})) {
            if(!running_)break;
            generation=generation_;
            next=Clock::now()+std::chrono::nanoseconds(period_ns_);
            continue;
          }
          pending_=true;tick_.notify_one();
          next=NextDeadline(next,Clock::now(),period_ns_);
        }
    });
    return thread_.joinable();
  }
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
