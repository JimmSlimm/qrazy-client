// The few things that differ by platform and are not worth a class: the monotonic clock the whole client times itself by.
// Windows: QueryPerformanceCounter. Linux: SDL_GetTicksNS, which is also the clock of SDL's event timestamps, so an input event's own time can be used directly.
#pragma once
#include <cstdint>
#include <string>

// NC_WINDOWS selects the Win32 backends (WASAPI, raw input, WinHTTP, WIC); otherwise the Linux ones (SDL3, libcurl, stb_image) are used.
// NC_FORCE_LINUX builds the Linux branches on a Windows compiler, for syntax checks only.
#if defined(_WIN32) && !defined(NC_FORCE_LINUX)
#define NC_WINDOWS 1
#endif

namespace nc {
int64_t qpcNow();
int64_t qpcFrequency();   // ticks per second
void sleepMs(int ms);
void setClipboardText(const std::string& utf8);   // plain UTF-8 text to the OS clipboard
std::string getClipboardText();                   // the OS clipboard's text as UTF-8 (empty when it holds none)
}
