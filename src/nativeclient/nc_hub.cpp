#include "nc_hub.h"
#include "nc_platform.h"
#include "nc_vk.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace nc {

Hub& appHub() {
  static Hub hub;
  return hub;
}

double Hub::now() const {
  return double(qpcNow()) / double(qpcFrequency());
}

void Hub::post(HubRequest request) {
  std::lock_guard<std::mutex> lock(requestsMutex_);
  requests_.push_back(std::move(request));
}

void Hub::reset() {
  ++generation_;
  helloSent_ = loadedSent_ = spectating_ = orderedFacts_ = false;
  inputCount_ = inputOffset_ = outputOffset_ = outputSize_ = 0;
  retryFacts_.clear();
  retryEffects_.clear();
  loaded_ = {};
  coreNet().fact(QC_NET_RESET, 0, 0, 0, nullptr, 0);
  qc_peer_reset();
}

void Hub::closed(const NetClose& info) {
  socket_.close();
  live_ = false;
  reset();
  if (sink_ && sid_) {
    if (!info.clean) sink_({sid_, "error", info.reason});
    sink_({sid_, "close", info.reason, info.code, info.clean});
  }
  sid_ = 0;
}

void Hub::shutdown() {
  if (opening_.valid()) live_ = opening_.get();
  if (live_) closed({1000, {}, true});
}

void Hub::openStandalone(const std::string& url, std::string hello) {
  post({HubRequest::Open, 0, 0, 0, 0, 0, url});
  post({HubRequest::Text, 0, 0, 0, 0, 0, std::move(hello)});
}

