#pragma once
// Detects a persistent shortfall (CEF paints far fewer frames than were requested while the
// window is focused and idle of loading) and asks for a "kick": the same host-side re-apply an Alt+Tab causes
// (frame rate, view size, repaint, adaptive-rate history), without sending focus/blur to the page.
#include <cstdint>
struct PacingWatchdog {
  static constexpr int64_t kShortfallNs=3000000000ll,kCooldownNs=10000000000ll;
  int64_t since=0,last_kick=0;
  // painted/requested are per-second rates over the last sample window. Returns true when a kick is due.
  bool Update(int64_t now_ns,double painted,double requested,bool eligible) {
    const bool short_now=eligible&&requested>=200&&painted<requested*0.6;
    if(!short_now){since=0;return false;}
    if(!since){since=now_ns;return false;}
    if(now_ns-since<kShortfallNs)return false;
    if(last_kick&&now_ns-last_kick<kCooldownNs)return false;
    last_kick=now_ns;since=0;return true;
  }
  void Reset(){since=0;}
};
