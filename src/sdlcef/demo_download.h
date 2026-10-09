#pragma once
#include <atomic>
#include <memory>
#include "include/cef_task.h"
#include "include/cef_download_handler.h"
#include <SDL3/SDL.h>

namespace DemoDownload {
inline bool TrustedBlob(const std::string& url) {
  const std::string prefix="blob:https://qrazy-game.onrender.com/";
  return url.size()>prefix.size() && url.compare(0,prefix.size(),prefix)==0;
}
inline bool ExportName(const std::string& name) {
  const auto dot=name.rfind('.');
  const auto extension=dot==std::string::npos?std::string():name.substr(dot);
  return dot!=std::string::npos && dot>0 && name.size()<=128 && (extension==".qdemo" || extension==".cfg" || extension==".json") &&
    name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==std::string::npos;
}
inline std::atomic<bool> active{false};
class SaveTask final : public CefTask {
 public:
  CefRefPtr<CefBeforeDownloadCallback> callback;
  std::string path;
  explicit SaveTask(CefRefPtr<CefBeforeDownloadCallback> value):callback(value){}
  void Execute() override {if(!path.empty())callback->Continue(path,false);active=false;}
 private:
  IMPLEMENT_REFCOUNTING(SaveTask);
};
inline void SDLCALL Chosen(void* userdata,const char* const* files,int) {
  std::unique_ptr<CefRefPtr<SaveTask>> task(static_cast<CefRefPtr<SaveTask>*>(userdata));
  if(files&&files[0])(*task)->path=files[0];
  if(!CefPostTask(TID_UI,*task))active=false;
}
inline void Save(SDL_Window* window,const std::string& name,CefRefPtr<CefBeforeDownloadCallback> callback) {
  if(active.exchange(true))return;
  auto task=new CefRefPtr<SaveTask>(new SaveTask(callback));
  static const SDL_DialogFileFilter filter{"Qrazy exports","qdemo;cfg;json"};
  SDL_ShowSaveFileDialog(Chosen,task,window,&filter,1,name.c_str());
}
}
