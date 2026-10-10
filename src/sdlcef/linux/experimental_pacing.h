#pragma once
// Opt-in experimental frame pacing for Linux. Off unless the player enables it from the game console; the saved flag
// applies on the NEXT start. A run that does not exit cleanly clears the flag again, so a broken start cannot repeat.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <time.h>

struct ExperimentalPacing {
  bool enabled=false; // saved preference for the next start
  bool active=false;  // this process runs the experimental clock
  bool vsync=true;
  int max_fps=250;
  std::filesystem::path file,running;
  void Load(const std::filesystem::path& profile) {
    file=profile/"experimental-pacing.json";running=profile/"experimental-pacing.running";
    std::ifstream input(file,std::ios::binary);std::string text(4097,'\0');input.read(text.data(),4097);text.resize(static_cast<size_t>(input.gcount()));
    if(input.is_open()&&!input.bad()&&text.size()<=4096) {
      auto value=CefParseJSON(text,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
      if(d) {
        if(d->GetType("enabled")==VTYPE_BOOL)enabled=d->GetBool("enabled");
        if(d->GetType("vsync")==VTYPE_BOOL)vsync=d->GetBool("vsync");
        if(d->GetType("com_maxfps")==VTYPE_INT){int n=d->GetInt("com_maxfps");if(n>=30&&n<=10000)max_fps=n;}
      }
    }
    std::error_code error;
    if(std::filesystem::exists(running,error)) {
      if(enabled){enabled=false;vsync=true;Save();std::fprintf(stderr,"QRAZY experimental pacing turned off: the previous run did not exit cleanly\n");}
      std::filesystem::remove(running,error);
    }
  }
  bool Save() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",enabled);d->SetBool("vsync",vsync);d->SetInt("com_maxfps",max_fps);
    auto v=CefValue::Create();v->SetDictionary(d);const auto text=CefWriteJSON(v,JSON_WRITER_DEFAULT).ToString();
    auto temporary=file;temporary+=".tmp";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<text;out.flush();if(!out)return false;}
    std::error_code error;std::filesystem::rename(temporary,file,error);return !error;
  }
  void MarkRunning() const {std::ofstream out(running,std::ios::binary|std::ios::trunc);out<<"1";}
  void MarkClean() const {std::error_code error;std::filesystem::remove(running,error);}
  int Target(int refresh) const {return vsync?std::min(refresh,max_fps):max_fps;}
  CefRefPtr<CefDictionaryValue> State() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",enabled);d->SetBool("active",active);d->SetBool("restartRequired",enabled!=active);return d;
  }
  CefRefPtr<CefDictionaryValue> VSyncState() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",vsync);d->SetBool("activeEnabled",vsync);d->SetBool("restartRequired",false);d->SetBool("canDisable",true);return d;
  }
  CefRefPtr<CefDictionaryValue> MaxFpsState() const {
    auto d=CefDictionaryValue::Create();d->SetInt("value",max_fps);d->SetInt("minimum",30);d->SetInt("maximum",10000);d->SetInt("defaultValue",250);return d;
  }
};

// Independent begin-frame clock: a worker sleeps to absolute CLOCK_MONOTONIC deadlines (no drift) and sets one coalesced
// "tick" flag. Only the SDL thread takes ticks and calls CEF; nothing else moves off that thread.
class TimerBeginClock {
  std::atomic<bool> running_{false},pending_{false};
  std::atomic<long long> period_ns_{4000000};
  std::thread thread_;
 public:
  ~TimerBeginClock(){Stop();}
  bool Start() {
    running_=true;
    thread_=std::thread([this]{
      timespec next{};clock_gettime(CLOCK_MONOTONIC,&next);
      while(running_) {
        long long period=period_ns_.load();
        next.tv_nsec+=period;
        while(next.tv_nsec>=1000000000){next.tv_nsec-=1000000000;++next.tv_sec;}
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,nullptr);
        timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
        const long long late=(now.tv_sec-next.tv_sec)*1000000000ll+(now.tv_nsec-next.tv_nsec);
        if(late>50000000ll)next=now; // far behind (suspend, stall): resynchronise instead of bursting
        if(running_)pending_=true;    // coalesce; never queue a backlog
      }
    });
    return thread_.joinable();
  }
  bool Take(){return pending_.exchange(false);}
  void Configure(int fps){period_ns_=1000000000ll/std::clamp(fps,30,10000);}
  void Stop(){running_=false;if(thread_.joinable())thread_.join();}
};
