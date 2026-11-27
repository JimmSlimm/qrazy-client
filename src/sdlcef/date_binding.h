#pragma once

#include <cmath>
#include "include/cef_v8.h"

namespace QrazyDate {

using TrustedUrl = bool (*)(const CefString&);
using Formatter = CefString (*)(double, bool, bool, bool, int);

class Binding final : public CefV8Handler {
 public:
  Binding(TrustedUrl trusted, Formatter format, int clock)
      : trusted_(trusted), format_(format), clock_(clock) {}

  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args,
               CefRefPtr<CefV8Value>& value, CefString&) override {
    const auto context = CefV8Context::GetCurrentContext();
    const auto frame = context ? context->GetFrame() : nullptr;
    CefString result;
    if (frame && frame->IsMain() && trusted_(frame->GetURL()) && args.size() == 3 &&
        (args[0]->IsDouble() || args[0]->IsInt() || args[0]->IsUInt()) &&
        args[1]->IsString() && args[2]->IsString() &&
        args[1]->GetStringValue().length() <= 5 && args[2]->GetStringValue().length() <= 6) {
      const double ms = args[0]->IsInt() ? static_cast<double>(args[0]->GetIntValue())
                      : args[0]->IsUInt() ? static_cast<double>(args[0]->GetUIntValue())
                      : args[0]->GetDoubleValue();
      const auto date = args[1]->GetStringValue();
      const auto time = args[2]->GetStringValue();
      if (std::isfinite(ms) && std::abs(ms) <= 8640000000000000.0 &&
          (date.empty() || date == "short") &&
          (time.empty() || time == "short" || time == "medium") &&
          (!date.empty() || !time.empty())) {
        result = format_(std::trunc(ms), !date.empty(), !time.empty(), time == "medium", clock_);
      }
    }
    value = CefV8Value::CreateString(result);
    return true;
  }

 private:
  TrustedUrl trusted_;
  Formatter format_;
  int clock_;
  IMPLEMENT_REFCOUNTING(Binding);
};

inline void Install(CefRefPtr<CefV8Context> context, TrustedUrl trusted, Formatter format, int clock = 0) {
  constexpr auto name = "qrazyNativeFormatDate";
  context->GetGlobal()->SetValue(name, CefV8Value::CreateFunction(name, new Binding(trusted, format, clock)),
                                 V8_PROPERTY_ATTRIBUTE_READONLY);
}

}  // namespace QrazyDate
