#pragma once
// Independent begin-frame clock; only the SDL thread consumes ticks and calls CEF.
class DisplayBeginClock {
  std::atomic<bool> running_{false},pending_{false},failed_{false};
  std::atomic<bool> display_clock_{true};
  std::atomic<int> requested_fps_{250};
  // With variable refresh rate the display only refreshes when we present, so a vblank-driven
  // clock settles at our own pipeline latency. When vblanks stay late, tick from a timer at the
  // real refresh rate instead (Present still limits the rate). Reset when the monitor changes.
  std::atomic<int> refresh_hz_{60};
  std::atomic<bool> timer_mode_{false};
  HANDLE timer_=nullptr;
  std::thread thread_;
  std::mutex output_mutex_;
  Microsoft::WRL::ComPtr<IDXGIOutput> output_;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
  HMONITOR monitor_=nullptr;
  unsigned output_generation_=0;
 public:
  ~DisplayBeginClock(){Stop();}
  // Called only by the SDL thread. Replace the output without joining the
  // clock thread or waiting for a vblank on the monitor we are leaving.
  bool UpdateDisplay(HWND target_window) {
    const HMONITOR monitor=MonitorFromWindow(target_window,MONITOR_DEFAULTTONEAREST);
    if(monitor==monitor_&&factory_&&factory_->IsCurrent())return true;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
    Microsoft::WRL::ComPtr<IDXGIOutput> selected;
    for(UINT a=0;!selected;++a){
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
      if(!adapter)continue;
      for(UINT o=0;;++o){
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
        if(!output)continue;
        DXGI_OUTPUT_DESC desc{};
        if(SUCCEEDED(output->GetDesc(&desc))&&desc.Monitor==monitor){selected=output;break;}
      }
    }
    if(!selected)return false;
    {std::lock_guard<std::mutex> lock(output_mutex_);
      output_=selected;++output_generation_;pending_=false;timer_mode_=false;
    }
    factory_=factory;monitor_=monitor;
    return true;
  }
  bool Start(HWND target_window) {
    if(!UpdateDisplay(target_window))return false;
    timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer_)return false;
    running_=true;
    thread_=std::thread([this]{
      auto last_vblank=std::chrono::steady_clock::now(),next=last_vblank;int slow_vblanks=0,timer_rate=0;bool timer_running=false;
      while(running_){
        Microsoft::WRL::ComPtr<IDXGIOutput> selected;unsigned tick_generation;
        {std::lock_guard<std::mutex> lock(output_mutex_);selected=output_;tick_generation=output_generation_;}
        bool ok=true;
        if(display_clock_&&!timer_mode_){
          ok=SUCCEEDED(selected->WaitForVBlank());
          const auto now=std::chrono::steady_clock::now();
          const auto period=std::chrono::nanoseconds(1000000000ll/std::max(1,refresh_hz_.load()));
          if(ok&&now-last_vblank>period*13/10){if(++slow_vblanks>=30){timer_mode_=true;slow_vblanks=0;}}else slow_vblanks=0;
          last_vblank=now;timer_running=false;
        }else{
          // Absolute deadlines: re-arming a relative wait after each wake-up adds the wake latency
          // to every period (a 240 Hz timer then only delivers ~220 Hz).
          using clock=std::chrono::steady_clock;
          const int rate=display_clock_?refresh_hz_.load():requested_fps_.load();
          const auto period=std::chrono::nanoseconds(1000000000ll/std::max(1,rate));
          const auto now=clock::now();
          if(!timer_running||rate!=timer_rate||next+period*4<now){next=now;timer_running=true;timer_rate=rate;} // (re)start, or fell far behind
          next+=period;
          LARGE_INTEGER due{};
          due.QuadPart=-std::max<LONGLONG>(1,std::chrono::duration_cast<std::chrono::nanoseconds>(next-now).count()/100);
          ok=SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)&&WaitForSingleObject(timer_,1000)==WAIT_OBJECT_0;
        }
        std::lock_guard<std::mutex> lock(output_mutex_);
        // Discard an old monitor's in-flight tick (or disconnect error).
        if(tick_generation!=output_generation_)continue;
        if(!ok){failed_=true;break;}
        if(running_)pending_=true; // Coalesce ticks; never enqueue a backlog.
      }
    });
    return true;
  }
  bool Take(){return pending_.exchange(false);}
  void Configure(bool vsync,int max_fps,int refresh_hz){requested_fps_=max_fps;display_clock_=vsync;refresh_hz_=refresh_hz;}
  bool Failed() const{return failed_;}
  void Stop(){running_=false;if(thread_.joinable())thread_.join();if(timer_){CloseHandle(timer_);timer_=nullptr;}}
};
