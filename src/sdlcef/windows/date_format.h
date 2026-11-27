#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace QrazyDate {

inline std::wstring FormatWindows(double ms, bool date, bool time, bool seconds) {
  // FILETIME counts 100 ns ticks from 1601. Keep conversion inside SYSTEMTIME's range.
  if (ms < -11644473600000.0 || ms >= 253402300800000.0) return {};
  const auto ticks = static_cast<uint64_t>((static_cast<int64_t>(ms) + 11644473600000LL) * 10000);
  const FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
  SYSTEMTIME utc{}, local{};
  if (!FileTimeToSystemTime(&file_time, &utc) ||
      !SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local)) return {};

  std::wstring result;
  if (date) {
    const int size = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, nullptr, 0, nullptr);
    std::wstring formatted(size > 0 ? size : 0, L'\0');
    if (!size || !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, formatted.data(), size, nullptr))
      return {};
    formatted.pop_back();
    result = formatted;
  }
  if (time) {
    const DWORD flags = seconds ? 0 : TIME_NOSECONDS;
    const int size = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &local, nullptr, nullptr, 0);
    std::wstring formatted(size > 0 ? size : 0, L'\0');
    if (!size || !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &local, nullptr, formatted.data(), size))
      return {};
    formatted.pop_back();
    if (date) result += L' ';
    result += formatted;
  }
  return result;
}

}  // namespace QrazyDate
