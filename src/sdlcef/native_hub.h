#pragma once

#include "native_match.h"
#include "nc_hub.h"

namespace QrazyNativeMatch {

inline bool HandleHub(const std::string& request) {
  auto value=CefParseJSON(request,JSON_PARSER_RFC);
  if(!value||value->GetType()!=VTYPE_DICTIONARY)return false;
  auto d=value->GetDictionary();
  const std::string op=d->GetString("op");
  nc::HubRequest r{};
  r.sid=d->GetInt("sid");
  if(op=="hubopen"){
    r.op=nc::HubRequest::Open;
    r.text=d->GetString("url");
    if(!QrazyNativeMatch::NativeText(r.text,2048))r.text.clear();
  }else if(op=="hubtext"){
    r.op=nc::HubRequest::Text;
    if(d->GetType("text")!=VTYPE_STRING)return true;
    r.text=d->GetString("text");
  }else if(op=="hubfact"){
    r.op=nc::HubRequest::Fact;
    r.kind=d->GetInt("kind");r.a=d->GetInt("a");r.b=d->GetInt("b");
    if(r.kind<QC_NET_LOADED||r.kind>QC_NET_FOLLOW||d->GetType("kind")!=VTYPE_INT)return true;
    r.value=d->GetType("value")==VTYPE_INT?d->GetInt("value"):d->GetDouble("value");
    r.text=d->GetString("text");
    if(r.text.size()>1024)return true;
  }else if(op=="hubclose")r.op=nc::HubRequest::Close;
  else return false;
  if(r.sid>0&&nc::coreNet())nc::appHub().post(std::move(r));
  return true;
}

inline std::string EncodeHubEvent(const nc::HubEvent& event) {
  auto d=CefDictionaryValue::Create();
  d->SetInt("sid",event.sid);d->SetString("type",event.type);
  const std::string_view type(event.type);
  if(type=="message")d->SetString("data",std::string(event.data));
  if(type=="close"){
    d->SetInt("code",event.code);d->SetString("reason",std::string(event.data));d->SetBool("wasClean",event.clean);
  }
  auto value=CefValue::Create();value->SetDictionary(d);
  return CefWriteJSON(value,JSON_WRITER_DEFAULT).ToString();
}

}  // namespace QrazyNativeMatch
