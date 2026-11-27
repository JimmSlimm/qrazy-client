#pragma once
#include "nc_coreload.h"
#include "nc_net.h"
#include <deque>
#include <functional>
#include <future>
#include <string_view>

namespace nc {

struct HubRequest {
  enum Op { Open, Text, Fact, Close } op;
  int sid, kind = 0, a = 0, b = 0;
  double value = 0;
  std::string text;
};

struct HubEvent {
  int sid;
  const char* type;
  std::string_view data;
  int code = 0;
  bool clean = false;
};

// The render thread owns this while a match runs. Between matches the host
// services it after joining the render thread and before starting another.
class Hub {
 public:
  void post(HubRequest request);
  void setSink(void (*sink)(const HubEvent&)) { sink_ = sink; }
  void service(const std::function<void(int, const std::string&)>& follow = {});
  void openStandalone(const std::string& url, std::string hello);
  void shutdown();
  void sendText(std::string text) { post({HubRequest::Text, sid_, 0, 0, 0, 0, std::move(text)}); }
  void mapLoading();
  void mapReady(std::string_view map);
  bool connected() const { return live_ && socket_.connected(); }
  unsigned generation() const { return generation_; }
  void state(const QcWireState& state);
  void fact(int kind, int a = 0, int b = 0, double value = 0, std::string_view text = {});
  void effect(int kind, int flags, int id, int mode, const double* pos, double yaw = 0, double pitch = 0);
 private:
  struct Effect { int kind, flags, id, mode; double x, y, z, yaw, pitch, now; };
  bool drain();
  void dropMapState();
  bool sendPending();
  void submitLoaded();
  void reset();
  void closed(const NetClose& info);
  double now() const;
  Net socket_;
  std::future<bool> opening_;
  void (*sink_)(const HubEvent&) = nullptr;
  std::mutex requestsMutex_;
  std::deque<HubRequest> requests_, work_, retryFacts_;
  std::deque<Effect> retryEffects_;
  std::vector<NetRaw> incoming_;
  size_t inputOffset_ = 0, inputCount_ = 0, outputOffset_ = 0, outputSize_ = 0;
  std::array<uint8_t, QC_NET_OUTPUT_CAP> output_{};
  unsigned generation_ = 0;
  int sid_ = 0;
  bool live_ = false, helloSent_ = false, loadedSent_ = false, spectating_ = false;
  bool orderedFacts_ = false;
  std::string readyMap_;
  HubRequest loaded_{};
};

Hub& appHub();

}  // namespace nc
