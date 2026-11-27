// The hold-Tab information board: reads the snapshot the page reports (information-overlay.js, layout 3) and keeps what the HUD draws.
#include "nc_hud.h"
#include "nc_json.h"
#include <cstdio>

namespace nc {

static std::string joinName(const J& r) {
  std::string n;
  const J* num = r.get("playerNumber"); if (num && num->t == J::Num && num->n >= 0) { char b[24]; snprintf(b, sizeof b, "#%d ", (int)num->n); n = b; }
  const std::string tag = r.str("clanTag");
  if (!tag.empty()) n += tag + " " + r.str("coloredBaseName", r.str("name").c_str()); else n += r.str("coloredName", r.str("name").c_str());
  return n;
}
static ScoreRow rowOf(const J& r) {
  ScoreRow o; o.name = joinName(r); o.pb = r.str("pb", "-"); o.best = r.str("sessionBest", "-"); o.ping = r.str("ping", "-"); o.pbCmp = r.str("pbWrComparison"); o.bestCmp = r.str("sessionWrComparison");
  const J* y = r.get("you"); o.you = y && y->t == J::Bool && y->b; const J* w = r.get("watching"); o.watching = w && w->t == J::Bool && w->b; return o;
}

bool parseScoreboard(const std::string& json, Scoreboard& b) {
  J root; if (!parseJson(json, root) || root.t != J::Obj) return false;
  const J* h = root.get("held"); b.held = h && h->t == J::Bool && h->b;
  const J* s = root.get("snap"); if (!s || s->t != J::Obj) return true;   // only the held flag changed
  b.map = s->str("map"); if (b.map.empty()) b.map = "No current map";
  const std::string mode = s->str("mode"), mapTime = s->str("mapTime");
  b.info = (mode.empty() ? "-" : mode) + " - Physics: " + s->str("physics", "-") + " - Mode: " + s->str("category", "-") + " - Session: " + s->str("sessionTime", "-") + (mapTime.empty() ? "" : " - Map: " + mapTime);
  b.cards.clear(); b.groups.clear(); b.spectators.clear(); b.notice.clear();
  const J* prof = s->get("profileRecords"); const J* recs = s->get("records");
  const bool profile = prof && prof->t == J::Arr && !prof->a.empty();
  const J* act = s->get(profile ? "activeProfileRecord" : "activeRecord");
  auto cardOf = [&](const J& r) {
    ScoreCard c; const std::string phys = r.str("physics"), cat = r.str("category"), st = r.str("state");
    std::string up = phys; for (char& ch : up) ch = (char)toupper((unsigned char)ch);
    c.title = (profile ? r.str("label") : up + (cat == "w" ? " WEAPONS" : cat == "h" ? " HOOK" : " STRAFE")) + " - WR";
    c.time = st == "ready" ? r.str("time", "No record") : st == "loading" ? "Loading..." : "Unavailable"; if (c.time.empty()) c.time = "No record";
    c.name = st == "ready" ? joinName(r) : "Awaiting game data";
    if (act && act->t == J::Obj) c.active = profile ? act->str("profileId") == r.str("profileId") : (act->str("physics") == phys && act->str("category") == cat);
    return c; };
  if (profile) for (const J& r : prof->a) b.cards.push_back(cardOf(r));
  else if (recs && recs->t == J::Arr) for (const J& r : recs->a) b.cards.push_back(cardOf(r));
  const J* groups = s->get("groups"); const J* specs = s->get("spectators");
  size_t rowsN = 0;
  if (groups && groups->t == J::Arr) for (const J& g : groups->a) { ScoreGroup o; o.label = g.str("label"); const J* rows = g.get("rows"); if (rows && rows->t == J::Arr) for (const J& r : rows->a) o.rows.push_back(rowOf(r)); rowsN += o.rows.size(); if (!o.rows.empty()) b.groups.push_back(o); }
  if (specs && specs->t == J::Arr) for (const J& r : specs->a) b.spectators.push_back(joinName(r));
  const J* mp = s->get("multiplayer"); int total = 0;
  if (mp && mp->t == J::Obj) {
    total = (int)mp->num("total", 0);
    if (mp->str("state") == "disconnected" && mode != "Solo") b.notice = "Not connected to multiplayer."; else if (b.groups.empty()) b.notice = "No active players.";
  }
  const int count = mode == "Solo" ? (int)(rowsN + b.spectators.size()) : total;
  char f[96]; snprintf(f, sizeof f, "%d %s - Release Tab to return", count, count == 1 ? "player" : "players"); b.footer = f;
  return true;
}

}  // namespace nc
