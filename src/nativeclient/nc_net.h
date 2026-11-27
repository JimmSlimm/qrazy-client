#pragma once
#include "nc_platform.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nc {

struct NetRaw { bool binary; std::string data; };
struct NetClose { int code = 1006; std::string reason; bool clean = false; };

class Net {
 public:
  ~Net();
  bool connect(const std::string& url);
  void close();
  bool connected() const { return up_; }
  const std::string& error() const { return error_; }
  bool send(bool binary, const uint8_t* data, size_t size, bool mapState = false);
  bool sendable();
  void discardUnsentMapState();
  size_t poll(std::vector<NetRaw>& out);
  NetClose closeInfo();
 private:
  void run();
#if defined(NC_WINDOWS)
  void write();
  std::condition_variable writable_;
  std::thread writer_;
#endif
  void receive(bool binary, std::string& message);
  void* session_ = nullptr;
  void* conn_ = nullptr;
  void* ws_ = nullptr;
  std::thread th_;
  std::atomic<bool> up_{false}, stop_{false};
  std::string error_;
  std::mutex mu_, sendMu_;
  std::vector<NetRaw> raw_;
  size_t rawCount_ = 0;
  NetClose close_;
  std::array<uint8_t, 81940> pending_{};
  std::vector<uint8_t> largePending_;
  size_t pendingSize_ = 0;
  bool pendingBinary_ = false, pendingReady_ = false;
  bool pendingStarted_ = false, pendingMapState_ = false;
};

}  // namespace nc
