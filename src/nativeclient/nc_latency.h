#pragma once

#include "nc_platform.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace nc {

inline thread_local uint64_t* frameAllocations = nullptr;

struct MotionReceipt {
  int64_t oldQpc = 0, newQpc = 0;
  long oldMessage = 0, newMessage = 0;
};

class LatencyLog {
  struct Frame {
    MotionReceipt motion;
    int64_t pump = 0, camera = 0, submit = 0, present = 0, ticks = 0;
    uint64_t allocs = 0;
  };
  static constexpr size_t capacity = 262144;
  std::filesystem::path path_;
  std::unique_ptr<Frame[]> frames_;
  uint64_t count_ = 0;
  Frame frame_;
  int64_t frequency_ = 0, previousTicks_ = 0;

public:
  bool active() const { return frames_ != nullptr; }

  void start(int64_t now, int64_t frequency) {
#if defined(NC_WINDOWS)
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
#else
    const char* base = std::getenv("LOCALAPPDATA");
#endif
    if (!base || !*base) return;
    const auto dir = std::filesystem::path(base) / "Qrazy" / "latlog";
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return;
    path_ = dir / ("latlog-" + std::to_string(now) + ".csv");
    frames_ = std::make_unique<Frame[]>(capacity);
    frequency_ = frequency;
  }

  void begin(int64_t ticks) {
    previousTicks_ = ticks;
    frame_ = {};
    frameAllocations = &frame_.allocs;
  }

  void camera(int64_t pump, MotionReceipt motion) {
    frame_.camera = qpcNow();
    frame_.pump = pump;
    frame_.motion = motion;
  }

  void submit() { frame_.submit = qpcNow(); }

  void present(int64_t ticks, int64_t returnedAt) {
    frame_.present = returnedAt;
    frameAllocations = nullptr;
    frame_.ticks = ticks - previousTicks_;
    previousTicks_ = ticks;
    frames_[count_++ % capacity] = frame_;
  }

  ~LatencyLog() { finish(); }

  void finish() {
    if (!active()) return;
    frameAllocations = nullptr;
#if defined(NC_WINDOWS)
    FILE* file = _wfopen(path_.c_str(), L"wb");
#else
    FILE* file = std::fopen(path_.c_str(), "wb");
#endif
    if (file) {
      std::fputs("frame,tEvOld,tEvNew,msgOld,msgNew,tPumpLast,tCamera,tSubmit,tPresentRet,tDisplayed,ticks,allocs\n", file);
      auto stamp = [&](int64_t qpc) {
        if (qpc) std::fprintf(file, "%.4f", qpc * 1000.0 / frequency_);
        std::fputc(',', file);
      };
      for (uint64_t i = count_ > capacity ? count_ - capacity : 0; i < count_; ++i) {
        const Frame& f = frames_[i % capacity];
        std::fprintf(file, "%llu,", static_cast<unsigned long long>(i));
        stamp(f.motion.oldQpc);
        stamp(f.motion.newQpc);
#if defined(NC_WINDOWS)
        if (f.motion.oldQpc) std::fprintf(file, "%ld,%ld", f.motion.oldMessage, f.motion.newMessage);
        else std::fputc(',', file);
#else
        std::fputc(',', file);
#endif
        std::fputc(',', file);
        stamp(f.pump);
        stamp(f.camera);
        stamp(f.submit);
        stamp(f.present);
        std::fprintf(file, ",%lld,%llu\n", static_cast<long long>(f.ticks), static_cast<unsigned long long>(f.allocs));
      }
      std::fclose(file);
    } else {
      std::fputs("latency recorder: could not open CSV\n", stderr);
    }
    frames_.reset();
  }
};
}  // namespace nc
