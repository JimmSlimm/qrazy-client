#pragma once
#include <cstdint>
namespace QrazyWindows {
inline constexpr uint64_t ServerRetryWindowMs=300000;
inline constexpr uint64_t ServerRetryIntervalMs=5000;
inline constexpr uint64_t ServerRetryBackoffAfterMs=60000;
inline constexpr uint64_t ServerRetryBackoffMs=10000;
inline constexpr uint64_t ServerConnectTimeoutMs=15000;
struct ServerRetry {
  bool active=false,pending=false,expired=false;
  uint64_t started=0,deadline=0,next=0;unsigned attempt=0;
  void Stop(){active=pending=expired=false;attempt=0;}
  void Start(uint64_t now){active=true;pending=expired=false;started=now;deadline=now+ServerRetryWindowMs;attempt=0;}
  void Failure(uint64_t now){
    if(!active){Start(now);attempt=1;}
    pending=false;next=now+(now-started<ServerRetryBackoffAfterMs?ServerRetryIntervalMs:ServerRetryBackoffMs);
  }
  bool Expire(uint64_t now){if(!active||now<deadline)return false;active=pending=false;expired=true;return true;}
  bool Due(uint64_t now) const{return active&&!pending&&now<deadline&&now>=next;}
  bool Attempt(uint64_t now){if(!active)Start(now);if(now>=deadline)return false;pending=true;++attempt;return true;}
  unsigned Countdown(uint64_t now) const{return next>now?static_cast<unsigned>((next-now+999)/1000):0;}
};
}
