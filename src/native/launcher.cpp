#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Windows graphics preferences belong to the rendering executable, not its
// launcher. Register this copy before Electron creates any graphics devices.
// Preserve explicit GPU choices, but upgrade Windows' automatic/default entry.
// Other graphics settings in the same value must remain intact.
static void PreferHighPerformanceGpu(const std::wstring& executable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
      L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
      KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  DWORD type = 0, bytes = 0;
  const LSTATUS existing = RegQueryValueExW(key, executable.c_str(), nullptr, &type, nullptr, &bytes);
  std::wstring preference;
  bool writable = existing == ERROR_FILE_NOT_FOUND;
  if (existing == ERROR_SUCCESS && type == REG_SZ && bytes <= 65536 && bytes % sizeof(wchar_t) == 0) {
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD readBytes = bytes;
    if (RegQueryValueExW(key, executable.c_str(), nullptr, &type,
        reinterpret_cast<BYTE*>(data.data()), &readBytes) == ERROR_SUCCESS && type == REG_SZ) {
      preference.assign(data.data());
      writable = true;
      size_t start = 0;
      while (start < preference.size()) {
        const size_t end = preference.find(L';', start);
        const std::wstring field = preference.substr(start, end == std::wstring::npos ? end : end - start);
        if (field.compare(0, 14, L"GpuPreference=") == 0) {
          if (field == L"GpuPreference=0") preference.replace(start, field.size(), L"GpuPreference=2");
          // Preserve explicit choices and unknown future preference values.
          else writable = false;
          break;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
      }
    }
  }
  if (writable) {
    if (preference.find(L"GpuPreference=") == std::wstring::npos) {
      if (!preference.empty() && preference.back() != L';') preference += L';';
      preference += L"GpuPreference=2;";
    }
    RegSetValueExW(key, executable.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(preference.c_str()),
        static_cast<DWORD>((preference.size() + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(key);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!length || length >= buffer.size()) return 1;
  const std::wstring self(buffer.data(), length);
  const std::wstring folder = self.substr(0, self.find_last_of(L"\\/"));
  const std::wstring runtime = folder + L"\\runtime";
  const std::wstring executable = runtime + L"\\electron.exe";
  PreferHighPerformanceGpu(executable);
  std::wstring command = L"\"" + executable + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, runtime.c_str(), &startup, &process)) {
    MessageBoxW(nullptr, L"Unable to start Qrazy. Keep Qrazy.exe together with its runtime folder and extract the complete download before starting it.", L"Qrazy", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}
