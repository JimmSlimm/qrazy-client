#pragma once
#include <algorithm>
#include <cmath>
namespace DesktopPolicy {
inline bool ShutdownReady(bool requested,bool completed){return requested&&completed;}
inline bool AllowCloseAction(bool closing,bool recovery,bool loading,bool captured,bool focused,bool hidden){
  return !closing&&!recovery&&!loading&&!captured&&focused&&!hidden;
}
struct RecoveryState {
  bool recovering=false,loading=true;
  void BeginLoad(){recovering=false;loading=true;}
  void FailLoad(){recovering=true;loading=false;}
  bool FinishLoad(bool trusted,int code){
    if(!trusted||!loading||recovering||code>=400)return false;
    loading=false;return true;
  }
};
struct Bounds {int x,y,width,height;};
inline Bounds RestoreBounds(Bounds saved,Bounds display) {
  saved.width=std::min(std::clamp(saved.width,640,8192),std::max(1,display.width));
  saved.height=std::min(std::clamp(saved.height,480,8192),std::max(1,display.height));
  saved.x=std::clamp(saved.x,display.x,display.x+std::max(0,display.width-saved.width));
  saved.y=std::clamp(saved.y,display.y,display.y+std::max(0,display.height-saved.height));
  return saved;
}
inline float PixelScale(int logical,int pixels) {
  return logical>0&&pixels>0?static_cast<float>(pixels)/logical:1.f;
}
inline bool AllowInput(bool closing,bool recovery,bool loading) {return !closing&&!recovery&&!loading;}
}
