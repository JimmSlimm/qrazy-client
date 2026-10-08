#pragma once
struct WindowState {
  std::filesystem::path profile;
  int x=0,y=0,width=1280,height=800;
  bool maximized=false,fullscreen=false;
  std::string monitor;
  void Observe(){auto flags=SDL_GetWindowFlags(window);fullscreen=!!(flags&SDL_WINDOW_FULLSCREEN);
    if(!(flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MINIMIZED)))maximized=!!(flags&SDL_WINDOW_MAXIMIZED);
    if(!(flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MINIMIZED|SDL_WINDOW_MAXIMIZED))){SDL_GetWindowSize(window,&width,&height);SDL_GetWindowPosition(window,&x,&y);}
    auto name=SDL_GetDisplayName(SDL_GetDisplayForWindow(window));monitor=name?name:"";
  }
  void Restore(){QrazyWindows::File file;if(!file.Open(profile/"window-state.json",GENERIC_READ,OPEN_EXISTING)||file.Size()>4096)return;
    std::string text(static_cast<size_t>(file.Size()),'\0');if(!file.Read(text.data(),static_cast<DWORD>(text.size())))return;
    auto value=CefParseJSON(text,JSON_PARSER_RFC);auto d=value&&value->GetType()==VTYPE_DICTIONARY?value->GetDictionary():nullptr;
    if(!d||d->GetInt("schema")!=1||d->GetType("width")!=VTYPE_INT||d->GetType("height")!=VTYPE_INT)return;
    monitor=d->GetString("monitor");maximized=d->GetBool("maximized");fullscreen=d->GetBool("fullscreen");
    auto target=SDL_GetPrimaryDisplay();int count=0;auto displays=SDL_GetDisplays(&count);
    for(int i=0;displays&&i<count;++i){auto name=SDL_GetDisplayName(displays[i]);if(name&&monitor==name){target=displays[i];break;}}SDL_free(displays);
    SDL_Rect display{};if(!SDL_GetDisplayUsableBounds(target,&display))return;
    auto b=DesktopPolicy::RestoreBounds({d->GetInt("x"),d->GetInt("y"),d->GetInt("width"),d->GetInt("height")},{display.x,display.y,display.w,display.h});
    x=b.x;y=b.y;width=b.width;height=b.height;SDL_SetWindowSize(window,width,height);SDL_SetWindowPosition(window,x,y);
    if(maximized)SDL_MaximizeWindow(window);if(fullscreen)SDL_SetWindowFullscreen(window,true);
  }
  void Save(){auto d=Dict();d->SetInt("schema",1);d->SetInt("x",x);d->SetInt("y",y);d->SetInt("width",width);d->SetInt("height",height);d->SetBool("maximized",maximized);d->SetBool("fullscreen",fullscreen);d->SetString("monitor",monitor);
    auto text=QrazyWindows::Serialize(d);auto temp=profile/"window-state.partial",dest=profile/"window-state.json";QrazyWindows::File file;
    if(!file.Open(temp,GENERIC_WRITE,CREATE_ALWAYS,0)){std::fprintf(stderr,"PROTOTYPE window-state save unavailable\n");return;}
    bool ok=file.Write(text.data(),static_cast<DWORD>(text.size()))&&FlushFileBuffers(file.handle);file.Close();
    if(!ok||!QrazyWindows::NoReparse(dest)||!MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))std::fprintf(stderr,"PROTOTYPE window-state replacement failed\n");
  }
} saved_window;
