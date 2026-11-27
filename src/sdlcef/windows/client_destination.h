#pragma once
#include <string_view>

#ifndef QRAZY_DEV_MODE
#define QRAZY_DEV_MODE 0
#endif

namespace QrazyDestination {
inline constexpr wchar_t LocalhostSwitch[] = L"--localhost";
inline constexpr wchar_t LocalhostMarker[] = L"QRAZY_SDLCEF_LOCALHOST";
inline constexpr char LiveOrigin[] = "https://qrazy-game.onrender.com/";
inline constexpr char LocalOrigin[] = "http://localhost:5173/";

constexpr bool ParseLaunch(std::wstring_view args, bool& local) {
  const auto first = args.find_first_not_of(L" \t");
  if (first != args.npos) args = args.substr(first, args.find_last_not_of(L" \t") - first + 1);
  else args = {};
  if (!args.empty() && args != LocalhostSwitch) return false;
  local = QRAZY_DEV_MODE || args == LocalhostSwitch;
  return true;
}
constexpr const char* Origin(bool local) { return local ? LocalOrigin : LiveOrigin; }
constexpr const wchar_t* Profile(bool local) {
  return local ? L"profile-sdlcef-windows-dev" : L"profile-sdlcef-windows";
}
}
