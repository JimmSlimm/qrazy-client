#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Windows graphics preferences belong to the rendering executable, not its
// launcher. Register this copy before Electron creates any graphics devices.
// Respect existing user choices and keep failure nonfatal (e.g. managed PCs).
static void PreferHighPerformanceGpu(const std::wstring& executable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
      L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
      KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  const LSTATUS existing = RegQueryValueExW(key, executable.c_str(), nullptr, nullptr, nullptr, nullptr);
  if (existing == ERROR_FILE_NOT_FOUND) {
    const wchar_t preference[] = L"GpuPreference=2;";
    RegSetValueExW(key, executable.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(preference), sizeof(preference));
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
