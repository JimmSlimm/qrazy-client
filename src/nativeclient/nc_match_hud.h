#pragma once
#include "qrazy_core_api.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

static_assert(sizeof(QcHud) == 288);
static_assert(sizeof(QcHudFrame) == 952);
static_assert(offsetof(QcHudFrame, chatFirst) == 56);
static_assert(offsetof(QcHudFrame, boardRevision) == 80);
static_assert(offsetof(QcHudFrame, startOrEnd) == 92);
static_assert(offsetof(QcHudFrame, now) == 96);
static_assert(offsetof(QcHudFrame, boardSpeed) == 160);
static_assert(offsetof(QcHudFrame, jumps) == 184);
static_assert(offsetof(QcHudFrame, loadingText) == 248);
static_assert(offsetof(QcHudFrame, boardCarried) == 504);
static_assert(offsetof(QcHudFrame, noticeEntities) == 508);
static_assert(offsetof(QcHudFrame, numbers) == 664);

namespace nc {

struct MatchChat {
  // A power of two keeps slots aligned when the core's uint32_t indices wrap.
  std::array<std::string, 64> lines;
  uint32_t end = 0;

  void reset() {
    end = 0;
    for (auto& line : lines) line.clear();
  }
  void append(std::string text) { lines[end++ % lines.size()] = std::move(text); }
  void join(const QcHudFrame& frame, std::string& out) const {
    out.clear();
    for (uint32_t i = frame.chatFirst; i != frame.chatEnd; ++i) {
      if (i != frame.chatFirst) out += '\n';
      out += lines[i % lines.size()];
    }
  }
};

struct MatchHudFeed {
  QcHudFrame sent{};
  bool first = true;

  std::string delta(const QcHudFrame& frame, const std::string& map) {
    const bool run = first || sent.runRevision != frame.runRevision;
    const bool speed = first || sent.speedRevision != frame.speedRevision;
    const bool split = first || sent.splitRevision != frame.splitRevision;
    const bool jumps = first || sent.jumpRevision != frame.jumpRevision;
    const bool notice = first || sent.noticeRevision != frame.noticeRevision;
    unsigned board = 0;
    for (unsigned i = 0; i < 3; ++i)
      if (sent.boardRevision[i] != frame.boardRevision[i]) board |= 1u << i;
    if (!(run || speed || split || jumps || notice || board) && sent.mode == frame.mode) return {};

    const auto age = [&](double stamp) { return std::max(0.0, (frame.now - stamp) * 1000.0); };
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(15) << "{\"map\":" << std::quoted(map) << ",\"mode\":" << frame.mode;
    if (run)
      out << ",\"run\":{\"kind\":" << frame.runText << ",\"checkpoint\":" << frame.checkpoint
          << ",\"deathCause\":" << frame.deathCause << '}';
    if (speed)
      out << ",\"speed\":{\"kind\":" << frame.startOrEnd << ",\"value\":" << frame.speed
          << ",\"ageMs\":" << (frame.startOrEnd ? age(frame.speedAt) : 0) << '}';
    if (split)
      out << ",\"split\":{\"checkpoint\":" << frame.checkpoint << ",\"elapsed\":" << frame.split
          << ",\"ageMs\":" << (frame.checkpoint ? age(frame.splitAt) : 0) << '}';
    if (jumps) {
      out << ",\"jumps\":[";
      for (unsigned i = 0; i < std::min(frame.jumpCount, 8u); ++i) {
        if (i) out << ',';
        out << frame.jumps[i];
      }
      out << ']';
    }
    if (notice) {
      out << ",\"notice\":{\"kind\":" << frame.noticeKind << ",\"id\":" << frame.noticeId << ",\"entities\":[";
      if (frame.noticeKind == QC_HUD_TARGET_PICKUP) {
        for (unsigned i = 0; i < 39 && frame.noticeEntities[i] != -1; ++i) {
          if (i) out << ',';
          out << frame.noticeEntities[i];
        }
      }
      out << "],\"ageMs\":" << (frame.noticeKind ? age(frame.noticeAt) : 0) << '}';
    }
    if (board) {
      out << ",\"board\":[";
      bool comma = false;
      for (unsigned i = 0; i < 3; ++i) {
        const unsigned bit = 1u << i;
        if (!(board & bit)) continue;
        if (comma) out << ',';
        comma = true;
        out << "{\"kind\":" << i << ",\"value\":" << frame.boardSpeed[i]
            << ",\"carried\":" << ((frame.boardCarried & bit) ? "true" : "false") << '}';
      }
      out << ']';
    }
    out << '}';
    sent = frame;
    first = false;
    return out.str();
  }
};

}  // namespace nc
