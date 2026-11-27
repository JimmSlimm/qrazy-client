#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iconv.h>
#include <langinfo.h>
#include <locale.h>
#include <string>

namespace QrazyDate {

// Initialize in main so the zygote opens locale and timezone data before sandboxing.
inline locale_t DateLocale() {
  static const locale_t locale = newlocale(LC_TIME_MASK, "", nullptr);
  return locale;
}

inline int DesktopClock() {
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  if (!desktop) return 0;
  const std::string session = ":" + std::string(desktop) + ":";
  const char* command = nullptr;
  if (session.find(":X-Cinnamon:") != std::string::npos || session.find(":Cinnamon:") != std::string::npos)
    command = "gsettings get org.cinnamon.desktop.interface clock-use-24h 2>/dev/null";
  else if (session.find(":GNOME:") != std::string::npos || session.find(":Unity:") != std::string::npos)
    command = "gsettings get org.gnome.desktop.interface clock-format 2>/dev/null";
  if (!command) return 0;

  FILE* pipe = popen(command, "r");
  if (!pipe) return 0;
  char buffer[32]{};
  const bool read = std::fgets(buffer, sizeof(buffer), pipe) != nullptr;
  if (pclose(pipe) != 0 || !read) return 0;
  const std::string setting(buffer);
  if (setting == "'12h'\n" || setting == "false\n") return 12;
  if (setting == "'24h'\n" || setting == "true\n") return 24;
  return 0;
}

inline std::string TimePattern(locale_t locale, int clock, bool seconds, int hour) {
  std::string pattern = nl_langinfo_l(T_FMT, locale);
  const std::string ampm = nl_langinfo_l(T_FMT_AMPM, locale);
  if (pattern == "%r") pattern = ampm;
  if (pattern == "%T") pattern = "%H:%M:%S";
  if (pattern == "%R" && seconds) pattern = "%H:%M:%S";

  const bool twelve = pattern.find("%p") != std::string::npos ||
                      pattern.find("%P") != std::string::npos ||
                      pattern.find("%I") != std::string::npos ||
                      pattern.find("%l") != std::string::npos ||
                      pattern.find("%OI") != std::string::npos;
  const bool use_twelve = clock ? clock == 12 : twelve;
  const bool has_marker = *nl_langinfo_l(AM_STR, locale) && *nl_langinfo_l(PM_STR, locale);
  if (use_twelve && !twelve) pattern = has_marker ? ampm : "";
  if (!use_twelve && twelve) pattern.clear();

  // Some LC_TIME formats include a zone or pad a single-digit hour with a space.
  for (const auto* zone : {"%Z", "%z"}) {
    if (const auto position = pattern.find(zone); position != std::string::npos) {
      const auto start = position > 0 && pattern[position - 1] == ' ' ? position - 1 : position;
      const auto end = position + 2;
      pattern.erase(start, end - start + (start == position && end < pattern.size() && pattern[end] == ' '));
    }
  }
  if (const auto position = pattern.find("%l"); position != std::string::npos)
    pattern.replace(position, 2, "%-I");

  // Remove seconds from locale formats such as "%I:%M:%S %p" and "%H時%M分%S秒".
  auto minute = pattern.find("%M");
  size_t minute_width = 2;
  if (minute == std::string::npos) {
    minute = pattern.find("%OM");
    minute_width = 3;
  }
  auto second = pattern.find("%S");
  size_t second_width = 2;
  if (second == std::string::npos) {
    second = pattern.find("%OS");
    second_width = 3;
  }
  if (!pattern.empty() &&
      (minute != std::string::npos || pattern == "%R") &&
      (!seconds || second != std::string::npos)) {
    if (!seconds && second != std::string::npos && minute < second) {
      const size_t minute_end = minute + minute_width;
      const size_t second_end = second + second_width;
      const auto next = pattern.find('%', second_end);
      if (pattern.find_first_not_of(":., -", minute_end) == second) {
        pattern.erase(minute_end, second_end - minute_end);
      } else {
        const auto space = pattern.find(' ', second_end);
        const auto end = std::min(space, next);
        pattern.erase(second, end == std::string::npos ? end : end - second);
      }
    } else if (!seconds && second != std::string::npos) {
      pattern.clear();
    }
    if (!pattern.empty()) return pattern;
  }
  if (!use_twelve) return seconds ? "%H:%M:%S" : "%H:%M";
  if (has_marker) return seconds ? "%I:%M:%S %p" : "%I:%M %p";
  // Some 24-hour locales have no translated AM/PM strings.
  return std::string(seconds ? "%I:%M:%S " : "%I:%M ") + (hour < 12 ? "AM" : "PM");
}

inline std::string FormatLinux(double ms, bool date, bool time, bool seconds, int clock) {
  const locale_t locale = DateLocale();
  if (!locale) return {};
  const auto timestamp = static_cast<time_t>(std::floor(ms / 1000.0));
  struct tm local{};
  if (!localtime_r(&timestamp, &local)) return {};
  std::string pattern;
  if (date) {
    pattern = nl_langinfo_l(D_FMT, locale);
    if (pattern.empty()) return {};
  }
  if (time) {
    if (date) pattern += ' ';
    pattern += TimePattern(locale, clock, seconds, local.tm_hour);
  }
  char buffer[512];
  size_t size = strftime_l(buffer, sizeof(buffer), pattern.c_str(), &local, locale);
  if (!size) return {};

  const std::string codeset = nl_langinfo_l(_NL_TIME_CODESET, locale);
  if (codeset == "UTF-8" || codeset == "ANSI_X3.4-1968") return {buffer, size};
  iconv_t converter = iconv_open("UTF-8", codeset.c_str());
  if (converter == reinterpret_cast<iconv_t>(-1)) return {};
  char utf8[sizeof(buffer) * 4];
  char* input = buffer;
  char* output = utf8;
  size_t remaining = sizeof(utf8);
  const auto converted = iconv(converter, &input, &size, &output, &remaining);
  iconv_close(converter);
  if (converted == static_cast<size_t>(-1)) return {};
  return {utf8, static_cast<size_t>(output - utf8)};
}

}  // namespace QrazyDate
