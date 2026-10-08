bool Trusted(const CefString& url) {
  CefURLParts parts;
  return CefParseURL(url, parts) && CefString(&parts.scheme) == "https" &&
    CefString(&parts.host) == "qrazy-game.onrender.com" &&
    (CefString(&parts.port).empty() || CefString(&parts.port) == "443") &&
    CefString(&parts.username).empty() && CefString(&parts.password).empty();
}
CefRefPtr<CefDictionaryValue> Dict() { return CefDictionaryValue::Create(); }

class Native final : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& value, CefString& exception) override {
    auto context = CefV8Context::GetCurrentContext();
    auto frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsMain() || !Trusted(frame->GetURL()) || args.size() != 3 || !args[2]->IsString() || args[2]->GetStringValue().length()>1500000 ||
        !args[0]->IsString() || !args[1]->IsInt()) { exception = "Unauthorized native bridge call"; return true; }
    const auto op = args[0]->GetStringValue().ToString();
    if (op != "capture" && op != "release" && op != "clock" && op != "quit" && op != "fullscreen-state" && op != "fullscreen-toggle" && op != "assets" && op != "status" && op != "retry" && op != "diagnostics" && op != "clipboard-write" && op != "config-import" && op != "config-export" && op != "update-state" && op != "update-check" && op != "update-stage" && op != "update-install" && op != "update-rollback" && op != "changelog") {
      exception = "Unknown native operation"; return true;
    }
    auto message = CefProcessMessage::Create("qrazy-command-v1");
    message->GetArgumentList()->SetString(0, op);
    message->GetArgumentList()->SetInt(1, args[1]->GetIntValue());
    message->GetArgumentList()->SetString(2,args[2]->GetStringValue());
    frame->SendProcessMessage(PID_BROWSER, message);
    value = CefV8Value::CreateUndefined(); return true;
  }
 private:
  IMPLEMENT_REFCOUNTING(Native);
};

class App final : public CefApp, public CefRenderProcessHandler {
  struct Binding { CefRefPtr<CefV8Context> context; CefRefPtr<CefV8Value> dispatch; };
  std::map<int, Binding> bindings;
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
  void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> line) override {
    // Deliberately no user-controlled switches in the browser process launch path.
    // Extensions are outside this game's trust boundary, including distro auto-installed ones.
    line->AppendSwitch("disable-extensions");

    line->AppendSwitchWithValue("use-angle", "d3d11");
    line->AppendSwitchWithValue("class", "qrazy-sdl-cef-prototype");
    // Do not add enable-logging: Chromium creates a Windows console for this
    // switch even with a file destination. CefSettings selects the log file.
    // Preserve CEF's inherited sandbox log handle; "handle" does not create a
    // console and must not be replaced with a path in sandboxed subprocesses.
    if(line->GetSwitchValue("enable-logging")!="handle")line->RemoveSwitch("enable-logging");
    // Initialize Mesa before sealing the GPU sandbox; failure is always fatal.
    // Disk-cache workers are disabled below, so they cannot race this step.
    line->AppendSwitchWithValue("gpu-sandbox-failures-fatal", "yes");
  }
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    if (!frame->IsMain() || !Trusted(frame->GetURL())) return;
    auto native = CefV8Value::CreateFunction("qrazyNative", new Native);
    // Temporary bootstrap slot; removed before any website script runs. Dispatcher stays private.
    context->GetGlobal()->SetValue("__qrazyBootstrap", native, V8_PROPERTY_ATTRIBUTE_NONE);
    CefRefPtr<CefV8Value> dispatch; CefRefPtr<CefV8Exception> exception;
    bool ok = context->Eval(bridge_source + "(__qrazyBootstrap)", "qrazy-native-bridge", 1, dispatch, exception);
    context->GetGlobal()->DeleteValue("__qrazyBootstrap");
    if (ok && dispatch && dispatch->IsFunction()) {
      bindings[browser->GetIdentifier()] = {context, dispatch};
      std::fprintf(stderr, "PROTOTYPE trusted main-frame bridge installed\n");
    }
    else std::fprintf(stderr, "PROTOTYPE bridge bootstrap failed\n");
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    auto it = bindings.find(browser->GetIdentifier());
    if (frame->IsMain() && it != bindings.end() && it->second.context->IsSame(context)) bindings.erase(it);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message) override {
    if (source != PID_BROWSER || message->GetName() != "qrazy-event-v1" || !frame->IsMain() || !Trusted(frame->GetURL())) return false;
    auto it = bindings.find(browser->GetIdentifier());
    if (it == bindings.end() || !it->second.context->IsValid()) return true;
    auto& binding = it->second;
    binding.context->Enter();
    binding.dispatch->ExecuteFunction(nullptr, {CefV8Value::CreateString(message->GetArgumentList()->GetString(0))});
    binding.context->Exit(); return true;
  }
 private:
  IMPLEMENT_REFCOUNTING(App);
};

