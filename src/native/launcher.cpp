#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!length || length >= buffer.size()) return 1;
  const std::wstring self(buffer.data(), length);
  const std::wstring folder = self.substr(0, self.find_last_of(L"\\/"));
  const std::wstring runtime = folder + L"\\runtime";
  const std::wstring executable = runtime + L"\\electron.exe";
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
