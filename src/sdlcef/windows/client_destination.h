#pragma once
// Selected at build time; development never grants the production origin a bridge.
#ifndef QRAZY_DEV_MODE
#define QRAZY_DEV_MODE 0
#endif
namespace QrazyDestination {
#if QRAZY_DEV_MODE
inline constexpr char Origin[] = "http://localhost:5173/";
inline constexpr wchar_t Profile[] = L"profile-sdlcef-windows-dev";
#else
inline constexpr char Origin[] = "https://qrazy-game.onrender.com/";
inline constexpr wchar_t Profile[] = L"profile-sdlcef-windows";
#endif
}
