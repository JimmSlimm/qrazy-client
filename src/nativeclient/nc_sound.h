#pragma once

#include "nc_json.h"
#include "qrazy_core_api.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nc {

static_assert(sizeof(QcCue) == 144);

inline int soundEvent(int kind, const QcCue& cue = {}) {
  return qc_ses_event(kind, reinterpret_cast<const char*>(&cue), sizeof cue);
}

inline int soundInt(double value, int fallback = 0) {
  return value >= INT32_MIN && value <= INT32_MAX && std::trunc(value) == value ? static_cast<int>(value) : fallback;
}

inline void soundName(QcCue& cue, const std::string& name) {
  memcpy(cue.name, name.data(), std::min(name.size(), sizeof cue.name - 1));
}

inline bool soundPosition(QcCue& cue, const J* pos) {
  if (!pos || pos->t != J::Arr || pos->a.size() != 3) return false;
  for (const J& axis : pos->a) if (axis.t != J::Num || !std::isfinite(axis.n)) return false;
  for (int i = 0; i < 3; ++i) cue.pos[i] = pos->a[i].n;
  return true;
}

inline void pageSound(const J& fact) {
  const int kind = soundInt(fact.num("kind"));
  if (kind < QC_SOUND_CHAT || (kind > QC_SOUND_WR_CLIP && kind != QC_SOUND_RESET && kind != QC_SOUND_FUNNY_STOP)) return;
  QcCue cue{};
  cue.flags = soundInt(fact.num("flags"));
  cue.emitter = soundInt(fact.num("emitter"));
  cue.group = soundInt(fact.num("group"));
  cue.gain = fact.num("gain");
  if (!std::isfinite(cue.gain)) return;
  soundName(cue, fact.str("name"));
  if (kind == QC_SOUND_TAUNT) {
    QcPeerPose pose{};
    if (!(cue.flags & 1) || cue.emitter <= 0 || !qc_peer_pose(cue.emitter, &pose)) return;
    memcpy(cue.pos, pose.pos, sizeof cue.pos);
  } else if (!soundPosition(cue, fact.get("pos")) && (cue.flags & 1) && kind == QC_SOUND_SPRAY) {
    return;
  }
  soundEvent(kind, cue);
}

inline bool isFollowedSender(int followed, const std::string& id) {
  if (followed <= 0 || id.empty()) return false;
  for (int slot = 0; slot < QC_PEER_MAX; ++slot) {
    QcPeerInfo peer{};
    if (qc_peer_info(slot, &peer) && peer.number == followed) return id == peer.id;
  }
  return false;
}

inline void watchedSound(const J& message, int followed) {
  if (!isFollowedSender(followed, message.str("id"))) return;
  const J* list = message.get("l");
  if (!list || list->t != J::Arr) return;
  const std::string voice = message.str("v");
  static constexpr const char* checkpoints[] = {"synth", "s_health", "n_health", "menu3", "off"};
  static constexpr const char* effects[] = {"jumppad", "teleport", "pickup", "button", "doorStart", "doorStop",
    "platStart", "platStop", "quadPickup", "hastePickup", "regenPickup", "invisPickup", "enviroPickup", "quadFire", "powerupWearoff"};
  for (const J& item : list->a) {
    QcCue cue{};
    const std::string play = item.str("play"), clip = item.str("voice"), finish = item.str("cue");
    if (!play.empty()) {
      soundName(cue, play);
      cue.flags = soundPosition(cue, item.get("p")) ? 1 : 0;
      cue.gain = item.num("g", 1);
      cue.group = soundInt(item.num("gr"));
      if (std::isfinite(cue.gain)) soundEvent(QC_SOUND_FOLLOW, cue);
    } else if (!clip.empty() && !voice.empty()) {
      soundName(cue, voice + "/" + clip);
      cue.flags = 16 | QC_CUE_EXPLICIT_VOICE;
      cue.gain = 1;
      soundEvent(QC_SOUND_FOLLOW, cue);
    } else if (finish == "checkpoint") {
      const std::string choice = item.str("sound");
      const auto at = std::find(std::begin(checkpoints), std::end(checkpoints), choice);
      cue.emitter = at == std::end(checkpoints) ? 0 : int(at - checkpoints);
      cue.group = 10;
      soundEvent(QC_SOUND_FOLLOW_DRY, cue);
    } else if (finish == "finish" || finish == "pb" || finish == "wr") {
      cue.emitter = finish == "wr" ? 2 : finish == "pb" ? 1 : 0;
      soundName(cue, item.str("sound"));
      soundEvent(QC_SOUND_FINISH, cue);
    } else if (const J* flight = item.get("flight"); flight && flight->t == J::Bool) {
      cue.flags = flight->b;
      soundEvent(QC_SOUND_FOLLOW_FLIGHT, cue);
    } else {
      const std::string effect = item.str("fx");
      const auto at = std::find(std::begin(effects), std::end(effects), effect);
      if (at != std::end(effects)) {
        cue.flags = soundPosition(cue, item.get("p")) ? 1 : 0;
        cue.emitter = int(at - effects);
        cue.group = 11;
      } else {
        cue.emitter = soundInt(item.num("speaker", -1), -1);
        if (cue.emitter < 0) continue;
        cue.group = 9;
      }
      soundEvent(QC_SOUND_FOLLOW_DRY, cue);
    }
  }
}

} // namespace nc