static uint32_t frameHeader(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

static bool mapStateFrame(bool binary, const uint8_t* data, size_t size) {
  if (binary) return true;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  return text.rfind("{\"type\":\"keys\"", 0) == 0 || text.rfind("{\"type\":\"run\"", 0) == 0 ||
         text.rfind("{\"type\":\"snd\"", 0) == 0 || text.rfind("{\"type\":\"inv\"", 0) == 0 ||
         text.rfind("{\"type\":\"world\"", 0) == 0;
}

bool Hub::sendPending() {
  while (outputOffset_ < outputSize_) {
    const uint8_t* p = output_.data() + outputOffset_;
    const uint32_t header = frameHeader(p);
    const size_t size = header & 0x7fffffffU;
    const bool binary = (header >> 31) != 0;
    if (!socket_.send(binary, p + 4, size, mapStateFrame(binary, p + 4, size))) return false;
    outputOffset_ += 4 + size;
  }
  return true;
}

bool Hub::drain() {
  while (helloSent_ && socket_.sendable() && sendPending() && socket_.sendable()) {
    const int size = coreNet().out(now(), output_.data(), int(output_.size()));
    if (size <= 0) {
      if (size < 0) logf("session network output returned %d", size);
      if (!size && retryFacts_.empty()) orderedFacts_ = false;
      return size == 0;
    }
    outputOffset_ = 0;
    outputSize_ = size;
  }
  return false;
}

void Hub::mapLoading() {
  readyMap_.clear();
  loadedSent_ = false;
  if (!coreNet()) return;
  qc_peer_clear_samples();
  dropMapState();
  retryFacts_.erase(std::remove_if(retryFacts_.begin(), retryFacts_.end(), [](const HubRequest& r) {
    return r.kind == QC_NET_LOADED || r.kind == QC_NET_SPEED;
  }), retryFacts_.end());
  retryEffects_.clear();
}

void Hub::dropMapState() {
  coreNet().fact(QC_NET_WATCH_VIEW, 0, 0, 0, nullptr, 0);
  coreNet().fact(QC_NET_DROP_POSE, 0, 0, 0, nullptr, 0);
  socket_.discardUnsentMapState();
  size_t write = 0;
  for (size_t at = outputOffset_; at < outputSize_;) {
    const uint8_t* p = output_.data() + at;
    const uint32_t header = frameHeader(p);
    const size_t size = header & 0x7fffffffU;
    if (!mapStateFrame((header >> 31) != 0, p + 4, size)) {
      std::memmove(output_.data() + write, p, size + 4);
      write += size + 4;
    }
    at += size + 4;
  }
  outputOffset_ = 0;
  outputSize_ = write;
}

void Hub::mapReady(std::string_view map) {
  readyMap_ = map;
  // The hub can deliver the roster and loaded notices while the local map is
  // opening. Clearing here would mark those peers as loading again.
  submitLoaded();
}

void Hub::submitLoaded() {
  if (readyMap_.empty() || loaded_.a != int(readyMap_.size()) ||
      loaded_.text.compare(0, loaded_.a, readyMap_) != 0) return;
  fact(QC_NET_LOADED, loaded_.a, 0, 0, loaded_.text);
  loaded_ = {};
}

void Hub::state(const QcWireState& state) {
  if (connected() && loadedSent_ && !spectating_) coreNet().state(&state);
}

void Hub::fact(int kind, int a, int b, double value, std::string_view text) {
  if (!connected() || text.size() > 1024) return;
  if (kind == QC_NET_WATCH_VIEW) {
    coreNet().fact(kind, loadedSent_ && !spectating_ && a, b, 0, nullptr, 0);
    return;
  }
  if (kind == QC_NET_SPEC && (a == 0 || a == 1)) {
    if (a && !spectating_) dropMapState();
    spectating_ = a != 0;
  }
  if (retryFacts_.empty()) {
    const int result = coreNet().fact(kind, a, b, value, text.data(), int(text.size()));
    if (result != -2) {
      if (kind == QC_NET_LOADED && result == 1) loadedSent_ = true;
      if (result == 1) orderedFacts_ = true;
      return;
    }
  }
  orderedFacts_ = true;
  retryFacts_.push_back({HubRequest::Fact, sid_, kind, a, b, value, std::string(text)});
}

void Hub::effect(int kind, int flags, int id, int mode, const double* pos, double yaw, double pitch) {
  if (!connected() || !loadedSent_ || spectating_) return;
  const Effect e{kind, flags, id, mode, pos ? pos[0] : 0, pos ? pos[1] : 0,
                 pos ? pos[2] : 0, yaw, pitch, kind == QC_NET_FIRE ? now() : 0};
  if (retryEffects_.empty() &&
      coreNet().effect(kind, flags, id, mode, e.x, e.y, e.z, yaw, pitch, e.now) != -2) return;
  retryEffects_.push_back(e);
}

void Hub::service(const std::function<void(int, const std::string&)>& follow) {
  if (!coreNet()) return;
  if (opening_.valid()) {
    if (opening_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    live_ = opening_.get();
    if (live_) {
      ++generation_;
      if (sink_ && sid_) sink_({sid_, "open", {}});
    } else closed({1006, socket_.error(), false});
  }
  {
    std::lock_guard<std::mutex> lock(requestsMutex_);
    if (work_.empty()) work_.swap(requests_);
    else if (!requests_.empty()) {
      work_.insert(work_.end(), std::make_move_iterator(requests_.begin()),
                   std::make_move_iterator(requests_.end()));
      requests_.clear();
    }
  }
  while (!work_.empty()) {
    auto& r = work_.front();
    if (r.op == HubRequest::Open) {
      if (live_) {
        if (sid_ == r.sid) sid_ = 0;
        closed({1000, {}, true});
      } else reset();
      sid_ = r.sid;
      opening_ = std::async(std::launch::async, [this, url = std::move(r.text)] {
        return socket_.connect(url);
      });
      work_.pop_front();
      return;
    } else if (r.sid == sid_ && live_) {
      if (r.op == HubRequest::Close) closed({1000, {}, true});
      if (r.op == HubRequest::Text && socket_.connected() &&
          ((helloSent_ && orderedFacts_ && (!drain() || !retryFacts_.empty())) ||
           !socket_.send(false, reinterpret_cast<const uint8_t*>(r.text.data()), r.text.size()))) {
        // A replacement or explicit close supersedes a blocked send.
        const auto replacement = std::find_if(work_.begin() + 1, work_.end(), [&](const HubRequest& next) {
          return next.op == HubRequest::Open || (next.op == HubRequest::Close && next.sid == sid_);
        });
        if (replacement == work_.end()) break;
        work_.erase(work_.begin(), replacement);
        continue;
      }
      if (r.op == HubRequest::Text && connected()) helloSent_ = true;
      if (r.op == HubRequest::Fact) {
        if (r.kind == QC_NET_LOADED) {
          if (r.a < 1 || r.a > 64 || size_t(r.a) > r.text.size()) { work_.pop_front(); continue; }
          if (!readyMap_.empty() && std::string_view(r.text.data(), r.a) != readyMap_) mapLoading();
          loaded_ = std::move(r);
          submitLoaded();
        }
        else fact(r.kind, r.a, r.b, r.value, r.text);
      }
    }
    work_.pop_front();
  }
  drain();
  while (!retryFacts_.empty() && socket_.connected()) {
    auto& r = retryFacts_.front();
    const int result = coreNet().fact(r.kind, r.a, r.b, r.value, r.text.data(), int(r.text.size()));
    if (result == -2) break;
    if (r.kind == QC_NET_LOADED && result == 1) loadedSent_ = true;
    retryFacts_.pop_front();
  }
  while (!retryEffects_.empty() && socket_.connected()) {
    const auto& e = retryEffects_.front();
    if (coreNet().effect(e.kind, e.flags, e.id, e.mode, e.x, e.y, e.z, e.yaw, e.pitch, e.now) == -2) break;
    retryEffects_.pop_front();
  }
  if (inputOffset_ == inputCount_) {
    inputCount_ = socket_.poll(incoming_);
    inputOffset_ = 0;
  }
  while (inputOffset_ < inputCount_) {
    const auto& r = incoming_[inputOffset_];
    if (r.data.size() > size_t(std::numeric_limits<int>::max())) { ++inputOffset_; continue; }
    const auto* bytes = reinterpret_cast<const uint8_t*>(r.data.data());
    int bits = coreNet().in(r.binary, bytes, int(r.data.size()), now());
    if (bits == -2) {
      drain();
      bits = coreNet().in(r.binary, bytes, int(r.data.size()), now());
      if (bits == -2) break;
    }
    if (bits > 0) {
      if (bits & QC_NET_PUMP) drain();
      if (!r.binary && (bits & QC_NET_PAGE) && sink_ && sid_) sink_({sid_, "message", r.data});
      if (!r.binary && follow && (bits & (QC_NET_FOLLOW_SOUND | QC_NET_FOLLOW_STATE)))
        follow(bits, r.data);
    }
    ++inputOffset_;
  }
  if (live_ && !socket_.connected()) closed(socket_.closeInfo());
  else drain();
}

}  // namespace nc
