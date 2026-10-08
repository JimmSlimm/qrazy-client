// Logical SDL coordinates; saved normal bounds survive maximize/fullscreen.
std::filesystem::path prototype_profile;
struct WindowState {
  int x=0,y=0,width=1280,height=800;
  bool maximized=false,fullscreen=false;
  std::string monitor;
  void Observe() {
    auto flags=SDL_GetWindowFlags(window);fullscreen=flags&SDL_WINDOW_FULLSCREEN;
    if(!(flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MINIMIZED)))maximized=flags&SDL_WINDOW_MAXIMIZED;
    if(!(flags&(SDL_WINDOW_MAXIMIZED|SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MINIMIZED))) {
      SDL_GetWindowSize(window,&width,&height);SDL_GetWindowPosition(window,&x,&y);
    }
    const char* name=SDL_GetDisplayName(SDL_GetDisplayForWindow(window));monitor=name?name:"";
  }
  void Restore() {
    std::ifstream f(prototype_profile/"window-state.json");char buffer[4097];f.read(buffer,sizeof(buffer));std::string json(buffer,static_cast<size_t>(f.gcount()));
    if(json.size()>4096)return;
    auto v=CefParseJSON(json,JSON_PARSER_RFC);if(!v||v->GetType()!=VTYPE_DICTIONARY)return;
    auto d=v->GetDictionary();
    if(d->GetInt("schema")!=1||d->GetType("width")!=VTYPE_INT||d->GetType("height")!=VTYPE_INT)return;
    width=std::clamp(d->GetInt("width"),640,8192);height=std::clamp(d->GetInt("height"),480,8192);
    monitor=d->GetString("monitor");maximized=d->GetBool("maximized");fullscreen=d->GetBool("fullscreen");
    int count=0;auto displays=SDL_GetDisplays(&count);auto target=SDL_GetPrimaryDisplay();
    for(int i=0;displays&&i<count;++i){const char* name=SDL_GetDisplayName(displays[i]);if(name&&monitor==name){target=displays[i];break;}}SDL_free(displays);
    SDL_Rect bounds{};if(SDL_GetDisplayUsableBounds(target,&bounds)){
      auto restored=DesktopPolicy::RestoreBounds({d->GetInt("x"),d->GetInt("y"),width,height},{bounds.x,bounds.y,bounds.w,bounds.h});
      width=restored.width;height=restored.height;x=restored.x;y=restored.y;
      SDL_SetWindowPosition(window,x,y); // Wayland may deny this request.
    }
    SDL_SetWindowSize(window,width,height);
    if(maximized)SDL_MaximizeWindow(window);
    if(fullscreen)SDL_SetWindowFullscreen(window,true);
  }
  void Save() {
    auto d=CefDictionaryValue::Create();d->SetInt("schema",1);d->SetInt("x",x);d->SetInt("y",y);d->SetInt("width",width);d->SetInt("height",height);
    d->SetBool("maximized",maximized);d->SetBool("fullscreen",fullscreen);d->SetString("monitor",monitor);
    auto v=CefValue::Create();v->SetDictionary(d);auto text=CefWriteJSON(v,JSON_WRITER_DEFAULT).ToString();
    auto temp=prototype_profile/"window-state.partial",dest=prototype_profile/"window-state.json";
    int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    bool ok=fd>=0;size_t offset=0;
    while(ok&&offset<text.size()){ssize_t n=write(fd,text.data()+offset,text.size()-offset);if(n<=0)ok=false;else offset+=n;}
    if(fd>=0){if(fsync(fd))ok=false;close(fd);}if(ok&&rename(temp.c_str(),dest.c_str()))ok=false;
    if(ok){int directory=open(prototype_profile.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory<0||fsync(directory))ok=false;if(directory>=0)close(directory);}
    if(!ok)std::fprintf(stderr,"PROTOTYPE window state save failed; previous state retained\n");
  }
} saved_window;
