#pragma once
// On-demand local support snapshot. No recording, uploads or personal identifiers.
std::string ReportUtf8(const wchar_t* value) {
  int size=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
  if(size<=1)return "Unavailable";
  std::string text(static_cast<size_t>(size),'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,text.data(),size,nullptr,nullptr);text.pop_back();return text;
}
void SupportGraphics(CefRefPtr<CefDictionaryValue> report) {
  report->SetInt("reportVersion",2);report->SetString("cefVersion",CEF_VERSION);report->SetString("chromiumVersion",std::to_string(CHROME_VERSION_MAJOR)+"."+std::to_string(CHROME_VERSION_MINOR)+"."+std::to_string(CHROME_VERSION_BUILD)+"."+std::to_string(CHROME_VERSION_PATCH));
  report->SetInt("sdlVersion",SDL_GetVersion());report->SetString("videoDriver",SDL_GetCurrentVideoDriver());
  SYSTEM_INFO system{};GetNativeSystemInfo(&system);report->SetInt("logicalProcessors",static_cast<int>(system.dwNumberOfProcessors));
  std::array<int,4> registers{};__cpuid(registers.data(),static_cast<int>(0x80000000u));
  if(static_cast<unsigned>(registers[0])>=0x80000004u){std::array<char,49> brand{};for(int i=0;i<3;++i){__cpuid(registers.data(),static_cast<int>(0x80000002u)+i);memcpy(brand.data()+i*16,registers.data(),16);}report->SetString("cpu",brand.data());}
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(GlobalMemoryStatusEx(&memory)){report->SetDouble("ramTotalMiB",memory.ullTotalPhys/1048576.0);report->SetDouble("ramAvailableMiB",memory.ullAvailPhys/1048576.0);report->SetInt("memoryLoadPercent",static_cast<int>(memory.dwMemoryLoad));}
  SYSTEM_POWER_STATUS power{};if(GetSystemPowerStatus(&power)){report->SetString("powerSource",power.ACLineStatus==1?"AC power":power.ACLineStatus==0?"Battery":"Unknown");if(power.BatteryLifePercent!=255)report->SetInt("batteryPercent",power.BatteryLifePercent);report->SetBool("batterySaver",power.SystemStatusFlag==1);}
  PROCESS_MEMORY_COUNTERS_EX process{};process.cb=sizeof(process);if(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))){report->SetDouble("hostWorkingSetMiB",process.WorkingSetSize/1048576.0);report->SetDouble("hostPrivateMiB",process.PrivateUsage/1048576.0);}
  auto powerLibrary=LoadLibraryExW(L"powrprof.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(powerLibrary){using PowerFn=DWORD(WINAPI*)(HKEY,GUID**);auto getScheme=reinterpret_cast<PowerFn>(GetProcAddress(powerLibrary,"PowerGetActiveScheme"));GUID* scheme=nullptr;if(getScheme&&getScheme(nullptr,&scheme)==ERROR_SUCCESS&&scheme){char id[40]{};snprintf(id,sizeof(id),"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",scheme->Data1,scheme->Data2,scheme->Data3,scheme->Data4[0],scheme->Data4[1],scheme->Data4[2],scheme->Data4[3],scheme->Data4[4],scheme->Data4[5],scheme->Data4[6],scheme->Data4[7]);report->SetString("powerPlan",id);LocalFree(scheme);}FreeLibrary(powerLibrary);}
  using VersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);auto versionFn=reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
  OSVERSIONINFOW os{};os.dwOSVersionInfoSize=sizeof(os);if(versionFn&&versionFn(&os)==0)report->SetString("windowsVersion",std::to_string(os.dwMajorVersion)+"."+std::to_string(os.dwMinorVersion)+" build "+std::to_string(os.dwBuildNumber));
  ULARGE_INTEGER freeBytes{};if(GetDiskFreeSpaceExW(updater_worker.root.c_str(),&freeBytes,nullptr,nullptr))report->SetDouble("installationFreeGiB",freeBytes.QuadPart/1073741824.0);
  std::array<wchar_t,32768> executable{};std::array<wchar_t,1024> preference{};DWORD preferenceBytes=static_cast<DWORD>(preference.size()*sizeof(wchar_t));
  if(GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()))&&RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\DirectX\\UserGpuPreferences",executable.data(),RRF_RT_REG_SZ,nullptr,preference.data(),&preferenceBytes)==ERROR_SUCCESS){std::wstring value(preference.data());report->SetString("windowsGpuPreference",value.find(L"GpuPreference=2;")!=std::wstring::npos?"High performance":value.find(L"GpuPreference=1;")!=std::wstring::npos?"Power saving":"System default or unrecognized");}
  else report->SetString("windowsGpuPreference","Unavailable or no explicit preference");
  report->SetString("frameClock",render_preferences.vsync?"Display vblank (current window monitor)":"High resolution timer");
  report->SetBool("remoteSession",GetSystemMetrics(SM_REMOTESESSION)!=0);
  report->SetBool("fullscreen",!!(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN));report->SetBool("focused",SDL_GetKeyboardFocus()==window);
  report->SetString("sandboxConfiguration","Renderer and GPU sandboxes requested; GPU sandbox initialization failures fatal (configuration, not live audit)");
  auto display=SDL_GetDisplayForWindow(window);const char* displayName=SDL_GetDisplayName(display);if(displayName)report->SetString("displayName",displayName);
  auto displays=CefListValue::Create();int count=0;auto ids=SDL_GetDisplays(&count);
  for(int i=0;ids&&i<count&&i<16;++i){auto d=CefDictionaryValue::Create();const auto* mode=SDL_GetCurrentDisplayMode(ids[i]);d->SetBool("current",ids[i]==display);const char* name=SDL_GetDisplayName(ids[i]);if(name)d->SetString("name",name);if(mode){d->SetInt("width",mode->w);d->SetInt("height",mode->h);d->SetDouble("refreshHz",mode->refresh_rate_denominator>0?static_cast<double>(mode->refresh_rate_numerator)/mode->refresh_rate_denominator:mode->refresh_rate);}displays->SetDictionary(static_cast<size_t>(i),d);}SDL_free(ids);report->SetList("displays",displays);
  Microsoft::WRL::ComPtr<IDXGIDevice> device;Microsoft::WRL::ComPtr<IDXGIAdapter> active;DXGI_ADAPTER_DESC activeDesc{};
  auto native=static_cast<ID3D11Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr));
  if(native&&SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&device)))&&SUCCEEDED(device->GetAdapter(&active)))active->GetDesc(&activeDesc);
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;auto adapters=CefListValue::Create();
  if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))for(UINT i=0;i<16;++i){Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)))continue;auto g=CefDictionaryValue::Create();g->SetString("name",ReportUtf8(desc.Description));g->SetBool("presentation",active&&desc.AdapterLuid.LowPart==activeDesc.AdapterLuid.LowPart&&desc.AdapterLuid.HighPart==activeDesc.AdapterLuid.HighPart);g->SetInt("vendorId",static_cast<int>(desc.VendorId));g->SetInt("deviceId",static_cast<int>(desc.DeviceId));g->SetBool("software",!!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));g->SetDouble("dedicatedMiB",desc.DedicatedVideoMemory/1048576.0);g->SetDouble("sharedMiB",desc.SharedSystemMemory/1048576.0);
    LARGE_INTEGER driver{};if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))g->SetString("driverVersion",std::to_string(HIWORD(driver.HighPart))+"."+std::to_string(LOWORD(driver.HighPart))+"."+std::to_string(HIWORD(driver.LowPart))+"."+std::to_string(LOWORD(driver.LowPart)));
    Microsoft::WRL::ComPtr<IDXGIAdapter3> memoryAdapter;DXGI_QUERY_VIDEO_MEMORY_INFO usage{};if(SUCCEEDED(adapter.As(&memoryAdapter))&&SUCCEEDED(memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&usage))){g->SetDouble("localBudgetMiB",usage.Budget/1048576.0);g->SetDouble("hostLocalUsageMiB",usage.CurrentUsage/1048576.0);}
    adapters->SetDictionary(adapters->GetSize(),g);
  }report->SetList("adapters",adapters);
}
