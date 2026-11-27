#include "nc_platform.h"
#if defined(NC_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace nc {
int64_t qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void sleepMs(int ms) { Sleep((DWORD)ms); }
void setClipboardText(const std::string& utf8) {
  std::wstring w; for (size_t i = 0; i < utf8.size();) { size_t j = utf8.find('\n', i); if (j == std::string::npos) j = utf8.size(); const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data() + i, (int)(j - i), nullptr, 0); const size_t at = w.size(); w.resize(at + (size_t)n); if (n) MultiByteToWideChar(CP_UTF8, 0, utf8.data() + i, (int)(j - i), &w[at], n); if (j < utf8.size()) w += L"\r\n"; i = j + 1; }
  if (!OpenClipboard(nullptr)) return; EmptyClipboard();
  if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) { wchar_t* d = (wchar_t*)GlobalLock(g); memcpy(d, w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
  CloseClipboard();
}
std::string getClipboardText() {
  std::string out; if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) { const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); if (n > 1) { out.resize((size_t)n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr); } GlobalUnlock(h); }
  CloseClipboard(); return out;
}
int64_t qpcFrequency() { static int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (int64_t)x.QuadPart; }(); return f; }
}
#else
#include <SDL3/SDL.h>
namespace nc {
int64_t qpcNow() { return (int64_t)SDL_GetTicksNS(); }
int64_t qpcFrequency() { return 1000000000LL; }
void sleepMs(int ms) { SDL_Delay((Uint32)ms); }
void setClipboardText(const std::string& utf8) { SDL_SetClipboardText(utf8.c_str()); }
std::string getClipboardText() { char* t = SDL_GetClipboardText(); std::string s = t ? t : ""; SDL_free(t); return s; }
}
#endif
