#pragma once
struct RenderPreferences {
  bool vsync=true;
  int max_fps=250;
  std::filesystem::path file;
  void Load(const std::filesystem::path& profile) {
    file=profile/L"render-settings.json";
    std::ifstream input(file,std::ios::binary);std::string text(4097,'\0');input.read(text.data(),4097);text.resize(static_cast<size_t>(input.gcount()));
    if(!input.is_open()||input.bad()||text.size()>4096)return;
    auto value=CefParseJSON(text,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
    if(!d)return;
    if(d->GetType("vsync")==VTYPE_BOOL)vsync=d->GetBool("vsync");
    if(d->GetType("com_maxfps")==VTYPE_INT){int n=d->GetInt("com_maxfps");if(n>=30&&n<=10000)max_fps=n;}
  }
  bool Save() const {
    auto d=CefDictionaryValue::Create();d->SetBool("vsync",vsync);d->SetInt("com_maxfps",max_fps);
    auto v=CefValue::Create();v->SetDictionary(d);const auto text=CefWriteJSON(v,JSON_WRITER_DEFAULT).ToString();
    auto temporary=file;temporary+=L".tmp";
    HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};DWORD written=0;
    bool ok=GetFileInformationByHandle(handle,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok&&MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
  }
  int CefTarget(int refresh) const {
    return vsync?std::min(refresh,max_fps):max_fps;
  }
  CefRefPtr<CefDictionaryValue> VSyncState() const {
    auto d=CefDictionaryValue::Create();d->SetBool("enabled",vsync);d->SetBool("activeEnabled",vsync);d->SetBool("restartRequired",false);d->SetBool("canDisable",true);return d;
  }
  CefRefPtr<CefDictionaryValue> MaxFpsState() const {
    auto d=CefDictionaryValue::Create();d->SetInt("value",max_fps);d->SetInt("minimum",30);d->SetInt("maximum",10000);d->SetInt("defaultValue",250);return d;
  }
};
