#pragma once
// Independent begin-frame clock; only the SDL thread consumes ticks and calls CEF.
class DisplayBeginClock {
  std::atomic<bool> running_{false},pending_{false},failed_{false};
  std::atomic<bool> display_clock_{true};
  std::atomic<int> requested_fps_{250};
  HANDLE timer_=nullptr;
  std::thread thread_;
 public:
  ~DisplayBeginClock(){Stop();}
  bool Start(HWND target_window) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
    const HMONITOR monitor=MonitorFromWindow(target_window,MONITOR_DEFAULTTONEAREST);
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
    timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer_)return false;
    running_=true;
    thread_=std::thread([this,selected]{
      while(running_){
        if(display_clock_){
          if(FAILED(selected->WaitForVBlank())){failed_=true;break;}
        }else{
          LARGE_INTEGER due{};
          due.QuadPart=-std::max<LONGLONG>(1,10000000ll/requested_fps_.load());
          if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)||WaitForSingleObject(timer_,1000)!=WAIT_OBJECT_0){failed_=true;break;}
        }
        if(running_)pending_=true; // Coalesce ticks; never enqueue a backlog.
      }
    });
    return true;
  }
  bool Take(){return pending_.exchange(false);}
  void Configure(bool vsync,int max_fps){requested_fps_=max_fps;display_clock_=vsync;}
  bool Failed() const{return failed_;}
  void Stop(){running_=false;if(thread_.joinable())thread_.join();if(timer_){CloseHandle(timer_);timer_=nullptr;}}
};
