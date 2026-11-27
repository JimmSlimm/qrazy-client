// A small JSON reader: enough for the hub's messages and the page's scoreboard snapshot (objects, arrays, strings, numbers, booleans, null).
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc {
inline std::string jsonQuote(std::string_view text) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    else result += char(c);
  }
  result += '"';
  return result;
}


// ---- a small JSON reader: enough for the hub's messages (objects, arrays, strings, numbers, booleans, null)
struct J {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null; bool b = false; double n = 0; std::string s; std::vector<J> a; std::vector<std::pair<std::string, J>> o;
  const J* get(const char* k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
  std::string str(const char* k, const char* d = "") const { const J* v = get(k); return v && v->t == Str ? v->s : d; }
  double num(const char* k, double d = 0) const { const J* v = get(k); return v && v->t == Num ? v->n : d; }
};
struct JP {
  const char* p; const char* e; bool ok = true;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p; }
  static void utf8(std::string& s, unsigned c) { if (c < 0x80) s += (char)c; else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 63)); } else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); } else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 63)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); } }
  std::string string() {
    std::string r; ++p;   // opening quote
    while (p < e && *p != '"') {
      if (*p == (char)92 && p + 1 < e) { ++p; const char c = *p++;
        if (c == 'n') r += (char)10; else if (c == 't') r += (char)9; else if (c == 'r') r += (char)13; else if (c == 'b') r += (char)8; else if (c == 'f') r += (char)12;
        else if (c == 'u' && p + 4 <= e) { unsigned u = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16); p += 4;
          if (u >= 0xD800 && u < 0xDC00 && p + 6 <= e && p[0] == (char)92 && p[1] == 'u') { unsigned lo = (unsigned)strtoul(std::string(p + 2, 4).c_str(), nullptr, 16); p += 6; u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); } utf8(r, u); }
        else r += c;
      } else r += *p++;
    }
    if (p < e) ++p; else ok = false; return r;
  }
  J value(int depth = 0) {
    J v; ws(); if (p >= e || depth > 32) { ok = false; return v; }
    if (*p == '{') { v.t = J::Obj; ++p; ws(); if (p < e && *p == '}') { ++p; return v; }
      while (ok && p < e) { ws(); if (p >= e || *p != '"') { ok = false; break; } std::string k = string(); ws(); if (p >= e || *p != ':') { ok = false; break; } ++p; v.o.emplace_back(k, value(depth + 1)); ws(); if (p < e && *p == ',') { ++p; continue; } if (p < e && *p == '}') { ++p; break; } ok = false; } }
    else if (*p == '[') { v.t = J::Arr; ++p; ws(); if (p < e && *p == ']') { ++p; return v; }
      while (ok && p < e) { v.a.push_back(value(depth + 1)); ws(); if (p < e && *p == ',') { ++p; continue; } if (p < e && *p == ']') { ++p; break; } ok = false; } }
    else if (*p == '"') { v.t = J::Str; v.s = string(); }
    else if (!strncmp(p, "true", 4)) { v.t = J::Bool; v.b = true; p += 4; } else if (!strncmp(p, "false", 5)) { v.t = J::Bool; p += 5; } else if (!strncmp(p, "null", 4)) p += 4;
    else { char* end = nullptr; v.n = strtod(p, &end); if (end == p) { ok = false; return v; } v.t = J::Num; p = end; }
    return v;
  }
};
inline bool parseJson(const std::string& s, J& out) { JP jp{s.data(), s.data() + s.size()}; out = jp.value(); return jp.ok; }

}  // namespace nc
